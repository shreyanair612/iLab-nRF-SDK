#ifndef BEAMFORM_GSC_H_
#define BEAMFORM_GSC_H_

#include <stddef.h>
#include <stdint.h>

/*
 * Adaptive sidelobe canceller behind the delay-and-sum beamformer.
 *
 *   y[n] = delay-and-sum output                   (voice + leftover side noise)
 *   r[n] = (gL * L[n] - gR * R[n]) / 2            (side noise only: the mouth is
 *                                                  equidistant from L and R, so the
 *                                                  wearer's voice cancels here)
 *   e[n] = y[n] - sum_k w[k] * r[n - k]           (output)
 *   w   += mu * e[n] * r_vec / (|r_vec|^2 + eps)  (NLMS, only while the gate is open)
 *
 * The gate opens for a frame when the reference power is within
 * CONFIG_APP_GSC_GATE_DB of the primary power, i.e. an interferer is present.
 * During the wearer's own speech the reference is small, the weights freeze,
 * and a slight L/R mismatch cannot turn into speech cancellation. |w| is capped.
 *
 * tools/dsp_paths.py gsc_nlms() runs the same arithmetic on recordings.
 */

#define GSC_TAPS CONFIG_APP_GSC_TAPS

struct beamform_gsc {
	float w[GSC_TAPS];
	float hist[GSC_TAPS];   /* r[n], r[n-1], ... circular, newest at pos */
	uint32_t pos;
	float gain_l;
	float gain_r;
	float mu;
	float gate_ratio;       /* linear power ratio */
	float w_max;
	uint32_t frames;        /* frames processed, for the adapt-every-N duty cycle */
	uint32_t adapt_frames;  /* frames in which the gate was open (telemetry) */
};

void gsc_init(struct beamform_gsc *g);

/*
 * Processes one frame in place: `y` holds the delay-and-sum output (24-bit
 * domain, as produced in beamform.c) and is overwritten with the cancelled
 * output. `left` / `right` are the raw 24-bit mic samples of the same frame.
 */
void gsc_process(struct beamform_gsc *g, int32_t *y, const int32_t *left, const int32_t *right,
		 size_t count);

#endif
