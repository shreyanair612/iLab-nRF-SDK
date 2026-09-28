#include <string.h>

#include "audio_metrics.h"

void am_reset(struct audio_metrics *m)
{
	memset(m, 0, sizeof(*m));
}

static void accum(struct am_accum *a, const int16_t *s, size_t n)
{
	uint64_t sumsq = 0;
	uint16_t peak = a->peak_abs;
	uint32_t clips = 0;

	for (size_t i = 0; i < n; i++) {
		int32_t v = s[i];
		uint16_t mag = (uint16_t)((v < 0) ? -v : v);

		sumsq += (uint64_t)((int64_t)v * v);
		if (mag > peak) {
			peak = mag;
		}
		if (v >= INT16_MAX || v <= INT16_MIN) {
			clips++;
		}
	}
	a->sumsq += sumsq;
	a->samples += (uint32_t)n;
	a->peak_abs = peak;
	a->clips += clips;
}

void am_update(struct audio_metrics *m, const int16_t *samples, size_t n)
{
	accum(&m->run, samples, n);
	accum(&m->window, samples, n);
	m->crc32 = am_crc32_update(m->crc32, (const uint8_t *)samples, n * sizeof(int16_t));
	m->frames++;
}

void am_window_flush(struct audio_metrics *m, struct am_accum *out)
{
	*out = m->window;
	memset(&m->window, 0, sizeof(m->window));
}

uint32_t am_isqrt64(uint64_t x)
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

uint32_t am_rms(const struct am_accum *a)
{
	if (a->samples == 0U) {
		return 0U;
	}
	return am_isqrt64(a->sumsq / a->samples);
}

/* Nibble-table CRC32, IEEE polynomial reflected (0xEDB88320): 64 bytes of table. */
static const uint32_t crc32_nib[16] = {
	0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
	0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

uint32_t am_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
	crc = ~crc;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		crc = crc32_nib[crc & 0x0F] ^ (crc >> 4);
		crc = crc32_nib[crc & 0x0F] ^ (crc >> 4);
	}
	return ~crc;
}
