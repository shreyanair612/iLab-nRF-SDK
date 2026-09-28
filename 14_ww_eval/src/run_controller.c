#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <dk_buttons_and_leds.h>

#include "audio_capture_adapter.h"
#include "audio_metrics.h"
#include "eval_paths.h"
#include "run_controller.h"
#include "telemetry_transport.h"
#include "wakeword_eval.h"

LOG_MODULE_REGISTER(run, LOG_LEVEL_INF);

#ifndef CONFIG_EVAL_LINK_BYTES_PER_SEC
#define CONFIG_EVAL_LINK_BYTES_PER_SEC 80000
#endif
#ifndef CONFIG_EVAL_STATUS_PERIOD_MS
#define CONFIG_EVAL_STATUS_PERIOD_MS 1000
#endif

#define FW_VERSION      "14_ww_eval-1.0"
#define STREAM_COUNT    (3U + EVAL_PATH_COUNT)     /* raw L, R, C + paths */
#define REPLAY_ACK_EVERY 4U
#define PERIOD_FRAMES   ((CONFIG_EVAL_STATUS_PERIOD_MS + EVAL_FRAME_MS - 1U) / EVAL_FRAME_MS)
#define BYTES_PER_STREAM_S (EVAL_SAMPLE_RATE_HZ * 2U)   /* 16-bit mono */
#define FRAMING_OVERHEAD_PCT 4U
#define TELEMETRY_BYTES_S 2500U
#define LED_RUN   DK_LED1
#define LED_EVENT DK_LED2

/* Stream bookkeeping: sequence numbers and unreported drops. */
struct stream_state {
	uint32_t seq;
	uint32_t pending_gap_frames;
	uint32_t pending_gap_from;
	uint32_t gaps;
	uint32_t frames_lost;
};

struct run_ctx {
	atomic_t state;
	uint16_t run_id;
	eval_run_config_t cfg;
	struct eval_paths ep;
	struct audio_metrics raw_m[3];
	struct audio_metrics path_m[EVAL_PATH_COUNT];
	struct stream_state streams[STREAM_COUNT];
	uint32_t sample_index;
	uint32_t frames;
	uint32_t frames_since_period;
	int64_t start_uptime_ms;
	uint32_t detections_total;
	char stop_reason[24];

	/* replay */
	eval_path_id_t replay_path;
	uint32_t replay_expected;
	uint32_t replay_received;
	uint32_t replay_consumed;
	uint32_t replay_dropped;
	uint32_t replay_bad;
};

static struct run_ctx ctx;
static K_THREAD_STACK_DEFINE(eval_stack, 8192);
static struct k_thread eval_thread_data;
static int64_t led_event_off_ms;
static int64_t idle_status_next_ms;

/* Text-mode microphone level readout: 0 off, 1 print once, 2 print every period. */
static atomic_t levels_mode = ATOMIC_INIT(0);
#define TEXT_PROGRESS_FRAMES (5U * PERIOD_FRAMES)   /* one progress line per ~5 s */
static uint32_t text_progress_frames;

static inline uint16_t stream_idx_for_path(eval_path_id_t p)
{
	return (uint16_t)(3U + p);
}

static inline uint16_t wire_stream_for_path(eval_path_id_t p)
{
	return EVAL_STREAM_FOR_PATH(p);
}

static inline uint16_t run_flags(void)
{
	return (atomic_get(&ctx.state) == RUN_REPLAY) ? TP_FLAG_REPLAY : 0;
}

/* ----------------------------------------------------- admission */

int run_controller_admission_check(const eval_run_config_t *cfg, char *msg, size_t msg_len,
				   uint32_t *bytes_per_s)
{
	uint32_t streams = 0;

	if (cfg->capture_mode != EVAL_CAPTURE_OFF) {
		streams += cfg->capture_raw_left ? 1U : 0U;
		streams += cfg->capture_raw_right ? 1U : 0U;
		streams += (cfg->capture_raw_center && audio_capture_channels() >= 3U) ? 1U : 0U;
		for (int p = EVAL_PATH_RAW_LR_MIX; p < EVAL_PATH_COUNT; p++) {
			/* raw_left/raw_right paths duplicate the physical channels. */
			if (cfg->path[p].enabled && cfg->path[p].capture_audio) {
				streams++;
			}
		}
	}

	uint32_t audio = streams * BYTES_PER_STREAM_S;
	uint32_t total = audio + (audio * FRAMING_OVERHEAD_PCT) / 100U + TELEMETRY_BYTES_S;

	if (bytes_per_s) {
		*bytes_per_s = total;
	}
	if (total > CONFIG_EVAL_LINK_BYTES_PER_SEC) {
		snprintf(msg, msg_len,
			 "link budget: %u streams need %u B/s, link allows %u B/s; "
			 "disable a capture stream or set_capture_mode off",
			 streams, total, (unsigned)CONFIG_EVAL_LINK_BYTES_PER_SEC);
		return -EINVAL;
	}
	if (msg && msg_len) {
		snprintf(msg, msg_len, "%u streams, %u B/s of %u", streams, total,
			 (unsigned)CONFIG_EVAL_LINK_BYTES_PER_SEC);
	}
	return 0;
}

/* ------------------------------------------------------- JSON out */

