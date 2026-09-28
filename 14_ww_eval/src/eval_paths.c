#include <string.h>

#include "eval_paths.h"

#ifndef CONFIG_EVAL_BF_LR_LEFT_DELAY
#define CONFIG_EVAL_BF_LR_LEFT_DELAY 0
#endif
#ifndef CONFIG_EVAL_BF_LR_RIGHT_DELAY
#define CONFIG_EVAL_BF_LR_RIGHT_DELAY 0
#endif
#ifndef CONFIG_EVAL_BF_GAIN_Q15
#define CONFIG_EVAL_BF_GAIN_Q15 32767
#endif
#ifndef CONFIG_EVAL_DEFAULT_LIVE_PATH
#define CONFIG_EVAL_DEFAULT_LIVE_PATH 3
#endif
#ifndef CONFIG_EVAL_DEFAULT_DURATION_S
#define CONFIG_EVAL_DEFAULT_DURATION_S 30
#endif
#ifndef CONFIG_EVAL_DEFAULT_PRESET
#define CONFIG_EVAL_DEFAULT_PRESET "raw_vs_bf_lr"
#endif
#ifndef CONFIG_WW_PROBABILITY_THRESHOLD
#define CONFIG_WW_PROBABILITY_THRESHOLD 700
#endif
#ifndef CONFIG_WW_COOLDOWN_MS
#define CONFIG_WW_COOLDOWN_MS 1200
#endif

/* ------------------------------------------------------------------ names */

static const char *const path_names[EVAL_PATH_COUNT] = {
	[EVAL_PATH_RAW_LEFT] = "raw_left",
	[EVAL_PATH_RAW_RIGHT] = "raw_right",
	[EVAL_PATH_RAW_LR_MIX] = "raw_lr_mix",
	[EVAL_PATH_BF_LR] = "bf_lr",
	[EVAL_PATH_RAW_3CH] = "raw_3ch",
	[EVAL_PATH_BF_3CH] = "bf_3ch",
};

static const char *const preset_names[EVAL_PRESET_COUNT] = {
	[EVAL_PRESET_LEFT] = "left",
	[EVAL_PRESET_RIGHT] = "right",
	[EVAL_PRESET_LR_MIX] = "lr_mix",
	[EVAL_PRESET_BF_LR] = "bf_lr",
	[EVAL_PRESET_RAW_3CH] = "raw_3ch",
	[EVAL_PRESET_BF_3CH] = "bf_3ch",
	[EVAL_PRESET_RAW_VS_BF_LR] = "raw_vs_bf_lr",
	[EVAL_PRESET_RAW_VS_BF_3CH] = "raw_vs_bf_3ch",
	[EVAL_PRESET_FULL] = "full",
};

static const char *const capture_mode_names[EVAL_CAPTURE_MODE_COUNT] = {
	[EVAL_CAPTURE_OFF] = "off",
	[EVAL_CAPTURE_EVENT_WINDOW] = "event_window",
	[EVAL_CAPTURE_CONTINUOUS_SHORT] = "continuous_short",
	[EVAL_CAPTURE_CONTINUOUS_FULL] = "continuous_full",
};

static const char *const reduce_names[RAW_3CH_REDUCE_COUNT] = {
	[RAW_3CH_CENTER_ONLY] = "center_only",
	[RAW_3CH_LR_AVERAGE] = "lr_average",
	[RAW_3CH_LC_AVERAGE] = "lc_average",
	[RAW_3CH_RC_AVERAGE] = "rc_average",
	[RAW_3CH_LRC_AVERAGE] = "lrc_average",
};

const char *eval_path_name(eval_path_id_t id)
{
	return (id < EVAL_PATH_COUNT) ? path_names[id] : "?";
}

const char *eval_preset_name(eval_preset_t p)
{
	return (p < EVAL_PRESET_COUNT) ? preset_names[p] : "?";
}

const char *eval_capture_mode_name(eval_capture_mode_t m)
{
	return (m < EVAL_CAPTURE_MODE_COUNT) ? capture_mode_names[m] : "?";
}

const char *eval_reduce_mode_name(raw_3ch_reduce_mode_t m)
{
	return (m < RAW_3CH_REDUCE_COUNT) ? reduce_names[m] : "?";
}

static int lookup(const char *const *names, int count, const char *name)
{
	for (int i = 0; i < count; i++) {
		if (strcmp(names[i], name) == 0) {
			return i;
		}
	}
	return -1;
}

int eval_path_from_name(const char *name)
{
	return lookup(path_names, EVAL_PATH_COUNT, name);
}

int eval_preset_from_name(const char *name)
{
	return lookup(preset_names, EVAL_PRESET_COUNT, name);
}

int eval_capture_mode_from_name(const char *name)
{
	return lookup(capture_mode_names, EVAL_CAPTURE_MODE_COUNT, name);
}

/* --------------------------------------------------------------- config */

