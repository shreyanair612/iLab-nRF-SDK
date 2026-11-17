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
#if IS_ENABLED(CONFIG_APP_BF_GSC)
#include "beamform_gsc.h"
#endif
#if IS_ENABLED(CONFIG_APP_BF_CAL)
#include "beamform_cal.h"
#endif

LOG_MODULE_REGISTER(beamform, LOG_LEVEL_INF);

#define SAMPLE_RATE       16000
#define SAMPLE_BIT_WIDTH  32
#define CHANNELS          CONFIG_APP_BF_TDM_SLOTS
#define MIC_COUNT         3

/*
 * TDM slot of each mic. Slot order is set by the WS -> WSO daisy chain:
 * the mic whose WS is wired to the nRF FSYNC pin owns slot 0, the next
 * mic in the chain owns slot 1, and so on. Slots 3.. are unused and read
 * back as 0 through the SD pulldown.
 */
#define MIC_LEFT          0
#define MIC_CENTER        1
#define MIC_RIGHT         2

#define SLOT_LEFT         CONFIG_APP_BF_SLOT_LEFT
#define SLOT_CENTER       CONFIG_APP_BF_SLOT_CENTER
#define SLOT_RIGHT        CONFIG_APP_BF_SLOT_RIGHT

#define BLOCK_FRAMES      256
#define BLOCK_BYTES       (BLOCK_FRAMES * CHANNELS * sizeof(int32_t))
#define BLOCK_COUNT       4
#define CAPTURE_GAIN_SHIFT 0

/* Per-mic delay line. Must be a power of two larger than the max delay. */
#define DELAY_LINE_LEN    16
#define DELAY_LINE_MASK   (DELAY_LINE_LEN - 1)

/* Roughly one stats line per second at 16 ms blocks. */
#define MIC_DEBUG_BLOCKS  62

#define TDM_NODE DT_NODELABEL(tdm)

#if !DT_NODE_EXISTS(TDM_NODE)
#error "TDM node does not exist in the final devicetree"
#endif

#if !DT_NODE_HAS_STATUS(TDM_NODE, okay)
#error "TDM node is not enabled in the final devicetree"
#endif

BUILD_ASSERT(SLOT_LEFT < CHANNELS && SLOT_CENTER < CHANNELS && SLOT_RIGHT < CHANNELS,
	     "Mic slot index must be below CONFIG_APP_BF_TDM_SLOTS");
BUILD_ASSERT(SLOT_LEFT != SLOT_CENTER && SLOT_LEFT != SLOT_RIGHT &&
	     SLOT_CENTER != SLOT_RIGHT, "Each mic needs its own TDM slot");
BUILD_ASSERT(CONFIG_APP_BF_DELAY_LEFT < DELAY_LINE_LEN &&
	     CONFIG_APP_BF_DELAY_CENTER < DELAY_LINE_LEN &&
	     CONFIG_APP_BF_DELAY_RIGHT < DELAY_LINE_LEN,
	     "Steering delay exceeds the delay line length");

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(rx_slab, __nocache, BLOCK_BYTES, BLOCK_COUNT, 4);
K_THREAD_STACK_DEFINE(capture_stack, 4096);

static const struct device *const i2s = DEVICE_DT_GET(TDM_NODE);

static const struct i2s_config i2s_cfg = {
	.word_size = SAMPLE_BIT_WIDTH,
	.channels = CHANNELS,
	/*
	 * The ICS-52000 chain is TDM: a one-SCK-wide frame sync pulse on WS
	 * starts the frame and each mic then owns one 32-bit slot. The nRF
	 * TDM driver only allows more than 2 channels in the PCM formats.
	 */
	.format = I2S_FMT_DATA_FORMAT_PCM_SHORT,
	/* The nRF TDM drives both clocks; the ICS-52000 is always a clock slave. */
	.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
	.frame_clk_freq = SAMPLE_RATE,
	.mem_slab = &rx_slab,
	.block_size = BLOCK_BYTES,
	.timeout = 2000,
};

static const uint8_t mic_slot[MIC_COUNT] = {
	[MIC_LEFT] = SLOT_LEFT,
	[MIC_CENTER] = SLOT_CENTER,
	[MIC_RIGHT] = SLOT_RIGHT,
};

/* Steering delays. Kconfig defaults; rewritten per wearer by the calibration. */
static uint8_t mic_delay[MIC_COUNT] = {
	[MIC_LEFT] = CONFIG_APP_BF_DELAY_LEFT,
	[MIC_CENTER] = CONFIG_APP_BF_DELAY_CENTER,
	[MIC_RIGHT] = CONFIG_APP_BF_DELAY_RIGHT,
};

static struct k_thread capture_thread_data;
static atomic_t beamforming_active = ATOMIC_INIT(0);
static void (*chunk_ready)(const int16_t *samples, size_t count);

/*
 * Delay lines persist across DMA blocks so a delayed mic reads samples from
 * the end of the previous block. Only touched by the capture thread.
 */