static int json_config(char *buf, size_t len, const eval_run_config_t *c)
{
	int n = snprintf(buf, len,
		"{\"run_id\":%u,\"label\":\"%s\",\"angle_deg\":%d,\"distance_cm\":%u,"
		"\"phrase\":\"%s\",\"environment\":\"%s\",\"preset\":\"%s\",\"live_path\":%d,"
		"\"reduce\":\"%s\",\"capture_mode\":\"%s\",\"capture_seconds\":%u,"
		"\"raw\":{\"left\":%d,\"right\":%d,\"center\":%d},\"duration_s\":%u,"
		"\"threshold_x1000\":%u,\"cooldown_ms\":%u,\"vote\":\"%d/%d\","
		"\"sample_rate\":%u,\"frame\":%u,\"window\":%u,\"model\":\"%s\",\"fw\":\"%s\","
		"\"channels\":%u,\"paths\":[",
		ctx.run_id, c->label, c->angle_deg, c->distance_cm, c->phrase, c->environment,
		eval_preset_name(c->preset), c->live_path, eval_reduce_mode_name(c->raw_3ch_reduce),
		eval_capture_mode_name(c->capture_mode), c->capture_seconds,
		c->capture_raw_left, c->capture_raw_right, c->capture_raw_center, c->duration_s,
		c->ww_threshold_x1000, c->ww_cooldown_ms, CONFIG_WW_COUNT_THRESHOLD,
		CONFIG_WW_HISTORY_SIZE, (unsigned)EVAL_SAMPLE_RATE_HZ, (unsigned)EVAL_FRAME_SAMPLES,
		ww_eval_window_samples(), ww_eval_model_id(), FW_VERSION, audio_capture_channels());

	for (int p = 0; p < EVAL_PATH_COUNT && n > 0 && (size_t)n < len; p++) {
		const eval_path_config_t *pc = &c->path[p];

		n += snprintf(buf + n, len - n,
			"%s{\"id\":%d,\"name\":\"%s\",\"enabled\":%d,\"capture\":%d,\"ww\":%d,"
			"\"dl\":%d,\"dr\":%d,\"dc\":%d,\"gain\":%d}",
			p ? "," : "", p, pc->name, pc->enabled, pc->capture_audio, pc->run_wakeword,
			pc->left_delay_samples, pc->right_delay_samples, pc->center_delay_samples,
			pc->gain_q15);
	}
	if (n > 0 && (size_t)n < len) {
		n += snprintf(buf + n, len - n, "]}");
	}
	return n;
}

void run_controller_emit_config(void)
{
	char buf[960];

	json_config(buf, sizeof(buf), &ctx.cfg);
	tt_send_json(TP_RUN_CONFIG, ctx.run_id, EVAL_STREAM_NONE, 0, k_uptime_get_32(),
		     run_flags(), "%s", buf);
}

void run_controller_emit_hello(void)
{
	tt_send_json(TP_HELLO, ctx.run_id, EVAL_STREAM_NONE, 0, k_uptime_get_32(), 0,
		"{\"device\":\"nrf54lm20dk/nrf54lm20b/cpuapp\",\"fw\":\"%s\",\"build\":\"%s %s\","
		"\"sample_rate\":%u,\"frame\":%u,\"channels\":%u,\"window\":%u,\"model\":\"%s\","
		"\"max_delay\":%d,\"link_bps\":%u,\"tx_ring\":%u,\"queue_depth\":%d,"
		"\"paths\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"],"
		"\"path_stream_base\":%d,\"state\":%d}",
		FW_VERSION, __DATE__, __TIME__, (unsigned)EVAL_SAMPLE_RATE_HZ,
		(unsigned)EVAL_FRAME_SAMPLES, audio_capture_channels(), ww_eval_window_samples(),
		ww_eval_model_id(), (int)EVAL_MAX_DELAY_SAMPLES,
		(unsigned)CONFIG_EVAL_LINK_BYTES_PER_SEC, (unsigned)CONFIG_EVAL_TX_RING_BYTES,
		CONFIG_EVAL_FRAME_QUEUE_DEPTH,
		eval_path_name(0), eval_path_name(1), eval_path_name(2), eval_path_name(3),
		eval_path_name(4), eval_path_name(5), (int)EVAL_STREAM_PATH_BASE,
		(int)atomic_get(&ctx.state));
}

static const char *state_name(void)
{
	switch (atomic_get(&ctx.state)) {
	case RUN_RUNNING: return "running";
	case RUN_REPLAY: return "replay";
	default: return "idle";
	}
}

void run_controller_emit_status(void)
{
	struct audio_capture_stats cs;
	struct tt_stats ts;
	int64_t elapsed = (atomic_get(&ctx.state) == RUN_IDLE) ? 0 :
			  (k_uptime_get() - ctx.start_uptime_ms);

	audio_capture_get_stats(&cs);
	tt_get_stats(&ts);
	tt_send_json(TP_STATUS, ctx.run_id, EVAL_STREAM_NONE, 0, k_uptime_get_32(), run_flags(),
		"{\"state\":\"%s\",\"run_id\":%u,\"uptime_ms\":%u,\"elapsed_ms\":%lld,"
		"\"sample_index\":%u,\"binary\":%d,\"live_path\":%d,"
		"\"cap\":{\"captured\":%u,\"dropped\":%u,\"discarded\":%u,\"qhw\":%u,\"qdepth\":%u,"
		"\"i2s_err\":%u,\"unpack_us\":%u,\"unpack_max_us\":%u},"
		"\"tx\":{\"sent\":%u,\"dropped\":%u,\"audio_dropped\":%u,\"ring_hw\":%u,\"ring\":%u,"
		"\"ring_used\":%u,\"rx_crc\":%u,\"rx_overflow\":%u,\"log_drop\":%u},"
		"\"replay\":{\"path\":%d,\"expected\":%u,\"received\":%u,\"consumed\":%u,"
		"\"dropped\":%u,\"bad\":%u}}",
		state_name(), ctx.run_id, k_uptime_get_32(), elapsed, ctx.sample_index,
		tt_binary_mode() ? 1 : 0, ctx.cfg.live_path,
		cs.frames_captured, cs.frames_dropped, cs.frames_discarded, cs.queue_high_water,
		cs.queue_depth, cs.i2s_errors, cs.unpack_us_last, cs.unpack_us_max,
		ts.packets_sent, ts.packets_dropped, ts.audio_dropped, ts.ring_high_water,
		ts.ring_size, tt_ring_used(), ts.rx_bad_crc, ts.rx_overflow, ts.log_lines_dropped,
		ctx.replay_path, ctx.replay_expected, ctx.replay_received, ctx.replay_consumed,
		ctx.replay_dropped, ctx.replay_bad);
}

