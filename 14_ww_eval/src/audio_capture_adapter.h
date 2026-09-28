/*
 * Microphone capture: synchronized multi-channel I2S/TDM frames delivered to
 * the evaluation thread through a message queue. Capture never blocks on the
 * consumer; if the queue is full the frame is dropped and counted.
 *
 * Channel map (from boards/nrf54lm20dk_nrf54lm20b_cpuapp.overlay and the
 * INMP441 datasheet; confirm with the end-fire test in the README):
 *
 *   TDM slot 0 (WS low)  -> LEFT   INMP441 with its L/R pin tied to GND
 *   TDM slot 1 (WS high) -> RIGHT  INMP441 with its L/R pin tied to VDD
 *   pins: SCK P1.23, WS/FSYNC P1.22, SD P1.13
 *
 * Both microphones share one data line, so they are sampled by the same
 * clock edges: the pair is synchronized by construction. A third (center)
 * microphone cannot share this line; Phase 2 adds it on the PDM peripheral.
 */
#ifndef AUDIO_CAPTURE_ADAPTER_H_
#define AUDIO_CAPTURE_ADAPTER_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#include "eval_config.h"

enum frame_source {
	FRAME_SRC_CAPTURE = 0,   /* live microphones                      */
	FRAME_SRC_REPLAY = 1,    /* audio sent back by the host            */
	FRAME_SRC_STOP = 2,      /* marker: finish the current run in order */
	FRAME_SRC_REPLAY_END = 3,/* marker: finish the current replay pass  */
	FRAME_SRC_START = 4,     /* marker: open a run (bind runtime, reset) */
	FRAME_SRC_REPLAY_START = 5, /* marker: open a replay pass          */
};

struct capture_frame {
	uint32_t seq;            /* capture counter, or replay frame index */
	uint8_t source;          /* enum frame_source                      */
	uint8_t channels;        /* 2 today, 3 with a center microphone    */
	uint16_t count;          /* samples per channel                    */
	int16_t left[EVAL_FRAME_SAMPLES];
	int16_t right[EVAL_FRAME_SAMPLES];
	int16_t center[EVAL_FRAME_SAMPLES];
};

struct audio_capture_stats {
	uint32_t frames_captured;
	uint32_t frames_dropped;     /* queue full                          */
	uint32_t frames_discarded;   /* enqueue disabled (replay in progress) */
	uint32_t queue_high_water;
	uint32_t queue_depth;
	uint32_t i2s_errors;
	uint32_t unpack_us_last;
	uint32_t unpack_us_max;
};

int audio_capture_init(void);
int audio_capture_start(void);

struct k_msgq *audio_capture_queue(void);

/* When false, captured frames are discarded instead of queued (replay mode). */
void audio_capture_set_enqueue(bool enable);

void audio_capture_get_stats(struct audio_capture_stats *out);
void audio_capture_reset_stats(void);

uint32_t audio_capture_sample_rate_hz(void);
uint8_t audio_capture_channels(void);

#endif /* AUDIO_CAPTURE_ADAPTER_H_ */
