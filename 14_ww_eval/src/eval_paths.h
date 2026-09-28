/*
 * Generates every enabled evaluation path from one synchronized microphone
 * frame, so all paths see exactly the same instant of sound.
 *
 *   RAW_LEFT     out[n] = left[n]
 *   RAW_RIGHT    out[n] = right[n]
 *   RAW_LR_MIX   out[n] = left[n]/2 + right[n]/2         (overflow-safe average)
 *   BF_LR        baseline integer delay-and-sum of left and right
 *   RAW_3CH      configurable reduction of left/right/center to mono
 *   BF_3CH       baseline 3-channel integer delay-and-sum, center is reference
 *
 * Integer division here truncates toward zero, and tools/dsp_reference.py
 * mirrors that exactly so the host can reproduce and verify every path.
 *
 * Pure C, no Zephyr dependency.
 */
#ifndef EVAL_PATHS_H_
#define EVAL_PATHS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "beamformer_delay_sum.h"
#include "eval_config.h"

#ifdef __cplusplus
extern "C" {
#endif

struct eval_frame {
	uint32_t seq;            /* capture frame counter                       */
	uint32_t sample_index;   /* index of the first sample since run start   */
	uint16_t count;          /* samples per channel, EVAL_FRAME_SAMPLES     */
	const int16_t *left;
	const int16_t *right;
	const int16_t *center;   /* NULL when no third microphone is captured   */
};

struct eval_paths {
	const eval_run_config_t *cfg;
	struct bf_delay_sum bf_lr;
	struct bf_delay_sum bf_3ch;
	int16_t out[EVAL_PATH_COUNT][EVAL_FRAME_SAMPLES];
	uint32_t produced_mask;  /* paths filled by the last eval_paths_process */
	uint32_t unavailable_mask; /* enabled paths that could not be produced  */
	uint32_t mix_clips;      /* RAW_3CH reductions never clip; kept for symmetry */
};

void eval_paths_init(struct eval_paths *ep, const eval_run_config_t *cfg);

/* Pushes the configured delays and gains into the beamformers. Returns -1 if
 * any delay is out of range. */
int eval_paths_apply_config(struct eval_paths *ep);

/* Clears delay histories; call at the start of every run and replay pass. */
void eval_paths_reset_state(struct eval_paths *ep);

/* Fills ep->out for every enabled path. Returns the produced bitmask. */
uint32_t eval_paths_process(struct eval_paths *ep, const struct eval_frame *f);

static inline const int16_t *eval_paths_output(const struct eval_paths *ep, eval_path_id_t id)
{
	return ep->out[id];
}

#ifdef __cplusplus
}
#endif

#endif /* EVAL_PATHS_H_ */
