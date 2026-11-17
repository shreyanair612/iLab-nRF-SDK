# 3-Mic Beamformer Prototype for Vision-Impairment Assistive Glasses

Same pipeline as `13_finalProto`, with the 2-mic INMP441 I2S capture replaced by three ICS-52000 mics (left / center / right) daisy-chained on one TDM bus and combined with a configurable delay-and-sum beamformer (`src/beamform.c`, `menu "3-mic TDM beamformer"` in Kconfig).

## Adaptive sidelobe canceller (`CONFIG_APP_BF_GSC`, off by default)

`src/beamform_gsc.c` adds one stage after the delay-and-sum:

```
y = (L + C + R) / 3                 delay-and-sum, the wearer's voice plus leftover side noise
r = (gL*L - gR*R) / 2               side noise only: the mouth is equidistant from L and R,
                                    so the wearer's voice cancels here at every frequency
out = y - w[16 taps] * r            NLMS; the weights only update while r is loud relative
                                    to y (an interferer is present), and |w| is capped
```

This is what gives side rejection below 1 kHz, where three mics 70 mm apart
cannot do it by summing. Cost is about 35 multiply-adds per sample (< 0.5 % of
the M33). Parameters are in `menu "3-mic TDM beamformer"`: taps, step size,
gate threshold, weight cap, update duty cycle and the two L/R gain trims (set
them from a pink-noise calibration run so L and R match; a mismatch leaks
voice into `r`). The same arithmetic runs on the host as the `gsc_nlms` path in
`16_3mic_eval`, so it can be tuned on recordings before enabling it here:

```
west build -b nrf54lm20dk/nrf54lm20b/cpuapp -- -DCONFIG_APP_BF_GSC=y
```

## Per-wearer fit (`CONFIG_APP_BF_CAL`, on by default)

Right after each wake-word detection the next ~0.6 s of loud audio is known to
be the wearer. `src/beamform_cal.c` accumulates, over 40 voiced frames, the
left/right cross-correlation at lags −2..+2, the centre-vs-(L+R) correlation at
lags 0..2 and the L/R energies, then sets:

- which of L/R to hold back and by how much (the frame sitting crooked),
- how far to hold the centre mic back (face shape; default 1 sample),
- the R gain trim that makes `L − R` cancel the wearer (feeds the canceller).

The symmetric-mount prior wins unless another lag beats it by 5 %, and a fit
whose L/R coherence is below 0.5 is discarded with a warning in the log. The
host runs the same estimator as `das_auto` / `gsc_auto` in `16_3mic_eval`.

Why these stages and not fractional delays / continuous self-alignment /
band-splitting: the cost analysis in `../16_3mic_eval/BEAMFORMING_NOTES.md` §5.

This project is a local, embedded voice interface for Nordic Semiconductor hardware running Zephyr/nRF Connect SDK. The device detects a wake word or start-button press, records one spoken utterance, automatically stops after sustained silence or manually upon a stop-button press, and plays the audio back. It is meant to be a prototype for the audio interface on iLab's Assistive Glasses.

## Features

- Local wake-word activation
- Start and manual Stop buttons
- Microphone capture and PCM recording
- RMS-based voice activity detection (VAD)
- Speech onset detection
- Automatic endpointing after a configurable pause
- Audio playback
- Runtime logging for state changes and VAD tuning
- No cloud service required for normal operation

## Behavior

```text
IDLE
  -> Wake word or Start button
  -> WAITING_FOR_SPEECH
  -> RECORDING
  -> Silence endpoint
  -> FINALIZE
  -> PLAYBACK
  -> IDLE
```
 ## Test Procedure

1. Press Start and stay silent: the device should wait, not stop.
2. Speak a sentence: recording should start and continue through short pauses.
3. Stop speaking: recording should end automatically after the configured pause.
4. Press Stop during recording: recording should end safely as a manual stop.
5. Trigger using the wake word: behavior should match Start-button activation.
6. Repeat multiple utterances: verify endpoint state and buffers reset correctly.
7. Listen to playback: verify normal speed, pitch, channels, and no clipping or stale audio.

