/*
 * Wake-word scoring for the evaluation paths.
 *
 * The nRF Edge AI runtime is one global, stateful instance: its audio
 * front end keeps FFT overlap and noise-tracking state between calls, and the
 * library exposes no way to save, restore, or duplicate that state. Feeding
 * frames from two different paths into it would mix their histories, so the
 * runtime scores exactly one path at a time:
 *
 *   - during a live run, the configured live path;
 *   - during a replay pass, the path the host asked for.
 *
 * Every path gets its own post-processing state (vote history, cooldown) and
 * its own metrics, and the runtime is flushed with one second of digital
 * silence at the start of every pass so nothing carries over.
 *
 * Model, preprocessing, window size, stride, threshold, cooldown, and vote
 * parameters are identical for all paths by construction: there is only one
 * of each.
 */
#ifndef WAKEWORD_EVAL_H_
#define WAKEWORD_EVAL_H_

#include <stdbool.h>
#include <stdint.h>

#include "eval_config.h"

struct ww_path_metrics {
	uint32_t frames;
	uint32_t samples;
	uint32_t windows;
	uint32_t detections;
	uint32_t cooldown_suppressed;
	uint32_t deadline_misses;
	uint32_t errors;
	float score_last;
	float score_peak;
	float score_min;
	float score_sum;
	float score_sumsq;
	uint32_t last_detection_sample;   /* sample index, 0xFFFFFFFF if none */
	uint32_t last_window_sample;      /* sample index at the end of the last window */
	uint32_t infer_last_us;
	uint32_t infer_max_us;
	uint64_t infer_sum_us;
};

typedef void (*ww_event_cb_t)(eval_path_id_t path, uint32_t sample_index, float score,
			      float peak_score, void *user);

int ww_eval_init(void);

/* Threshold (x1000) and cooldown apply to every path. Takes effect on next bind. */
void ww_eval_set_params(uint16_t threshold_x1000, uint16_t cooldown_ms);

/* Clears one path's post-processing and metrics. */
void ww_eval_reset_path(eval_path_id_t path);

/*
 * Selects the path that owns the runtime from now on, resets that path's
 * post-processing and flushes the runtime with silence. Blocks for the
 * flush (about 100 inference windows).
 */
int ww_eval_bind(eval_path_id_t path);
eval_path_id_t ww_eval_bound_path(void);

/*
 * Feeds one frame of the bound path. sample_index is the run-relative index
 * of samples[0]. Detections are reported through cb with the sample index of
 * the window that confirmed them. Returns 0, or a negative errno. Windows the
 * runtime only buffers (NRF_EDGEAI_ERR_INPROGRESS) are not scored.
 */
int ww_eval_process(const int16_t *samples, uint16_t n, uint32_t sample_index,
		    ww_event_cb_t cb, void *user);

const struct ww_path_metrics *ww_eval_metrics(eval_path_id_t path);

uint16_t ww_eval_window_samples(void);
uint16_t ww_eval_threshold_x1000(void);
uint16_t ww_eval_cooldown_ms(void);
const char *ww_eval_model_id(void);

#endif /* WAKEWORD_EVAL_H_ */
