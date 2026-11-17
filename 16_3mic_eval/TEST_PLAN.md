# Test plan: is the 3-mic beamformer (DAS + per-wearer fit + GSC) better, and safe?

A result is *conclusive* when it was predicted before the test, measured on
the same audio for every candidate, repeated enough times to put an interval
on it, and would have been allowed to fail. This plan does that for five
claims. `tools/verdict.py` applies the criteria below to a session and prints
PASS / FAIL per claim with 95 % intervals; nothing here is judged by eye.

## 0. Rules that make it conclusive

1. **Criteria are fixed now**, in this file, before any recording. Changing a
   threshold after seeing data is a new experiment.
2. **Same audio for every path.** Every candidate is computed on the host from
   the one 3-channel recording, so no path ever gets an easier take.
3. **Repeats and randomised order.** Every condition is recorded 5 times
   (`--repeat 5`); the order of angles is shuffled per repeat
   (`--shuffle`). Intervals are bootstrap 95 % CIs over repeats.
4. **A control in every comparison.** `raw_center` (one mic, what the product
   would be with no array) and `das` (what ships today).
5. **The failure modes are tested, not just the wins.** Claims 3 and 4 exist
   to catch the two ways this design can hurt the wearer.
6. **Integrity first.** A session whose claim-1 checks fail is discarded, not
   interpreted.

## 1. Rig

Same platform as before (lazy Susan, arcs at 30/60/100 cm), plus:

- **Mouth position**: a second speaker mount 120 mm below and 25 mm in front of
  the centre mic, driver pointing up at the bridge. This is the wearer. Adjust
  to the real frame's numbers and write them in the session notes.
- **Two speakers from one stereo output.** A 3.5 mm stereo splitter: left
  channel → the "wearer" speaker (mouth position or 0° arc), right channel → the
  "interferer" speaker on the arc. The two-speaker clip is stereo, so the
  interferer is sample-synchronous with the speech and the cue sheet knows
  exactly when each is playing. Set both channels to 65 dB at the mics with the
  calibration noise (left then right) and tape the knobs.
- **Tilt jig**: a 10 mm shim under one temple, to make the frame sit crooked
  on purpose for claim 3.

## 2. Clips

```
tools/rig.sh clip --speech-dir recordings/wearer --name wearer
tools/rig.sh clip --speech-dir recordings/wearer --interferer-dir recordings/other --name twospk
```

`twospk.wav` is stereo: left = wearer, right = interferer, with segment kinds
`speech` (wearer only), `interf` (interferer only), `both`, `noise` (pink, both
channels), `sweep` and `silence`. Use real recordings of two different people.

## 3. Claims, conditions and pass criteria

### Claim 1 — the capture is trustworthy (integrity)

Condition: wearer clip, speaker on the 60 cm arc at 0, ±30, ±60, ±90°.

| check | criterion |
|---|---|
| no audio lost | `frames_lost == 0` and no WAV gaps in every run |
| mics hear the right angle | `tdoa.angle_from_lr_deg` within ±10° of the rig angle for |angle| ≤ 60° |
| left/right matched | `raw_left` and `raw_right` noise-burst level within 1.0 dB at 0° |
| device = host | `device_bf_matches_host == true` in every run |

Any failure: fix the rig, discard the session.

### Claim 2 — delay-and-sum beats one mic without hurting the wearer

Condition: same as claim 1.

| metric | criterion |
|---|---|
| side rejection | `side_rejection(das) − side_rejection(raw_center)` > 0, 95 % CI excludes 0, in the 1–4 kHz bands |
| no speech loss at 0° | `snr_front(das) ≥ snr_front(raw_center) − 0.5 dB` |
| aliasing is as predicted | at ±60/90°, `beam_vs_freq_das` is within 3 dB of the free-field prediction in `BEAMFORMING_NOTES.md` §1 for the 1.6–2.5 kHz bands (sanity check of geometry, not a pass/fail) |

### Claim 3 — the per-wearer fit measures the wearer, not the room

Condition A (centred): wearer clip from the mouth position, 5 repeats.
Condition B (crooked): same with the 10 mm shim under the left temple, then
under the right, 5 repeats each.
Condition C (distraction): condition A with the interferer at 90° playing
continuously at 65 dB.

| metric | criterion |
|---|---|
| A: finds centre | `wearer_fit.lr_lag == 0` in ≥ 90 % of runs; `delay_center == 1` in ≥ 80 %; `gain_right` within ±1 dB (0.89–1.12) |
| B: finds the tilt | `lr_lag` has the sign of the shim in ≥ 80 % of runs; magnitude ≥ 1 |
| C: not fooled | `lr_lag == 0` in ≥ 80 % of runs, or `valid == false` (rejected) — never a confident wrong lag |
| fit helps or is neutral | `snr_front(das_auto) ≥ snr_front(das) − 0.3 dB` in A, and `> snr_front(das)` in B with CI excluding 0 |

### Claim 4 — the canceller never harms the wearer (safety)

Condition: wearer clip from the mouth position, **no** interferer, 5 repeats;
then the same at 60 cm / 0°.

