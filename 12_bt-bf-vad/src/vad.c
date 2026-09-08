#include <errno.h>
#include <limits.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "vad.h"

struct vad_state {
	struct vad_config config;

	/* Tracked noise floor and the RMS of the most recent frame. */
	uint32_t noise_rms;
	uint32_t rms;

	/* Derived from the actual frame length seen on the last call. */
	uint32_t frame_samples;
	uint32_t frame_ms;
	uint32_t silence_end_frames;
	uint32_t min_speech_frames;

	/* Per-utterance counters. */
	uint32_t silence_frames;
	uint32_t speech_frames;

	bool frame_is_speech;
	bool speech_active;
	bool endpoint_latched;
	bool initialized;
};

static struct vad_state state;

/* Integer square root, 64-bit input. No division, no floating point. */
static uint32_t isqrt64(uint64_t x)
{
	uint64_t op = x;
	uint64_t res = 0;
	uint64_t one = 1ULL << 62;

	while (one > op) {
		one >>= 2;
	}

	while (one != 0U) {
		if (op >= (res + one)) {
			op -= res + one;
			res = (res >> 1) + one;
		} else {
			res >>= 1;
		}
		one >>= 2;
	}

	return (uint32_t)res;
}

/*
 * RMS of a mono int16 frame.
 *
 * `count` is a sample count, not a byte count. Worst case energy for
 * INT16_MIN is 2^30 per sample, so a 64-bit accumulator cannot overflow for
 * any realistic frame length. The caller guarantees count > 0.
 */
static uint32_t rms_i16(const int16_t *samples, size_t count)
{
	uint64_t energy = 0;

	for (size_t i = 0; i < count; i++) {
		int32_t sample = samples[i];

		energy += (uint64_t)((int64_t)sample * sample);
	}

	return isqrt64(energy / (uint64_t)count);
}

static uint32_t threshold_on(void)
{
	return state.noise_rms + state.config.margin_on;
}

static uint32_t threshold_off(void)
{
	return state.noise_rms + state.config.margin_off;
}

/*
 * Adapt the noise floor towards the current frame, but only from frames that
 * are clearly not speech. Never called while an utterance is in progress, so
 * the floor cannot chase speech energy and cannot drift during the trailing
 * pause we are trying to measure.
 */
static void update_noise_floor(uint32_t rms)
{
	int32_t error;
	int32_t updated;

	if (rms >= threshold_off()) {
		return;
	}

	error = (int32_t)rms - (int32_t)state.noise_rms;
	updated = (int32_t)state.noise_rms + (error >> state.config.noise_alpha_shift);

	state.noise_rms = (uint32_t)MAX(updated, 0);
}

/*
 * Recompute frame timing from the real frame length. `count` is mono samples,
 * so the channel count is already accounted for by the caller.
 */
static void update_frame_timing(size_t count)
{
	if (state.frame_samples == (uint32_t)count) {
		return;
	}

	state.frame_samples = (uint32_t)count;
	state.frame_ms = MAX(1U, (uint32_t)((1000ULL * count) / state.config.sample_rate_hz));

	state.silence_end_frames =
		MAX(1U, (state.config.silence_end_ms + state.frame_ms - 1U) / state.frame_ms);
	state.min_speech_frames =
		(state.config.min_speech_ms + state.frame_ms - 1U) / state.frame_ms;
}

int vad_init(const struct vad_config *config)
{
	if (config == NULL || config->sample_rate_hz == 0U ||
	    config->silence_end_ms == 0U || config->noise_alpha_shift == 0U ||
	    config->noise_alpha_shift >= 31U) {
		return -EINVAL;
	}

	/* Hysteresis is only meaningful if the onset gate sits above the off gate. */
	if (config->margin_on <= config->margin_off) {
		return -EINVAL;
	}

	state.config = *config;
	state.noise_rms = config->noise_init_rms;
	state.rms = 0U;

	state.frame_samples = 0U;
	state.frame_ms = 1U;
	state.silence_end_frames = 1U;
	state.min_speech_frames = 0U;

	state.silence_frames = 0U;
	state.speech_frames = 0U;
	state.frame_is_speech = false;
	state.speech_active = false;
	state.endpoint_latched = false;
	state.initialized = true;

	return 0;
}

