# Delay-and-sum vs differential beamforming for the glasses array

Research notes, October 2026. Numbers marked *(calc)* are from the standard
free-field formulas for a 3-element line array / first-order differential pair
(c = 343 m/s, ideal mics, no head or frame diffraction); the rest are from the
sources at the bottom. Treat the *(calc)* tables as estimates to be checked on
the rig, not as measurements.

## Our geometry

Left / center / right along the frame, ~70 mm between the center and each side
mic (~140 mm end to end). The wearer's mouth is 10–15 cm below and in front of
the bridge: a **near-field, broadside** source. Anything from the sides or behind
is what we want to suppress.

## 1. Delay-and-sum (what we run now)

*(calc)* 3 mics, d = 70 mm, broadside look:

| f (Hz) | DI (dB) | −3 dB beamwidth | gain at 90° |
|---|---|---|---|
| 300 | 0.1 | ~180° | −0.4 dB |
| 500 | 0.4 | ~180° | −1.2 dB |
| 1000 | 1.5 | ~99° | −5.6 dB |
| 2000 | 4.1 | ~45° | −12.9 dB |
| 2450 | c/(2d): grating lobe begins | | |
| 3000 | 5.4 | ~29° | −15 dB, but a 0 dB grating lobe near ±55° |
| 4000 | 6.2 | ~22° | −4.4 dB (grating lobe toward the side) |

- Below ~1 kHz the aperture is smaller than half a wavelength: DAS is essentially
  omni. Most speech and room-noise energy lives there.
- Between 1 and 2.4 kHz it works as intended.
- Above 2.45 kHz (= c/2d for 70 mm) spatial aliasing brings sound from the sides
  back at full level at some frequencies. `beam_vs_freq_das.png` from the rig shows
  exactly this band structure.
- White-noise gain +4.8 dB: averaging three mics *lowers* self-noise and
  uncorrelated wind noise. No calibration needed; ±3 dB mic mismatch costs almost
  nothing.
- Near field: the mouth is ~19 mm closer to the center mic than to the side mics
  (≈ 0.9 sample at 16 kHz, 1.3 dB). A 1-sample delay on the center mic (`das_c1`)
  is the only correction worth trying; the rig measures whether it helps.

## 2. Differential (first-order: dipole / cardioid / hypercardioid)

Pattern is frequency-independent **only while the pair spacing is much less
than half a wavelength** (textbook spacing 10–20 mm for 300–4000 Hz). DI: dipole
and cardioid 4.8 dB, supercardioid 5.7, hypercardioid 6.0; second order ≈ 9.5 dB.

The costs:

- The on-axis response rises 6 dB/octave and must be equalised back to flat with a
  low-shelf; that equaliser amplifies mic self-noise. *(calc)* white-noise gain for a
  20 mm pair: −16 dB at 300 Hz, −12 dB at 500 Hz, −6 dB at 1 kHz. For a 10 mm pair,
  6 dB worse again.
- Needs the two mics matched to ~0.2–0.5 dB in gain and a few degrees in phase at
  500 Hz (ICS-52000 is spec'd ±1 dB, better than most MEMS, but still needs a
  per-unit calibration stored in flash). Mismatch fills in the null.
- Wind and handling noise are *worse* than omni (hearing-aid wind-tunnel studies);
  the standard fix is to fall back to omni below ~1 kHz when wind is detected.
  Glasses outdoors are exactly that case.
- An unequalised differential output has a +6 dB/oct tilt, which an omni-trained
  wake-word model has never seen. Amazon's work on front-end gain invariance for
  wake words shows even flat ±12 dB gain changes cost 19–34 % relative FAR/FRR; a
  spectral tilt is worse. Equalise, or retrain on beamformed audio.

**At our 70 mm spacing a first-order differential does not work as a broadband
design**: the cardioid's null lands at c/(2d) = 2.45 kHz (a dead band around
2–2.9 kHz on axis) and above it the pattern flips front-to-back. It is only
cleanly first-order up to ~c/(4d) ≈ 1.2 kHz.

**And the geometry is wrong for it**: differential arrays are end-fire. For a
L–R pair the look direction would be along the frame (toward the ear), and the
mouth at broadside sits *in the dipole's null*. That is why `diff_lr` in the
evaluation is used as the "everything except the wearer" reference, not as a
candidate output.

Near field: first-order gradient mics do get a proximity boost for a close talker
(the reason headset booms are gradient mics), but it only matters within a few cm
of the mouth, and the pair would have to point at the mouth (down / forward from
the bridge), not along the frame.

## 3. Compute, for completeness

