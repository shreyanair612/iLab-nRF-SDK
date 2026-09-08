#include <errno.h>
#include <limits.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "vad.h"

struct vad_state {
    struct vad_config config;
    uint32_t noise_rms;
    uint32_t rms;
    uint32_t silence_frames;
    uint32_t silence_end_frames;
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

    while (one != 0U) {
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

static uint32_t threshold_off(void) {
    return state.noise_rms + state.config.margin_off;
}

static void update_noise_floor(uint32_t rms) {
    int32_t error;
    int32_t updated;

    if(rms >= threshold_off()) {
        return;
    }

    error = (int32_t)rms - (int32_t)state.noise_rms;
    updated = (int32_t)state.noise_rms + (error >> state.config.noise_alpha_shift);

    state.noise_rms = (uint32_t)MAX(updated,0);
}

static void set_frame_duration(size_t count) {
    uint32_t frame_ms;

    frame_ms = MAX(1U, (uint32_t)((1000ULL * count) / state.config.sample_rate_hz));
    state.silence_end_frames = MAX(1U, (state.config.silence_end_ms + frame_ms - 1U)/frame_ms);
}

int vad_init(const struct vad_config *config) {
    if(config == NULL || config->sample_rate_hz == 0U ||
        config->silence_end_ms == 0U || config->noise_alpha_shift == 0U) {
        return -EINVAL;
    }

    state.config = *config;
    state.noise_rms = config->noise_init_rms;
    state.rms = 0U;
    state.silence_frames = 0U;
    state.silence_end_frames = 1U;
    state.initialized = true;

    return 0;
}

void vad_reset_endpoint(void) {
    state.silence_frames = 0U;
}

void vad_reset_all(void) {
    state.noise_rms = state.config.noise_init_rms;
    state.rms = 0U;
    vad_reset_endpoint();
}

enum vad_event vad_process(const int16_t *samples, size_t count, bool endpoint_tracking) {
    if(!state.initialized || samples == NULL || count == 0U) {
        return VAD_NO_EVENT;
    }

    set_frame_duration(count);
    state.rms = rms_i16(samples, count);

    if (state.rms < threshold_off()) {
        update_noise_floor(state.rms);
    }

    if (!endpoint_tracking) {
        update_noise_floor(state.rms);
        state.silence_frames = 0U;
        return VAD_NO_EVENT;
    }

    if (state.rms < threshold_off()) {
        state.silence_frames++;
        if(state.silence_frames >= state.silence_end_frames) {
            state.silence_frames = 0U;
            return VAD_ENDPOINT;
        }
    } else {
        state.silence_frames = 0U;
    }

    return VAD_NO_EVENT;
}

void vad_get_debug(struct vad_debug *debug) {
    if (debug == NULL) return;

    debug->rms = state.rms;
    debug->noise_rms = state.noise_rms;
    debug->threshold_off = threshold_off();
    debug->silence_frames = state.silence_frames;
    debug->silence_end_frames = state.silence_end_frames;
}