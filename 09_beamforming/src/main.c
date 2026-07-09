/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Dual-channel I2S microphone capture and simple beamforming stats.
 *
 * Captures stereo I2S input from two MEMS mics sharing one bus:
 *   - Left mic  -> I2S left slot
 *   - Right mic -> I2S right slot
 *
 * Prints text stats over UART for:
 *   - Left channel
 *   - Right channel
 *   - Beam/difference channel: left - right
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/sys/printk.h>
#include <stdbool.h>
#include <stdint.h>
#include <arm_math.h>
#include "beamformer.h"

#define I2S_MIC_RX DT_NODELABEL(tdm)

BUILD_ASSERT(DT_NODE_HAS_STATUS(I2S_MIC_RX, okay),
	     "TDM is disabled. Check that the board overlay is applied.");

#define SAMPLE_RATE_HZ     8000U
#define SAMPLE_BIT_WIDTH   32U
#define CHANNELS           2U
#define CHUNK_MS           100U
#define SAMPLES_PER_CHUNK  ((SAMPLE_RATE_HZ / 1000U) * CHUNK_MS)
#define BYTES_PER_SAMPLE   (SAMPLE_BIT_WIDTH / 8U)
#define BLOCK_SIZE         (SAMPLES_PER_CHUNK * CHANNELS * BYTES_PER_SAMPLE)
#define BLOCK_COUNT        8U
#define I2S_TIMEOUT_MS     2000U

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(rx_mem_slab, __nocache,
				 BLOCK_SIZE, BLOCK_COUNT, 4);

static int16_t pcm_left[SAMPLES_PER_CHUNK];
static int16_t pcm_right[SAMPLES_PER_CHUNK];
static int16_t pcm_beam[SAMPLES_PER_CHUNK];

static int16_t extract_sample(int32_t raw)
{
	/* Matches your original extraction logic.
	 * If your mic data alignment differs, this is the first place to tweak.
	 */
	return (int16_t)(raw >> 16);
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

	while (one != 0) {
		if (op >= res + one) {
			op -= res + one;
			res = (res >> 1) + one;
		} else {
			res >>= 1;
		}
		one >>= 2;
	}

	return (uint32_t)res;
}

static void analyze_chunk(const int16_t *samples, size_t count,
			  uint32_t *rms, int16_t *peak,
			  uint32_t *zcr, uint32_t *est_hz)
{
	uint64_t energy = 0;
	int16_t peak_abs = 0;
	uint32_t crossings = 0;
	bool was_positive;

	if (count == 0U) {
		*rms = 0;
		*peak = 0;
		*zcr = 0;
		*est_hz = 0;
		return;
	}

	was_positive = (samples[0] >= 0);

	for (size_t i = 0; i < count; i++) {
		int32_t s = samples[i];
		int16_t abs_s = (s < 0) ? (int16_t)(-s) : (int16_t)s;

		energy += (uint64_t)((int64_t)s * (int64_t)s);

		if (abs_s > peak_abs) {
			peak_abs = abs_s;
		}

		if (i > 0U) {
			bool is_positive = (s >= 0);

			if (is_positive != was_positive) {
				crossings++;
			}
			was_positive = is_positive;
		}
	}

	*rms = isqrt64(energy / count);
	*peak = peak_abs;
	*zcr = crossings;
	*est_hz = (count > 1U) ? (crossings * SAMPLE_RATE_HZ) / (2U * count) : 0U;
}

static void unpack_and_beamform(const int32_t *raw, size_t frames)
{
	for (size_t i = 0; i < frames; i++) {
		int16_t left = extract_sample(raw[i * CHANNELS + 0]);
		int16_t right = extract_sample(raw[i * CHANNELS + 1]);
		int32_t beam = (int32_t)left - (int32_t)right;

		pcm_left[i] = left;
		pcm_right[i] = right;
		pcm_beam[i] = saturate_int16(beam / 2);
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

	printk("Dual-channel I2S mic monitor\n");
	printk("sample_rate=%u chunk_ms=%u channels=%u bits=%u\n",
	       SAMPLE_RATE_HZ, CHUNK_MS, CHANNELS, SAMPLE_BIT_WIDTH);
	printk("fields: L[rms peak zcr est_hz] R[rms peak zcr est_hz] B[rms peak zcr est_hz]\n");

	if (!device_is_ready(i2s_dev)) {
		printk("error: I2S device is not ready\n");
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
		void *mem_block;
		size_t block_size = BLOCK_SIZE;
		int32_t *raw;

		uint32_t rms_l, rms_r, rms_b;
		int16_t peak_l, peak_r, peak_b;
		uint32_t zcr_l, zcr_r, zcr_b;
		uint32_t est_hz_l, est_hz_r, est_hz_b;

		ret = i2s_read(i2s_dev, &mem_block, &block_size);
		if (ret < 0) {
			printk("error: i2s_read failed: %d\n", ret);
			k_msleep(CHUNK_MS);
			continue;
		}

		raw = (int32_t *)mem_block;

		unpack_and_beamform(raw, SAMPLES_PER_CHUNK);

		analyze_chunk(pcm_left, SAMPLES_PER_CHUNK,
			      &rms_l, &peak_l, &zcr_l, &est_hz_l);
		analyze_chunk(pcm_right, SAMPLES_PER_CHUNK,
			      &rms_r, &peak_r, &zcr_r, &est_hz_r);
		analyze_chunk(pcm_beam, SAMPLES_PER_CHUNK,
			      &rms_b, &peak_b, &zcr_b, &est_hz_b);

		printk("L[%u %d %u %u] R[%u %d %u %u] B[%u %d %u %u]\n",
		       rms_l, peak_l, zcr_l, est_hz_l,
		       rms_r, peak_r, zcr_r, est_hz_r,
		       rms_b, peak_b, zcr_b, est_hz_b);

		k_mem_slab_free(&rx_mem_slab, mem_block);
	}

	return 0;
}