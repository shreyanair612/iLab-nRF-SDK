/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "dmic.h"

LOG_MODULE_REGISTER(dmic);

#define I2S_MIC_RX DT_NODELABEL(tdm)

BUILD_ASSERT(DT_NODE_HAS_STATUS(I2S_MIC_RX, okay),
	     "TDM/I2S is disabled. Check that the board overlay is applied.");

#define I2S_WORD_SIZE_BITS 32
#define I2S_CHANNELS 2
#define I2S_TIMEOUT_MS 2000

#define BLOCK_SIZE (DMIC_PCM_RATE * SAMPLES_BLOCK_LENGTH_MS / 1000 * \
		    I2S_CHANNELS * (I2S_WORD_SIZE_BITS / 8))
#define BLOCK_COUNT 8

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(rx_mem_slab, __nocache, BLOCK_SIZE, BLOCK_COUNT, 4);
K_MEM_SLAB_DEFINE_STATIC(app_mem_slab,
			 (DMIC_PCM_RATE * SAMPLES_BLOCK_LENGTH_MS / 1000) * DMIC_SAMPLE_BYTES,
			 BLOCK_COUNT, 4);

static const struct device *const i2s_dev = DEVICE_DT_GET(I2S_MIC_RX);

static int16_t extract_left_sample(int32_t raw)
{
	return (int16_t)(raw >> 16);
}

int dmic_init(void)
{
	struct i2s_config i2s_cfg = {
		.word_size = I2S_WORD_SIZE_BITS,
		.channels = I2S_CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
		.frame_clk_freq = DMIC_PCM_RATE,
		.mem_slab = &rx_mem_slab,
		.block_size = BLOCK_SIZE,
		.timeout = I2S_TIMEOUT_MS,
	};
	int ret;

	if (!device_is_ready(i2s_dev)) {
		LOG_ERR("I2S device is not ready");
		return -ENODEV;
	}

	ret = i2s_configure(i2s_dev, I2S_DIR_RX, &i2s_cfg);
	if (ret < 0) {
		LOG_ERR("i2s_configure failed: %d", ret);
		return ret;
	}

	LOG_INF("INMP441 I2S audio initialized at %d Hz", DMIC_PCM_RATE);
	return 0;
}

int dmic_start(void)
{
	int ret = i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_START);

	if (ret < 0) {
		LOG_ERR("I2S start failed: %d", ret);
		return ret;
	}

	LOG_INF("INMP441 I2S capture started");
	return 0;
}

int dmic_read(void **buffer, size_t *buffer_size, int32_t timeout_ms)
{
	ARG_UNUSED(timeout_ms);

	void *mem_block;
	size_t raw_block_size = BLOCK_SIZE;
	int ret;

	ret = i2s_read(i2s_dev, &mem_block, &raw_block_size);
	if (ret < 0) {
		LOG_ERR("i2s_read failed: %d", ret);
		return ret;
	}

	void *app_block;
	ret = k_mem_slab_alloc(&app_mem_slab, &app_block, K_NO_WAIT);
	if (ret < 0) {
		LOG_ERR("Failed to allocate app audio buffer: %d", ret);
		k_mem_slab_free(&rx_mem_slab, mem_block);
		return ret;
	}

	int32_t *raw = (int32_t *)mem_block;
	int16_t *pcm = (int16_t *)app_block;
	const size_t samples_per_chunk = DMIC_PCM_RATE * SAMPLES_BLOCK_LENGTH_MS / 1000;

	for (size_t i = 0; i < samples_per_chunk; i++) {
		pcm[i] = extract_left_sample(raw[i * I2S_CHANNELS]);
	}

	k_mem_slab_free(&rx_mem_slab, mem_block);

	*buffer = app_block;
	*buffer_size = samples_per_chunk * DMIC_SAMPLE_BYTES;

	return 0;
}

void free_dmic_buffer(void *buffer)
{
	k_mem_slab_free(&app_mem_slab, buffer);
}