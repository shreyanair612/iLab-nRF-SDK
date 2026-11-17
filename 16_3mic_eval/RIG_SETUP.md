# Test rig setup (3-mic, hands-free)

From a bare bench to a finished comparison report. Commands run from the
`16_3mic_eval` folder in Terminal.

```
cd ~/Documents/iLAB/nRF_SDK/16_3mic_eval
```

## 1. One-time software setup (Mac)

```
pip3 install pyserial numpy matplotlib
tools/rig.sh clip --speech-dir ../14_ww_eval/test_samples/hey_vision
```

The second command builds `clips/standard.wav` (about 16 s) and `clips/standard_cues.csv`:

| Part | Length | Used for |
|---|---|---|
| silence | 1 s | noise floor |
| pink noise | 3 s | beam pattern (level vs angle) and the angle check |
| 4 × "Hey Vision" | ~6 s | speech SNR |
| log sweep 200 Hz–7 kHz | 4 s | frequency response / beam pattern vs frequency |
| silence | 1 s | |

Without `--speech-dir` it uses the Mac's voice. To use more or other recordings:
`tools/rig.sh clip --speech-dir recordings/people --speech-count 6 --name people`
and then pass `--clip clips/people.wav` to the test command.

## 2. Wire the microphones

Three ICS-52000 breakouts. Everything shares SCK and SD; the frame sync is a chain:

```
DK P1.23 -> SCK of all three
DK P1.13 -> SD  of all three, and a 100k resistor from SD to GND
DK P1.22 -> WS of the LEFT mic
LEFT  WSO -> WS of the CENTER mic
CENTER WSO -> WS of the RIGHT mic
RIGHT WSO -> nothing
3V3 -> VDD, GND -> GND on all three
```

Mount them in a line, capsules facing forward, and **measure the left-to-right
capsule spacing in mm** — that number goes into every test command as `--spacing-mm`.

## 3. Flash

1. USB cable from the DK's **IMCU** port to the Mac, POWER switch on.
2. `tools/rig.sh build` then `tools/rig.sh flash`.
3. Second USB cable from the DK's **nRF USB** (USB-C) port to the Mac. LED2 lights
   whenever a program on the Mac has that port open.

## 4. First power-on check

```
tools/rig.sh monitor
levels on
```

One line a second with `left[rms ..] center[rms ..] right[rms ..]`. Gently rub a
fingertip over each capsule's sound hole in turn: only that mic's numbers should
jump. If the order is wrong, the WS→WSO chain is wired in a different order than
you think — either rewire, or set `CONFIG_EVAL_SLOT_LEFT/CENTER/RIGHT` in `prj.conf`
to match and rebuild. A mic stuck at 0 has a broken WS→WSO link; all three pinned
at a huge value means the slot bit alignment is off (`CONFIG_EVAL_SLOT_BIT_OFFSET=1`).

```
levels off
start
```

LED1 lights for 20 s and a `RESULT` line prints when the run ends. Ctrl+C to leave.

## 5. Build the physical rig

Same platform as before: base → scissor jack → fixed plate → lazy Susan → top plate,
breadboard and DK fixed with Dual Lock.

- **Origin**: the center microphone's capsule, directly over the lazy Susan's axis.
- **Markings**: forward arrow on the top plate; arcs at 30 / 60 / 100 cm from the origin
  to the front of the speaker driver, with 0°, ±30°, ±60°, ±90°, 180° on each. Right of
  the array is positive.
- **Speaker** on its own stand, driver centre at capsule height, pointing at the origin,
  connected to the Mac (cable or Bluetooth — whichever, it must be the Mac's default
  output, because `afplay` plays through that).
- Tape both USB cables to the plate and the table so turning the platform cannot tug them.

## 6. Set the loudness (each session)

Speaker at 0°, 60 cm. In Terminal: `afplay clips/standard.wav` and, while the pink
noise plays (seconds 1–4), read a sound-level meter held at the microphones. Set the
speaker volume for **65 dB** and tape the knob. Write the reading down.

## 7. Run a session

```
tools/rig.sh test --session day1_quiet --angles 0,30,60,90,-30,-60,-90 --distance 60 --spacing-mm 140 --env quiet
```

Then, for each angle, the Mac tells you what to do:

> "Test 1 of 7. Set the rig to 0 degrees, then press button 1."

Turn the platform, press **BTN1** on the DK, step back. About 20 s later:

> "Done. 6 left."

If it says "That recording was degraded. Repeating the same angle." something
was lost on the way (USB unplugged, speaker not playing) — just press BTN1 again.

Useful options:

| Option | Meaning |
|---|---|
| `--angles -90..90:15` | every 15° from −90° to +90° |
| `--repeat 2` | each angle twice (averaged in the report) |
| `--advance auto --auto-seconds 15` | no button: the Mac counts down 15 s and starts by itself |
| `--delays 0,1,0` | the device's own beamformer delays (left, center, right); the host computes the others anyway |
| `--env loud` | label for the session; run the noise speaker as in `14_ww_eval/RIG_SETUP.md` §7 |
| `--no-voice` | print instead of speaking |

Ctrl+C stops the loop; everything recorded so far is already saved and analysed.

## 7b. The two-speaker test (for the adaptive canceller)

A single speaker on the turntable shows each path's *pattern* but cannot show
*cancellation*: the canceller needs the wearer and an interferer at the same
time. Once per session:

1. Speech speaker at 0°, 60 cm (or at the mouth position, 12 cm below and in
   front of the bridge, which is the case that matters).
2. Noise speaker at 90°, 60 cm, playing a second person talking (a podcast is
   fine) at the same 65 dB, on loop, from a phone.
3. `tools/rig.sh test --session day1_twospk --angles 0 --repeat 3 --env two_speaker`

In `session_report.html` compare `snr_front` for `das` and `gsc_nlms`: the
canceller should be several dB higher. If it is *lower*, it is cancelling the
wearer: raise `CONFIG_APP_GSC_GATE_DB`, and check the L/R gain trims against the
per-mic levels in `analysis.json` (`raw_left` and `raw_right` `noise_level_dbfs`
should match within 0.5 dB at 0°).

## 8. Read the results

`open sessions/day1_quiet/session_report.html`

- **Ranking table** at the top: `score = side_rejection + (snr_front − snr_side_mean)`.
  Higher is better. `raw_center` is the control — anything that scores below it is
  making things worse.
- **polar_noise.png**: the beam pattern of every path, relative to its own 0° level.
  A good beamformer dips at ±60–90°; a single mic is a circle.
- **snr_vs_angle.png**: speech SNR vs angle. Want high at 0°, falling toward the sides.
- **beam_vs_freq_<path>.png**: angle × frequency. Shows *where in the spectrum* the
  beamformer helps (expect: little below ~1 kHz, most above 2 kHz, and odd lobes above
  c/2d ≈ 2.4 kHz for 140 mm spacing).
- **angle_check.png**: the angle the mics heard vs the rig setting. Points off the
  diagonal mean a wrong `--spacing-mm`, a swapped left/right, or a misread marking.
- Each test's folder has its own `report.html` with waveforms, spectrograms, the
  per-path numbers and players for every path's audio.

## 9. Trying a new beamformer idea

Add a function in `tools/dsp_paths.py`, register it in `build_paths()`, then:

```
for d in sessions/day1_quiet/runs/*/; do tools/rig.sh analyze "$d"; done
tools/rig.sh report sessions/day1_quiet
```

No new recordings needed — it is scored on the same audio as everything else.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `No USB serial port found` | Both cables plugged in? The audio link is the **nRF USB** (USB-C) port, not IMCU. |
| Ports found but none answered | Firmware flashed? Press RESET. `tools/rig.sh monitor --port /dev/cu.usbmodemXXXX` to look. |
| "clip alignment is weak" | The speaker was silent or the Mac's output went elsewhere. Check System Settings → Sound → Output, play `afplay clips/standard.wav`. |
| "microphones hear the source at +X but the rig says Y" | Left/right swapped (see §4), wrong `--spacing-mm`, or the platform marking. Also expected near ±90° where the estimate saturates. |
| Audio gaps / frames lost | USB hub? Plug the nRF USB cable directly into the Mac. Check `tx_dropped` in `run_metadata.json`. |
| Nothing on `levels` | Check the TDM wiring and the 100k pulldown; look at uart20 (IMCU port, 115200) for `I2S` errors. |
