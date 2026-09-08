/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "dmic.h"
#include "wakeword.h"

LOG_MODULE_REGISTER(main);

#define LED1_NODE DT_ALIAS(led1)
#define LED2_NODE DT_ALIAS(led2)
#define LED3_NODE DT_ALIAS(led3)

#if !DT_NODE_HAS_STATUS(LED1_NODE, okay)
#error "Unsupported board: led1 devicetree alias is not defined"
#endif

#if !DT_NODE_HAS_STATUS(LED2_NODE, okay)
#error "Unsupported board: led2 devicetree alias is not defined"
#endif

#if !DT_NODE_HAS_STATUS(LED3_NODE, okay)
#error "Unsupported board: led3 devicetree alias is not defined"
#endif

static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(LED1_NODE, gpios);
static const struct gpio_dt_spec led2 = GPIO_DT_SPEC_GET(LED2_NODE, gpios);
static const struct gpio_dt_spec led3 = GPIO_DT_SPEC_GET(LED3_NODE, gpios);

#define WW_LED_PULSE_MS       500
#define VAD_LATCH_WINDOW_MS   800

static bool ww_led2_on;
static bool ww_led3_on;
static int64_t ww_led_off_time_ms;
static int64_t last_vad_on_time_ms;
static int64_t vad_recent_until_ms;

/*
 * Stricter VAD tuning for less background-noise sensitivity.
 */
#define SILENCE_CHUNKS_OFF    35U
#define NOISE_INIT_RMS        25U
#define NOISE_MARGIN_ON       120U
#define NOISE_MARGIN_OFF      70U
#define NOISE_ALPHA_NUM       1U
#define NOISE_ALPHA_DEN       24U
#define VAD_ON_CHUNKS_REQ     2U

static bool speech_active;
static uint32_t silence_chunks;
static uint32_t speech_on_chunks;
static uint32_t noise_rms = NOISE_INIT_RMS;

struct chunk_stats {
    uint32_t rms;
    int16_t peak;
    uint32_t zcr;
    uint32_t est_hz;
};

static void trigger_ww_leds(bool arm_led3)
{
    (void)gpio_pin_set_dt(&led2, 1);
    ww_led2_on = true;

    if (arm_led3) {
        (void)gpio_pin_set_dt(&led3, 1);
        ww_led3_on = true;
    } else {
        (void)gpio_pin_set_dt(&led3, 0);
        ww_led3_on = false;
    }

    ww_led_off_time_ms = k_uptime_get() + WW_LED_PULSE_MS;
}

static void service_ww_leds(void)
{
    if (k_uptime_get() >= ww_led_off_time_ms) {
        if (ww_led2_on) {
            (void)gpio_pin_set_dt(&led2, 0);
            ww_led2_on = false;
        }
        if (ww_led3_on) {
            (void)gpio_pin_set_dt(&led3, 0);
            ww_led3_on = false;
        }
    }
}

static void set_vad_led(bool on)
{
    (void)gpio_pin_set_dt(&led1, on ? 1 : 0);

    if (on) {
        int64_t now = k_uptime_get();
        last_vad_on_time_ms = now;
        vad_recent_until_ms = now + VAD_LATCH_WINDOW_MS;
    }
}

static bool vad_recently_active(void)
{
    return (k_uptime_get() <= vad_recent_until_ms);
}

static uint32_t isqrt64(uint64_t x)
{
    uint64_t op = x;
    uint64_t res = 0;
    uint64_t one = 1ULL << 62;

    while (one > op) {
        one >>= 2;
    }

    while (one != 0U) {
        if (op >= (res + one)) {
            op -= (res + one);
            res = (res >> 1) + one;
        } else {
            res >>= 1;
        }
        one >>= 2;
    }

    return (uint32_t)res;
}

static void analyze_chunk(const int16_t *samples, size_t count,
              struct chunk_stats *stats)
{
    uint64_t energy = 0;
    int16_t peak_abs = 0;
    uint32_t crossings = 0;
    bool was_positive;

    if ((samples == NULL) || (stats == NULL) || (count == 0U)) {
        if (stats != NULL) {
            stats->rms = 0;
            stats->peak = 0;
            stats->zcr = 0;
            stats->est_hz = 0;
        }
        return;
    }

    was_positive = (samples[0] >= 0);

    for (size_t i = 0; i < count; i++) {
        int32_t s = samples[i];
        int32_t abs32 = (s < 0) ? -s : s;

        if (abs32 > INT16_MAX) {
            abs32 = INT16_MAX;
        }

        energy += (uint64_t)((int64_t)s * (int64_t)s);

        if ((int16_t)abs32 > peak_abs) {
            peak_abs = (int16_t)abs32;
        }

        if (i > 0U) {
            bool is_positive = (s >= 0);
            if (is_positive != was_positive) {
                crossings++;
            }
            was_positive = is_positive;
        }
    }

    stats->rms = isqrt64(energy / count);
    stats->peak = peak_abs;
    stats->zcr = crossings;
    stats->est_hz = (count > 1U) ? ((crossings * DMIC_PCM_RATE) / (2U * count)) : 0U;
}

