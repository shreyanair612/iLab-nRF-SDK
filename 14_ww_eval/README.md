# 14_ww_eval — wake-word beamforming evaluation

Firmware plus a local dashboard for one question: **does beamforming improve
wake-word detection compared with a single raw microphone?**

The device captures a synchronized two-microphone frame, derives every enabled
"path" from that same frame (raw left, raw right, L+R mix, beamformed L+R, and
the three-microphone variants once a center microphone exists), scores paths
with the unmodified on-device "Hey Vision" model, and streams audio, metrics and
detection events to a laptop over the DK's serial link. The host records each
run in its own folder with WAV files, CSV/JSON data and a static report, and
shows everything at `http://localhost:5000`.

## 1. What is being measured

For each physical experiment (one speaker position, one playback clip) and each
path, the system records:

- wake-word detections with their sample-accurate timestamps and scores
- the model's score statistics per path (peak, mean, min, std)
- input signal level, peak and clipping per path
- inference time per window and deadline misses
- capture drops, link drops, and audio gaps

Everything else, precision, recall, F1 and latency, requires ground-truth
timestamps of when the wake word was actually spoken. Section 13 explains how to
add them offline; the firmware never fabricates them.

## 2. Why raw versus beamformed matters

A wearable microphone array only earns its complexity if a spatial filter makes
the wake-word model fire more reliably on the wearer and less on other people.
Comparing a beamformed path against the raw microphones **on the same audio,
with the same model, threshold and post-processing** is the only fair way to
answer that. This project guarantees the "same audio" part by construction.

## 3. Base project and why

Built on **13_finalProto** (nRF54LM20 DK, nRF Connect SDK / Zephyr, nRF Edge AI
wake-word runtime), the only project in the workspace that has all of: 32-bit
I²S capture of two INMP441 microphones, the beamformer prototype, the "Hey
Vision" model with its exact preprocessing, and a working state machine. Its
capture code, wake-word post-processing, board files and build configuration
are reused unchanged in spirit; the Bluetooth path is dropped in favour of a
serial link that can carry raw audio.

Findings from the inspection that shaped the design:

| Item | Finding |
|---|---|
| Chip / board | nRF54LM20B on `nrf54lm20dk/nrf54lm20b/cpuapp` (secure target; the `/ns` target cannot link the Axon driver) |
| SDK | nRF Connect SDK 3.3.x with Zephyr 4.3; the Edge AI module lives only in `~/Documents/iLAB/nrfedgeAI_workspace`, so builds run from there |
| Microphones | 2× INMP441 on one I²S/TDM data line, 16 kHz, 24-bit in 32-bit slots. Slot 0 = LEFT (L/R pin to GND), slot 1 = RIGHT |
| Frame | 256 samples per channel, 16 ms, via DMA into a memory slab |
| Wake word | model 94081, 160-sample (10 ms) windows, 1 output class, vote 4-of-6, cooldown 1200 ms |
| **Runtime** | **one global, stateful instance**: the audio front end (`FrontendProcessSamples`) keeps overlap and noise state in a private static, the library exposes no save/restore or second instance |
| Transport | uart20 → J-Link VCOM. UARTE tops out at 1 Mbaud on this SoC, about 100 kB/s raw |
| RAM | 511 KB; this firmware uses 127 KB |

The runtime finding matters: feeding two paths into the model alternately would
mix their histories, so **the device scores exactly one path at a time**. A run
scores the *live path* (default `bf_lr`) in real time and streams the raw
microphones to the host. The host then replays that recording back through the
device once per remaining path, so every path is scored by the same model on
the same audio. Replay runs at roughly real time per path.

## 4. Hardware and channel map

```
INMP441 #1  L/R -> GND   ->  TDM slot 0  ->  LEFT
INMP441 #2  L/R -> VDD   ->  TDM slot 1  ->  RIGHT
SCK  P1.23      WS/FSYNC  P1.22      SD  P1.13      (boards/nrf54lm20dk_nrf54lm20b_cpuapp.overlay)
Telemetry: uart20 (J-Link VCOM), 1 000 000 baud, 8N1
Buttons: BTN1 start, BTN2 stop.  LED1 on while a run or replay is active, LED2 blinks on detection.
```

Confirm the channel order with the end-fire test in section 14 before trusting
any left/right result. A third (center) microphone cannot share the I²S data
line; Phase 2 adds it on the PDM peripheral.

## 5. Build and flash

```bash
cd ~/Documents/iLAB/nrfedgeAI_workspace
west build -d ~/Documents/iLAB/nRF_SDK/14_ww_eval/build ~/Documents/iLAB/nRF_SDK/14_ww_eval \
     --board nrf54lm20dk/nrf54lm20b/cpuapp --pristine
west flash -d ~/Documents/iLAB/nRF_SDK/14_ww_eval/build
```

Host-side checks that need no hardware:

```bash
cd ~/Documents/iLAB/nRF_SDK/14_ww_eval
make -C tests_host                      # C unit tests of the DSP/protocol objects + cross-check vectors
python3 -m unittest discover -s tools/tests -v
```

