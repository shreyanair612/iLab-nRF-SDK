/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <nrf_edgeai/nrf_edgeai.h>

#include "wakeword.h"
#include "nrf_edgeai_generated/nrf_edgeai_user_model.h"

LOG_MODULE_REGISTER(ww);

static nrf_edgeai_t *ww_model;

int ww_init(void)
{
    ww_model = nrf_edgeai_user_model();
    __ASSERT_NO_MSG(ww_model);

    nrf_edgeai_err_t err = nrf_edgeai_init(ww_model);
    if (err) {
        LOG_ERR("Model initialization failed (err %d)", err);
        return -ENOENT;
    }

    return 0;
}

static bool ww_postprocess(void)
{
    static uint32_t ww_count;
    static uint32_t ww_history;
    static int64_t last_detect_time_ms;

    const float ww_threshold = CONFIG_WW_PROBABILITY_THRESHOLD / 1000.f;
    const int64_t now_ms = k_uptime_get();
    const int64_t cooldown_ms = 1200;

    const uint16_t predicted_class = ww_model->decoded_output.classif.predicted_class;
    const float class_probability =
        ww_model->decoded_output.classif.probabilities.p_f32[predicted_class];
    const bool hit = class_probability > ww_threshold;

    const bool oldest_entry =
        (bool)(ww_history & BIT(CONFIG_WW_HISTORY_SIZE - 1));

    ww_count = ww_count + hit - oldest_entry;
    ww_history = (ww_history << 1) | hit;

    LOG_DBG("postprocess: count=%u prob=%f",
            ww_count, (double)class_probability);

    if (ww_count >= CONFIG_WW_COUNT_THRESHOLD) {
        ww_count = 0;
        ww_history = 0;

        if ((now_ms - last_detect_time_ms) < cooldown_ms) {
            return false;
        }

        last_detect_time_ms = now_ms;
        return true;
    }

    return false;
}

int ww_process(const int16_t *audio_samples, uint16_t num_samples, bool *ww_detected)
{
    nrf_edgeai_err_t err;

    __ASSERT_NO_MSG(audio_samples != NULL);
    __ASSERT_NO_MSG(ww_detected != NULL);
    __ASSERT_NO_MSG(num_samples == nrf_edgeai_input_window_size(ww_model));

    err = nrf_edgeai_feed_inputs(ww_model, (void *)audio_samples, num_samples);
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