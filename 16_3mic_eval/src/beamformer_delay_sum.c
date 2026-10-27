#include <string.h>

#include "beamformer_delay_sum.h"

#if EVAL_MAX_DELAY_SAMPLES >= BF_HISTORY_LEN
#error "BF_HISTORY_LEN must exceed EVAL_MAX_DELAY_SAMPLES"
#endif

static inline int16_t sat16(int32_t x, uint32_t *clips)
{
	if (x > INT16_MAX) {
		(*clips)++;
		return INT16_MAX;
	}
	if (x < INT16_MIN) {
		(*clips)++;
		return INT16_MIN;
	}
	return (int16_t)x;
}

void bf_init(struct bf_delay_sum *bf, const eval_bf_config_t *cfg)
{
	memset(bf, 0, sizeof(*bf));
	bf->cfg = *cfg;
	for (int m = 0; m < (int)EVAL_MIC_COUNT; m++) {
		if (bf->cfg.delay_samples[m] < 0) {
			bf->cfg.delay_samples[m] = 0;
		}
		if (bf->cfg.delay_samples[m] > EVAL_MAX_DELAY_SAMPLES) {
			bf->cfg.delay_samples[m] = EVAL_MAX_DELAY_SAMPLES;
		}
	}
}

void bf_reset(struct bf_delay_sum *bf)
{
	memset(bf->history, 0, sizeof(bf->history));
	bf->pos = 0;
	bf->clips = 0;
}

void bf_process(struct bf_delay_sum *bf, const int16_t *const mic[EVAL_MIC_COUNT],
		int16_t *out, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		int64_t acc = 0;

		for (int m = 0; m < (int)EVAL_MIC_COUNT; m++) {
			bf->history[m][bf->pos] = mic[m][i];

			int32_t tap = bf->history[m][(bf->pos - (uint32_t)bf->cfg.delay_samples[m]) &
						     BF_HISTORY_MASK];

			/* gain is Q15: x * g >> 15 */
			acc += ((int64_t)tap * bf->cfg.gain_q15[m]) >> 15;
		}
		bf->pos = (bf->pos + 1U) & BF_HISTORY_MASK;

		/* Average of the three weighted taps, then output gain, truncating toward zero. */
		int32_t avg = (int32_t)(acc / (int64_t)EVAL_MIC_COUNT);
		int32_t y = (int32_t)(((int64_t)avg * bf->cfg.out_gain_q15) >> 15);

		out[i] = sat16(y, &bf->clips);
	}
}