static void emit_path_metrics(eval_path_id_t p, bool final)
{
	const eval_path_config_t *pc = &ctx.cfg.path[p];
	const struct ww_path_metrics *w = ww_eval_metrics(p);
	const struct audio_metrics *a = &ctx.path_m[p];
	const struct bf_delay_sum *bf = (p == EVAL_PATH_BF_LR) ? &ctx.ep.bf_lr :
					(p == EVAL_PATH_BF_3CH) ? &ctx.ep.bf_3ch : NULL;
	float mean = w->windows ? (w->score_sum / (float)w->windows) : 0.0f;
	float var = w->windows ? (w->score_sumsq / (float)w->windows) - mean * mean : 0.0f;
	float sd = var > 0.0f ? sqrtf(var) : 0.0f;
	uint32_t inf_mean = w->windows ? (uint32_t)(w->infer_sum_us / w->windows) : 0U;
	bool scored = (p == ww_eval_bound_path());

	tt_send_json(TP_PATH_METRICS, ctx.run_id, wire_stream_for_path(p), 0, ctx.sample_index,
		run_flags() | (final ? TP_FLAG_FINAL : 0),
		"{\"path\":%d,\"name\":\"%s\",\"enabled\":%d,\"scored\":%d,\"live\":%d,"
		"\"frames\":%u,\"samples\":%u,\"windows\":%u,\"det\":%u,\"cool\":%u,\"miss\":%u,"
		"\"err\":%u,\"last_x1000\":%d,\"peak_x1000\":%d,\"mean_x1000\":%d,\"min_x1000\":%d,"
		"\"std_x1000\":%d,\"last_det_sample\":%u,\"last_win_sample\":%u,"
		"\"inf_last_us\":%u,\"inf_mean_us\":%u,\"inf_max_us\":%u,"
		"\"rms\":%u,\"peak_abs\":%u,\"clips\":%u,\"crc\":\"%08x\",\"audio_frames\":%u,"
		"\"bf\":{\"dl\":%d,\"dr\":%d,\"dc\":%d,\"gain\":%d,\"clips\":%u}}",
		p, pc->name, pc->enabled, scored, (p == ctx.cfg.live_path),
		w->frames, w->samples, w->windows, w->detections, w->cooldown_suppressed,
		w->deadline_misses, w->errors, (int)(w->score_last * 1000.0f),
		(int)(w->score_peak * 1000.0f), (int)(mean * 1000.0f),
		(int)((w->windows ? w->score_min : 0.0f) * 1000.0f), (int)(sd * 1000.0f),
		w->last_detection_sample, w->last_window_sample, w->infer_last_us, inf_mean,
		w->infer_max_us, am_rms(&a->run), a->run.peak_abs, a->run.clips, a->crc32,
		a->frames, pc->left_delay_samples, pc->right_delay_samples,
		pc->center_delay_samples, pc->gain_q15, bf ? bf->clip_count : 0U);
}

static void emit_audio_stats(void)
{
	char buf[700];
	int n = snprintf(buf, sizeof(buf), "{\"streams\":[");
	bool first = true;

	for (int r = 0; r < 3; r++) {
		if (r == 2 && audio_capture_channels() < 3U) {
			continue;
		}
		struct am_accum w;

		am_window_flush(&ctx.raw_m[r], &w);
		n += snprintf(buf + n, sizeof(buf) - n,
			"%s{\"id\":%d,\"rms\":%u,\"peak\":%u,\"clips\":%u}", first ? "" : ",",
			r, am_rms(&w), w.peak_abs, w.clips);
		first = false;
	}
	for (int p = 0; p < EVAL_PATH_COUNT; p++) {
		if (!(ctx.ep.produced_mask & (1U << p)) && !ctx.cfg.path[p].enabled) {
			continue;
		}
		struct am_accum w;

		am_window_flush(&ctx.path_m[p], &w);
		n += snprintf(buf + n, sizeof(buf) - n,
			"%s{\"id\":%d,\"rms\":%u,\"peak\":%u,\"clips\":%u}", first ? "" : ",",
			wire_stream_for_path((eval_path_id_t)p), am_rms(&w), w.peak_abs, w.clips);
		first = false;
	}
	snprintf(buf + n, sizeof(buf) - n, "]}");
	tt_send_json(TP_AUDIO_STATS, ctx.run_id, EVAL_STREAM_NONE, 0, ctx.sample_index,
		     run_flags(), "%s", buf);
}

/* Prints one readable line with the raw microphones' level since the last call. */
static void text_levels_line(const char *prefix)
{
	struct am_accum w[2];

	am_window_flush(&ctx.raw_m[0], &w[0]);
	am_window_flush(&ctx.raw_m[1], &w[1]);
	for (int i = 0; i < 2; i++) {
		if (w[i].samples == 0U) {
			w[i].peak_abs = 0;
		}
	}
	int db[2];

	for (int i = 0; i < 2; i++) {
		db[i] = w[i].peak_abs ? (int)lroundf(20.0f * log10f((float)w[i].peak_abs / 32768.0f)) : -96;
	}
	tt_send_text(TP_TEXT, "%sleft rms %4u peak %5u (%3d dBFS) | right rms %4u peak %5u (%3d dBFS) | clipped %u/%u",
		     prefix, am_rms(&w[0]), w[0].peak_abs, db[0], am_rms(&w[1]), w[1].peak_abs, db[1],
		     w[0].clips, w[1].clips);
}

void run_controller_request_levels(bool continuous, bool off)
{
	atomic_set(&levels_mode, off ? 0 : (continuous ? 2 : 1));
}