| metric | criterion |
|---|---|
| speech preserved | `speech_preservation(gsc_nlms)` (correlation with `das` over wearer-only segments) ≥ 0.98 in every run |
| no SNR loss | `snr_front(gsc_nlms) ≥ snr_front(das) − 0.5 dB`, CI of the difference excludes anything below −0.5 |
| gate stayed closed | device log: canceller adapted in ≤ 10 % of frames during wearer-only speech (`CONFIG_APP_BF_MIC_DEBUG` line) |
| no clipping | `clips == 0` on `gsc_nlms` |

Any single failure here blocks enabling `CONFIG_APP_BF_GSC` by default.

### Claim 5 — the canceller helps against a second talker (the point)

Condition: two-speaker clip, wearer at the mouth position, interferer on the
60 cm arc at 45°, 90° and 135°, 5 repeats each; quiet room, then with the
medium (55 dB) background from `14_ww_eval` §7.

| metric | criterion |
|---|---|
| interferer removed | `interf_rejection(gsc_nlms) − interf_rejection(das)` ≥ 6 dB in the 250–1000 Hz bands, CI lower bound ≥ 3 dB, at every interferer angle |
| wearer wins under interference | `snr_under_interference(gsc_nlms) − …(das)` ≥ 3 dB, CI lower bound > 0 |
| not worse than the bound | `gsc_nlms` within 3 dB of `gsc` (the offline optimum) — otherwise the step size or gate is mis-tuned |
| wake word still works | replay `gsc_nlms.wav` and `das.wav` through `14_ww_eval`; detections on `gsc_nlms` ≥ detections on `das` on the wearer's words, false triggers on the interferer's words ≤ `das` |

## 4. Session plan

| session | claims | runs | time |
|---|---|---|---|
| `s1_integrity_sweep` | 1, 2 | 7 angles × 5 | ~70 min |
| `s2_fit_centred` / `s2_fit_shimL` / `s2_fit_shimR` / `s2_fit_distract` | 3 | 5 each | ~40 min |
| `s3_safety_mouth` / `s3_safety_60cm` | 4 | 5 each | ~20 min |
| `s4_twospk_quiet` / `s4_twospk_medium` | 5 | 3 angles × 5 each | ~60 min |

Commands (each is one hands-free loop):

```
tools/rig.sh test --session s1_integrity_sweep --angles 0,30,60,90,-30,-60,-90 --repeat 5 --shuffle --distance 60 --clip clips/wearer.wav
tools/rig.sh test --session s2_fit_centred   --angles 0 --repeat 5 --distance 12 --label mouth   --clip clips/wearer.wav
tools/rig.sh test --session s2_fit_shimL     --angles 0 --repeat 5 --distance 12 --label shim_left  --clip clips/wearer.wav
tools/rig.sh test --session s2_fit_shimR     --angles 0 --repeat 5 --distance 12 --label shim_right --clip clips/wearer.wav
tools/rig.sh test --session s2_fit_distract  --angles 0 --repeat 5 --distance 12 --label distract --clip clips/wearer.wav   # interferer speaker on loop from a phone, 90°
tools/rig.sh test --session s3_safety_mouth  --angles 0 --repeat 5 --distance 12 --clip clips/wearer.wav
tools/rig.sh test --session s3_safety_60cm   --angles 0 --repeat 5 --distance 60 --clip clips/wearer.wav
tools/rig.sh test --session s4_twospk_quiet  --angles 45,90,135 --repeat 5 --shuffle --distance 60 --clip clips/twospk.wav
tools/rig.sh test --session s4_twospk_medium --angles 45,90,135 --repeat 5 --shuffle --distance 60 --env medium --clip clips/twospk.wav
```

In the two-speaker sessions `--angles` is the **interferer's** angle; the
wearer speaker stays at the mouth position.

Then:

```
tools/rig.sh verdict sessions/s1_integrity_sweep sessions/s2_* sessions/s3_* sessions/s4_*
```

## 5. Reading the verdict

`verdict.py` prints one block per claim: each criterion, the measured value,
the 95 % CI over repeats, and PASS / FAIL. The decision rule:

- Claims 1–2 pass → the array and the baseline are sound.
- Claim 3 passes → leave `CONFIG_APP_BF_CAL=y`. Fails → set it to `n` and file
  the fit numbers; the product still works with the fixed delays.
- Claim 4 passes **and** claim 5 passes → set `CONFIG_APP_BF_GSC=y` by default.
- Claim 4 fails → the canceller stays off, whatever claim 5 says. Tune
  `CONFIG_APP_GSC_GATE_DB` upward and the gain trims, re-run session s3 only.
- Claim 5 fails with claim 4 passing → the canceller is safe but not useful at
  this geometry; keep it off, and the next experiment is the second reference
  `(L + R)/2 − C'` (`BEAMFORMING_NOTES.md` §5).

Everything in a session is re-scorable offline, so a changed threshold or a
new DSP idea is a re-run of `analyze` + `verdict`, not a new day on the rig.
