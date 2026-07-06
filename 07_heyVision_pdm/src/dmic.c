/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/audio/dmic.h>

#include "dmic.h"

LOG_MODULE_REGISTER(dmic);

#define AUDIO_BLOCK_SIZE \
    ((DMIC_PCM_RATE * SAMPLES_BLOCK_LENGTH_MS / 1000) * DMIC_SAMPLE_BYTES)
#define BLOCK_COUNT 8

/* Devicetree node: your overlay labels this pdm20 node as dmic_dev */
static const struct device *const pdm_dev = DEVICE_DT_GET(DT_NODELABEL(dmic_dev));

K_MEM_SLAB_DEFINE_STATIC(app_mem_slab, AUDIO_BLOCK_SIZE, BLOCK_COUNT, 4);

static struct pcm_stream_cfg stream_cfg;
static struct dmic_cfg cfg;

int dmic_init(void)
{
    int ret;

    if (!device_is_ready(pdm_dev)) {
        LOG_ERR("%s is not ready", pdm_dev->name);
        return -ENODEV;
    }

    stream_cfg.pcm_rate = DMIC_PCM_RATE;
    stream_cfg.pcm_width = 16;
    stream_cfg.block_size = AUDIO_BLOCK_SIZE;
    stream_cfg.mem_slab = &app_mem_slab;

    cfg.io.min_pdm_clk_freq = 1000000;
    cfg.io.max_pdm_clk_freq = 3500000;
    cfg.io.min_pdm_clk_dc = 40;
    cfg.io.max_pdm_clk_dc = 60;

    cfg.streams = &stream_cfg;

    cfg.channel.req_num_streams = 1;
    cfg.channel.req_num_chan = 1;
    cfg.channel.req_chan_map_lo =
        dmic_build_channel_map(0, 0, PDM_CHAN_LEFT);
    cfg.channel.req_chan_map_hi = 0;

    ret = dmic_configure(pdm_dev, &cfg);
    if (ret < 0) {
        LOG_ERR("dmic_configure failed: %d", ret);
        return ret;
    }

    LOG_INF("PDM DMIC initialized at %d Hz", DMIC_PCM_RATE);
    return 0;
}

int dmic_start(void)
{
    int ret = dmic_trigger(pdm_dev, DMIC_TRIGGER_START);

    if (ret < 0) {
        LOG_ERR("DMIC start failed: %d", ret);
        return ret;
    }

    LOG_INF("PDM DMIC capture started");
    return 0;
}

int dmic_read_buffer(void **buffer, size_t *buffer_size, int32_t timeout_ms)
{
    int ret;
    size_t size;
    void *mem_block;

    ret = dmic_read(pdm_dev, 0, &mem_block, &size, timeout_ms);
    if (ret < 0) {
        LOG_ERR("dmic_read failed: %d", ret);
        return ret;
    }

    *buffer = mem_block;
    *buffer_size = size;

    return 0;
}

void free_dmic_buffer(void *buffer)
{
    k_mem_slab_free(&app_mem_slab, buffer);
}