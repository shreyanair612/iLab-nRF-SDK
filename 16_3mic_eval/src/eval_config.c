#include <string.h>

#include "eval_config.h"

#ifndef CONFIG_EVAL_DEFAULT_DURATION_S
#define CONFIG_EVAL_DEFAULT_DURATION_S 20
#endif
#ifndef CONFIG_EVAL_BF_LEFT_DELAY
#define CONFIG_EVAL_BF_LEFT_DELAY 0
#endif
#ifndef CONFIG_EVAL_BF_CENTER_DELAY
#define CONFIG_EVAL_BF_CENTER_DELAY 0
#endif
#ifndef CONFIG_EVAL_BF_RIGHT_DELAY
#define CONFIG_EVAL_BF_RIGHT_DELAY 0
#endif
#ifndef CONFIG_EVAL_BF_GAIN_Q15
#define CONFIG_EVAL_BF_GAIN_Q15 32767
#endif

static const char *const mic_names[EVAL_MIC_COUNT] = { "left", "center", "right" };

const char *eval_mic_name(eval_mic_t m)
{
	return ((unsigned)m < EVAL_MIC_COUNT) ? mic_names[m] : "?";
}

const char *eval_stream_name(uint16_t stream_id)
{
	switch (stream_id) {
	case EVAL_STREAM_RAW_LEFT: return "raw_left";
	case EVAL_STREAM_RAW_CENTER: return "raw_center";
	case EVAL_STREAM_RAW_RIGHT: return "raw_right";
	case EVAL_STREAM_BF: return "bf";
	default: return "?";
	}
}

int eval_mic_from_name(const char *name)
{
	for (int i = 0; i < (int)EVAL_MIC_COUNT; i++) {
		if (strcmp(name, mic_names[i]) == 0) {
			return i;
		}
	}
	/* Accept the single-letter forms too. */
	if (strcmp(name, "l") == 0) {
		return EVAL_MIC_LEFT;
	}
	if (strcmp(name, "c") == 0) {
		return EVAL_MIC_CENTER;
	}
	if (strcmp(name, "r") == 0) {
		return EVAL_MIC_RIGHT;
	}
	return -1;
}

void eval_run_config_defaults(eval_run_config_t *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->bf.delay_samples[EVAL_MIC_LEFT] = CONFIG_EVAL_BF_LEFT_DELAY;
	cfg->bf.delay_samples[EVAL_MIC_CENTER] = CONFIG_EVAL_BF_CENTER_DELAY;
	cfg->bf.delay_samples[EVAL_MIC_RIGHT] = CONFIG_EVAL_BF_RIGHT_DELAY;
	for (int i = 0; i < (int)EVAL_MIC_COUNT; i++) {
		cfg->bf.gain_q15[i] = CONFIG_EVAL_BF_GAIN_Q15;
		cfg->stream_raw[i] = true;
	}
	cfg->bf.out_gain_q15 = 32767;
	cfg->stream_bf = true;
	cfg->duration_s = CONFIG_EVAL_DEFAULT_DURATION_S;
	strcpy(cfg->label, "untitled");
	strcpy(cfg->clip, "");
	strcpy(cfg->environment, "quiet");
}
