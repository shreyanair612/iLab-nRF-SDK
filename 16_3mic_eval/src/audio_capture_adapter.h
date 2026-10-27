/*
 * Microphone capture: three ICS-52000 microphones daisy-chained on one TDM
 * bus, delivered to the evaluation thread as synchronized frames through a
 * message queue. Capture never blocks on the consumer; if the queue is full
 * the frame is dropped and counted (the host sees the hole in the sample
 * index and records a gap).
 *
 * Channel map (boards/nrf54lm20dk_nrf54lm20b_cpuapp.overlay):
 *
 *   TDM slot 0 -> LEFT    WS driven by the nRF FSYNC pin
 *   TDM slot 1 -> CENTER  WS driven by the left mic's WSO
 *   TDM slot 2 -> RIGHT   WS driven by the center mic's WSO
 *   slot 3     -> unused, reads 0 through the SD pulldown
 *   pins: SCK P1.23, FSYNC P1.22, SD P1.13
 *
 * All three share SCK and one frame-sync pulse, so they are sampled on the
 * same clock edge: the array is synchronized by construction.
 */
#ifndef AUDIO_CAPTURE_ADAPTER_H_
#define AUDIO_CAPTURE_ADAPTER_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#include "eval_config.h"

struct capture_frame {
	uint32_t seq;            /* capture counter; holes mean dropped frames */
	uint16_t count;          /* samples per channel                        */
	int16_t mic[EVAL_MIC_COUNT][EVAL_FRAME_SAMPLES];
};

struct audio_capture_stats {
	uint32_t frames_captured;
	uint32_t frames_dropped;     /* queue full */
	uint32_t queue_high_water;
	uint32_t queue_depth;
	uint32_t i2s_errors;
	uint32_t unpack_us_last;
	uint32_t unpack_us_max;
};

int audio_capture_init(void);
int audio_capture_start(void);

struct k_msgq *audio_capture_queue(void);

void audio_capture_get_stats(struct audio_capture_stats *out);
void audio_capture_reset_stats(void);

uint32_t audio_capture_sample_rate_hz(void);
uint8_t audio_capture_channels(void);
uint8_t audio_capture_tdm_slots(void);

#endif /* AUDIO_CAPTURE_ADAPTER_H_ */
