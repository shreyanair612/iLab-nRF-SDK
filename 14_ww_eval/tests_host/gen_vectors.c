/*
 * Emits cross-check vectors for tools/tests/test_host.py: the firmware's own
 * objects process a deterministic pseudo-random stereo input through
 * RAW_LR_MIX and BF_LR (with delays) and print the CRC32 of each output. The
 * Python reference must reproduce the same CRCs, which proves the host can
 * rebuild the device's processed audio bit for bit.
 */
#include <stdio.h>
#include <string.h>

#include "audio_metrics.h"
#include "eval_paths.h"

#define FRAMES 40

static uint32_t lcg(uint32_t *s)
{
	*s = *s * 1664525u + 1013904223u;
	return *s;
}

int main(void)
{
	static int16_t l[FRAMES * EVAL_FRAME_SAMPLES], r[FRAMES * EVAL_FRAME_SAMPLES];
	uint32_t seed = 12345;

	for (unsigned i = 0; i < FRAMES * EVAL_FRAME_SAMPLES; i++) {
		l[i] = (int16_t)(lcg(&seed) >> 16);
		r[i] = (int16_t)(lcg(&seed) >> 16);
	}

	eval_run_config_t cfg;
	struct eval_paths ep;

	eval_run_config_defaults(&cfg);
	eval_run_config_apply_preset(&cfg, EVAL_PRESET_RAW_VS_BF_LR);
	cfg.path[EVAL_PATH_BF_LR].left_delay_samples = 5;
	cfg.path[EVAL_PATH_BF_LR].right_delay_samples = 2;
	cfg.path[EVAL_PATH_BF_LR].gain_q15 = 30000;
	eval_paths_init(&ep, &cfg);
	eval_paths_apply_config(&ep);
	eval_paths_reset_state(&ep);

	struct audio_metrics m[EVAL_PATH_COUNT];

	for (int p = 0; p < EVAL_PATH_COUNT; p++) {
		am_reset(&m[p]);
	}
	for (int f = 0; f < FRAMES; f++) {
		struct eval_frame fr = { .seq = f, .sample_index = f * EVAL_FRAME_SAMPLES,
					 .count = EVAL_FRAME_SAMPLES,
					 .left = l + f * EVAL_FRAME_SAMPLES,
					 .right = r + f * EVAL_FRAME_SAMPLES, .center = NULL };
		uint32_t mask = eval_paths_process(&ep, &fr);

		for (int p = 0; p < EVAL_PATH_COUNT; p++) {
			if (mask & (1U << p)) {
				am_update(&m[p], ep.out[p], EVAL_FRAME_SAMPLES);
			}
		}
	}
	printf("{\"seed\":%u,\"frames\":%d,\"frame\":%d,\"dl\":5,\"dr\":2,\"gain\":30000,"
	       "\"crc\":{\"raw_left\":\"%08x\",\"raw_right\":\"%08x\",\"raw_lr_mix\":\"%08x\","
	       "\"bf_lr\":\"%08x\"},\"bf_clips\":%u,\"rms_bf\":%u}\n",
	       12345u, FRAMES, EVAL_FRAME_SAMPLES, m[0].crc32, m[1].crc32, m[2].crc32, m[3].crc32,
	       ep.bf_lr.clip_count, am_rms(&m[3].run));
	return 0;
}
