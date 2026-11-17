#include <math.h>
#include <string.h>

#include "beamform_cal.h"

#define MAX_LAG BF_CAL_MAX_LAG

/* Voiced-frame gate, in the 24-bit domain (CONFIG value is on the int16 scale). */
#define MIN_ENERGY_PER_SAMPLE \
	((int64_t)CONFIG_APP_BF_CAL_MIN_RMS * CONFIG_APP_BF_CAL_MIN_RMS * 256LL * 256LL)

void bf_cal_begin(struct bf_cal *cal)
{
	memset(cal, 0, sizeof(*cal));
	cal->active = true;
}

static void finish(const struct bf_cal *cal, struct bf_cal_result *out)
{
	memset(out, 0, sizeof(*out));

	/*
	 * Left/right: the lag with the largest correlation, but the symmetric
	 * mount (lag 0) is the prior: another lag only wins if it beats lag 0
	 * by CONFIG_APP_BF_CAL_MARGIN_PERCENT, so background sound from one
	 * side cannot drag the fit off a wearer who is actually centred.
	 */
	/* (v / 100) * margin rather than v * margin / 100: the sums are near 2^60. */
	const int64_t r0 = cal->r_lr[MAX_LAG];
	const int64_t bar_lr = r0 + (r0 / 100) * CONFIG_APP_BF_CAL_MARGIN_PERCENT;
	int best = 0;
	int64_t best_v = r0;

	for (int k = -MAX_LAG; k <= MAX_LAG; k++) {
		int64_t v = cal->r_lr[k + MAX_LAG];

		if (k != 0 && v > best_v && v > bar_lr) {
			best_v = v;
			best = k;
		}
	}

	double denom = sqrt((double)cal->e_left * (double)cal->e_right);

	out->coherence = denom > 0.0 ? (float)((double)best_v / denom) : 0.0f;
	out->lr_lag = (int8_t)best;

	/*
	 * r[k] = sum L[n] R[n+k] peaks at k > 0 when R hears the wearer k samples
	 * later than L: hold L back by k so they line up (and vice versa).
	 */
	out->delay_left = best > 0 ? (uint8_t)best : 0U;
	out->delay_right = best < 0 ? (uint8_t)(-best) : 0U;

	/* Centre: same rule, with the Kconfig default as the prior. */
	int prior = CONFIG_APP_BF_DELAY_CENTER > MAX_LAG ? MAX_LAG : CONFIG_APP_BF_DELAY_CENTER;
	const int64_t c0 = cal->c_lag[prior];
	const int64_t bar_c = c0 + (c0 / 100) * CONFIG_APP_BF_CAL_MARGIN_PERCENT;
	int bc = prior;
	int64_t bc_v = c0;

	for (int k = 0; k <= MAX_LAG; k++) {
		int64_t v = cal->c_lag[k];

		if (k != prior && v > bc_v && v > bar_c) {
			bc_v = v;
			bc = k;
		}
	}
	out->delay_center = (uint8_t)bc;

	/* Gain trim so that L - gR*R cancels the wearer. Bounded to +-3 dB. */
	float g = cal->e_right > 0 ? sqrtf((float)cal->e_left / (float)cal->e_right) : 1.0f;

	if (g < 0.708f) {
		g = 0.708f;
	} else if (g > 1.413f) {
		g = 1.413f;
	}
	out->gain_right = g;

	out->valid = out->coherence >= (CONFIG_APP_BF_CAL_MIN_COHERENCE_X100 / 100.0f);
}

bool bf_cal_feed(struct bf_cal *cal, const int32_t *left, const int32_t *center,
		 const int32_t *right, size_t count, struct bf_cal_result *out)
{
	if (!cal->active || count < (size_t)(2 * MAX_LAG + 2)) {
		return false;
	}

	/* Only learn from frames with enough energy: the wearer, not the room. */
	int64_t e = 0;

	for (size_t n = 0; n < count; n++) {
		e += (int64_t)center[n] * center[n];
	}
	if (e / (int64_t)count < MIN_ENERGY_PER_SAMPLE) {
		return false;
	}

	/*
	 * Stay inside the frame for the lagged index; dropping MAX_LAG samples
	 * at each edge of a 256-sample frame costs nothing worth a carry buffer.
	 */
	for (size_t n = MAX_LAG; n < count - MAX_LAG; n++) {
		int64_t l = left[n];
		int64_t r = right[n];
		int64_t lr = l + r;

		for (int k = -MAX_LAG; k <= MAX_LAG; k++) {
			cal->r_lr[k + MAX_LAG] += l * right[n + k];
		}
		for (int k = 0; k <= MAX_LAG; k++) {
			cal->c_lag[k] += (int64_t)center[n - k] * lr;
		}
		cal->e_left += l * l;
		cal->e_right += r * r;
		cal->e_center += (int64_t)center[n] * center[n];
	}

	if (++cal->voiced_frames < CONFIG_APP_BF_CAL_FRAMES) {
		return false;
	}

	finish(cal, out);
	cal->active = false;
	return true;
}
