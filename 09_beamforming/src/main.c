/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Dual-channel I2S microphone capture with:
 * - simple broadside delay-and-sum beamforming (center beam)
 * - optional difference-channel debug signal
 * - adaptive RMS-threshold VAD on beamformed output
 *
 * Hardware intent:
 * - 2x INMP441 microphones
 * - shared SCK + WS
 * - one mic strapped L/R low, the other high
 * - stereo I2S RX into Zephyr I2S driver
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>

#define I2S_MIC_RX DT_NODELABEL(tdm)
#define LED1_NODE  DT_ALIAS(led1)

#if !DT_NODE_HAS_STATUS(I2S_MIC_RX, okay)
#error "TDM is disabled. Check that the board overlay is applied."
#endif

#if !DT_NODE_HAS_STATUS(LED1_NODE, okay)
#error "Unsupported board: led1 devicetree alias is not defined"
#endif

#define SAMPLE_RATE_HZ        8000U
#define SAMPLE_BIT_WIDTH      32U
#define CHANNELS              2U
#define CHUNK_MS              100U
#define SAMPLES_PER_CHUNK     ((SAMPLE_RATE_HZ / 1000U) * CHUNK_MS)
#define BYTES_PER_SAMPLE      (SAMPLE_BIT_WIDTH / 8U)
#define FRAME_SIZE_BYTES      (CHANNELS * BYTES_PER_SAMPLE)
#define BLOCK_SIZE            (SAMPLES_PER_CHUNK * FRAME_SIZE_BYTES)
#define BLOCK_COUNT           8U
#define I2S_TIMEOUT_MS        2000U

#define SILENCE_CHUNKS_OFF    5U

/*
 * Dynamic-threshold knobs:
 *
 * noise_rms tracks the background noise floor on the beamformed signal.
 * TH_ON  = noise_rms + margin_on
 * TH_OFF = noise_rms + margin_off
 *
 * margin_on  ↑ : fewer false triggers, but may miss quiet speech
 * margin_on  ↓ : more sensitive to quiet speech, but more false positives
 *
 * margin_off ↑ : speech state ends sooner
 * margin_off ↓ : speech state holds longer
 *
 * noise_alpha_den ↑ : slower noise-floor adaptation
 * noise_alpha_den ↓ : faster noise-floor adaptation
 *
 * silence_chunks_off ↑ : LED stays on longer after speech
 * silence_chunks_off ↓ : LED turns off sooner
 */
#define NOISE_INIT_RMS        20U
#define NOISE_MARGIN_ON       40U
#define NOISE_MARGIN_OFF      20U
#define NOISE_ALPHA_NUM       1U
#define NOISE_ALPHA_DEN       16U

#define LOG_DIFF_CHANNEL      1

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(rx_mem_slab, __nocache,
                 BLOCK_SIZE, BLOCK_COUNT, 4);

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED1_NODE, gpios);

static int16_t pcm_left[SAMPLES_PER_CHUNK];
static int16_t pcm_right[SAMPLES_PER_CHUNK];
static int16_t pcm_sum[SAMPLES_PER_CHUNK];
#if LOG_DIFF_CHANNEL
static int16_t pcm_diff[SAMPLES_PER_CHUNK];
#endif

static bool speech_active;
static uint32_t silence_chunks;
static uint32_t noise_rms = NOISE_INIT_RMS;

struct chunk_stats {
    uint32_t rms;
    int16_t peak;
    uint32_t zcr;
    uint32_t est_hz;
};

static int32_t extract_sample(int32_t raw)
{
    /*
     * INMP441 presents 24-bit signed sample data over I2S, commonly
     * carried in a 32-bit container. Arithmetic right shift preserves sign.
     */
    return raw >> 8;
}

static int16_t saturate_int16(int32_t x)
{
    if (x > INT16_MAX) {
        return INT16_MAX;
    }
    if (x < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)x;
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
    stats->est_hz = (count > 1U) ? ((crossings * SAMPLE_RATE_HZ) / (2U * count)) : 0U;
}

static void unpack_and_beamform(const int32_t *raw, size_t frames)
{
    for (size_t i = 0; i < frames; i++) {
        int32_t left = extract_sample(raw[i * CHANNELS + 0]);
        int32_t right = extract_sample(raw[i * CHANNELS + 1]);

        /*
         * Center-steered broadside 2-mic beamformer:
         * y[n] = (L[n] + R[n]) / 2
         */
        int32_t sum = (left + right) / 2;

#if LOG_DIFF_CHANNEL
        /*
         * Debug difference channel:
         * d[n] = (L[n] - R[n]) / 2
         */
        int32_t diff = (left - right) / 2;
#endif

        /*
         * Downscale into int16 for chunk-level stats and VAD.
         */
        pcm_left[i] = saturate_int16(left >> 8);
        pcm_right[i] = saturate_int16(right >> 8);
        pcm_sum[i] = saturate_int16(sum >> 8);

#if LOG_DIFF_CHANNEL
        pcm_diff[i] = saturate_int16(diff >> 8);
#endif
    }
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

    /*
     * Only learn noise floor from frames that look non-speech-ish.
     * This keeps speech from dragging the noise baseline upward.
     */
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
            speech_active = true;
            silence_chunks = 0;
        }
        return;
    }

    if (beam_rms < th_off) {
        silence_chunks++;
        if (silence_chunks >= SILENCE_CHUNKS_OFF) {
            speech_active = false;
            silence_chunks = 0;
        }
    } else {
        silence_chunks = 0;
    }
}

