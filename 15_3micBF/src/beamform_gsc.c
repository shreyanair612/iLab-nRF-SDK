#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "beamform_gsc.h"

#define GSC_MASK  (GSC_TAPS - 1)
#define EPS       1e-6f

/* 24-bit mic domain <-> float [-1, 1), so the Kconfig thresholds are level-independent. */
#define SCALE     (1.0f / 8388608.0f)
#define UNSCALE   8388608.0f

#if (GSC_TAPS & (GSC_TAPS - 1)) != 0
#error "CONFIG_APP_GSC_TAPS must be a power of two"
#endif

void gsc_init(struct beamform_gsc *g)
{
	memset(g, 0, sizeof(*g));
	g->gain_l = CONFIG_APP_GSC_GAIN_LEFT_Q15 / 32768.0f;
	g->gain_r = CONFIG_APP_GSC_GAIN_RIGHT_Q15 / 32768.0f;
	g->mu = CONFIG_APP_GSC_MU_X1000 / 1000.0f;
	/* opens when P(ref) > P(y) * 10^(-GATE_DB/10) */
	g->gate_ratio = powf(10.0f, -(float)CONFIG_APP_GSC_GATE_DB / 10.0f);
	g->w_max = CONFIG_APP_GSC_W_MAX_X100 / 100.0f;
}

void gsc_process(struct beamform_gsc *g, int32_t *y, const int32_t *left, const int32_t *right,
		 size_t count)
{
	float py = 0.0f;
	float pr = 0.0f;
	float ref[256];

	if (count > 256U) {
		count = 256U;
	}

	/* Reference for the frame, plus the two powers that drive the gate. */
	for (size_t i = 0; i < count; i++) {
		float yl = (float)y[i] * SCALE;

		ref[i] = 0.5f * (g->gain_l * (float)left[i] - g->gain_r * (float)right[i]) * SCALE;
		py += yl * yl;
		pr += ref[i] * ref[i];
	}

	bool adapt = (pr > g->gate_ratio * py) &&
		     ((g->frames++ % CONFIG_APP_GSC_ADAPT_EVERY_N_FRAMES) == 0U);

	if (adapt) {
		g->adapt_frames++;
	}

	for (size_t i = 0; i < count; i++) {
		g->pos = (g->pos + 1U) & GSC_MASK;
		g->hist[g->pos] = ref[i];

		/* e = y - w . hist, hist[k] = r[n-k] */
		float est = 0.0f;
		float norm = 0.0f;

		for (uint32_t k = 0; k < GSC_TAPS; k++) {
			float h = g->hist[(g->pos - k) & GSC_MASK];

			est += g->w[k] * h;
			norm += h * h;
		}

		float e = (float)y[i] * SCALE - est;

		if (adapt) {
			float step = g->mu * e / (norm + EPS);

			for (uint32_t k = 0; k < GSC_TAPS; k++) {
				g->w[k] += step * g->hist[(g->pos - k) & GSC_MASK];
			}
		}

		float out = e * UNSCALE;

		if (out > 8388607.0f) {
			out = 8388607.0f;
		} else if (out < -8388608.0f) {
			out = -8388608.0f;
		}
		y[i] = (int32_t)out;
	}

	/* Keep the weights bounded so a bad frame cannot blow the filter up. */
	if (adapt) {
		float n2 = 0.0f;

		for (uint32_t k = 0; k < GSC_TAPS; k++) {
			n2 += g->w[k] * g->w[k];
		}
		if (n2 > g->w_max * g->w_max) {
			float s = g->w_max / sqrtf(n2);

			for (uint32_t k = 0; k < GSC_TAPS; k++) {
				g->w[k] *= s;
			}
		}
	}
}
