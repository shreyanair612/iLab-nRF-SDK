#include <errno.h>
#include <limits.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "vad.h"

struct vad_state {
	struct vad_config config;

	uint32_t noise_rms;
	uint32_t rms;

	uint32_t frame_samples;
	uint32_t frame_ms;
	uint32_t silence_end_frames;
	uint32_t min_speech_frames;
	uint32_t onset_frames_req;

	uint32_t silence_frames;
	uint32_t speech_frames;
	uint32_t onset_frames;

	bool frame_is_speech;
	bool speech_active;
	bool endpoint_latched;
	bool initialized;
};

static struct vad_state state;

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
 * RMS of one mono int16 frame. Worst-case energy per sample is 2^30, so the
 * 64-bit accumulator cannot overflow for any realistic frame length.
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
	uint32_t scaled = (state.noise_rms * state.config.onset_ratio_x8) / 8U;

	return MAX(scaled, state.noise_rms + state.config.margin_on);
}

static uint32_t threshold_off(void)
{
	uint32_t scaled = (state.noise_rms * state.config.off_ratio_x8) / 8U;

	return MAX(scaled, state.noise_rms + state.config.margin_off);
}

/*
 * Adapt the noise floor towards the current frame, and only from frames that
 * are clearly not speech. Never called once an utterance is in progress, so
 * the floor cannot chase speech energy and cannot drift during the very pause
 * being measured.
 */
static void update_noise_floor(uint32_t rms)
{
	int32_t noise = (int32_t)state.noise_rms;
	int32_t error = (int32_t)rms - noise;
	int32_t step;

	/*
	 * Learn only from frames that are clearly not speech, meaning anything
	 * under the onset gate. Gating on the lower off-threshold instead is a
	 * ratchet: once the floor sinks beneath the ambient level no frame ever
	 * qualifies again, and the floor is stuck there for good.
	 */
	if (rms >= threshold_on()) {
		return;
	}

	if (error < 0) {
		/* Drop quickly onto quiet moments so the floor hugs the minima. */
		step = error >> 2;
	} else {
		/*
		 * Rise slowly, but always by at least one count. A pure shift
		 * truncates small errors to zero, which is the other way the
		 * floor freezes in place.
		 */
		step = MAX(1, error >> state.config.noise_alpha_shift);
	}

	state.noise_rms = (uint32_t)MAX(noise + step, 1);
}

/* Derive frame counts from the real frame length rather than assuming one. */
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
	state.onset_frames_req =
		MAX(1U, (state.config.onset_ms + state.frame_ms - 1U) / state.frame_ms);
}

int vad_init(const struct vad_config *config)
{
	if (config == NULL || config->sample_rate_hz == 0U ||
	    config->silence_end_ms == 0U || config->noise_alpha_shift == 0U ||
	    config->noise_alpha_shift >= 31U) {
		return -EINVAL;
	}

	/* Hysteresis only means anything if the onset gate sits above the off gate. */
	if (config->margin_on <= config->margin_off ||
	    config->onset_ratio_x8 <= config->off_ratio_x8 ||
	    config->off_ratio_x8 <= 8U) {
		return -EINVAL;
	}

	state.config = *config;
	state.noise_rms = MAX(1U, config->noise_init_rms);
	state.rms = 0U;

	state.frame_samples = 0U;
	state.frame_ms = 1U;
	state.silence_end_frames = 1U;
	state.min_speech_frames = 0U;
	state.onset_frames_req = 1U;

	state.silence_frames = 0U;
	state.speech_frames = 0U;
	state.onset_frames = 0U;
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
	state.onset_frames = 0U;
	state.frame_is_speech = false;
	state.speech_active = false;
	state.endpoint_latched = false;
}

void vad_reset_all(void)
{
	state.noise_rms = MAX(1U, state.config.noise_init_rms);
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
	 * Idle. Learn the room and hold the utterance counters clear, so a
	 * well-converged noise floor is ready the moment recording starts.
	 */
	if (!endpoint_tracking) {
		update_noise_floor(state.rms);
		state.silence_frames = 0U;
		state.speech_frames = 0U;
		state.onset_frames = 0U;
		state.speech_active = false;
		state.endpoint_latched = false;
		return VAD_NO_EVENT;
	}

	/*
	 * Waiting for speech. Quiet frames here mean the talker has not begun,
	 * not that the utterance has ended, so they must never count towards
	 * the endpoint. This is what stops a session ending before it starts.
	 */
	if (!state.speech_active) {
		if (state.rms > threshold_on()) {
			state.onset_frames++;

			if (state.onset_frames >= state.onset_frames_req) {
				state.speech_active = true;
				state.silence_frames = 0U;
				state.speech_frames = state.onset_frames;
				state.onset_frames = 0U;
				return VAD_SPEECH_STARTED;
			}

			state.silence_frames = 0U;
			return VAD_NO_EVENT;
		}

		state.onset_frames = 0U;
		update_noise_floor(state.rms);
		state.silence_frames = 0U;
		return VAD_NO_EVENT;
	}

	/*
	 * Recording speech. The noise floor stays frozen from here until the
	 * next vad_reset_endpoint(): if it kept adapting during the trailing
	 * pause, threshold_off() would sink under the room noise and the
	 * silence run would keep restarting, so the utterance would never end.
	 */
	state.speech_frames++;

	if (state.frame_is_speech) {
		/* An ordinary pause between words ends here; the run restarts. */
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
	debug->onset_frames = state.onset_frames;
	debug->onset_frames_req = state.onset_frames_req;
	debug->frame_samples = state.frame_samples;
	debug->frame_bytes = state.frame_samples * (uint32_t)sizeof(int16_t);
	debug->frame_ms = state.frame_ms;
	debug->sample_rate_hz = state.config.sample_rate_hz;
	debug->speech_active = state.speech_active;
	debug->frame_is_speech = state.frame_is_speech;
	debug->endpoint_latched = state.endpoint_latched;
}