static void stop_i2s_rx(const struct device *i2s_dev)
{
    int ret;

    ret = i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_STOP);
    if (ret < 0) {
        printk("warn: I2S stop failed: %d\n", ret);
    }

    ret = i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
    if (ret < 0) {
        printk("warn: I2S drop failed: %d\n", ret);
    }
}

int main(void)
{
    const struct device *const i2s_dev = DEVICE_DT_GET(I2S_MIC_RX);

    struct i2s_config i2s_cfg = {
        .word_size = SAMPLE_BIT_WIDTH,
        .channels = CHANNELS,
        .format = I2S_FMT_DATA_FORMAT_I2S,
        .options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
        .frame_clk_freq = SAMPLE_RATE_HZ,
        .mem_slab = &rx_mem_slab,
        .block_size = BLOCK_SIZE,
        .timeout = I2S_TIMEOUT_MS,
    };

    int ret;

    printk("Dual-channel I2S mic monitor + center beamformer + adaptive VAD\n");
    printk("sample_rate=%u chunk_ms=%u channels=%u bits=%u block_size=%u\n",
           SAMPLE_RATE_HZ, CHUNK_MS, CHANNELS, SAMPLE_BIT_WIDTH, BLOCK_SIZE);
    printk("fields: L[rms peak zcr est_hz] R[rms peak zcr est_hz] S[rms peak zcr est_hz]");
#if LOG_DIFF_CHANNEL
    printk(" D[rms peak zcr est_hz]");
#endif
    printk("\n");

    if (!device_is_ready(i2s_dev)) {
        printk("error: I2S device is not ready\n");
        return 0;
    }

    if (!gpio_is_ready_dt(&led)) {
        printk("error: LED device is not ready\n");
        return 0;
    }

    ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        printk("error: failed to configure LED: %d\n", ret);
        return 0;
    }

    ret = i2s_configure(i2s_dev, I2S_DIR_RX, &i2s_cfg);
    if (ret < 0) {
        printk("error: i2s_configure failed: %d\n", ret);
        return 0;
    }

    ret = i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_START);
    if (ret < 0) {
        printk("error: I2S start failed: %d\n", ret);
        return 0;
    }

    while (true) {
        void *mem_block = NULL;
        size_t block_size = 0;
        int32_t *raw;
        size_t frames;

        struct chunk_stats stats_l;
        struct chunk_stats stats_r;
        struct chunk_stats stats_s;
#if LOG_DIFF_CHANNEL
        struct chunk_stats stats_d;
#endif

        ret = i2s_read(i2s_dev, &mem_block, &block_size);
        if (ret < 0) {
            printk("error: i2s_read failed: %d\n", ret);
            k_msleep(CHUNK_MS);
            continue;
        }

        if ((mem_block == NULL) || (block_size == 0U)) {
            printk("warn: empty I2S block\n");
            continue;
        }

        raw = (int32_t *)mem_block;
        frames = block_size / FRAME_SIZE_BYTES;

        if (frames > SAMPLES_PER_CHUNK) {
            frames = SAMPLES_PER_CHUNK;
        }

        unpack_and_beamform(raw, frames);

        analyze_chunk(pcm_left, frames, &stats_l);
        analyze_chunk(pcm_right, frames, &stats_r);
        analyze_chunk(pcm_sum, frames, &stats_s);
#if LOG_DIFF_CHANNEL
        analyze_chunk(pcm_diff, frames, &stats_d);
#endif

        update_noise_estimate(stats_s.rms);
        update_vad(stats_s.rms);

        printk("L[%u %d %u %u] R[%u %d %u %u] S[%u %d %u %u]",
               stats_l.rms, stats_l.peak, stats_l.zcr, stats_l.est_hz,
               stats_r.rms, stats_r.peak, stats_r.zcr, stats_r.est_hz,
               stats_s.rms, stats_s.peak, stats_s.zcr, stats_s.est_hz);
#if LOG_DIFF_CHANNEL
        printk(" D[%u %d %u %u]",
               stats_d.rms, stats_d.peak, stats_d.zcr, stats_d.est_hz);
#endif
        printk("\n");

        (void)gpio_pin_set_dt(&led, speech_active ? 1 : 0);

        printk("noise=%u th_on=%u th_off=%u S_rms=%u vad=%d silence_chunks=%u\n",
               noise_rms, vad_th_on(), vad_th_off(),
               stats_s.rms, speech_active ? 1 : 0, silence_chunks);

        k_mem_slab_free(&rx_mem_slab, mem_block);
    }

    stop_i2s_rx(i2s_dev);
    return 0;
}