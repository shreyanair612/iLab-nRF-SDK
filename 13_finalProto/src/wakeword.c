/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <nrf_edgeai/nrf_edgeai.h>

#include "wakeword.h"
#include "nrf_edgeai_generated/nrf_edgeai_user_model.h"

LOG_MODULE_REGISTER(ww);

static nrf_edgeai_t *ww_model;
static uint16_t ww_input_samples;
static int16_t ww_input_buffer[512];
static uint16_t ww_input_fill;
static uint32_t ww_count;
static uint32_t ww_history;
static int64_t last_detect_time_ms;

BUILD_ASSERT(CONFIG_WW_HISTORY_SIZE <= 32, "WW_HISTORY_SIZE must fit in uint32_t");

int ww_init(void)
{
    ww_model = nrf_edgeai_user_model();
    __ASSERT_NO_MSG(ww_model);

    nrf_edgeai_err_t err = nrf_edgeai_init(ww_model);
    if (err) {
        LOG_ERR("Model initialization failed (err %d)", err);
        return -ENOENT;
    }

    ww_input_samples = nrf_edgeai_input_window_size(ww_model);
    __ASSERT_NO_MSG(ww_input_samples > 0);
    __ASSERT_NO_MSG(ww_input_samples <= ARRAY_SIZE(ww_input_buffer));

    ww_input_fill = 0U;

    LOG_INF("Wakeword initialized, input_window=%u samples", ww_input_samples);
    return 0;
}

void ww_reset(void)
{
    ww_input_fill = 0U;
    ww_count = 0U;
    ww_history = 0U;
}

static bool ww_postprocess(void)
{
    const float ww_threshold = CONFIG_WW_PROBABILITY_THRESHOLD / 1000.f;
    const int64_t now_ms = k_uptime_get();
    const int64_t cooldown_ms = 1200;

    const uint16_t predicted_class = ww_model->decoded_output.classif.predicted_class;
    const float class_probability =
        ww_model->decoded_output.classif.probabilities.p_f32[predicted_class];
    const bool ww_frame_positive = class_probability > ww_threshold;

    const bool oldest_entry = (bool)(ww_history & BIT(CONFIG_WW_HISTORY_SIZE - 1));

    ww_count = ww_count + ww_frame_positive - oldest_entry;
    ww_history = (ww_history << 1) | ww_frame_positive;

    LOG_DBG("postprocess: pred=%u prob=%f count=%u",
        predicted_class, (double)class_probability, ww_count);

    if (ww_count >= CONFIG_WW_COUNT_THRESHOLD) {
        ww_count = 0U;
        ww_history = 0U;

        if ((now_ms - last_detect_time_ms) < cooldown_ms) {
            return false;
        }

        last_detect_time_ms = now_ms;
        return true;
    }

    return false;
}

static int ww_run_window(bool *const ww_detected)
{
    nrf_edgeai_err_t err;

    err = nrf_edgeai_feed_inputs(ww_model, ww_input_buffer, ww_input_samples);
    if (err == NRF_EDGEAI_ERR_INPROGRESS) {
        return -EBUSY;
    } else if (err) {
        LOG_ERR("Failed to feed inputs (err %d)", err);
        return -EPERM;
    }

    err = nrf_edgeai_run_inference(ww_model);
    if (err == NRF_EDGEAI_ERR_INPROGRESS) {
        return -EBUSY;
    } else if (err) {
        LOG_ERR("Failed to run inference (err %d)", err);
        return -EPERM;
    }

    *ww_detected = ww_postprocess();
    return 0;
}

int ww_process(const int16_t *const samples, const uint16_t num_samples, bool *const ww_detected)
{
    uint16_t consumed = 0U;

    __ASSERT_NO_MSG(samples);
    __ASSERT_NO_MSG(ww_detected);

    *ww_detected = false;

    if (ww_model == NULL) {
        return -ENODEV;
    }

    while (consumed < num_samples) {
        const uint16_t space_left = ww_input_samples - ww_input_fill;
        const uint16_t copy_now =
            MIN((uint16_t)(num_samples - consumed), space_left);

        memcpy(&ww_input_buffer[ww_input_fill],
               &samples[consumed],
               copy_now * sizeof(int16_t));

        ww_input_fill += copy_now;
        consumed += copy_now;

        if (ww_input_fill == ww_input_samples) {
            int ret = ww_run_window(ww_detected);
            ww_input_fill = 0U;

            if (ret < 0) {
                return ret;
            }

            if (*ww_detected) {
                return 0;
            }
        }
    }

    return 0;
}
