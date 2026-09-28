#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <nrf_edgeai/nrf_edgeai.h>

#include "wakeword_eval.h"
#include "nrf_edgeai_generated/nrf_edgeai_user_model.h"

LOG_MODULE_REGISTER(ww_eval, LOG_LEVEL_INF);

#ifndef CONFIG_WW_HISTORY_SIZE
#define CONFIG_WW_HISTORY_SIZE 6
#endif
#ifndef CONFIG_WW_COUNT_THRESHOLD
#define CONFIG_WW_COUNT_THRESHOLD 4
#endif
#ifndef CONFIG_WW_PROBABILITY_THRESHOLD
#define CONFIG_WW_PROBABILITY_THRESHOLD 700
#endif
#ifndef CONFIG_WW_COOLDOWN_MS
#define CONFIG_WW_COOLDOWN_MS 1200
#endif
#ifndef CONFIG_EVAL_INFERENCE_DEADLINE_US
#define CONFIG_EVAL_INFERENCE_DEADLINE_US 10000
#endif

BUILD_ASSERT(CONFIG_WW_HISTORY_SIZE <= 32, "WW_HISTORY_SIZE must fit in uint32_t");

#define FLUSH_WINDOWS 100   /* 1 s of silence through the runtime at every bind */
#define NO_DETECTION  0xFFFFFFFFU

struct pp_state {
	uint32_t count;
	uint32_t history;
	uint32_t last_detect_sample;
	bool has_detection;
};

static nrf_edgeai_t *model;
static uint16_t window_samples;
static int16_t window_buf[512];
static uint16_t window_fill;
static uint32_t window_start_sample;
static eval_path_id_t bound = EVAL_PATH_COUNT;
static uint16_t threshold_x1000 = CONFIG_WW_PROBABILITY_THRESHOLD;
static uint16_t cooldown_ms = CONFIG_WW_COOLDOWN_MS;
static struct pp_state pp[EVAL_PATH_COUNT];
static struct ww_path_metrics metrics[EVAL_PATH_COUNT];

int ww_eval_init(void)
{
	model = nrf_edgeai_user_model();
	if (model == NULL) {
		return -ENODEV;
	}

	nrf_edgeai_err_t err = nrf_edgeai_init(model);

	if (err) {
		LOG_ERR("Model initialization failed (err %d)", err);
		return -ENOENT;
	}

	window_samples = nrf_edgeai_input_window_size(model);
	if (window_samples == 0U || window_samples > ARRAY_SIZE(window_buf)) {
		return -EINVAL;
	}
	for (int p = 0; p < EVAL_PATH_COUNT; p++) {
		ww_eval_reset_path((eval_path_id_t)p);
	}
	LOG_INF("wake word: window=%u samples, threshold=%u/1000, vote %d of %d, cooldown %u ms",
		window_samples, threshold_x1000, CONFIG_WW_COUNT_THRESHOLD, CONFIG_WW_HISTORY_SIZE,
		cooldown_ms);
	return 0;
}

void ww_eval_set_params(uint16_t th, uint16_t cd)
{
	threshold_x1000 = MIN(th, 1000U);
	cooldown_ms = cd;
}

void ww_eval_reset_path(eval_path_id_t path)
{
	if (path >= EVAL_PATH_COUNT) {
		return;
	}
	memset(&pp[path], 0, sizeof(pp[path]));
	pp[path].last_detect_sample = NO_DETECTION;
	memset(&metrics[path], 0, sizeof(metrics[path]));
	metrics[path].score_min = 1.0f;
	metrics[path].last_detection_sample = NO_DETECTION;
}

/* Feed one window and run inference. Returns inference time in us, or a negative errno. */
static int run_window(const int16_t *win, float *score)
{
	uint32_t t0 = k_cycle_get_32();
	nrf_edgeai_err_t err = nrf_edgeai_feed_inputs(model, (void *)win, window_samples);

	if (err == NRF_EDGEAI_ERR_INPROGRESS) {
		return -EBUSY;
	}
	if (err) {
		return -EIO;
	}
	err = nrf_edgeai_run_inference(model);
	if (err == NRF_EDGEAI_ERR_INPROGRESS) {
		return -EBUSY;
	}
	if (err) {
		return -EIO;
	}
	*score = model->decoded_output.classif.probabilities.p_f32[0];
	return (int)k_cyc_to_us_floor32(k_cycle_get_32() - t0);
}

