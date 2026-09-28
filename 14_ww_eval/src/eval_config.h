/*
 * Central configuration for the wake-word beamforming evaluation.
 *
 * Pure C: no Zephyr dependencies, so the DSP modules that include this header
 * can be compiled and unit-tested on a host machine (see tests_host/).
 */
#ifndef EVAL_CONFIG_H_
#define EVAL_CONFIG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Capture geometry. Must match audio_capture_adapter.c and the wake-word model. */
#define EVAL_SAMPLE_RATE_HZ   16000U
#define EVAL_FRAME_SAMPLES    256U            /* one capture block, 16 ms */
#define EVAL_FRAME_MS         ((EVAL_FRAME_SAMPLES * 1000U) / EVAL_SAMPLE_RATE_HZ)
#define EVAL_MAX_CHANNELS     3U

#ifndef EVAL_MAX_DELAY_SAMPLES
#ifdef CONFIG_EVAL_MAX_DELAY_SAMPLES
#define EVAL_MAX_DELAY_SAMPLES CONFIG_EVAL_MAX_DELAY_SAMPLES
#else
#define EVAL_MAX_DELAY_SAMPLES 64
#endif
#endif

/*
 * Evaluation paths. The order is the wire order: stream/path ids in packets,
 * dashboard rows, and command names all use it.
 */
typedef enum {
	EVAL_PATH_RAW_LEFT = 0,
	EVAL_PATH_RAW_RIGHT,
	EVAL_PATH_RAW_LR_MIX,
	EVAL_PATH_BF_LR,
	EVAL_PATH_RAW_3CH,
	EVAL_PATH_BF_3CH,
	EVAL_PATH_COUNT
} eval_path_id_t;

/*
 * Physical microphone channels as captured. Packet stream ids for raw audio
 * use EVAL_STREAM_*; processed paths reuse their eval_path_id_t offset by
 * EVAL_STREAM_PATH_BASE so raw channels and paths never collide.
 */
typedef enum {
	EVAL_STREAM_RAW_LEFT = 0,
	EVAL_STREAM_RAW_RIGHT = 1,
	EVAL_STREAM_RAW_CENTER = 2,
	EVAL_STREAM_PATH_BASE = 16,   /* + eval_path_id_t */
	EVAL_STREAM_NONE = 0xFFFF,
} eval_stream_id_t;

#define EVAL_STREAM_FOR_PATH(p) ((uint16_t)(EVAL_STREAM_PATH_BASE + (p)))

typedef enum {
	RAW_3CH_CENTER_ONLY = 0,
	RAW_3CH_LR_AVERAGE,
	RAW_3CH_LC_AVERAGE,
	RAW_3CH_RC_AVERAGE,
	RAW_3CH_LRC_AVERAGE,
	RAW_3CH_REDUCE_COUNT
} raw_3ch_reduce_mode_t;

typedef enum {
	EVAL_PRESET_LEFT = 0,
	EVAL_PRESET_RIGHT,
	EVAL_PRESET_LR_MIX,
	EVAL_PRESET_BF_LR,
	EVAL_PRESET_RAW_3CH,
	EVAL_PRESET_BF_3CH,
	EVAL_PRESET_RAW_VS_BF_LR,
	EVAL_PRESET_RAW_VS_BF_3CH,
	EVAL_PRESET_FULL,
	EVAL_PRESET_COUNT
} eval_preset_t;

/*
 * What the device streams to the host during a run.
 *
 * The event-window and short-continuous policies are applied on the host:
 * the device streams the selected raw channels continuously and the host
 * keeps either everything, a window around each detection, or the first N
 * seconds. Keeping the policy on the host means no device RAM is spent on
 * multi-second rings and no burst has to be squeezed through the link.
 */
typedef enum {
	EVAL_CAPTURE_OFF = 0,
	EVAL_CAPTURE_EVENT_WINDOW,     /* host keeps +-N s around detections */
	EVAL_CAPTURE_CONTINUOUS_SHORT, /* host keeps the first N s            */
	EVAL_CAPTURE_CONTINUOUS_FULL,  /* host keeps the whole run            */
	EVAL_CAPTURE_MODE_COUNT
} eval_capture_mode_t;

typedef struct {
	eval_path_id_t id;
	const char *name;
	bool enabled;
	bool capture_audio;      /* stream this path's processed audio live   */
	bool run_wakeword;       /* score this path (live if it is the live path,
				    otherwise by replay)                       */
	int16_t left_delay_samples;
	int16_t right_delay_samples;
	int16_t center_delay_samples;
	int16_t gain_q15;        /* per-path output gain, 32767 = unity        */
} eval_path_config_t;

typedef struct {
	eval_path_config_t path[EVAL_PATH_COUNT];
	eval_path_id_t live_path;
	eval_preset_t preset;
	raw_3ch_reduce_mode_t raw_3ch_reduce;
	eval_capture_mode_t capture_mode;
	uint16_t capture_seconds;     /* CONTINUOUS_SHORT length / window half-size */
	bool capture_raw_left;
	bool capture_raw_right;
	bool capture_raw_center;
	uint16_t duration_s;          /* 0 = until stop */
	uint16_t ww_threshold_x1000;
	uint16_t ww_cooldown_ms;
	int16_t angle_deg;
	uint16_t distance_cm;
	char label[48];
	char phrase[32];
	char environment[32];
} eval_run_config_t;

const char *eval_path_name(eval_path_id_t id);
const char *eval_preset_name(eval_preset_t p);
const char *eval_capture_mode_name(eval_capture_mode_t m);
const char *eval_reduce_mode_name(raw_3ch_reduce_mode_t m);
int eval_path_from_name(const char *name);        /* -1 if unknown */
int eval_preset_from_name(const char *name);      /* -1 if unknown */
int eval_capture_mode_from_name(const char *name);

void eval_run_config_defaults(eval_run_config_t *cfg);
void eval_run_config_apply_preset(eval_run_config_t *cfg, eval_preset_t preset);
uint32_t eval_run_config_enabled_mask(const eval_run_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* EVAL_CONFIG_H_ */
