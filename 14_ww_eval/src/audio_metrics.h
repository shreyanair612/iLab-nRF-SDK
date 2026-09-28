/*
 * Per-stream signal statistics: RMS, peak, clipping, and a running CRC32 of
 * every sample the stream produced. The CRC lets the host prove that its own
 * reconstruction of a processed path is bit-identical to what the device
 * computed, and lets a replay pass be checked against the live run.
 *
 * Two accumulators run side by side: "run" covers the whole run, "window"
 * covers the interval since the last am_window_flush() and feeds the periodic
 * AUDIO_STATS packets.
 *
 * Pure C, no Zephyr dependency.
 */
#ifndef AUDIO_METRICS_H_
#define AUDIO_METRICS_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct am_accum {
	uint64_t sumsq;
	uint32_t samples;
	uint16_t peak_abs;
	uint32_t clips;
};

struct audio_metrics {
	struct am_accum run;
	struct am_accum window;
	uint32_t crc32;
	uint32_t frames;
};

void am_reset(struct audio_metrics *m);
void am_update(struct audio_metrics *m, const int16_t *samples, size_t n);

/* Copies the window accumulator out and clears it. */
void am_window_flush(struct audio_metrics *m, struct am_accum *out);

uint32_t am_rms(const struct am_accum *a);

/* IEEE 802.3 CRC32 (same as zlib.crc32), incremental. Start with crc = 0. */
uint32_t am_crc32_update(uint32_t crc, const uint8_t *data, size_t len);

/* Integer square root of a 64-bit value (floor). */
uint32_t am_isqrt64(uint64_t x);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_METRICS_H_ */
