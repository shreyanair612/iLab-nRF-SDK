#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <dk_buttons_and_leds.h>

#include "audio_capture_adapter.h"
#include "beamformer_delay_sum.h"
#include "run_controller.h"
#include "telemetry_transport.h"

LOG_MODULE_REGISTER(run, LOG_LEVEL_INF);

#ifndef CONFIG_EVAL_STATUS_PERIOD_MS
#define CONFIG_EVAL_STATUS_PERIOD_MS 1000
#endif

#define FW_VERSION "16_3mic_eval-1.0"

/* One second of frames at 16 ms each. */
#define STATS_FRAMES ((CONFIG_EVAL_STATUS_PERIOD_MS + EVAL_FRAME_MS - 1) / EVAL_FRAME_MS)

struct stream_stats {
	uint64_t energy;
	uint32_t samples;
	int32_t peak;
	uint32_t clips;
};

enum control_request {
	REQ_NONE = 0,
	REQ_START,
	REQ_STOP,
};

static eval_run_config_t config;
static struct bf_delay_sum beamformer;

static atomic_t state = ATOMIC_INIT(RUN_IDLE);
static atomic_t request = ATOMIC_INIT(REQ_NONE);
static char stop_reason[24] = "stop";

static uint16_t run_id;
static uint32_t run_start_seq;        /* capture seq of the first frame in the run  */
static uint32_t run_frames;           /* frames processed in this run               */
static uint32_t run_frames_expected;  /* from the run's seq span, includes drops    */
static uint32_t run_target_frames;    /* 0 = until stop                             */
static uint32_t frames_since_stats;
static struct stream_stats stats[EVAL_STREAM_COUNT_ALL];
static struct stream_stats levels[EVAL_MIC_COUNT];
static uint32_t levels_frames;

/* levels: 0 off, 1 once, 2 continuous */
static atomic_t levels_mode = ATOMIC_INIT(0);

K_THREAD_STACK_DEFINE(eval_stack, 8192);
static struct k_thread eval_thread_data;

static inline uint16_t stream_slot(uint16_t stream_id)
{
	return stream_id == EVAL_STREAM_BF ? 3U : stream_id;
}

static void stats_reset(struct stream_stats *s, size_t n)
{
	memset(s, 0, n * sizeof(*s));
}

static void stats_add(struct stream_stats *s, const int16_t *pcm, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		int32_t v = pcm[i];
		int32_t a = v < 0 ? -v : v;

		s->energy += (uint64_t)((int64_t)v * v);
		if (a > s->peak) {
			s->peak = a;
		}
		if (v == INT16_MAX || v == INT16_MIN) {
			s->clips++;
		}
	}
	s->samples += (uint32_t)count;
}

