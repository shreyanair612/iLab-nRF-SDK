#ifndef VAD_H
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
	uint32_t onset_ms;
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

void vad_reset_endpoint(void);

void vad_reset_all(void);

enum vad_event vad_process(const int16_t *samples, size_t count,
			   bool endpoint_tracking);

bool vad_speech_active(void);

void vad_get_debug(struct vad_debug *debug);

#endif /* VAD_H */