void vad_reset_endpoint(void)
{
	state.silence_frames = 0U;
	state.speech_frames = 0U;
	state.frame_is_speech = false;
	state.speech_active = false;
	state.endpoint_latched = false;
}

void vad_reset_all(void)
{
	state.noise_rms = state.config.noise_init_rms;
	state.rms = 0U;
	vad_reset_endpoint();
}

enum vad_event vad_process(const int16_t *samples, size_t count, bool endpoint_tracking)
{
	if (!state.initialized || samples == NULL || count == 0U) {
		return VAD_NO_EVENT;
	}

	update_frame_timing(count);
	state.rms = rms_i16(samples, count);
	state.frame_is_speech = (state.rms >= threshold_off());

	/*
	 * Idle path. No utterance is open, so learn the room and hold the
	 * utterance counters cleared. This is what keeps a sensible noise-floor
	 * estimate ready for the moment the Start button is pressed.
	 */
	if (!endpoint_tracking) {
		update_noise_floor(state.rms);
		state.silence_frames = 0U;
		state.speech_frames = 0U;
		state.speech_active = false;
		state.endpoint_latched = false;
		return VAD_NO_EVENT;
	}

	/*
	 * Waiting for speech. Quiet frames here are not an endpoint, they are
	 * the user not having started yet, so they must never be counted
	 * towards silence_end_frames. Keep adapting the floor while we wait.
	 */
	if (!state.speech_active) {
		if (state.rms > threshold_on()) {
			state.speech_active = true;
			state.silence_frames = 0U;
			state.speech_frames = 1U;
			return VAD_SPEECH_STARTED;
		}

		update_noise_floor(state.rms);
		state.silence_frames = 0U;
		return VAD_NO_EVENT;
	}

	/*
	 * Recording speech. The noise floor is deliberately frozen from here
	 * until the next vad_reset_endpoint(): if it kept adapting downwards
	 * during the trailing pause, threshold_off() would sink under the room
	 * noise and the silence run would keep resetting, so the utterance
	 * would never end.
	 */
	state.speech_frames++;

	if (state.frame_is_speech) {
		/* Ordinary pause between words ends here; the run restarts. */
		state.silence_frames = 0U;
		return VAD_NO_EVENT;
	}

	state.silence_frames++;

	if (state.endpoint_latched) {
		return VAD_NO_EVENT;
	}

	if (state.silence_frames >= state.silence_end_frames &&
	    state.speech_frames >= state.min_speech_frames) {
		state.endpoint_latched = true;
		return VAD_ENDPOINT;
	}

	return VAD_NO_EVENT;
}

bool vad_speech_active(void)
{
	return state.speech_active;
}

void vad_get_debug(struct vad_debug *debug)
{
	if (debug == NULL) {
		return;
	}

	debug->rms = state.rms;
	debug->noise_rms = state.noise_rms;
	debug->threshold_on = threshold_on();
	debug->threshold_off = threshold_off();
	debug->silence_frames = state.silence_frames;
	debug->silence_end_frames = state.silence_end_frames;
	debug->speech_frames = state.speech_frames;
	debug->min_speech_frames = state.min_speech_frames;
	debug->frame_samples = state.frame_samples;
	debug->frame_bytes = state.frame_samples * (uint32_t)sizeof(int16_t);
	debug->frame_ms = state.frame_ms;
	debug->sample_rate_hz = state.config.sample_rate_hz;
	debug->speech_active = state.speech_active;
	debug->frame_is_speech = state.frame_is_speech;
	debug->endpoint_latched = state.endpoint_latched;
}
