#ifndef BEAMFORM_H_
#define BEAMFORM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Always-on 2-microphone capture with a centre-steered broadside beamformer.
 * The registered callback runs on the dedicated capture thread (not an ISR)
 * and receives mono, little-endian, signed 16-bit PCM. `count` is a sample
 * count, not a byte count. The buffer is only valid for the duration of the
 * call.
 */
int beamform_init(void (*chunk_cb)(const int16_t *samples, size_t count));
int beamform_start(void);

uint32_t beamform_sample_rate_hz(void);
uint32_t beamform_frame_samples(void);

#endif