Both are trivial on the Cortex-M33: DAS is three multiply-adds per sample;
first-order differential is one delay line, one subtract and one biquad (plus a
3–4 tap fractional-delay interpolator for τ = d/c = 0.93 samples at 20 mm). A
2nd-order DMA or an adaptive GSC is still well under 10 MHz. Cost is not what
decides this.

## 4. Recommendation

**Keep delay-and-sum as the baseline. Do not replace it with a 70 mm L–R
differential.** The experiment worth running on the current hardware is the
cheap fixes to DAS, then an adaptive stage:

1. **Measure first** with `16_3mic_eval` at 0, ±30, ±60, ±90°: `raw_center` vs
   `lr_mix` vs `das` vs `das_c1`. The beam-vs-frequency maps will show the
   below-1 kHz omni region and the above-2.4 kHz grating lobes directly.
2. **Tame the aliasing**: either move the side mics to ~40 mm from the center
   (c/2d → 4.3 kHz, no grating lobe in the speech band) or keep 70 mm and low-pass
   the side-mic contribution above ~2.4 kHz (center mic alone at HF). A 0.5/1/0.5
   weighting lowers sidelobes at ~1 dB DI cost (`set_gain 16384 32767 16384`).
3. **Add null steering, not differential**: the L−R dipole contains *only* off-axis
   sound. `gsc` in the evaluation (DAS minus what a short filter predicts from L−R)
   is the offline upper bound of a generalized-sidelobe-canceller; if it wins
   clearly on the rig, an NLMS version of it (16 taps, ~1 MHz) is the next
   firmware step. This is what commercial glasses do (Meta's 5-mic array and the
   eyewear patents all describe adaptive multi-mic cancellation, not fixed
   differential pairs).

**If you still want to try differential, do it like this:** add a 4th mic 15–20 mm
behind or below the center mic (a front/back or up/down pair aimed at the mouth),
y[n] = x_front[n] − x_back[n − τ] with τ = d/c ≈ 0.93 samples at 20 mm (fractional
delay; τ = 0.58·d/c for hypercardioid), a first-order low-shelf with corner
≈ c/(4πd) ≈ 1.4 kHz capped at +15 dB plus a 150–200 Hz high-pass, per-unit gain
calibration, and a coherence-based wind guard that falls back to DAS. Measure it on
the same rig: flat on-axis response ±2 dB after EQ, null depth > 15 dB at 500 Hz
and 1 kHz, and the silence floor (the WNG penalty) against the room floor you care
about.

## 5. Decision and what was implemented (27 Oct 2026)

### Cost analysis

Loss in the 3-mic sum from the centre mic leading by τ samples, weighted by
the long-term average speech spectrum *(calc)*:

| centre lead τ | loss at 4 kHz | speech-weighted loss |
|---|---|---|
| 0.9 (no delay, nominal frame) | 2.0 dB | 0.06 dB |
| 0.1 (integer delay 1) | 0.02 dB | 0.001 dB |
| 0.4 (integer delay 1, worst plausible frame) | 0.4 dB | 0.01 dB |
| 1.3 (no delay, wide frame) | 4.5 dB | 0.12 dB |

Over every plausible frame (mouth drop 100–140 mm, half-spacing 60–80 mm) the
centre lead is 0.57–1.31 samples, so an integer delay of 1 is within 0.43
samples everywhere: at most 0.012 dB speech-weighted.

Compute at 16 kHz on the M33 (≈2.2 cycles per multiply-add, ≈47 µA/MHz):

| option | MAC/sample | % of 128 MHz | ≈ current | speech benefit |
|---|---|---|---|---|
| DAS + integer centre delay | 3 | 0.1 % | 5 µA | baseline |
| + fractional 8-tap delay | +8 | 0.3 % | +18 µA | +0.001 dB |
| + continuous self-alignment search | +13 | 0.7 % | +40 µA | ≤ +0.01 dB |
| + band-split biquad | +5 | 0.2 % | +13 µA | pattern only; canceller covers it |
| + adaptive canceller (GSC, 16 taps) | +32 | 1.0 % | +58 µA | +4 dB SNR in sim, 5–15 dB in literature |
| + per-wearer fit after each wake word | ~10 for 0.6 s per wake word | ~0 | ~0 | robustness, see below |

The three ICS-52000 mics draw on the order of 1 mA each; the wake-word NPU and
BLE are larger still. DSP is not where the power goes, so the choice is made on
benefit and risk, not microamps.

### Decision

1. **Delay-and-sum with the centre mic held back 1 sample** (`CONFIG_APP_BF_DELAY_CENTER=1`,
   now the default). Free; captures 99 % of the alignment benefit for every frame.
