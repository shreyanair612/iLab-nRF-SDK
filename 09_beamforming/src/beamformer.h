#ifndef BEAMFORMER_H
#define BEAMFORMER_H

#include <arm_math.h>
#include <stdint.h>
#include <stddef.h>

// call once during system init before any audio processing
void beamformer_init(void);

// process one frame of interleaved stereo i2s samples
void beamformer_process(const int32_t *buf, size_t n_frames, float32_t *out);

#endif // BEAMFORMER_H