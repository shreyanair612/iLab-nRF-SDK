#ifndef BEAMFORM_H_
#define BEAMFORM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Always-on 3-microphone (left / center / right) TDM capture with a
 * delay-and-sum beamformer. With the default zero steering delays this is a
 * centre-steered broadside beam.
 * The registered callback runs on the dedicated capture thread (not an ISR)
 * and receives mono, little-endian, signed 16-bit PCM. `count` is a sample
 * count, not a byte count. The buffer is only valid for the duration of the
 * call.
 */
int beamform_init(void (*chunk_cb)(const int16_t *samples, size_t count));
int beamform_start(void);

/*
 * Re-fit the steering delays and the L-R gain trim to the current wearer
 * from the next ~0.6 s of loud audio. Call right after a wake-word detection,
 * when that audio is known to be the wearer. No-op if a fit is in progress.
 */
void beamform_calibrate(void);

uint32_t beamform_sample_rate_hz(void);
uint32_t beamform_frame_samples(void);

#endif