2. **Adaptive sidelobe canceller** (`CONFIG_APP_BF_GSC`): the only option that adds
   rejection where speech energy is (below 1 kHz). Off by default until the
   two-speaker rig test shows it does not touch the wearer's voice.
3. **Per-wearer fit after each wake word** (`CONFIG_APP_BF_CAL`, default on). The
   wake word proves the next second of loud audio is the wearer, so the beamformer
   measures, from that audio, the three numbers that vary between wearers and fits
   of the frame: which of L/R to hold back and by how much (crooked frame, ±2
   samples), the centre lead (face shape), and the R gain trim that makes `L − R`
   cancel the wearer. The symmetric-mount prior (lag 0, centre 1) is kept unless
   another lag beats it by 5 %, and a fit with L/R coherence below 0.5 is
   discarded. Costs ~10 MAC/sample for ~0.6 s per wake word, nothing in between.
4. **Rejected:** fractional delay and continuous self-alignment (≤ 0.01 dB for
   58 µA and two more things to tune), band-split (redundant with the canceller's
   1 ms of taps, which cover the 0.41 ms maximum L–R delay).

Total shipping cost ≈ 35 MAC/sample, about 1 % of the core.

### Verification so far

- Host `gsc_nlms` and firmware `beamform_gsc.c`: < 8 LSB difference, correlation
  0.999999 on a 17 s recording.
- Host `estimate_alignment` and firmware `beamform_cal.c`: identical lag, delay
  and gain decisions on three simulated recordings (0°, 30°, 90°).
- Simulation with a side interferer: speech SNR at 0° 13.8 dB (`das`) → 17.9 dB
  (`gsc_nlms`) / 17.9 dB (`gsc_auto`), against an offline optimum of 18.2 dB.

Note on the rig: `das_auto` / `gsc_auto` fit to whatever speech is in the
recording, so in an angle sweep they *steer toward the speaker* — judge them
only on the mouth-position and two-speaker tests, where the speech is where the
wearer's mouth is.

What would change the decision: `gsc_nlms` or `gsc_auto` scoring a lower
`snr_front` than `das` on the two-speaker test (gate or trims wrong), or the
wearer fit logging coherence < 0.5 on real wake words (raise
`CONFIG_APP_BF_CAL_FRAMES` or `CONFIG_APP_BF_CAL_MIN_RMS`).

Second reference for later: `(L + R)/2 − C'` is also voice-free once the centre
is aligned, and sees front/back and up/down differences that `L − R` cannot.
Add it only if the rig shows noise from behind the head getting through.

## Sources

- Elko, "Superdirectional Microphone Arrays", in *Acoustic Signal Processing for Telecommunication* (Springer 2000) — https://link.springer.com/chapter/10.1007/978-1-4419-8644-3_10
- Benesty & Chen, *Study and Design of Differential Microphone Arrays* (Springer) — https://www.springerprofessional.de/study-and-design-of-differential-microphone-arrays/3988454
- MathWorks, "Introduction to Differential Beamforming" (WNG < 0 dB, d ≈ λ/10) — https://www.mathworks.com/help/phased/ug/introduction-to-differential-beamforming.html
- InvenSense AN-1140, "Microphone Array Beamforming" — https://www.mouser.lt/pdfDocs/Microphone-Array-Beamforming-3.pdf
- ST DT0117, "Microphone array beamforming in the PCM and PDM domain" — https://www.st.com/resource/en/design_tip/dt0117-microphone-array-beamforming-in-the-pcm-and-pdm-domain-stmicroelectronics.pdf
- Teutsch & Elko, "An Adaptive Close-Talking Microphone Array", WASPAA 2001
- Knowles AN-18, directional / close-talk microphones — https://www.knowles.com/docs/default-source/default-document-library/an-18-issue00.pdf
- US 11070907 (MEMS mismatch degrades a 1 cm cardioid) — https://patents.google.com/patent/US11070907
- Phase Error Analysis for First-Order Linear DMAs, IWAENC 2022
- Widrow, "A microphone array for hearing aids", JASA 2001
- Chung et al., wind noise in directional vs omni hearing aids, JAAA 2011 — https://www.thieme-connect.com/products/ejournals/pdf/10.3766/jaaa.22.9.4.pdf
- US 9753311, eyeglasses with microphone array — https://patents.google.com/patent/US9753311
- Amazon, "On front-end gain invariant modeling for wake word spotting", Interspeech 2020 — https://www.amazon.science/publications/on-front-end-gain-invariant-modeling-for-wake-word-spotting
