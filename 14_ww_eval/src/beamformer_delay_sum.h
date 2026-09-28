/*
 * Baseline integer delay-and-sum beamformer, 2 or 3 channels.
 *
 * This is deliberately the simplest spatial filter that has a steering
 * parameter: each channel is delayed by a whole number of samples, the
 * delayed channels are scaled by a Q15 gain, summed, divided by the channel
 * count, and saturated to int16. It is a controlled baseline for comparison,
 * not an adaptive or production beamformer.
 *
 * Sign convention
 * ---------------
 *   out[n] = sat16( ( sum_c  gain_c * x_c[n - delay_c] >> 15 ) / N )
 *
 * delay_c is how many samples channel c is held back before summing. To steer
 * toward a source whose wavefront reaches the LEFT microphone first by d
 * samples, delay the left channel: left_delay = d, right_delay = 0. With all
 * delays zero and unity gains the output is the plain average, which is what
 * 13_finalProto shipped as "beamform".
 *
 * Delay history carries across frame boundaries. At the start of a stream the
 * history is silence, so the first delay_c samples of a delayed channel read
 * as zero. bf_reset_history() must be called at the start of every run and
 * every replay pass so results do not depend on what came before.
 *
 * Pure C, no Zephyr dependency: compiled into the firmware and into the host
 * unit tests unchanged.
 */
#ifndef BEAMFORMER_DELAY_SUM_H_
#define BEAMFORMER_DELAY_SUM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "eval_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BF_MAX_CHANNELS EVAL_MAX_CHANNELS
#define BF_MAX_DELAY    EVAL_MAX_DELAY_SAMPLES

struct bf_channel_state {
	int16_t history[BF_MAX_DELAY];
	uint16_t head;              /* next write position in history */
};

struct bf_delay_sum {
	uint8_t num_channels;
	uint16_t delay[BF_MAX_CHANNELS];
	int16_t gain_q15[BF_MAX_CHANNELS];
	struct bf_channel_state ch[BF_MAX_CHANNELS];
	uint32_t clip_count;        /* output samples that hit int16 limits  */
	uint32_t frames_processed;
};

/* Sets num_channels, zero delays, unity gains, empty history. */
void bf_init(struct bf_delay_sum *bf, uint8_t num_channels);

/* Returns -1 if any delay exceeds BF_MAX_DELAY or the channel count is wrong. */
int bf_set_delays(struct bf_delay_sum *bf, const int16_t *delays, uint8_t count);
int bf_set_gains(struct bf_delay_sum *bf, const int16_t *gains_q15, uint8_t count);

void bf_reset_history(struct bf_delay_sum *bf);
void bf_reset_stats(struct bf_delay_sum *bf);

/*
 * inputs[c] points at n samples of channel c. out receives n samples.
 * inputs and out must not overlap.
 */
void bf_process(struct bf_delay_sum *bf, const int16_t *const *inputs, size_t n, int16_t *out);

static inline int16_t bf_saturate_int16(int32_t x)
{
	if (x > INT16_MAX) {
		return INT16_MAX;
	}
	if (x < INT16_MIN) {
		return INT16_MIN;
	}
	return (int16_t)x;
}

#ifdef __cplusplus
}
#endif

#endif /* BEAMFORMER_DELAY_SUM_H_ */