/* ------------------------------------------------------ streaming */

static void stream_audio(uint16_t sidx, uint16_t wire_id, const int16_t *pcm, uint16_t n,
			 uint32_t sample_index)
{
	struct stream_state *s = &ctx.streams[sidx];
	int rc = tt_send_audio(ctx.run_id, wire_id, s->seq, sample_index, run_flags(), pcm, n);

	if (rc == 0) {
		if (s->pending_gap_frames) {
			tt_send_json(TP_AUDIO_GAP, ctx.run_id, wire_id, s->seq, sample_index,
				run_flags(),
				"{\"stream\":%u,\"frames_lost\":%u,\"from_sample\":%u,"
				"\"to_sample\":%u,\"reason\":\"tx_ring_full\"}",
				wire_id, s->pending_gap_frames, s->pending_gap_from, sample_index);
			s->gaps++;
			s->frames_lost += s->pending_gap_frames;
			s->pending_gap_frames = 0;
		}
		s->seq++;
	} else {
		if (s->pending_gap_frames == 0) {
			s->pending_gap_from = sample_index;
		}
		s->pending_gap_frames++;
		s->seq++;   /* the host sees the hole in seq as well */
	}
}

static void on_ww_event(eval_path_id_t path, uint32_t sample_index, float score, float peak,
			void *user)
{
	ARG_UNUSED(user);
	const struct ww_path_metrics *w = ww_eval_metrics(path);

	ctx.detections_total++;
	if (!tt_binary_mode()) {
		uint32_t ms = (uint32_t)((uint64_t)sample_index * 1000U / EVAL_SAMPLE_RATE_HZ);

		tt_send_text(TP_TEXT, ">>> wake word #%u on %s at %u.%02u s (score 0.%03d)",
			     w->detections, eval_path_name(path), ms / 1000U, (ms % 1000U) / 10U,
			     (int)(score * 1000.0f));
		dk_set_led_on(LED_EVENT);
		led_event_off_ms = k_uptime_get() + 200;
		return;
	}
	tt_send_json(TP_WAKEWORD_EVENT, ctx.run_id, wire_stream_for_path(path), w->detections,
		sample_index, run_flags(),
		"{\"path\":%d,\"name\":\"%s\",\"sample\":%u,\"ms\":%u,\"score_x1000\":%d,"
		"\"peak_x1000\":%d,\"n\":%u}",
		path, eval_path_name(path), sample_index,
		(unsigned)((uint64_t)sample_index * 1000U / EVAL_SAMPLE_RATE_HZ),
		(int)(score * 1000.0f), (int)(peak * 1000.0f), w->detections);
	dk_set_led_on(LED_EVENT);
	led_event_off_ms = k_uptime_get() + 200;
}

/* --------------------------------------------------- frame handling */

static void reset_run_state(void)
{
	ctx.sample_index = 0;
	ctx.frames = 0;
	ctx.frames_since_period = 0;
	text_progress_frames = 0;
	ctx.detections_total = 0;
	ctx.stop_reason[0] = '\0';
	memset(ctx.streams, 0, sizeof(ctx.streams));
	for (int i = 0; i < 3; i++) {
		am_reset(&ctx.raw_m[i]);
	}
	for (int p = 0; p < EVAL_PATH_COUNT; p++) {
		am_reset(&ctx.path_m[p]);
		ww_eval_reset_path((eval_path_id_t)p);
	}
	eval_paths_apply_config(&ctx.ep);
	eval_paths_reset_state(&ctx.ep);
	audio_capture_reset_stats();
	tt_reset_stats();
	ww_eval_set_params(ctx.cfg.ww_threshold_x1000, ctx.cfg.ww_cooldown_ms);
}

static void process_audio_frame(const struct capture_frame *f, bool replay)
{
	const uint32_t frame_start = ctx.sample_index;
	struct eval_frame ef = {
		.seq = f->seq, .sample_index = frame_start, .count = f->count,
		.left = f->left, .right = f->right,
		.center = (f->channels >= 3U) ? f->center : NULL,
	};

	ctx.sample_index += f->count;
	ctx.frames++;

	uint32_t produced = eval_paths_process(&ctx.ep, &ef);

	/* Raw microphone streams: metrics always, audio only on a live run. */
	am_update(&ctx.raw_m[0], f->left, f->count);
	am_update(&ctx.raw_m[1], f->right, f->count);
	if (f->channels >= 3U) {
		am_update(&ctx.raw_m[2], f->center, f->count);
	}
	if (!replay && ctx.cfg.capture_mode != EVAL_CAPTURE_OFF) {
		if (ctx.cfg.capture_raw_left) {
			stream_audio(0, EVAL_STREAM_RAW_LEFT, f->left, f->count, frame_start);
		}
		if (ctx.cfg.capture_raw_right) {
			stream_audio(1, EVAL_STREAM_RAW_RIGHT, f->right, f->count, frame_start);
		}
		if (ctx.cfg.capture_raw_center && f->channels >= 3U) {
			stream_audio(2, EVAL_STREAM_RAW_CENTER, f->center, f->count, frame_start);
		}
	}

	eval_path_id_t scored = replay ? ctx.replay_path : ctx.cfg.live_path;

	for (int p = 0; p < EVAL_PATH_COUNT; p++) {
		if (!(produced & (1U << p))) {
			continue;
		}
		const int16_t *out = eval_paths_output(&ctx.ep, (eval_path_id_t)p);

		am_update(&ctx.path_m[p], out, f->count);

		bool duplicate_of_raw = (p == EVAL_PATH_RAW_LEFT || p == EVAL_PATH_RAW_RIGHT);
		bool want_audio = replay ? (p == (int)ctx.replay_path)
					 : (!duplicate_of_raw &&
					    ctx.cfg.capture_mode != EVAL_CAPTURE_OFF &&
					    ctx.cfg.path[p].capture_audio);
		if (want_audio) {
			stream_audio(stream_idx_for_path((eval_path_id_t)p),
				     wire_stream_for_path((eval_path_id_t)p), out, f->count,
				     frame_start);
		}
		if (p == (int)scored && ctx.cfg.path[p].run_wakeword) {
			ww_eval_process(out, f->count, frame_start, on_ww_event, NULL);
		}
	}

	if (!tt_binary_mode()) {
		if (!replay && ++text_progress_frames >= TEXT_PROGRESS_FRAMES) {
			char prefix[80];
			uint32_t ms = (uint32_t)((uint64_t)ctx.sample_index * 1000U / EVAL_SAMPLE_RATE_HZ);

			text_progress_frames = 0;
			snprintf(prefix, sizeof(prefix), "t=%3u s  %s detections %u  |  ", ms / 1000U,
				 eval_path_name(ctx.cfg.live_path),
				 ww_eval_metrics(ctx.cfg.live_path)->detections);
			text_levels_line(prefix);
		}
		return;
	}
	if (++ctx.frames_since_period >= PERIOD_FRAMES) {
		ctx.frames_since_period = 0;
		emit_audio_stats();
		for (int p = 0; p < EVAL_PATH_COUNT; p++) {
			if (ctx.cfg.path[p].enabled) {
				emit_path_metrics((eval_path_id_t)p, false);
			}
		}
		run_controller_emit_status();
	}
}

