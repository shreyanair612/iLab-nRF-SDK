/*
 * Central configuration for the 3-microphone beamforming evaluation.
 *
 * Pure C, no Zephyr dependency, so the DSP can be compiled on the host too.
 */
#ifndef EVAL_CONFIG_H_
#define EVAL_CONFIG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVAL_SAMPLE_RATE_HZ   16000U
#define EVAL_FRAME_SAMPLES    256U            /* one capture block, 16 ms */
#define EVAL_FRAME_MS         ((EVAL_FRAME_SAMPLES * 1000U) / EVAL_SAMPLE_RATE_HZ)
#define EVAL_MIC_COUNT        3U

#ifndef EVAL_MAX_DELAY_SAMPLES
#ifdef CONFIG_EVAL_MAX_DELAY_SAMPLES
#define EVAL_MAX_DELAY_SAMPLES CONFIG_EVAL_MAX_DELAY_SAMPLES
#else
#define EVAL_MAX_DELAY_SAMPLES 64
#endif
#endif

/* Physical microphones, in TDM slot order (left -> center -> right chain). */
typedef enum {
	EVAL_MIC_LEFT = 0,
	EVAL_MIC_CENTER = 1,
	EVAL_MIC_RIGHT = 2,
} eval_mic_t;

/*
 * Packet stream ids. Raw microphones use their eval_mic_t value; the on-device
 * beamformer output is EVAL_STREAM_BF. The host mirrors these in
 * tools/serial_protocol.py.
 */
typedef enum {
	EVAL_STREAM_RAW_LEFT = 0,
	EVAL_STREAM_RAW_CENTER = 1,
	EVAL_STREAM_RAW_RIGHT = 2,
	EVAL_STREAM_BF = 16,
	EVAL_STREAM_COUNT_ALL = 4,
	EVAL_STREAM_NONE = 0xFFFF,
} eval_stream_id_t;

typedef struct {
	int16_t delay_samples[EVAL_MIC_COUNT];   /* left, center, right            */
	int16_t gain_q15[EVAL_MIC_COUNT];        /* per-mic weight, 32767 = 1.0    */
	int16_t out_gain_q15;                    /* applied after the sum          */
} eval_bf_config_t;

typedef struct {
	eval_bf_config_t bf;
	bool stream_raw[EVAL_MIC_COUNT];
	bool stream_bf;
	uint16_t duration_s;          /* 0 = until stop */
	int16_t angle_deg;
	uint16_t distance_cm;
	char label[48];
	char clip[48];
	char environment[32];
} eval_run_config_t;

const char *eval_mic_name(eval_mic_t m);
const char *eval_stream_name(uint16_t stream_id);
int eval_mic_from_name(const char *name);        /* -1 if unknown */

void eval_run_config_defaults(eval_run_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* EVAL_CONFIG_H_ */
