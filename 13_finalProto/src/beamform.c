#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "beamform.h"

LOG_MODULE_REGISTER(beamform, LOG_LEVEL_INF);

#define SAMPLE_RATE       16000
#define SAMPLE_BIT_WIDTH  32
#define CHANNELS          2

#define BLOCK_PAIRS       256
#define BLOCK_BYTES       (BLOCK_PAIRS * CHANNELS * sizeof(int32_t))
#define BLOCK_COUNT       4
#define CAPTURE_GAIN_SHIFT 0

#define TDM_NODE DT_NODELABEL(tdm)

#if !DT_NODE_EXISTS(TDM_NODE)
#error "TDM node does not exist in the final devicetree"
#endif

#if !DT_NODE_HAS_STATUS(TDM_NODE, okay)
#error "TDM node is not enabled in the final devicetree"
#endif

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(rx_slab, __nocache, BLOCK_BYTES, BLOCK_COUNT, 4);
K_THREAD_STACK_DEFINE(capture_stack, 4096);

static const struct device *const i2s = DEVICE_DT_GET(TDM_NODE);

static const struct i2s_config i2s_cfg = {
	.word_size = SAMPLE_BIT_WIDTH,
	.channels = CHANNELS,
	.format = I2S_FMT_DATA_FORMAT_I2S,
	/* The nRF TDM drives both clocks; the INMP441 is always a clock slave. */
	.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
	.frame_clk_freq = SAMPLE_RATE,
	.mem_slab = &rx_slab,
	.block_size = BLOCK_BYTES,
	.timeout = 2000,
};

static struct k_thread capture_thread_data;
static atomic_t beamforming_active = ATOMIC_INIT(0);
static void (*chunk_ready)(const int16_t *samples, size_t count);

/* Sign-preserving unpack of the 24-bit payload from its 32-bit I2S slot. */
static inline int32_t extract_sample_24(int32_t raw)
{
	return raw >> 8;
}

static inline int16_t saturate_int16(int32_t x)
{
	if (x > INT16_MAX) {
		return INT16_MAX;
	}
	if (x < INT16_MIN) {
		return INT16_MIN;
	}
	return (int16_t)x;
}

static void capture(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	int err = i2s_trigger(i2s, I2S_DIR_RX, I2S_TRIGGER_START);

	if (err) {
		LOG_ERR("I2S start failed: %d", err);
		atomic_clear(&beamforming_active);
		return;
	}

	LOG_INF("capture: %d Hz, %d ch, %d-bit slots, %u frames/block (%u ms)",
		SAMPLE_RATE, CHANNELS, SAMPLE_BIT_WIDTH, (unsigned int)BLOCK_PAIRS,
		(unsigned int)((1000U * BLOCK_PAIRS) / SAMPLE_RATE));

	while (atomic_get(&beamforming_active)) {
		void *block;
		size_t bytes;
		int16_t mono_chunk[BLOCK_PAIRS];

		err = i2s_read(i2s, &block, &bytes);
		if (err) {
			if (err != -EAGAIN) {
				LOG_WRN("I2S read failed: %d", err);
			}
			continue;
		}

		const int32_t *stereo = block;
		size_t pairs = bytes / (CHANNELS * sizeof(int32_t));

		pairs = MIN(pairs, (size_t)BLOCK_PAIRS);

		for (size_t i = 0; i < pairs; i++) {
			/*
			 * Broadside 2-mic beamformer, center steered:
			 *   y[n] = (L[n] + R[n]) / 2
			 * Summing in the 24-bit domain keeps the average exact
			 * and cannot overflow int32_t.
			 */
			int32_t left = extract_sample_24(stereo[CHANNELS * i]);
			int32_t right = extract_sample_24(stereo[CHANNELS * i + 1]);
			int32_t sum = (left + right) / 2;

			mono_chunk[i] = saturate_int16(sum >> (8 - CAPTURE_GAIN_SHIFT));
		}

		/*
		 * Hand the frame over before releasing the DMA block. The
		 * consumer copies synchronously, so mono_chunk staying on this
		 * stack is safe and no ownership is transferred.
		 */
		if (chunk_ready && pairs > 0U) {
			chunk_ready(mono_chunk, pairs);
		}

		k_mem_slab_free(&rx_slab, block);
	}

	i2s_trigger(i2s, I2S_DIR_RX, I2S_TRIGGER_DROP);
}

int beamform_init(void (*chunk_cb)(const int16_t *samples, size_t count))
{
	if (!device_is_ready(i2s)) {
		return -ENODEV;
	}

	chunk_ready = chunk_cb;
	return i2s_configure(i2s, I2S_DIR_RX, &i2s_cfg);
}

int beamform_start(void)
{
	if (!atomic_cas(&beamforming_active, 0, 1)) {
		return -EALREADY;
	}

	k_thread_create(&capture_thread_data, capture_stack,
			K_THREAD_STACK_SIZEOF(capture_stack), capture,
			NULL, NULL, NULL, 6, 0, K_NO_WAIT);
	k_thread_name_set(&capture_thread_data, "audio_capture");

	return 0;
}

uint32_t beamform_sample_rate_hz(void)
{
	return SAMPLE_RATE;
}

uint32_t beamform_frame_samples(void)
{
	return BLOCK_PAIRS;
}
