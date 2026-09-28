#include <string.h>

#include "beamformer_delay_sum.h"

void bf_init(struct bf_delay_sum *bf, uint8_t num_channels)
{
	memset(bf, 0, sizeof(*bf));
	if (num_channels < 1U) {
		num_channels = 1U;
	}
	if (num_channels > BF_MAX_CHANNELS) {
		num_channels = BF_MAX_CHANNELS;
	}
	bf->num_channels = num_channels;
	for (uint8_t c = 0; c < BF_MAX_CHANNELS; c++) {
		bf->gain_q15[c] = INT16_MAX;
	}
}

int bf_set_delays(struct bf_delay_sum *bf, const int16_t *delays, uint8_t count)
{
	if (count != bf->num_channels) {
		return -1;
	}
	for (uint8_t c = 0; c < count; c++) {
		if (delays[c] < 0 || delays[c] > BF_MAX_DELAY) {
			return -1;
		}
	}
	for (uint8_t c = 0; c < count; c++) {
		bf->delay[c] = (uint16_t)delays[c];
	}
	return 0;
}

int bf_set_gains(struct bf_delay_sum *bf, const int16_t *gains_q15, uint8_t count)
{
	if (count != bf->num_channels) {
		return -1;
	}
	for (uint8_t c = 0; c < count; c++) {
		if (gains_q15[c] < 0) {
			return -1;
		}
	}
	for (uint8_t c = 0; c < count; c++) {
		bf->gain_q15[c] = gains_q15[c];
	}
	return 0;
}

void bf_reset_history(struct bf_delay_sum *bf)
{
	for (uint8_t c = 0; c < BF_MAX_CHANNELS; c++) {
		memset(bf->ch[c].history, 0, sizeof(bf->ch[c].history));
		bf->ch[c].head = 0U;
	}
}

void bf_reset_stats(struct bf_delay_sum *bf)
{
	bf->clip_count = 0U;
	bf->frames_processed = 0U;
}

/*
 * Read x_c[n - delay] where n indexes the current frame. Samples earlier than
 * the frame come from the channel's history ring, which holds the last
 * BF_MAX_DELAY samples of the previous frames.
 */
static inline int32_t delayed_sample(const struct bf_channel_state *st, const int16_t *in,
				     size_t n, uint16_t delay)
{
	if (delay == 0U || n >= delay) {
		return in[n - delay];
	}
	/* Need a sample from before this frame: (delay - n) samples back in history. */
	uint16_t back = (uint16_t)(delay - n);
	uint16_t idx = (uint16_t)((st->head + BF_MAX_DELAY - back) % BF_MAX_DELAY);

	return st->history[idx];
}

static inline void push_history(struct bf_channel_state *st, const int16_t *in, size_t n)
{
	/* Keep only the tail of the frame; anything older is unreachable. */
	size_t start = (n > BF_MAX_DELAY) ? (n - BF_MAX_DELAY) : 0U;

	for (size_t i = start; i < n; i++) {
		st->history[st->head] = in[i];
		st->head = (uint16_t)((st->head + 1U) % BF_MAX_DELAY);
	}
}

void bf_process(struct bf_delay_sum *bf, const int16_t *const *inputs, size_t n, int16_t *out)
{
	const uint8_t nch = bf->num_channels;

	for (size_t i = 0; i < n; i++) {
		int32_t acc = 0;

		for (uint8_t c = 0; c < nch; c++) {
			int32_t s = delayed_sample(&bf->ch[c], inputs[c], i, bf->delay[c]);
			/* |s| <= 32768, gain <= 32767: product < 2^31, sum of 3 < 2^17 after shift. */
			acc += (s * (int32_t)bf->gain_q15[c]) >> 15;
		}

		/* Equal-weight average. Truncation toward zero, matching the host reference. */
		acc /= (int32_t)nch;

		int16_t y = bf_saturate_int16(acc);

		if (y == INT16_MAX || y == INT16_MIN) {
			bf->clip_count++;
		}
		out[i] = y;
	}

	for (uint8_t c = 0; c < nch; c++) {
		push_history(&bf->ch[c], inputs[c], n);
	}
	bf->frames_processed++;
}
