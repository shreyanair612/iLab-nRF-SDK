#include "beamformer.h"
#include <math.h>
#include <string.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(beamformer, LOG_LEVEL_INF);

// configuration
#define SAMPLE_RATE_HZ     8000
#define MIC_SPACING_M      0.1508f
#define SPEED_OF_SOUND_MS  343.0f
#define STEER_ANGLE_DEG    (0.0f)  // broadside for your current breadboard geometry

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// No FIR, no delay-line for this diagnostic version
void beamformer_init(void)
{
    float32_t angle_rad   = STEER_ANGLE_DEG * (M_PI / 180.0f);
    float32_t tau_seconds = (MIC_SPACING_M * sinf(angle_rad)) / SPEED_OF_SOUND_MS;
    float32_t delay_samples = tau_seconds * SAMPLE_RATE_HZ;

    LOG_INF("Beamformer (minimal): angle=%.2f deg, tau=%.2f us, delay_samples=%.3f",
            (double)STEER_ANGLE_DEG,
            (double)(tau_seconds * 1e6f),
            (double)delay_samples);
}

void beamformer_process(const int32_t *buf, size_t n_frames, float32_t *out)
{
    for (size_t i = 0; i < n_frames; i++) {
        // Convert Q24 PCM from I2S to float32 in [-1, 1]
        float32_t mic_L = (float32_t)(buf[2 * i]     >> 8) / 8388608.0f;
        float32_t mic_R = (float32_t)(buf[2 * i + 1] >> 8) / 8388608.0f;

        // Pure broadside delay-and-sum (no delay needed at θ = 0°)
        out[i] = 0.5f * (mic_L + mic_R);
    }
}