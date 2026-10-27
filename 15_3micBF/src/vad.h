#ifndef VAD_H
#define VAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct vad_config {
	uint32_t sample_rate_hz;
	uint32_t noise_init_rms;

	/*
	 * Thresholds scale with the tracked noise floor, expressed in eighths:
	 *   threshold = noise * ratio_x8 / 8
	 * Speech-to-noise ratio holds far steadier across rooms than any fixed
	 * margin does, so this keeps the decision boundary at the same relative
	 * point whether the room is quiet or noisy.
	 */
	uint32_t onset_ratio_x8;
	uint32_t off_ratio_x8;

	/*
	 * Absolute floors beneath those ratios, so a near-silent room cannot
	 * collapse the thresholds down onto the noise itself.
	 *   threshold_on  = MAX(noise * onset_ratio_x8 / 8, noise + margin_on)
	 *   threshold_off = MAX(noise * off_ratio_x8   / 8, noise + margin_off)
	 */
	uint32_t margin_on;
	uint32_t margin_off;

	uint32_t onset_ms;
	uint32_t silence_end_ms;
	uint32_t min_speech_ms;
	uint8_t noise_alpha_shift;
};

struct vad_debug {
	uint32_t rms;
	uint32_t noise_rms;
	uint32_t threshold_on;
	uint32_t threshold_off;
	uint32_t silence_frames;
	uint32_t silence_end_frames;
	uint32_t speech_frames;
	uint32_t min_speech_frames;
	uint32_t onset_frames;
	uint32_t onset_frames_req;
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
 * Process one mono int16 PCM frame. `count` is a sample count, not bytes.
 *
 * endpoint_tracking false : idle. The noise floor adapts, utterance state is
 *                           held clear, and no event is ever returned.
 * endpoint_tracking true  : an utterance is open. Returns VAD_SPEECH_STARTED
 *                           once at onset, then VAD_ENDPOINT once the trailing
 *                           pause is long enough. The endpoint latches so it
 *                           cannot fire twice before vad_reset_endpoint().
 */
enum vad_event vad_process(const int16_t *samples, size_t count,
			   bool endpoint_tracking);

bool vad_speech_active(void);

void vad_get_debug(struct vad_debug *debug);

#endif /* VAD_H */