static int32_t delay_line[MIC_COUNT][DELAY_LINE_LEN];
static uint32_t delay_pos;

#if IS_ENABLED(CONFIG_APP_BF_CAL)
static struct bf_cal cal;
static atomic_t cal_requested = ATOMIC_INIT(0);
static int32_t raw24[MIC_COUNT][BLOCK_FRAMES];
#endif

#if IS_ENABLED(CONFIG_APP_BF_GSC)
static struct beamform_gsc gsc;
/* Frame-sized scratch: the summed beam and the aligned L/R that feed the canceller. */
static int32_t beam24[BLOCK_FRAMES];
static int32_t left24[BLOCK_FRAMES];
static int32_t right24[BLOCK_FRAMES];
#endif

/*
 * Sign-preserving unpack of the 24-bit payload from its 32-bit TDM slot.
 * CONFIG_APP_BF_SLOT_BIT_OFFSET compensates for a mic that starts its MSB
 * one SCK after the frame sync edge while the driver samples with no delay.
 */
static inline int32_t extract_sample_24(int32_t raw)
{
	return (int32_t)((uint32_t)raw << CONFIG_APP_BF_SLOT_BIT_OFFSET) >> 8;
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

static inline int32_t abs32(int32_t x)
{
	return (x < 0) ? -x : x;
}

/* Push one sample per mic into the delay lines and read back the steered taps. */
static inline void delay_and_steer(const int32_t in[MIC_COUNT], int32_t out[MIC_COUNT])
{
	for (size_t m = 0; m < MIC_COUNT; m++) {
		delay_line[m][delay_pos] = in[m];
		out[m] = delay_line[m][(delay_pos - mic_delay[m]) & DELAY_LINE_MASK];
	}

	delay_pos = (delay_pos + 1U) & DELAY_LINE_MASK;
}

#if IS_ENABLED(CONFIG_APP_BF_CAL)
static void apply_calibration(const struct bf_cal_result *fit)
{
	if (!fit->valid) {
		LOG_WRN("wearer fit rejected: L/R coherence %d%% (lag %d); keeping delays %u/%u/%u",
			(int)(fit->coherence * 100.0f), fit->lr_lag, mic_delay[MIC_LEFT],
			mic_delay[MIC_CENTER], mic_delay[MIC_RIGHT]);
		return;
	}

	/* Only touched on this thread, so the per-sample loop never sees a half-written set. */
	mic_delay[MIC_LEFT] = fit->delay_left;
	mic_delay[MIC_RIGHT] = fit->delay_right;
	mic_delay[MIC_CENTER] = fit->delay_center;

#if IS_ENABLED(CONFIG_APP_BF_GSC)
	gsc.gain_r = (CONFIG_APP_GSC_GAIN_RIGHT_Q15 / 32768.0f) * fit->gain_right;
#endif

	LOG_INF("wearer fit: delays L/C/R = %u/%u/%u, R gain x%d.%02d, coherence %d%%",
		mic_delay[MIC_LEFT], mic_delay[MIC_CENTER], mic_delay[MIC_RIGHT],
		(int)fit->gain_right, (int)(fit->gain_right * 100.0f) % 100,
		(int)(fit->coherence * 100.0f));
}
#endif

void beamform_calibrate(void)
{
#if IS_ENABLED(CONFIG_APP_BF_CAL)
	atomic_set(&cal_requested, 1);
#endif
}

static void log_mic_peaks(const int32_t peak[MIC_COUNT], int32_t beam_peak)
{
	/*
	 * Bring-up check: every mic should show a similar non-zero peak when
	 * you talk from the front. A mic stuck at 0 is a broken WS -> WSO link
	 * or a wrong slot index; values pinned near 2^23 usually mean the
	 * slot bit offset is wrong.
	 */
	LOG_INF("mic peak (24-bit): L=%d C=%d R=%d  beam(int16)=%d",
		peak[MIC_LEFT], peak[MIC_CENTER], peak[MIC_RIGHT], beam_peak);
#if IS_ENABLED(CONFIG_APP_BF_GSC)
	LOG_INF("canceller adapted in %u frames so far", gsc.adapt_frames);
#endif
}

static void capture(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	uint32_t blocks_since_log = 0;
	int32_t peak[MIC_COUNT] = {0};
	int32_t beam_peak = 0;

	int err = i2s_trigger(i2s, I2S_DIR_RX, I2S_TRIGGER_START);

	if (err) {
		LOG_ERR("I2S start failed: %d", err);
		atomic_clear(&beamforming_active);
		return;
	}

#if IS_ENABLED(CONFIG_APP_BF_GSC)
	gsc_init(&gsc);
	LOG_INF("sidelobe canceller on: %d taps, mu %d/1000, gate %d dB, adapt every %d frames",
		GSC_TAPS, CONFIG_APP_GSC_MU_X1000, CONFIG_APP_GSC_GATE_DB,
		CONFIG_APP_GSC_ADAPT_EVERY_N_FRAMES);
#endif

	LOG_INF("capture: %d Hz, %d mics in %d TDM slots, %d-bit slots, %u frames/block (%u ms)",
		SAMPLE_RATE, MIC_COUNT, CHANNELS, SAMPLE_BIT_WIDTH, (unsigned int)BLOCK_FRAMES,
		(unsigned int)((1000U * BLOCK_FRAMES) / SAMPLE_RATE));
	LOG_INF("slots L/C/R = %d/%d/%d, delays L/C/R = %d/%d/%d samples, bit offset %d",
		SLOT_LEFT, SLOT_CENTER, SLOT_RIGHT,
		CONFIG_APP_BF_DELAY_LEFT, CONFIG_APP_BF_DELAY_CENTER, CONFIG_APP_BF_DELAY_RIGHT,
		CONFIG_APP_BF_SLOT_BIT_OFFSET);

	while (atomic_get(&beamforming_active)) {
		void *block;
		size_t bytes;
		int16_t mono_chunk[BLOCK_FRAMES];

		err = i2s_read(i2s, &block, &bytes);
		if (err) {
			if (err != -EAGAIN) {
				LOG_WRN("I2S read failed: %d", err);
			}
			continue;
		}

		const int32_t *tdm = block;
		size_t frames = bytes / (CHANNELS * sizeof(int32_t));

		frames = MIN(frames, (size_t)BLOCK_FRAMES);

		for (size_t i = 0; i < frames; i++) {
			int32_t mic[MIC_COUNT];
			int32_t aligned[MIC_COUNT];

			for (size_t m = 0; m < MIC_COUNT; m++) {
				mic[m] = extract_sample_24(tdm[CHANNELS * i + mic_slot[m]]);

				if (IS_ENABLED(CONFIG_APP_BF_MIC_DEBUG)) {
					peak[m] = MAX(peak[m], abs32(mic[m]));
				}
			}

#if IS_ENABLED(CONFIG_APP_BF_CAL)
			for (size_t m = 0; m < MIC_COUNT; m++) {
				raw24[m][i] = mic[m];
			}
#endif

			delay_and_steer(mic, aligned);

			/*
			 * Linear 3-mic delay-and-sum beamformer:
			 *   y[n] = (L[n - dL] + C[n - dC] + R[n - dR]) / 3
			 * With all delays 0 this is a broadside beam, which is right
			 * for a far source. The wearer's mouth is near-field and
			 * about one sample closer to the center mic; a center delay
			 * of 1 (CONFIG_APP_BF_DELAY_CENTER) lines that up.
			 * Summing three 24-bit values cannot overflow int32_t.
			 */
			int32_t sum = (aligned[MIC_LEFT] + aligned[MIC_CENTER] +
				       aligned[MIC_RIGHT]) / MIC_COUNT;

#if IS_ENABLED(CONFIG_APP_BF_GSC)
			beam24[i] = sum;
			left24[i] = aligned[MIC_LEFT];
			right24[i] = aligned[MIC_RIGHT];
#else
			mono_chunk[i] = saturate_int16(sum >> (8 - CAPTURE_GAIN_SHIFT));
#endif
		}

#if IS_ENABLED(CONFIG_APP_BF_CAL)
		if (atomic_get(&cal_requested)) {
			if (!cal.active) {
				bf_cal_begin(&cal);
			}

			struct bf_cal_result fit;

			if (bf_cal_feed(&cal, raw24[MIC_LEFT], raw24[MIC_CENTER], raw24[MIC_RIGHT],
					frames, &fit)) {
				atomic_clear(&cal_requested);
				apply_calibration(&fit);
			}
		}
#endif

#if IS_ENABLED(CONFIG_APP_BF_GSC)
		/*
		 * Adaptive sidelobe canceller: removes from the beam what can be
		 * predicted from L - R, which holds side noise but no wearer voice.
		 */
		gsc_process(&gsc, beam24, left24, right24, frames);
		for (size_t i = 0; i < frames; i++) {
			mono_chunk[i] = saturate_int16(beam24[i] >> (8 - CAPTURE_GAIN_SHIFT));
		}
#endif

		if (IS_ENABLED(CONFIG_APP_BF_MIC_DEBUG)) {
			for (size_t i = 0; i < frames; i++) {
				beam_peak = MAX(beam_peak, abs32(mono_chunk[i]));
			}
		}

		/*
		 * Hand the frame over before releasing the DMA block. The
		 * consumer copies synchronously, so mono_chunk staying on this
		 * stack is safe and no ownership is transferred.
		 */
		if (chunk_ready && frames > 0U) {
			chunk_ready(mono_chunk, frames);
		}

		k_mem_slab_free(&rx_slab, block);

		if (IS_ENABLED(CONFIG_APP_BF_MIC_DEBUG) &&
		    ++blocks_since_log >= MIC_DEBUG_BLOCKS) {
			log_mic_peaks(peak, beam_peak);
			blocks_since_log = 0;
			beam_peak = 0;
			for (size_t m = 0; m < MIC_COUNT; m++) {
				peak[m] = 0;
			}
		}
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
	return BLOCK_FRAMES;
}