static uint32_t vad_th_on(void)
{
    return noise_rms + NOISE_MARGIN_ON;
}

static uint32_t vad_th_off(void)
{
    return noise_rms + NOISE_MARGIN_OFF;
}

static void update_noise_estimate(uint32_t beam_rms)
{
    uint32_t th_off = vad_th_off();

    if (beam_rms < th_off) {
        int32_t diff = (int32_t)beam_rms - (int32_t)noise_rms;
        int32_t step = (diff * (int32_t)NOISE_ALPHA_NUM) / (int32_t)NOISE_ALPHA_DEN;
        noise_rms = (uint32_t)((int32_t)noise_rms + step);
    }
}

static void update_vad(uint32_t beam_rms)
{
    uint32_t th_on = vad_th_on();
    uint32_t th_off = vad_th_off();

    if (!speech_active) {
        if (beam_rms > th_on) {
            speech_on_chunks++;
            if (speech_on_chunks >= VAD_ON_CHUNKS_REQ) {
                speech_active = true;
                silence_chunks = 0;
                speech_on_chunks = 0;
                set_vad_led(true);
                LOG_INF("VAD: speech start (rms=%u noise=%u th_on=%u)",
                    beam_rms, noise_rms, th_on);
            }
        } else {
            speech_on_chunks = 0;
        }
        return;
    }

    if (beam_rms < th_off) {
        silence_chunks++;
        if (silence_chunks >= SILENCE_CHUNKS_OFF) {
            speech_active = false;
            silence_chunks = 0;
            speech_on_chunks = 0;
            set_vad_led(false);
            LOG_INF("VAD: speech stop (rms=%u noise=%u th_off=%u)",
                beam_rms, noise_rms, th_off);
        }
    } else {
        silence_chunks = 0;
        last_vad_on_time_ms = k_uptime_get();
        vad_recent_until_ms = last_vad_on_time_ms + VAD_LATCH_WINDOW_MS;
    }
}

int main(void)
{
    int err;

    if (!gpio_is_ready_dt(&led1)) {
        LOG_ERR("LED1 device is not ready");
        return -ENODEV;
    }

    if (!gpio_is_ready_dt(&led2)) {
        LOG_ERR("LED2 device is not ready");
        return -ENODEV;
    }

    if (!gpio_is_ready_dt(&led3)) {
        LOG_ERR("LED3 device is not ready");
        return -ENODEV;
    }

    err = gpio_pin_configure_dt(&led1, GPIO_OUTPUT_INACTIVE);
    if (err < 0) {
        LOG_ERR("Failed to configure LED1 (err %d)", err);
        return err;
    }

    err = gpio_pin_configure_dt(&led2, GPIO_OUTPUT_INACTIVE);
    if (err < 0) {
        LOG_ERR("Failed to configure LED2 (err %d)", err);
        return err;
    }

    err = gpio_pin_configure_dt(&led3, GPIO_OUTPUT_INACTIVE);
    if (err < 0) {
        LOG_ERR("Failed to configure LED3 (err %d)", err);
        return err;
    }

    set_vad_led(false);
    (void)gpio_pin_set_dt(&led2, 0);
    (void)gpio_pin_set_dt(&led3, 0);
    ww_led_off_time_ms = 0;
    last_vad_on_time_ms = 0;
    vad_recent_until_ms = 0;
    speech_on_chunks = 0;

    err = dmic_init();
    if (err) {
        return err;
    }

    err = ww_init();
    if (err) {
        return err;
    }

    LOG_INF("Initialization completed (LED1=VAD, LED2=WW, LED3=WW+recent VAD, stricter VAD)");

    err = dmic_start();
    if (err < 0) {
        LOG_ERR("Failed to start audio capture (err %d)", err);
        return err;
    }

    while (true) {
        void *audio_buffer;
        size_t audio_buffer_size;
        bool ww_detected;
        struct chunk_stats stats_bf;

        const int32_t read_timeout = 100;

        service_ww_leds();

        err = dmic_read(&audio_buffer, &audio_buffer_size, read_timeout);
        if (err < 0) {
            LOG_ERR("Failed to read audio (err %d)", err);
            return err;
        }

        analyze_chunk((const int16_t *)audio_buffer,
                  audio_buffer_size / DMIC_SAMPLE_BYTES,
                  &stats_bf);
        update_noise_estimate(stats_bf.rms);
        update_vad(stats_bf.rms);

        err = ww_process((uint8_t *)audio_buffer,
                 (uint16_t)(audio_buffer_size / DMIC_SAMPLE_BYTES),
                 &ww_detected);
        if (err == -EBUSY) {
            continue;
        } else if (err < 0) {
            LOG_ERR("Wakeword detection failed (err %d)", err);
            return err;
        }

        if (ww_detected) {
            bool led3_should_pulse = vad_recently_active();
            LOG_INF("Wake word detected (led3=%d)", led3_should_pulse ? 1 : 0);
            trigger_ww_leds(led3_should_pulse);
        }
    }

    return 0;
}