void eval_run_config_defaults(eval_run_config_t *cfg)
{
	memset(cfg, 0, sizeof(*cfg));

	for (int i = 0; i < EVAL_PATH_COUNT; i++) {
		eval_path_config_t *p = &cfg->path[i];

		p->id = (eval_path_id_t)i;
		p->name = path_names[i];
		p->enabled = false;
		p->capture_audio = false;
		p->run_wakeword = true;
		p->gain_q15 = INT16_MAX;
	}
	cfg->path[EVAL_PATH_BF_LR].left_delay_samples = CONFIG_EVAL_BF_LR_LEFT_DELAY;
	cfg->path[EVAL_PATH_BF_LR].right_delay_samples = CONFIG_EVAL_BF_LR_RIGHT_DELAY;
	cfg->path[EVAL_PATH_BF_LR].gain_q15 = CONFIG_EVAL_BF_GAIN_Q15;
	cfg->path[EVAL_PATH_BF_3CH].gain_q15 = CONFIG_EVAL_BF_GAIN_Q15;

	cfg->live_path = (eval_path_id_t)CONFIG_EVAL_DEFAULT_LIVE_PATH;
	cfg->raw_3ch_reduce = RAW_3CH_LRC_AVERAGE;
	cfg->capture_mode = EVAL_CAPTURE_EVENT_WINDOW;
	cfg->capture_seconds = 2;
	cfg->capture_raw_left = true;
	cfg->capture_raw_right = true;
	cfg->capture_raw_center = false;
	cfg->duration_s = CONFIG_EVAL_DEFAULT_DURATION_S;
	cfg->ww_threshold_x1000 = CONFIG_WW_PROBABILITY_THRESHOLD;
	cfg->ww_cooldown_ms = CONFIG_WW_COOLDOWN_MS;
	cfg->angle_deg = 0;
	cfg->distance_cm = 0;
	strncpy(cfg->label, "unlabeled", sizeof(cfg->label) - 1);
	strncpy(cfg->phrase, "hey vision", sizeof(cfg->phrase) - 1);
	strncpy(cfg->environment, "unspecified", sizeof(cfg->environment) - 1);

	int preset = eval_preset_from_name(CONFIG_EVAL_DEFAULT_PRESET);

	eval_run_config_apply_preset(cfg, preset < 0 ? EVAL_PRESET_RAW_VS_BF_LR : (eval_preset_t)preset);
}

void eval_run_config_apply_preset(eval_run_config_t *cfg, eval_preset_t preset)
{
	uint32_t mask;

	switch (preset) {
	case EVAL_PRESET_LEFT:
		mask = 1U << EVAL_PATH_RAW_LEFT;
		break;
	case EVAL_PRESET_RIGHT:
		mask = 1U << EVAL_PATH_RAW_RIGHT;
		break;
	case EVAL_PRESET_LR_MIX:
		mask = 1U << EVAL_PATH_RAW_LR_MIX;
		break;
	case EVAL_PRESET_BF_LR:
		mask = 1U << EVAL_PATH_BF_LR;
		break;
	case EVAL_PRESET_RAW_3CH:
		mask = 1U << EVAL_PATH_RAW_3CH;
		break;
	case EVAL_PRESET_BF_3CH:
		mask = 1U << EVAL_PATH_BF_3CH;
		break;
	case EVAL_PRESET_RAW_VS_BF_3CH:
		mask = (1U << EVAL_PATH_RAW_3CH) | (1U << EVAL_PATH_BF_3CH);
		break;
	case EVAL_PRESET_FULL:
		mask = (1U << EVAL_PATH_COUNT) - 1U;
		break;
	case EVAL_PRESET_RAW_VS_BF_LR:
	default:
		preset = EVAL_PRESET_RAW_VS_BF_LR;
		mask = (1U << EVAL_PATH_RAW_LEFT) | (1U << EVAL_PATH_RAW_RIGHT) |
		       (1U << EVAL_PATH_RAW_LR_MIX) | (1U << EVAL_PATH_BF_LR);
		break;
	}

	cfg->preset = preset;
	for (int i = 0; i < EVAL_PATH_COUNT; i++) {
		cfg->path[i].enabled = (mask >> i) & 1U;
	}

	/* Keep the live path on an enabled path so a run always scores something. */
	if (!cfg->path[cfg->live_path].enabled) {
		for (int i = EVAL_PATH_COUNT - 1; i >= 0; i--) {
			if (cfg->path[i].enabled) {
				cfg->live_path = (eval_path_id_t)i;
				break;
			}
		}
	}
}

uint32_t eval_run_config_enabled_mask(const eval_run_config_t *cfg)
{
	uint32_t m = 0;

	for (int i = 0; i < EVAL_PATH_COUNT; i++) {
		if (cfg->path[i].enabled) {
			m |= 1U << i;
		}
	}
	return m;
}

/* ---------------------------------------------------------------- paths */

void eval_paths_init(struct eval_paths *ep, const eval_run_config_t *cfg)
{
	memset(ep, 0, sizeof(*ep));
	ep->cfg = cfg;
	bf_init(&ep->bf_lr, 2);
	bf_init(&ep->bf_3ch, 3);
}

