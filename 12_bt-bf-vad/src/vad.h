#ifndef VAD_H
#define VAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Energy-based voice activity detector with hysteresis and an explicit
 * speech-onset gate.
 *
 * Design notes:
 *   - All arithmetic is integer/fixed point. No floating point, no allocation.
 *   - vad_process() is expected to be called from exactly one context (the
 *     audio capture thread). It is NOT internally locked.
 *   - Endpointing is a two-stage machine: nothing can be reported as an
 *     endpoint until speech onset has been observed for the current session.
 */

struct vad_config {
	/* PCM sample rate of the frames handed to vad_process(), in Hz. */
	uint32_t sample_rate_hz;

	/* Starting noise-floor RMS estimate, used at init and on vad_reset_all(). */
	uint32_t noise_init_rms;

	/*
	 * Hysteresis margins above the tracked noise floor.
	 * margin_on  : frame RMS must exceed noise+margin_on to declare onset.
	 * margin_off : frame RMS below noise+margin_off counts as silence.
	 * Requires margin_on > margin_off.
	 */
	uint32_t margin_on;
	uint32_t margin_off;

	/* Continuous quiet time after speech that ends the utterance. */
	uint32_t silence_end_ms;

	/* Utterance must be at least this long before an endpoint may fire. */
	uint32_t min_speech_ms;

	/* Noise floor IIR: noise += (rms - noise) >> noise_alpha_shift. */
	uint8_t noise_alpha_shift;
};

/* Telemetry snapshot. Cheap to fill; safe to call from the audio thread. */
struct vad_debug {
	uint32_t rms;
	uint32_t noise_rms;
	uint32_t threshold_on;
	uint32_t threshold_off;
	uint32_t silence_frames;
	uint32_t silence_end_frames;
	uint32_t speech_frames;
	uint32_t min_speech_frames;
	uint32_t frame_samples;
	uint32_t frame_bytes;
	uint32_t frame_ms;
	uint32_t sample_rate_hz;
	bool speech_active;
	bool frame_is_speech;
	bool endpoint_latched;
};

enum vad_event {
	VAD_NO_EVENT = 0,
	VAD_SPEECH_STARTED,
	VAD_ENDPOINT,
};

int vad_init(const struct vad_config *config);

/* Begin a new utterance: clears onset, silence and endpoint-latch state. */
void vad_reset_endpoint(void);

/* As vad_reset_endpoint(), and additionally re-seeds the noise floor. */
void vad_reset_all(void);

/*
 * Process one mono int16 PCM frame.
 *
 * endpoint_tracking == false : idle/monitoring. The noise floor adapts and
 *                              utterance state is held reset. Never returns
 *                              an event.
 * endpoint_tracking == true  : an utterance session is open. Returns
 *                              VAD_SPEECH_STARTED once at onset and
 *                              VAD_ENDPOINT once when the trailing pause is
 *                              long enough. VAD_ENDPOINT latches, so it
 *                              cannot fire twice before vad_reset_endpoint().
 */
enum vad_event vad_process(const int16_t *samples, size_t count,
			   bool endpoint_tracking);

bool vad_speech_active(void);

void vad_get_debug(struct vad_debug *debug);

#endif /* VAD_H */
