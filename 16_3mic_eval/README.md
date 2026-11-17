# 16_3mic_eval — hands-free evaluation of the 3-microphone beamformer

Firmware plus host tools for one question: **how well does each way of
combining the three microphones (left / center / right) pick out sound from
straight ahead and reject sound from the sides?**

Unlike `14_ww_eval`, nothing here depends on wake-word detections. The device
records the three raw microphones (and its own beamformer output) during each
test; the laptop then builds every beamformer variant from the *same* raw audio,
measures them, draws the pictures and ranks them. Adding a new beamformer idea
is a few lines of numpy in `tools/dsp_paths.py` and a re-run of the analysis on
recordings you already have.

## What a test looks like

1. The Mac says "set the rig to 30 degrees, then press button 1".
2. You turn the platform and press BTN1 on the DK. That is the only thing you touch.
3. The device streams the three mics + beamformer over USB; the Mac plays the test
   clip through the speaker; the run ends on its own.
4. The Mac saves the WAVs, analyses them, updates the session report and says "done".

The full procedure, from a bare bench, is in [RIG_SETUP.md](RIG_SETUP.md).

## Hardware

```
3x ICS-52000 TDM daisy chain:  all SCK -> P1.23, all SD -> P1.13 (+100k pulldown)
  nRF FSYNC P1.22 -> WS of LEFT  -> WSO -> WS of CENTER -> WSO -> WS of RIGHT
  slot 0 = left, slot 1 = center, slot 2 = right, slot 3 unused
Two USB cables to the Mac:
  "nRF USB" (USB-C)  -> USB CDC ACM at high speed: audio + telemetry
  "IMCU"             -> J-Link: flashing, and Zephyr logs on uart20 @ 115200
LEDs: LED1 on while a test runs, LED2 on while the laptop has the USB port open.
Buttons: BTN1 start, BTN2 stop.
```

Capture is the same as `15_3micBF`: 16 kHz, 24-bit samples in 32-bit TDM slots,
`I2S_FMT_DATA_FORMAT_PCM_SHORT`, 256-sample frames (16 ms). Slot order, the
slot bit offset and the number of slots are Kconfig options (`CONFIG_EVAL_*`).

## Link

USB high speed removes the bandwidth problem that limited `14_ww_eval` to two
raw channels at 1 Mbaud: four 16-bit streams are 128 kB/s, a few percent of the
link. The packet format is the same framed protocol (`src/telemetry_protocol.h`,
`tools/serial_protocol.py`); stream ids are 0 left, 1 center, 2 right, 16 bf.
The device boots in text mode (`tools/rig.sh monitor`, type `help`); the host
tool switches it to binary framing.

## Firmware layout

| File | Role |
|---|---|
| `audio_capture_adapter.c` | TDM DMA -> `capture_frame{left,center,right}` -> message queue |
| `beamformer_delay_sum.c` | integer delay-and-sum, per-mic delay and Q15 weight; reproduced bit-exactly on the host |
| `run_controller.c` | eval thread: one run = RUN_START, audio streams, per-second AUDIO_STATS, RUN_END |
| `telemetry_transport.c` | USB CDC ACM, ring buffer drained through the interrupt-driven UART API |
| `command_interface.c` | text commands: start/stop, set_angle, set_delay, set_stream, levels ... |

## Host tools (`tools/`)

| Tool | Role |
|---|---|
| `rig.sh` | build / flash / monitor / clip / test / analyze / report / simulate |
| `make_test_clip.py` | builds `clips/standard.wav`: silence, pink noise, "Hey Vision" clips, log sweep, plus a cue sheet |
| `autotest.py` | the hands-free loop described above |
| `device_link.py` | USB link: commands, run start detection, audio collection with gap tracking |
| `dsp_paths.py` | beamformer variants and measurements (numpy only) |
| `analyze_run.py` | one recording -> `analysis.json`, `paths/*.wav`, figures, `report.html` |
| `session_report.py` | all recordings of a session -> polar beam pattern, SNR vs angle, beam pattern vs frequency, ranking |

### Paths that are compared

| Path | What it is |
|---|---|
| `raw_left`, `raw_center`, `raw_right` | the microphones |
| `lr_mix` | (L+R)/2, the 2-mic prototype in `13_finalProto` |
| `das` | 3-mic delay-and-sum, zero delays (broadside), what the device runs |
| `das_c1`, `das_c2` | center mic held back 1 / 2 samples (near-field mouth below the bridge) |
| `diff_lr` | (L−R)/2, a first-order dipole along the frame: a *null* at broadside, so it is the "everything except the wearer" reference |
| `gsc` | `das` minus what a 16-tap filter can predict from `diff_lr` (offline least squares): an upper bound on an adaptive null-steering stage |
| `gsc_nlms` | the deployable version: causal, gated NLMS, the same arithmetic as `15_3micBF/src/beamform_gsc.c` |
| `das_auto`, `gsc_auto` | delays and R gain trim fitted from the speech in the recording, as the firmware does after a wake word (`beamform_cal.c`). They steer toward wherever the speech is, so judge them on the mouth-position and two-speaker tests, not on angle sweeps |
| `bf_device` | what the device computed live; checked bit-for-bit against the host's `das` |

### What is measured

Per recording, per path, using the clip's cue sheet to find each part in the audio:

- **noise level** during the pink-noise burst → beam pattern when compared across angles
- **speech SNR**: speech level over the silence floor
- **silence floor**: room + self-noise (differential designs raise this)
- **third-octave levels** during the sweep → frequency response at this angle; across angles, the beam pattern vs frequency
- **TDOA**: the left-right delay of the pink noise, turned into the angle the mics *heard*. If it disagrees with the rig angle, the slot order, the marking or `--spacing-mm` is wrong.

Per session, `session_report.py` ranks the paths by `side_rejection + (snr_front − snr_side_mean)`
and shows the figures that justify it.

## Build, flash, run

```bash
cd ~/Documents/iLAB/nRF_SDK/16_3mic_eval
tools/rig.sh build && tools/rig.sh flash
tools/rig.sh clip --speech-dir ../14_ww_eval/test_samples/hey_vision
tools/rig.sh test --session day1 --angles 0,30,60,90,-30,-60,-90 --distance 60 --spacing-mm 140
open sessions/day1/session_report.html
```

No hardware: `tools/rig.sh simulate` runs the whole host side on synthetic recordings.

Host dependencies: `pip3 install pyserial numpy matplotlib` (no scipy needed).