## 6. Serial monitor (no dashboard)

The device boots in **text mode**: readable lines, commands typed as text.

```bash
screen /dev/tty.usbmodem<ID> 1000000        # or minicom / PuTTY at 1000000 8N1
help
list_paths
set_preset raw_vs_bf_lr
set_duration 10
start
```

The run summary prints as a text block at the end (section 12). Audio is never
sent in text mode. `binary_mode on` switches to framed packets for the dashboard;
the dashboard does this automatically.

## 7. Start the dashboard

```bash
pip3 install pyserial                     # the only dependency
python3 tools/dashboard_server.py --port /dev/tty.usbmodem<ID> --baud 1000000 --web-port 5000
# no hardware: python3 tools/dashboard_server.py --simulate
```

Open <http://localhost:5000>. Runs are saved under `runs/` next to `tools/`
(override with `--runs-dir`). The page has: live run status, a start form, the
detection comparison table, a per-path detection timeline, audio artifacts with
players and downloads, beamformer settings and audio health, run history with a
two-run compare view, and a device console. The simulator injects one dropped
audio chunk when `WW_SIM_DROP=1` is set, to show what gap handling looks like.

## 8. Presets and paths

| Preset | Enabled paths |
|---|---|
| `left`, `right`, `lr_mix`, `bf_lr`, `raw_3ch`, `bf_3ch` | that one path |
| `raw_vs_bf_lr` (default) | raw_left, raw_right, raw_lr_mix, bf_lr |
| `raw_vs_bf_3ch` | raw_3ch, bf_3ch |
| `full` | all six |

`enable <path>` / `disable <path>` / `enable_all` / `disable_all` adjust the
set; `set_live_path <path>` chooses which enabled path is scored live. The
device rejects `start` if the live path is disabled or the link budget is
exceeded. Compile-time defaults live in `Kconfig` (`CONFIG_EVAL_*`,
`CONFIG_WW_*`) and `prj.conf`.

## 9. Angle, distance, labels, threshold, delays

```
set_angle 45                 set_distance 100           set_label angle_45_distance_100cm
set_phrase hey_vision        set_env lab_quiet
set_threshold 650            (or 0.65)                  set_cooldown 1200
set_delay bf_lr 3 0          left delayed 3 samples, right 0
set_delay bf_3ch 2 0 1       left, right, center
set_gain bf_lr 32767         Q15, 32767 = unity
```

Angle and distance are metadata typed by you; the device cannot sense them.
Delay sign convention: a positive delay holds that channel back. To steer
toward a source whose wavefront reaches the LEFT microphone first by *d*
samples, delay LEFT by *d*. At 16 kHz one sample is 62.5 µs, about 2.1 cm of
path difference. With all delays zero and unity gain the beamformer is the plain
average, identical to what 13_finalProto shipped.

## 10. Audio capture modes

`set_capture_mode <mode>` and `set_capture <stream> on|off`:

| Mode | Device streams | Host keeps |
|---|---|---|
| `off` | nothing | metrics and events only |
| `event_window` (default) | selected streams, continuously | full raw files **plus** ±`capture_seconds` cut-outs around each live detection (`raw_left_event01.wav` …) |
| `continuous_short` | selected streams | the first `capture_seconds` seconds |
| `continuous_full` | selected streams | everything |

The device always streams the selected raw channels continuously while a mode
other than `off` is active; the policy is applied on the host. The full raw
recording is kept in every mode except `continuous_short` because it is the
input for replay scoring and for rebuilding the processed paths.

**Link budget.** One 16-bit mono stream is 32 kB/s. The default 1 Mbaud link is
budgeted at 80 kB/s usable, so raw left + raw right fit; adding a processed path
live (`set_capture bf_lr on`) would exceed it and `start` refuses with the exact
numbers. You do not need to stream processed paths live: the host rebuilds
`raw_lr_mix.wav` and `bf_lr.wav` bit-exactly from the raw channels and proves it
by matching the device's CRC32 of the same path (`crc_match` in the summary),
and every replay pass additionally saves the device-generated audio
(`<path>_device_replay.wav`).

## 11. Where the WAV files are

`runs/<date>_<time>_angle045_distance100cm_<label>/`:

```
raw_left.wav  raw_right.wav  raw_stereo.wav        physical channels, plus event cut-outs in event_window mode
raw_lr_mix.wav  bf_lr.wav                          host reconstruction, CRC-verified against the device
raw_left_device_replay.wav  raw_right_device_replay.wav  raw_lr_mix_device_replay.wav
                                                   what the device actually fed the model during each replay pass
run_metadata.json  run_summary.csv  events.jsonl  metrics_timeseries.csv  audio_stats.csv
device_serial_log.txt  dashboard_export.html
```

The dashboard's audio section plays and downloads each file and marks any file
with gaps as incomplete.

## 12. Reading the results

The device prints this at the end of every run and replay pass, and the
dashboard shows the same fields:

```
Path         Enabled Scored Det  Peak  Mean  RMS    Clips AvgInf  MaxInf  Miss
raw_left     Yes     replay -    -     -     108    0     -       -       -
bf_lr        Yes     yes    3    0.918 0.105 96     0     2050    2400    0
...
Captured audio frames:    1500
Dropped capture frames:   0
Audio packet gaps:        0 (0 frames lost)
Result quality:           VALID FOR COMPARISON
```

- **Detections** are model firings after the vote and cooldown. Two paths with
  different counts on the same audio differ in *sensitivity*, not accuracy.
- **Peak / mean score** show how far above or below the threshold a path sits;
  a path that detects the same events with a higher mean margin is more robust.
- **RMS / clips**: clipping on `bf_*` means the sum saturated int16; lower the
  gain. Very different RMS between raw_left and raw_right means level mismatch
  or a bad microphone.
- **AvgInf / MaxInf / Miss**: inference time per 10 ms window and how often it
  exceeded the 10 ms deadline. Misses mean the live path could not keep up and
  windows were skipped; the run is flagged.
- **Dropped capture frames / gaps**: audio lost between microphone and host. Any
  non-zero value marks the run degraded and the affected WAV incomplete.
- **crc_match**: 1 when the host's rebuilt path is byte-identical to what the
  device computed; 0 with an explanation in warnings.

Warnings the dashboard shows prominently: dropped capture frames, inference
deadline misses, WAV gaps, beamformed stream clipped, a path skipped, run ended
early, device disconnected before RUN_END, CRC mismatch, replay incomplete.

## 13. Counts are not accuracy; adding ground truth

Detection counts alone cannot say whether a path is *better*: a path that fires
on everything wins on count. To score properly:

1. Play a clip where the wake word occurs at known times. Record those times
   relative to the start of the clip.
2. Note the sample index at which the clip started in the recording (the first
   detection on any path, or a click at the start of the clip, gives the offset).
3. For every path, match each detection in `events.jsonl` to the nearest ground
   truth within a tolerance (for example 1.0 s after the phrase ends).
4. True positives = matched detections; false positives = unmatched detections;
   false negatives = unmatched ground truth. Precision, recall, F1 and mean
   latency follow directly.

`events.jsonl` has `sample`, `ms`, `score_x1000`, `name`, and `replay` for every
event, so this is a short script over one file. Phase 3 adds a ground-truth
import and this calculation to the dashboard.

## 14. Manual test plan

1. `set_preset left`, `start`. Speak; check `raw_left.wav` is intelligible and
   `raw_left` shows detections.
2. Repeat with `set_preset right`.
3. `set_preset lr_mix`. `raw_lr_mix.wav` should sound like both microphones
   summed at half level; `crc_match` must be 1.
4. `set_preset bf_lr`, `set_delay bf_lr 0 0`. Output must equal the L/R mix
   apart from gain rounding (gain 32767 differs from unity by one LSB).
5. Sweep `set_delay bf_lr d 0` for d in 1..8 and listen / compare detection
   counts under replay.
6. **End-fire test for the channel map:** put the speaker to the array's right,
   play a click, and check that `raw_right.wav` leads `raw_left.wav` by a few
   samples. If it lags, the microphones are swapped.
7. Once a center microphone exists: `set_preset raw_3ch`, verify three WAVs
   are mapped correctly; then `bf_3ch` with zero delays equals the 3-way
   average.
8. Fixed angle and distance: `set_preset raw_vs_bf_lr`, run, wait for the three
   automatic replay passes, compare detections and mean scores across paths.
9. Confirm one run folder holds every file in section 11.
10. Unplug the USB cable mid-run; the dashboard must mark the run aborted with
    a warning, and WAVs written so far must be marked incomplete.

## 15. Known limits

- **One live path per run.** The Edge AI runtime cannot score paths concurrently
  (section 3); all other paths are scored by replay after the run, roughly one
  run-length per path.
- **Link budget** at 1 Mbaud: two raw streams live. If your VCOM adapter cannot
  hold 1 Mbaud, set `current-speed = <921600>` in the board overlay and
  `CONFIG_EVAL_LINK_BYTES_PER_SEC=70000`. USB CDC on the DK's high-speed USB port
  would remove the limit but is untested in this SDK tree.
- **Replay flushes the runtime with one second of silence** before each pass,
  since the library exposes no reset; scores in the first second of a pass are
  from a fresh state, exactly as at the start of a live run.
- **Three-microphone paths** are implemented in the DSP and protocol but need a
  PDM center microphone capture path (Phase 2).
- **Inference timing** on the nRF54LM20 with the Axon accelerator has not been
  measured under this firmware; the per-window numbers in PATH_METRICS are the
  measurement. If `Miss` is non-zero on the live path, the run is flagged.
- The simulator (`--simulate`) uses an energy heuristic in place of the model;
  it exists to exercise the dashboard, storage and replay plumbing, not to
  produce results.