int eval_paths_apply_config(struct eval_paths *ep)
{
	const eval_path_config_t *lr = &ep->cfg->path[EVAL_PATH_BF_LR];
	const eval_path_config_t *c3 = &ep->cfg->path[EVAL_PATH_BF_3CH];
	int16_t d2[2] = { lr->left_delay_samples, lr->right_delay_samples };
	int16_t g2[2] = { lr->gain_q15, lr->gain_q15 };
	int16_t d3[3] = { c3->left_delay_samples, c3->right_delay_samples, c3->center_delay_samples };
	int16_t g3[3] = { c3->gain_q15, c3->gain_q15, c3->gain_q15 };

	if (bf_set_delays(&ep->bf_lr, d2, 2) || bf_set_gains(&ep->bf_lr, g2, 2)) {
		return -1;
	}
	if (bf_set_delays(&ep->bf_3ch, d3, 3) || bf_set_gains(&ep->bf_3ch, g3, 3)) {
		return -1;
	}
	return 0;
}

void eval_paths_reset_state(struct eval_paths *ep)
{
	bf_reset_history(&ep->bf_lr);
	bf_reset_history(&ep->bf_3ch);
	bf_reset_stats(&ep->bf_lr);
	bf_reset_stats(&ep->bf_3ch);
	ep->produced_mask = 0;
	ep->unavailable_mask = 0;
}

static inline int16_t avg2(int16_t a, int16_t b)
{
	/* Each half fits in int16, so the sum cannot overflow. */
	return (int16_t)((a / 2) + (b / 2));
}

static inline int16_t avg3(int16_t a, int16_t b, int16_t c)
{
	return (int16_t)((a / 3) + (b / 3) + (c / 3));
}

static void reduce_3ch(const struct eval_paths *ep, const struct eval_frame *f, int16_t *out)
{
	const uint16_t n = f->count;

	switch (ep->cfg->raw_3ch_reduce) {
	case RAW_3CH_CENTER_ONLY:
		memcpy(out, f->center, n * sizeof(int16_t));
		break;
	case RAW_3CH_LR_AVERAGE:
		for (uint16_t i = 0; i < n; i++) {
			out[i] = avg2(f->left[i], f->right[i]);
		}
		break;
	case RAW_3CH_LC_AVERAGE:
		for (uint16_t i = 0; i < n; i++) {
			out[i] = avg2(f->left[i], f->center[i]);
		}
		break;
	case RAW_3CH_RC_AVERAGE:
		for (uint16_t i = 0; i < n; i++) {
			out[i] = avg2(f->right[i], f->center[i]);
		}
		break;
	case RAW_3CH_LRC_AVERAGE:
	default:
		for (uint16_t i = 0; i < n; i++) {
			out[i] = avg3(f->left[i], f->right[i], f->center[i]);
		}
		break;
	}
}

uint32_t eval_paths_process(struct eval_paths *ep, const struct eval_frame *f)
{
	const eval_run_config_t *cfg = ep->cfg;
	const uint16_t n = (f->count > EVAL_FRAME_SAMPLES) ? EVAL_FRAME_SAMPLES : f->count;
	uint32_t produced = 0;
	uint32_t unavailable = 0;

	if (cfg->path[EVAL_PATH_RAW_LEFT].enabled) {
		memcpy(ep->out[EVAL_PATH_RAW_LEFT], f->left, n * sizeof(int16_t));
		produced |= 1U << EVAL_PATH_RAW_LEFT;
	}
	if (cfg->path[EVAL_PATH_RAW_RIGHT].enabled) {
		memcpy(ep->out[EVAL_PATH_RAW_RIGHT], f->right, n * sizeof(int16_t));
		produced |= 1U << EVAL_PATH_RAW_RIGHT;
	}
	if (cfg->path[EVAL_PATH_RAW_LR_MIX].enabled) {
		int16_t *o = ep->out[EVAL_PATH_RAW_LR_MIX];

		for (uint16_t i = 0; i < n; i++) {
			o[i] = avg2(f->left[i], f->right[i]);
		}
		produced |= 1U << EVAL_PATH_RAW_LR_MIX;
	}
	if (cfg->path[EVAL_PATH_BF_LR].enabled) {
		const int16_t *in[2] = { f->left, f->right };

		bf_process(&ep->bf_lr, in, n, ep->out[EVAL_PATH_BF_LR]);
		produced |= 1U << EVAL_PATH_BF_LR;
	}
	if (cfg->path[EVAL_PATH_RAW_3CH].enabled) {
		if (f->center) {
			reduce_3ch(ep, f, ep->out[EVAL_PATH_RAW_3CH]);
			produced |= 1U << EVAL_PATH_RAW_3CH;
		} else {
			unavailable |= 1U << EVAL_PATH_RAW_3CH;
		}
	}
	if (cfg->path[EVAL_PATH_BF_3CH].enabled) {
		if (f->center) {
			const int16_t *in[3] = { f->left, f->right, f->center };

			bf_process(&ep->bf_3ch, in, n, ep->out[EVAL_PATH_BF_3CH]);
			produced |= 1U << EVAL_PATH_BF_3CH;
		} else {
			unavailable |= 1U << EVAL_PATH_BF_3CH;
		}
	}

	ep->produced_mask = produced;
	ep->unavailable_mask = unavailable;
	return produced;
}
