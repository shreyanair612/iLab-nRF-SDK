/*
 * Three-microphone delay-and-sum beamformer, integer arithmetic only.
 *
 *   y[n] = out_gain * sum_m( gain[m] * x_m[n - delay[m]] ) / 3
 *
 * Pure C with no Zephyr dependency. tools/dsp_paths.py reproduces the exact
 * same arithmetic so the host can rebuild the device's output bit for bit
 * from the raw microphone recordings, and compute any other delay set
 * offline from the same audio.
 */
#ifndef BEAMFORMER_DELAY_SUM_H_
#define BEAMFORMER_DELAY_SUM_H_

#include <stddef.h>
#include <stdint.h>

#include "eval_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Power of two, larger than EVAL_MAX_DELAY_SAMPLES. */
#define BF_HISTORY_LEN   128
#define BF_HISTORY_MASK  (BF_HISTORY_LEN - 1)

struct bf_delay_sum {
	eval_bf_config_t cfg;
	int16_t history[EVAL_MIC_COUNT][BF_HISTORY_LEN];
	uint32_t pos;
	uint32_t clips;
};

void bf_init(struct bf_delay_sum *bf, const eval_bf_config_t *cfg);
void bf_reset(struct bf_delay_sum *bf);

/* Processes `count` samples from each mic into `out`. */
void bf_process(struct bf_delay_sum *bf, const int16_t *const mic[EVAL_MIC_COUNT],
		int16_t *out, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* BEAMFORMER_DELAY_SUM_H_ */