static uint32_t isqrt64(uint64_t x)
{
	uint64_t op = x, res = 0, one = 1ULL << 62;

	while (one > op) {
		one >>= 2;
	}
	while (one != 0U) {
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

static uint32_t stats_rms(const struct stream_stats *s)
{
	return s->samples ? isqrt64(s->energy / s->samples) : 0U;
}

static void emit_audio_stats(uint32_t sample_index)
{
	tt_send_json(TP_AUDIO_STATS, run_id, (uint16_t)EVAL_STREAM_NONE, 0, sample_index, 0,
		     "{\"left\":[%u,%d,%u],\"center\":[%u,%d,%u],\"right\":[%u,%d,%u],"
		     "\"bf\":[%u,%d,%u],\"tx_ring\":%u}",
		     stats_rms(&stats[0]), stats[0].peak, stats[0].clips,
		     stats_rms(&stats[1]), stats[1].peak, stats[1].clips,
		     stats_rms(&stats[2]), stats[2].peak, stats[2].clips,
		     stats_rms(&stats[3]), stats[3].peak, stats[3].clips,
		     tt_ring_used());
	stats_reset(stats, EVAL_STREAM_COUNT_ALL);
}

static void emit_run_start(void)
{
	tt_send_json(TP_RUN_START, run_id, (uint16_t)EVAL_STREAM_NONE, 0, 0, 0,
		     "{\"run_id\":%u,\"label\":\"%s\",\"angle_deg\":%d,\"distance_cm\":%u,"
		     "\"environment\":\"%s\",\"clip\":\"%s\",\"duration_s\":%u,"
		     "\"sample_rate\":%u,\"frame\":%u,\"channels\":%u,\"tdm_slots\":%u,"
		     "\"streams\":{\"left\":%d,\"center\":%d,\"right\":%d,\"bf\":%d},"
		     "\"bf\":{\"delay\":[%d,%d,%d],\"gain\":[%d,%d,%d],\"out_gain\":%d},"
		     "\"fw\":\"%s\"}",
		     run_id, config.label, config.angle_deg, config.distance_cm,
		     config.environment, config.clip, config.duration_s,
		     (unsigned)EVAL_SAMPLE_RATE_HZ, (unsigned)EVAL_FRAME_SAMPLES,
		     (unsigned)EVAL_MIC_COUNT, audio_capture_tdm_slots(),
		     config.stream_raw[0], config.stream_raw[1], config.stream_raw[2],
		     config.stream_bf,
		     config.bf.delay_samples[0], config.bf.delay_samples[1],
		     config.bf.delay_samples[2], config.bf.gain_q15[0], config.bf.gain_q15[1],
		     config.bf.gain_q15[2], config.bf.out_gain_q15, FW_VERSION);
}

static void emit_run_end(const char *reason)
{
	struct audio_capture_stats cs;
	struct tt_stats ts;

	audio_capture_get_stats(&cs);
	tt_get_stats(&ts);

	uint32_t lost = run_frames_expected > run_frames ? run_frames_expected - run_frames : 0U;

	tt_send_json(TP_RUN_END, run_id, (uint16_t)EVAL_STREAM_NONE, 0, run_frames * EVAL_FRAME_SAMPLES,
		     TP_FLAG_FINAL,
		     "{\"run_id\":%u,\"reason\":\"%s\",\"frames\":%u,\"frames_expected\":%u,"
		     "\"frames_lost\":%u,\"duration_ms\":%u,\"capture_dropped\":%u,"
		     "\"i2s_errors\":%u,\"tx_dropped\":%u,\"tx_audio_dropped\":%u,"
		     "\"tx_high_water\":%u,\"tx_ring\":%u,\"bf_clips\":%u,\"unpack_us_max\":%u}",
		     run_id, reason, run_frames, run_frames_expected, lost,
		     (unsigned)(run_frames * EVAL_FRAME_MS), cs.frames_dropped, cs.i2s_errors,
		     ts.packets_dropped, ts.audio_dropped, ts.ring_high_water, ts.ring_size,
		     beamformer.clips, cs.unpack_us_max);

	if (!tt_binary_mode()) {
		tt_send_text(TP_TEXT, "RESULT  run %u (%s) at %d deg / %u cm / %s: %u frames (%u.%03u s), "
			     "%u lost, %u capture drops, %u link drops  %s",
			     run_id, config.label, config.angle_deg, config.distance_cm,
			     config.environment, run_frames, run_frames * EVAL_FRAME_MS / 1000U,
			     run_frames * EVAL_FRAME_MS % 1000U, lost, cs.frames_dropped,
			     ts.audio_dropped,
			     (lost == 0 && ts.audio_dropped == 0) ? "(VALID)" : "(DEGRADED)");
	}
}

static void begin_run(uint32_t first_seq)
{
	run_id++;
	run_start_seq = first_seq;
	run_frames = 0;
	run_frames_expected = 0;
	run_target_frames = config.duration_s
				    ? (uint32_t)config.duration_s * 1000U / EVAL_FRAME_MS
				    : 0U;
	frames_since_stats = 0;
	stats_reset(stats, EVAL_STREAM_COUNT_ALL);
	audio_capture_reset_stats();
	tt_reset_stats();
	bf_init(&beamformer, &config.bf);
	bf_reset(&beamformer);
	atomic_set(&state, RUN_RUNNING);
	dk_set_led_on(DK_LED1);
	emit_run_start();
	LOG_INF("run %u started: %s, %d deg, %u cm, %u s", run_id, config.label, config.angle_deg,
		config.distance_cm, config.duration_s);
}

static void end_run(const char *reason)
{
	emit_audio_stats(run_frames * EVAL_FRAME_SAMPLES);
	emit_run_end(reason);
	atomic_set(&state, RUN_IDLE);
	dk_set_led_off(DK_LED1);
	LOG_INF("run %u ended (%s): %u frames", run_id, reason, run_frames);
}

static void process_frame(const struct capture_frame *f)
{
	int16_t bf_out[EVAL_FRAME_SAMPLES];
	const int16_t *const mic[EVAL_MIC_COUNT] = { f->mic[0], f->mic[1], f->mic[2] };

	/*
	 * Sample index is derived from the capture seq, not from frames seen,
	 * so a dropped frame leaves a hole the host can detect and fill.
	 */
	uint32_t sample_index = (f->seq - run_start_seq) * EVAL_FRAME_SAMPLES;

	run_frames_expected = f->seq - run_start_seq + 1U;

	bf_process(&beamformer, mic, bf_out, f->count);

	for (int m = 0; m < (int)EVAL_MIC_COUNT; m++) {
		stats_add(&stats[m], f->mic[m], f->count);
		if (config.stream_raw[m]) {
			tt_send_audio(run_id, (uint16_t)m, f->seq - run_start_seq, sample_index, 0,
				      f->mic[m], f->count);
		}
	}
	stats_add(&stats[3], bf_out, f->count);
	if (config.stream_bf) {
		tt_send_audio(run_id, (uint16_t)EVAL_STREAM_BF, f->seq - run_start_seq, sample_index,
			      0, bf_out, f->count);
	}

	run_frames++;
	if (++frames_since_stats >= STATS_FRAMES) {
		frames_since_stats = 0;
		emit_audio_stats(sample_index + f->count);
	}
}

static void process_levels(const struct capture_frame *f)
{
	int mode = (int)atomic_get(&levels_mode);

	if (mode == 0) {
		return;
	}
	for (int m = 0; m < (int)EVAL_MIC_COUNT; m++) {
		stats_add(&levels[m], f->mic[m], f->count);
	}
	if (++levels_frames < STATS_FRAMES) {
		return;
	}
	tt_send_text(TP_TEXT, "levels  left[rms %u peak %d clips %u]  center[rms %u peak %d clips %u]  "
		     "right[rms %u peak %d clips %u]",
		     stats_rms(&levels[0]), levels[0].peak, levels[0].clips,
		     stats_rms(&levels[1]), levels[1].peak, levels[1].clips,
		     stats_rms(&levels[2]), levels[2].peak, levels[2].clips);
	stats_reset(levels, EVAL_MIC_COUNT);
	levels_frames = 0;
	if (mode == 1) {
		atomic_set(&levels_mode, 0);
	}
}

static void eval_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	static struct capture_frame frame;
	struct k_msgq *q = audio_capture_queue();

	for (;;) {
		if (k_msgq_get(q, &frame, K_FOREVER) != 0) {
			continue;
		}

		enum control_request req = (enum control_request)atomic_set(&request, REQ_NONE);
		bool running = atomic_get(&state) == RUN_RUNNING;

		if (req == REQ_START && !running) {
			begin_run(frame.seq);
			running = true;
		} else if (req == REQ_STOP && running) {
			end_run(stop_reason);
			running = false;
		}

		if (running) {
			process_frame(&frame);
			if (run_target_frames && run_frames >= run_target_frames) {
				end_run("duration");
			}
		} else {
			process_levels(&frame);
		}
	}
}

/* ----------------------------------------------------------- public */

int run_controller_init(void)
{
	eval_run_config_defaults(&config);
	bf_init(&beamformer, &config.bf);

	k_thread_create(&eval_thread_data, eval_stack, K_THREAD_STACK_SIZEOF(eval_stack),
			eval_thread, NULL, NULL, NULL, 7, 0, K_NO_WAIT);
	k_thread_name_set(&eval_thread_data, "eval");
	return 0;
}

eval_run_config_t *run_controller_config(void)
{
	return &config;
}

bool run_controller_config_mutable(void)
{
	return atomic_get(&state) == RUN_IDLE && atomic_get(&request) == REQ_NONE;
}

enum run_state run_controller_state(void)
{
	return (enum run_state)atomic_get(&state);
}

uint16_t run_controller_run_id(void)
{
	return run_id;
}

int run_controller_start(char *err, size_t err_len)
{
	if (atomic_get(&state) != RUN_IDLE) {
		snprintf(err, err_len, "stop the run first");
		return -EBUSY;
	}
	if (!config.stream_raw[0] && !config.stream_raw[1] && !config.stream_raw[2] &&
	    !config.stream_bf) {
		snprintf(err, err_len, "no stream enabled (set_stream <left|center|right|bf> on)");
		return -EINVAL;
	}
	atomic_set(&request, REQ_START);
	return 0;
}

int run_controller_stop(const char *reason)
{
	if (atomic_get(&state) != RUN_RUNNING) {
		return -EALREADY;
	}
	strncpy(stop_reason, reason, sizeof(stop_reason) - 1);
	stop_reason[sizeof(stop_reason) - 1] = '\0';
	atomic_set(&request, REQ_STOP);
	return 0;
}

void run_controller_reset(void)
{
	eval_run_config_defaults(&config);
}

void run_controller_tick(void)
{
	/* LED2 shows that a host has the USB port open. */
	dk_set_led(DK_LED2, tt_host_connected() ? 1 : 0);
}

void run_controller_emit_hello(void)
{
	tt_send_json(TP_HELLO, 0, (uint16_t)EVAL_STREAM_NONE, 0, k_uptime_get_32(), 0,
		     "{\"device\":\"nrf54lm20dk\",\"fw\":\"%s\",\"protocol\":%u,\"sample_rate\":%u,"
		     "\"frame\":%u,\"channels\":%u,\"tdm_slots\":%u,"
		     "\"streams\":[\"raw_left\",\"raw_center\",\"raw_right\",\"bf\"],"
		     "\"stream_ids\":[0,1,2,16],\"binary\":%d}",
		     FW_VERSION, (unsigned)TP_VERSION, (unsigned)EVAL_SAMPLE_RATE_HZ,
		     (unsigned)EVAL_FRAME_SAMPLES, (unsigned)EVAL_MIC_COUNT,
		     audio_capture_tdm_slots(), tt_binary_mode());
}

void run_controller_emit_status(void)
{
	struct audio_capture_stats cs;
	struct tt_stats ts;

	audio_capture_get_stats(&cs);
	tt_get_stats(&ts);
	tt_send_json(TP_STATUS, run_id, (uint16_t)EVAL_STREAM_NONE, 0, k_uptime_get_32(), 0,
		     "{\"state\":\"%s\",\"run_id\":%u,\"frames\":%u,\"captured\":%u,"
		     "\"capture_dropped\":%u,\"queue_hw\":%u,\"i2s_errors\":%u,\"tx_dropped\":%u,"
		     "\"tx_high_water\":%u,\"tx_ring\":%u,\"rx_bad_crc\":%u,\"host\":%d}",
		     atomic_get(&state) == RUN_RUNNING ? "running" : "idle", run_id, run_frames,
		     cs.frames_captured, cs.frames_dropped, cs.queue_high_water, cs.i2s_errors,
		     ts.packets_dropped, ts.ring_high_water, ts.ring_size, ts.rx_bad_crc,
		     tt_host_connected());
}

void run_controller_emit_config(void)
{
	tt_send_json(TP_RUN_CONFIG, run_id, (uint16_t)EVAL_STREAM_NONE, 0, k_uptime_get_32(), 0,
		     "{\"label\":\"%s\",\"angle_deg\":%d,\"distance_cm\":%u,\"environment\":\"%s\","
		     "\"clip\":\"%s\",\"duration_s\":%u,"
		     "\"streams\":{\"left\":%d,\"center\":%d,\"right\":%d,\"bf\":%d},"
		     "\"bf\":{\"delay\":[%d,%d,%d],\"gain\":[%d,%d,%d],\"out_gain\":%d}}",
		     config.label, config.angle_deg, config.distance_cm, config.environment,
		     config.clip, config.duration_s, config.stream_raw[0], config.stream_raw[1],
		     config.stream_raw[2], config.stream_bf, config.bf.delay_samples[0],
		     config.bf.delay_samples[1], config.bf.delay_samples[2], config.bf.gain_q15[0],
		     config.bf.gain_q15[1], config.bf.gain_q15[2], config.bf.out_gain_q15);
}

void run_controller_request_levels(bool continuous, bool off)
{
	stats_reset(levels, EVAL_MIC_COUNT);
	levels_frames = 0;
	atomic_set(&levels_mode, off ? 0 : (continuous ? 2 : 1));
}