static void print_summary(bool replay)
{
	const eval_run_config_t *c = &ctx.cfg;
	struct audio_capture_stats cs;
	struct tt_stats ts;
	uint32_t gaps = 0, lost = 0, misses = 0;

	audio_capture_get_stats(&cs);
	tt_get_stats(&ts);
	for (unsigned i = 0; i < STREAM_COUNT; i++) {
		gaps += ctx.streams[i].gaps;
		lost += ctx.streams[i].frames_lost;
	}
	for (int p = 0; p < EVAL_PATH_COUNT; p++) {
		misses += ww_eval_metrics((eval_path_id_t)p)->deadline_misses;
	}

	tt_send_text(TP_TEXT, "============================================================");
	tt_send_text(TP_TEXT, "Wake-Word Beamforming Evaluation %s", replay ? "Replay Pass" : "Run");
	tt_send_text(TP_TEXT, "============================================================");
	tt_send_text(TP_TEXT, "Run ID:                   %u", ctx.run_id);
	tt_send_text(TP_TEXT, "Label:                    %s", c->label);
	tt_send_text(TP_TEXT, "Angle / distance:         %d degrees / %u cm", c->angle_deg, c->distance_cm);
	tt_send_text(TP_TEXT, "Environment:              %s", c->environment);
	tt_send_text(TP_TEXT, "Preset:                   %s", eval_preset_name(c->preset));
	tt_send_text(TP_TEXT, "Duration:                 %u.%02u seconds", ctx.sample_index / EVAL_SAMPLE_RATE_HZ,
		     (ctx.sample_index % EVAL_SAMPLE_RATE_HZ) * 100U / EVAL_SAMPLE_RATE_HZ);
	tt_send_text(TP_TEXT, "Sample rate:              %u Hz", (unsigned)EVAL_SAMPLE_RATE_HZ);
	tt_send_text(TP_TEXT, "Audio frame:              %u samples / %u ms", (unsigned)EVAL_FRAME_SAMPLES, (unsigned)EVAL_FRAME_MS);
	tt_send_text(TP_TEXT, "Wake-word threshold:      0.%03u   vote %d/%d", ww_eval_threshold_x1000(),
		     CONFIG_WW_COUNT_THRESHOLD, CONFIG_WW_HISTORY_SIZE);
	tt_send_text(TP_TEXT, "Cooldown:                 %u ms", ww_eval_cooldown_ms());
	tt_send_text(TP_TEXT, "Capture mode:             %s", eval_capture_mode_name(c->capture_mode));
	tt_send_text(TP_TEXT, "Scored path:              %s (%s)", eval_path_name(ww_eval_bound_path()),
		     replay ? "replay" : "live");
	tt_send_text(TP_TEXT, "Stop reason:              %s", ctx.stop_reason);
	tt_send_text(TP_TEXT, "------------------------------------------------------------");
	tt_send_text(TP_TEXT, "Path         Enabled Scored Det  Peak  Mean  RMS    Clips AvgInf  MaxInf  Miss");
	for (int p = 0; p < EVAL_PATH_COUNT; p++) {
		const eval_path_config_t *pc = &c->path[p];
		const struct ww_path_metrics *w = ww_eval_metrics((eval_path_id_t)p);
		const struct audio_metrics *a = &ctx.path_m[p];

		if (!pc->enabled) {
			tt_send_text(TP_TEXT, "%-12s No      -      -    -     -     -      -     -       -       -", pc->name);
			continue;
		}
		bool scored = (p == (int)ww_eval_bound_path()) && w->windows;
		uint32_t mean_x1000 = w->windows ? (uint32_t)(w->score_sum * 1000.0f / w->windows) : 0;

		if (scored) {
			tt_send_text(TP_TEXT, "%-12s Yes     yes    %-4u 0.%03d 0.%03u %-6u %-5u %-7u %-7u %u",
				     pc->name, w->detections, (int)(w->score_peak * 1000.0f), mean_x1000,
				     am_rms(&a->run), a->run.clips,
				     w->windows ? (uint32_t)(w->infer_sum_us / w->windows) : 0U,
				     w->infer_max_us, w->deadline_misses);
		} else {
			tt_send_text(TP_TEXT, "%-12s Yes     replay -    -     -     %-6u %-5u -       -       -",
				     pc->name, am_rms(&a->run), a->run.clips);
		}
	}
	tt_send_text(TP_TEXT, "------------------------------------------------------------");
	tt_send_text(TP_TEXT, "Captured audio frames:    %u", ctx.frames);
	tt_send_text(TP_TEXT, "Dropped capture frames:   %u", cs.frames_dropped);
	tt_send_text(TP_TEXT, "Audio packet gaps:        %u (%u frames lost)", gaps, lost);
	tt_send_text(TP_TEXT, "Max queue depth:          %u / %u", cs.queue_high_water, cs.queue_depth);
	tt_send_text(TP_TEXT, "TX ring high water:       %u / %u bytes", ts.ring_high_water, ts.ring_size);
	tt_send_text(TP_TEXT, "Inference deadline misses:%u", misses);
	tt_send_text(TP_TEXT, "Result quality:           %s",
		     (cs.frames_dropped || lost || misses) ? "DEGRADED, see warnings" : "VALID FOR COMPARISON");
	tt_send_text(TP_TEXT, "============================================================");
}

