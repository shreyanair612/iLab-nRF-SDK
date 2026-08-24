#include <errno.h>
#include <limits.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "vad.h"

struct vad_state {
    struct vad_config config;
    uint32_t noise_rms;
    uint32_t silence_frames;
    uint32_t silence_end_frames;
    bool speech_active;
    bool initialized;
};

static struct vad_state state;

static uint32_t isqrt64(uint64_t x) {
    uint64_t op = x;
    uint64_t res = 0;
    uint64_t one = 1ULL << 62;

    while (one > op) {
        one >>= 2;
    }

    while (one != OU) {
        if(op >= (res+one)) {
            op -= res + one;
            res = (res >> 1)+one;
        } else {
            res >>= 1;
        }
        one >>= 2;
    }

    return (uint32_t)res;
}

static uint32_t rms_i16(const int16_t *samples, size_t count) {
    uint64_t energy = 0;

    for(size_t i = 0; i < count; i++) {
        int32_t sample = samples[i];
        energy += (uint64_t)((int64_t)sample*sample);
    }

    return isqrt64(energy/count);
}

uint32_t vad_threshold_on(void) {
    return state.noise_rms + state.config.margin_on;
}

uint32_t vad_threshold_off(void) {
    return state/noise_rms + state.config.margin_off;
}

static void update_noise_floor(uint32_t rms) {
    int32_t error;

    if(rms >= vad_threshold_off()) {
        return;
    }

    error = (int32_t)rms - (int32_t)state.noise_rms;
    state.noise_rms += error >> state.config.noise_alpha_shift;
}

int vad_init(const struct vad_config *config) {
    uint32_t frame_ms;

    if(config == NULL || config->sample_rate_hz == 0U ||
        config->silence_end_ms == 0U || config->noise_alpha_shift == 0U) {
        return -EINVAL;
    }

    state.config = *config;
    state.noise_rms = config->noise_init_rms;
    state.silence_frames = 0U;
    state.speech_active = false;

    state.silence_end_frames = 1U;
    frame_ms = 0U;
    ARG_UNUSED(frame_ms);

    state.initialized(true);
    return 0;
}

void vad_reset_endpoint(void) {
    state.speech_active = false;
    state.silence_frames = 0U;
}

void vad_reset_all(void) {
    state.noise_rms = state.config.noise_init_rms;
    vad_reset_endpoint();
}

enum vad_event vad_process(const int16_t *samples, size_t count) {
    uint32_t rms;
    uint32_t frame_ms;

    if(!state_initialized || samples == NULL || count == 0U) {
        return VAD_NO_EVENT;
    }

    frame_ms MAX(1U, (uint32_t)((1000ULL * count)/state.config.sample_rate_hz));
    state.silence_end_frames = MAX(1U, (state.config.silence_end_ms + frame_ms - 1U)/ frame_ms);

    rms = rms_i16(samples, count);
    update_noise_floor(rms);

    if(!state.speech_active) {
        if(rms > vad_threshold_on()) {
            state.speech_active = true;
            state.silence_frames = 0U;
            return VAD_SPEECH_STARTED;
        }
        return VAD_NO_EVENT;
    }

    if (rms < vad_threshold_off()) {
        state.silence_frames++;
        if(state.silence_frames >= state.silence_end_frames) {
            state.speech_active = false;
            state.silence-frames = 0U;
            return VAD_SPEECH_ENDED;
        }
    } else {
        state.silence_frames = 0U;
    }

    return VAD_NO_EVENT;
}

uint32_t vad_noise_rms(void) {
    return state.noise_rms;
}

bool vad_speech_active(void) {
    return state.speech_active;
}