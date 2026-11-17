#ifndef BEAMFORM_CAL_H_
#define BEAMFORM_CAL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Per-wearer alignment calibration.
 *
 * Triggered right after a wake-word detection, when the next second of loud
 * audio is known to be the wearer. Over CONFIG_APP_BF_CAL_FRAMES voiced frames
 * it accumulates, on the raw 24-bit mic samples:
 *
 *   r[k]  = sum L[n] * R[n + k]        k = -MAX_LAG .. +MAX_LAG   (L/R timing)
 *   c[k]  = sum C[n - k] * (L[n] + R[n])  k = 0 .. MAX_LAG       (centre lead)
 *   EL, ER                                                       (L/R gain)
 *
 * and turns them into the three numbers that vary from wearer to wearer:
 * which of L/R to hold back and by how much (crooked frame), how far to hold
 * the centre mic back (face shape), and the R gain trim that makes L - R
 * cancel the wearer's voice. About 10 multiply-adds per sample while active,
 * nothing otherwise.
 */

#define BF_CAL_MAX_LAG CONFIG_APP_BF_CAL_MAX_LAG

struct bf_cal_result {
	bool valid;
	uint8_t delay_left;      /* samples to hold the left mic back     */
	uint8_t delay_right;
	uint8_t delay_center;
	float gain_right;        /* multiply R by this to match L          */
	float coherence;         /* 0..1, how trustworthy the L/R estimate is */
	int8_t lr_lag;           /* raw estimate, for logging              */
};

struct bf_cal {
	bool active;
	uint32_t voiced_frames;
	int64_t r_lr[2 * BF_CAL_MAX_LAG + 1];
	int64_t c_lag[BF_CAL_MAX_LAG + 1];
	int64_t e_left;
	int64_t e_right;
	int64_t e_center;
};

void bf_cal_begin(struct bf_cal *cal);

/*
 * Feeds one frame of raw 24-bit samples. Returns true when enough voiced
 * frames have been seen; `out` is then filled and the calibration stops.
 */
bool bf_cal_feed(struct bf_cal *cal, const int32_t *left, const int32_t *center,
		 const int32_t *right, size_t count, struct bf_cal_result *out);

#endif
