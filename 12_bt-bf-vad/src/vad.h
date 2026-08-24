#ifdef VAD_H
#define VAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct vad_config {
    uint32_t sample_rate_hz;
    uint32_t noise_init_rms;
    uint32_t margin_on;
    uint32_t margin_off;
    uint32_t silence_end_ms;
    uint8_t noise_alpha_shift;
};

enum vad_event {
    VAD_NO_EVENT = 0,
    VAD_SPEECH_STARTED,
    VAD_SPEECH_ENDED,
};

int vad_init(const struct vad_config *config);
void vad_reset_endpoint(void);
void vad_reset_all(void);
enum vad_event vad_process(const int16_t *samples, size_t count);

uint32_t vad_noise_rms(void);
uint32_t vad_threshold_on(void);
uint32_t vad_threshold_off(void);
bool vad_speech_active(void);

#endif