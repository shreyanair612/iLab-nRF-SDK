#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "audio_capture_adapter.h"

LOG_MODULE_REGISTER(capture, LOG_LEVEL_INF);

/*
 * Capture format, unchanged from 13_finalProto: the INMP441 emits 24-bit
 * samples left-justified in a 32-bit slot and needs 64 SCK per frame, so the
 * slot width stays at 32 bits (SCK = 16000 * 2 * 32 = 1.024 MHz).
 */
#define SAMPLE_RATE       EVAL_SAMPLE_RATE_HZ
#define SAMPLE_BIT_WIDTH  32
#define CHANNELS          2
#define BLOCK_PAIRS       EVAL_FRAME_SAMPLES
#define BLOCK_BYTES       (BLOCK_PAIRS * CHANNELS * sizeof(int32_t))
#define BLOCK_COUNT       4

#ifndef CONFIG_EVAL_CAPTURE_GAIN_SHIFT
#define CONFIG_EVAL_CAPTURE_GAIN_SHIFT 0
#endif
#ifndef CONFIG_EVAL_FRAME_QUEUE_DEPTH
#define CONFIG_EVAL_FRAME_QUEUE_DEPTH 8
#endif

#define TDM_NODE DT_NODELABEL(tdm)

#if !DT_NODE_HAS_STATUS(TDM_NODE, okay)
#error "TDM node is not enabled in the devicetree"
#endif

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(rx_slab, __nocache, BLOCK_BYTES, BLOCK_COUNT, 4);
K_THREAD_STACK_DEFINE(capture_stack, 4096);
K_MSGQ_DEFINE(frame_queue, sizeof(struct capture_frame), CONFIG_EVAL_FRAME_QUEUE_DEPTH, 4);

static const struct device *const i2s = DEVICE_DT_GET(TDM_NODE);

static const struct i2s_config i2s_cfg = {
	.word_size = SAMPLE_BIT_WIDTH,
	.channels = CHANNELS,
	.format = I2S_FMT_DATA_FORMAT_I2S,
	.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
	.frame_clk_freq = SAMPLE_RATE,
	.mem_slab = &rx_slab,
	.block_size = BLOCK_BYTES,
	.timeout = 2000,
};

static struct k_thread capture_thread_data;
static atomic_t capture_active = ATOMIC_INIT(0);
static atomic_t enqueue_enabled = ATOMIC_INIT(1);
static struct audio_capture_stats stats;
static struct k_spinlock stats_lock;

/* The one frame being assembled; copied into the queue by value. */
static struct capture_frame work_frame;

static inline int16_t unpack(int32_t raw)
{
	/* 32-bit slot -> 24-bit sample -> 16-bit with optional gain, saturated. */
	int32_t s24 = raw >> 8;
	int32_t s = s24 >> (8 - CONFIG_EVAL_CAPTURE_GAIN_SHIFT);

	if (s > INT16_MAX) {
		return INT16_MAX;
	}
	if (s < INT16_MIN) {
		return INT16_MIN;
	}
	return (int16_t)s;
}

static void capture(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	int err = i2s_trigger(i2s, I2S_DIR_RX, I2S_TRIGGER_START);

	if (err) {
		LOG_ERR("I2S start failed: %d", err);
		atomic_clear(&capture_active);
		return;
	}

	LOG_INF("capture: %u Hz, %d ch, %d-bit slots, %u samples/frame (%u ms), queue %d",
		(unsigned)SAMPLE_RATE, CHANNELS, SAMPLE_BIT_WIDTH, (unsigned)BLOCK_PAIRS,
		(unsigned)EVAL_FRAME_MS, CONFIG_EVAL_FRAME_QUEUE_DEPTH);

	while (atomic_get(&capture_active)) {
		void *block;
		size_t bytes;

		err = i2s_read(i2s, &block, &bytes);
		if (err) {
			if (err != -EAGAIN) {
				k_spinlock_key_t key = k_spin_lock(&stats_lock);

				stats.i2s_errors++;
				k_spin_unlock(&stats_lock, key);
				LOG_WRN("I2S read failed: %d", err);
			}
			continue;
		}

		uint32_t t0 = k_cycle_get_32();
		const int32_t *stereo = block;
		size_t pairs = MIN(bytes / (CHANNELS * sizeof(int32_t)), (size_t)BLOCK_PAIRS);

		for (size_t i = 0; i < pairs; i++) {
			work_frame.left[i] = unpack(stereo[CHANNELS * i]);
			work_frame.right[i] = unpack(stereo[CHANNELS * i + 1]);
		}
		k_mem_slab_free(&rx_slab, block);

		work_frame.source = FRAME_SRC_CAPTURE;
		work_frame.channels = CHANNELS;
		work_frame.count = (uint16_t)pairs;

		uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - t0);
		k_spinlock_key_t key = k_spin_lock(&stats_lock);

		work_frame.seq = stats.frames_captured++;
		stats.unpack_us_last = us;
		if (us > stats.unpack_us_max) {
			stats.unpack_us_max = us;
		}

		if (!atomic_get(&enqueue_enabled)) {
			stats.frames_discarded++;
			k_spin_unlock(&stats_lock, key);
			continue;
		}

		uint32_t used = k_msgq_num_used_get(&frame_queue);

		if (used > stats.queue_high_water) {
			stats.queue_high_water = used;
		}
		if (k_msgq_put(&frame_queue, &work_frame, K_NO_WAIT) != 0) {
			stats.frames_dropped++;
		}
		k_spin_unlock(&stats_lock, key);
	}

	i2s_trigger(i2s, I2S_DIR_RX, I2S_TRIGGER_DROP);
}

int audio_capture_init(void)
{
	if (!device_is_ready(i2s)) {
		return -ENODEV;
	}
	memset(&stats, 0, sizeof(stats));
	stats.queue_depth = CONFIG_EVAL_FRAME_QUEUE_DEPTH;
	return i2s_configure(i2s, I2S_DIR_RX, &i2s_cfg);
}

int audio_capture_start(void)
{
	if (!atomic_cas(&capture_active, 0, 1)) {
		return -EALREADY;
	}
	k_thread_create(&capture_thread_data, capture_stack, K_THREAD_STACK_SIZEOF(capture_stack),
			capture, NULL, NULL, NULL, 6, 0, K_NO_WAIT);
	k_thread_name_set(&capture_thread_data, "audio_capture");
	return 0;
}

struct k_msgq *audio_capture_queue(void)
{
	return &frame_queue;
}

void audio_capture_set_enqueue(bool enable)
{
	atomic_set(&enqueue_enabled, enable ? 1 : 0);
}

void audio_capture_get_stats(struct audio_capture_stats *out)
{
	k_spinlock_key_t key = k_spin_lock(&stats_lock);

	*out = stats;
	k_spin_unlock(&stats_lock, key);
}

void audio_capture_reset_stats(void)
{
	k_spinlock_key_t key = k_spin_lock(&stats_lock);
	uint32_t captured = stats.frames_captured;

	memset(&stats, 0, sizeof(stats));
	stats.frames_captured = captured;   /* keep the running frame counter monotonic */
	stats.queue_depth = CONFIG_EVAL_FRAME_QUEUE_DEPTH;
	k_spin_unlock(&stats_lock, key);
}

uint32_t audio_capture_sample_rate_hz(void)
{
	return SAMPLE_RATE;
}

uint8_t audio_capture_channels(void)
{
	return CHANNELS;
}