int ww_eval_bind(eval_path_id_t path)
{
	static const int16_t silence[512];
	float score;

	if (model == NULL || path >= EVAL_PATH_COUNT) {
		return -EINVAL;
	}
	bound = path;
	window_fill = 0;
	memset(&pp[path], 0, sizeof(pp[path]));
	pp[path].last_detect_sample = NO_DETECTION;

	/* Flush the runtime's front-end and history with silence. */
	for (int i = 0; i < FLUSH_WINDOWS; i++) {
		int rc = run_window(silence, &score);

		if (rc == -EBUSY) {
			k_sleep(K_MSEC(2));
			i--;
			continue;
		}
		if (rc < 0) {
			LOG_ERR("runtime flush failed: %d", rc);
			return rc;
		}
	}
	LOG_INF("wake word bound to %s", eval_path_name(path));
	return 0;
}

eval_path_id_t ww_eval_bound_path(void)
{
	return bound;
}

static void postprocess(eval_path_id_t path, float score, uint32_t window_end_sample,
			ww_event_cb_t cb, void *user)
{
	struct pp_state *s = &pp[path];
	struct ww_path_metrics *m = &metrics[path];
	const bool positive = (score * 1000.0f) > (float)threshold_x1000;
	const bool oldest = (s->history >> (CONFIG_WW_HISTORY_SIZE - 1)) & 1U;

	m->windows++;
	m->score_last = score;
	m->score_sum += score;
	m->score_sumsq += score * score;
	if (score > m->score_peak) {
		m->score_peak = score;
	}
	if (score < m->score_min) {
		m->score_min = score;
	}
	m->last_window_sample = window_end_sample;

	s->count = s->count + (positive ? 1U : 0U) - (oldest ? 1U : 0U);
	s->history = (s->history << 1) | (positive ? 1U : 0U);

	if (s->count < CONFIG_WW_COUNT_THRESHOLD) {
		return;
	}
	s->count = 0;
	s->history = 0;

	uint32_t cooldown_samples = (uint32_t)cooldown_ms * (EVAL_SAMPLE_RATE_HZ / 1000U);

	if (s->has_detection && (window_end_sample - s->last_detect_sample) < cooldown_samples) {
		m->cooldown_suppressed++;
		return;
	}
	s->has_detection = true;
	s->last_detect_sample = window_end_sample;
	m->detections++;
	m->last_detection_sample = window_end_sample;
	if (cb) {
		cb(path, window_end_sample, score, m->score_peak, user);
	}
}

int ww_eval_process(const int16_t *samples, uint16_t n, uint32_t sample_index, ww_event_cb_t cb,
		    void *user)
{
	if (model == NULL || bound >= EVAL_PATH_COUNT) {
		return -EINVAL;
	}

	struct ww_path_metrics *m = &metrics[bound];
	uint16_t consumed = 0;
	int result = 0;

	m->frames++;
	m->samples += n;

	while (consumed < n) {
		if (window_fill == 0) {
			window_start_sample = sample_index + consumed;
		}
		uint16_t space = window_samples - window_fill;
		uint16_t copy = MIN((uint16_t)(n - consumed), space);

		memcpy(&window_buf[window_fill], &samples[consumed], copy * sizeof(int16_t));
		window_fill += copy;
		consumed += copy;

		if (window_fill < window_samples) {
			continue;
		}
		window_fill = 0;

		float score = 0.0f;
		int us = run_window(window_buf, &score);

		if (us == -EBUSY) {
			/* The runtime is still collecting input for its next inference. */
			continue;
		}
		if (us < 0) {
			m->errors++;
			result = us;
			continue;
		}
		m->infer_last_us = (uint32_t)us;
		m->infer_sum_us += (uint32_t)us;
		if ((uint32_t)us > m->infer_max_us) {
			m->infer_max_us = (uint32_t)us;
		}
		if ((uint32_t)us > CONFIG_EVAL_INFERENCE_DEADLINE_US) {
			m->deadline_misses++;
		}
		postprocess(bound, score, window_start_sample + window_samples, cb, user);
	}
	return result;
}

const struct ww_path_metrics *ww_eval_metrics(eval_path_id_t path)
{
	return (path < EVAL_PATH_COUNT) ? &metrics[path] : NULL;
}

uint16_t ww_eval_window_samples(void)
{
	return window_samples;
}

uint16_t ww_eval_threshold_x1000(void)
{
	return threshold_x1000;
}

uint16_t ww_eval_cooldown_ms(void)
{
	return cooldown_ms;
}

const char *ww_eval_model_id(void)
{
	return "94081";
}