static void finalize(bool replay, const char *reason)
{
	struct audio_capture_stats cs;
	struct tt_stats ts;
	uint32_t gaps = 0, lost = 0, misses = 0;

	strncpy(ctx.stop_reason, reason, sizeof(ctx.stop_reason) - 1);
	audio_capture_get_stats(&cs);
	tt_get_stats(&ts);
	for (unsigned i = 0; i < STREAM_COUNT; i++) {
		gaps += ctx.streams[i].gaps + (ctx.streams[i].pending_gap_frames ? 1U : 0U);
		lost += ctx.streams[i].frames_lost + ctx.streams[i].pending_gap_frames;
	}
	for (int p = 0; p < EVAL_PATH_COUNT; p++) {
		if (ctx.cfg.path[p].enabled) {
			emit_path_metrics((eval_path_id_t)p, true);
			misses += ww_eval_metrics((eval_path_id_t)p)->deadline_misses;
		}
	}
	emit_audio_stats();

	tt_send_json(TP_RUN_END, ctx.run_id, EVAL_STREAM_NONE, 0, ctx.sample_index,
		(replay ? TP_FLAG_REPLAY : 0) | TP_FLAG_FINAL,
		"{\"run_id\":%u,\"replay\":%d,\"replay_path\":%d,\"reason\":\"%s\","
		"\"duration_ms\":%u,\"samples\":%u,\"frames\":%u,\"detections\":%u,"
		"\"cap_dropped\":%u,\"cap_discarded\":%u,\"i2s_err\":%u,\"tx_audio_dropped\":%u,"
		"\"tx_dropped\":%u,\"gaps\":%u,\"frames_lost\":%u,\"deadline_misses\":%u,"
		"\"replay_expected\":%u,\"replay_received\":%u,\"replay_consumed\":%u,"
		"\"replay_dropped\":%u,\"quality\":\"%s\"}",
		ctx.run_id, replay, replay ? (int)ctx.replay_path : -1, reason,
		(unsigned)((uint64_t)ctx.sample_index * 1000U / EVAL_SAMPLE_RATE_HZ),
		ctx.sample_index, ctx.frames, ctx.detections_total, cs.frames_dropped,
		cs.frames_discarded, cs.i2s_errors, ts.audio_dropped, ts.packets_dropped, gaps,
		lost, misses, ctx.replay_expected, ctx.replay_received, ctx.replay_consumed,
		ctx.replay_dropped,
		(cs.frames_dropped || lost || misses || ctx.replay_dropped) ? "degraded" : "valid");

	print_summary(replay);
	atomic_set(&ctx.state, RUN_IDLE);
	audio_capture_set_enqueue(true);
	dk_set_led_off(LED_RUN);
	LOG_INF("%s finished: %s", replay ? "replay" : "run", reason);
}

static void handle_start_marker(void)
{
	ctx.run_id++;
	ww_eval_set_params(ctx.cfg.ww_threshold_x1000, ctx.cfg.ww_cooldown_ms);
	/*
	 * Flush the model first: it takes longer than the frame queue can hold,
	 * so capture overflows while it runs. That audio predates the run, so
	 * discard whatever queued up and only then zero the drop counters;
	 * otherwise every run would start with drops it never experienced.
	 */
	if (ww_eval_bind(ctx.cfg.live_path) != 0) {
		tt_send_text(TP_ERROR, "err: could not bind wake-word runtime");
		return;
	}
	k_msgq_purge(audio_capture_queue());
	reset_run_state();
	ctx.start_uptime_ms = k_uptime_get();
	atomic_set(&ctx.state, RUN_RUNNING);
	dk_set_led_on(LED_RUN);

	char buf[960];

	json_config(buf, sizeof(buf), &ctx.cfg);
	tt_send_json(TP_RUN_START, ctx.run_id, EVAL_STREAM_NONE, 0, 0, 0, "%s", buf);
	tt_send_text(TP_ACK, "ok: run %u started (%s, live path %s)", ctx.run_id,
		     eval_preset_name(ctx.cfg.preset), eval_path_name(ctx.cfg.live_path));
	LOG_INF("run %u started", ctx.run_id);
}

static void handle_replay_start_marker(void)
{
	reset_run_state();
	ctx.replay_received = 0;
	ctx.replay_consumed = 0;
	ctx.replay_dropped = 0;
	ctx.replay_bad = 0;
	if (ww_eval_bind(ctx.replay_path) != 0) {
		tt_send_text(TP_ERROR, "err: could not bind wake-word runtime");
		audio_capture_set_enqueue(true);
		return;
	}
	ctx.start_uptime_ms = k_uptime_get();
	atomic_set(&ctx.state, RUN_REPLAY);
	dk_set_led_on(LED_RUN);
	tt_send_json(TP_RUN_START, ctx.run_id, wire_stream_for_path(ctx.replay_path), 0, 0,
		TP_FLAG_REPLAY, "{\"run_id\":%u,\"replay\":1,\"replay_path\":%d,\"name\":\"%s\","
		"\"expected_frames\":%u,\"threshold_x1000\":%u,\"cooldown_ms\":%u}",
		ctx.run_id, ctx.replay_path, eval_path_name(ctx.replay_path), ctx.replay_expected,
		ww_eval_threshold_x1000(), ww_eval_cooldown_ms());
	tt_send_json(TP_REPLAY_ACK, ctx.run_id, wire_stream_for_path(ctx.replay_path), 0, 0,
		TP_FLAG_REPLAY, "{\"consumed\":0,\"credits\":%u}",
		k_msgq_num_free_get(audio_capture_queue()));
	tt_send_text(TP_ACK, "ok: replay of %s started, expecting %u frames",
		     eval_path_name(ctx.replay_path), ctx.replay_expected);
}

static void eval_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	static struct capture_frame f;

	for (;;) {
		k_msgq_get(audio_capture_queue(), &f, K_FOREVER);

		switch (f.source) {
		case FRAME_SRC_CAPTURE:
			if (atomic_get(&ctx.state) == RUN_RUNNING) {
				process_audio_frame(&f, false);
				if (ctx.cfg.duration_s &&
				    ctx.sample_index >= (uint32_t)ctx.cfg.duration_s * EVAL_SAMPLE_RATE_HZ) {
					finalize(false, "duration");
				}
			} else if (atomic_get(&ctx.state) == RUN_IDLE) {
				/* Idle level monitoring so the dashboard can check mic health. */
				am_update(&ctx.raw_m[0], f.left, f.count);
				am_update(&ctx.raw_m[1], f.right, f.count);
				if (++ctx.frames_since_period >= PERIOD_FRAMES) {
					ctx.frames_since_period = 0;
					if (tt_binary_mode()) {
						emit_audio_stats();
					} else if (atomic_get(&levels_mode)) {
						text_levels_line("levels  ");
						atomic_cas(&levels_mode, 1, 0);
					} else {
						/* keep the window short so 'levels' reports the last second */
						struct am_accum discard;

						am_window_flush(&ctx.raw_m[0], &discard);
						am_window_flush(&ctx.raw_m[1], &discard);
					}
				}
			}
			break;
		case FRAME_SRC_REPLAY:
			if (atomic_get(&ctx.state) != RUN_REPLAY) {
				ctx.replay_bad++;
				break;
			}
			process_audio_frame(&f, true);
			ctx.replay_consumed++;
			if ((ctx.replay_consumed % REPLAY_ACK_EVERY) == 0 ||
			    ctx.replay_consumed == ctx.replay_expected) {
				tt_send_json(TP_REPLAY_ACK, ctx.run_id,
					wire_stream_for_path(ctx.replay_path), ctx.replay_consumed,
					ctx.sample_index, TP_FLAG_REPLAY,
					"{\"consumed\":%u,\"credits\":%u}", ctx.replay_consumed,
					k_msgq_num_free_get(audio_capture_queue()));
			}
			break;
		case FRAME_SRC_START:
			if (atomic_get(&ctx.state) == RUN_IDLE) {
				handle_start_marker();
			}
			break;
		case FRAME_SRC_STOP:
			if (atomic_get(&ctx.state) == RUN_RUNNING) {
				finalize(false, (const char *)f.left);
			} else if (atomic_get(&ctx.state) == RUN_REPLAY) {
				finalize(true, (const char *)f.left);
			}
			break;
		case FRAME_SRC_REPLAY_START:
			if (atomic_get(&ctx.state) == RUN_IDLE) {
				handle_replay_start_marker();
			}
			break;
		case FRAME_SRC_REPLAY_END:
			if (atomic_get(&ctx.state) == RUN_REPLAY) {
				finalize(true, "replay_end");
			}
			break;
		default:
			break;
		}
	}
}

/* ------------------------------------------------------- commands */

static int post_marker(uint8_t source, const char *text)
{
	static struct capture_frame m;   /* markers are rare; copied by value */

	memset(&m, 0, sizeof(m));
	m.source = source;
	if (text) {
		strncpy((char *)m.left, text, sizeof(m.left) - 1);
	}
	return k_msgq_put(audio_capture_queue(), &m, K_MSEC(200)) == 0 ? 0 : -EAGAIN;
}

int run_controller_init(void)
{
	memset(&ctx, 0, sizeof(ctx));
	eval_run_config_defaults(&ctx.cfg);
	eval_paths_init(&ctx.ep, &ctx.cfg);
	eval_paths_apply_config(&ctx.ep);
	atomic_set(&ctx.state, RUN_IDLE);
	for (int p = 0; p < EVAL_PATH_COUNT; p++) {
		am_reset(&ctx.path_m[p]);
	}
	for (int i = 0; i < 3; i++) {
		am_reset(&ctx.raw_m[i]);
	}
	k_thread_create(&eval_thread_data, eval_stack, K_THREAD_STACK_SIZEOF(eval_stack),
			eval_thread, NULL, NULL, NULL, 7, 0, K_NO_WAIT);
	k_thread_name_set(&eval_thread_data, "eval");
	return 0;
}

eval_run_config_t *run_controller_config(void)
{
	return &ctx.cfg;
}

bool run_controller_config_mutable(void)
{
	return atomic_get(&ctx.state) == RUN_IDLE;
}

enum run_state run_controller_state(void)
{
	return (enum run_state)atomic_get(&ctx.state);
}

uint16_t run_controller_run_id(void)
{
	return ctx.run_id;
}

int run_controller_start(char *err, size_t err_len)
{
	if (atomic_get(&ctx.state) != RUN_IDLE) {
		snprintf(err, err_len, "not idle");
		return -EBUSY;
	}
	if (eval_run_config_enabled_mask(&ctx.cfg) == 0U) {
		snprintf(err, err_len, "no path enabled");
		return -EINVAL;
	}
	if (!ctx.cfg.path[ctx.cfg.live_path].enabled) {
		snprintf(err, err_len, "live path %s is not enabled", eval_path_name(ctx.cfg.live_path));
		return -EINVAL;
	}
	if (run_controller_admission_check(&ctx.cfg, err, err_len, NULL) != 0) {
		return -EINVAL;
	}
	if (eval_paths_apply_config(&ctx.ep) != 0) {
		snprintf(err, err_len, "beamformer delay out of range (max %d)", (int)EVAL_MAX_DELAY_SAMPLES);
		return -EINVAL;
	}
	return post_marker(FRAME_SRC_START, NULL);
}

int run_controller_stop(const char *reason)
{
	if (atomic_get(&ctx.state) == RUN_IDLE) {
		return -EALREADY;
	}
	return post_marker(FRAME_SRC_STOP, reason ? reason : "stop");
}

void run_controller_reset(void)
{
	if (!run_controller_config_mutable()) {
		return;
	}
	eval_run_config_defaults(&ctx.cfg);
	eval_paths_apply_config(&ctx.ep);
	eval_paths_reset_state(&ctx.ep);
	for (int p = 0; p < EVAL_PATH_COUNT; p++) {
		ww_eval_reset_path((eval_path_id_t)p);
		am_reset(&ctx.path_m[p]);
	}
}

int run_controller_replay_start(eval_path_id_t path, uint32_t total_frames, char *err,
				size_t err_len)
{
	if (atomic_get(&ctx.state) != RUN_IDLE) {
		snprintf(err, err_len, "not idle");
		return -EBUSY;
	}
	if (path >= EVAL_PATH_COUNT) {
		snprintf(err, err_len, "unknown path");
		return -EINVAL;
	}
	if (!tt_binary_mode()) {
		snprintf(err, err_len, "replay needs binary mode");
		return -EINVAL;
	}
	if (eval_paths_apply_config(&ctx.ep) != 0) {
		snprintf(err, err_len, "beamformer delay out of range");
		return -EINVAL;
	}
	ctx.cfg.path[path].enabled = true;
	ctx.replay_path = path;
	ctx.replay_expected = total_frames;
	audio_capture_set_enqueue(false);   /* only replay frames from here on */
	return post_marker(FRAME_SRC_REPLAY_START, NULL);
}

int run_controller_replay_end(void)
{
	if (atomic_get(&ctx.state) != RUN_REPLAY) {
		return -EALREADY;
	}
	return post_marker(FRAME_SRC_REPLAY_END, NULL);
}

void run_controller_on_packet(const struct tp_header *h, const uint8_t *payload)
{
	if (h->type == TP_REPLAY_END) {
		(void)run_controller_replay_end();
		return;
	}
	if (h->type != TP_REPLAY_AUDIO) {
		return;
	}
	if (atomic_get(&ctx.state) != RUN_REPLAY) {
		ctx.replay_bad++;
		return;
	}

	static struct capture_frame f;
	const uint8_t channels = 2U;
	const uint16_t n = (uint16_t)(h->payload_len / (channels * sizeof(int16_t)));

	if (n == 0U || n > EVAL_FRAME_SAMPLES) {
		ctx.replay_bad++;
		return;
	}
	const int16_t *pcm = (const int16_t *)payload;

	for (uint16_t i = 0; i < n; i++) {
		f.left[i] = pcm[2 * i];
		f.right[i] = pcm[2 * i + 1];
	}
	f.seq = h->timestamp;
	f.source = FRAME_SRC_REPLAY;
	f.channels = channels;
	f.count = n;
	ctx.replay_received++;
	if (k_msgq_put(audio_capture_queue(), &f, K_NO_WAIT) != 0) {
		ctx.replay_dropped++;
	}
}

void run_controller_tick(void)
{
	int64_t now = k_uptime_get();

	if (led_event_off_ms && now >= led_event_off_ms) {
		dk_set_led_off(LED_EVENT);
		led_event_off_ms = 0;
	}
	if (atomic_get(&ctx.state) == RUN_IDLE && now >= idle_status_next_ms) {
		idle_status_next_ms = now + CONFIG_EVAL_STATUS_PERIOD_MS;
		if (tt_binary_mode()) {
			run_controller_emit_status();
		}
	}
}

void run_controller_print_paths(void)
{
	tt_send_text(TP_TEXT, "id name        enabled capture ww   dl   dr   dc   gain    live");
	for (int p = 0; p < EVAL_PATH_COUNT; p++) {
		const eval_path_config_t *pc = &ctx.cfg.path[p];

		tt_send_text(TP_TEXT, "%-2d %-11s %-7s %-7s %-4s %-4d %-4d %-4d %-7d %s", p, pc->name,
			     pc->enabled ? "yes" : "no", pc->capture_audio ? "yes" : "no",
			     pc->run_wakeword ? "yes" : "no", pc->left_delay_samples,
			     pc->right_delay_samples, pc->center_delay_samples, pc->gain_q15,
			     (p == ctx.cfg.live_path) ? "*" : "");
	}
	tt_send_text(TP_TEXT, "raw capture: left=%d right=%d center=%d mode=%s seconds=%u; "
		     "reduce=%s; preset=%s; duration=%us; threshold=%u; cooldown=%u ms",
		     ctx.cfg.capture_raw_left, ctx.cfg.capture_raw_right, ctx.cfg.capture_raw_center,
		     eval_capture_mode_name(ctx.cfg.capture_mode), ctx.cfg.capture_seconds,
		     eval_reduce_mode_name(ctx.cfg.raw_3ch_reduce), eval_preset_name(ctx.cfg.preset),
		     ctx.cfg.duration_s, ctx.cfg.ww_threshold_x1000, ctx.cfg.ww_cooldown_ms);
}
