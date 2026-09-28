# Test rig setup

Everything you need to go from a bare bench to logging results. Commands run
from the `14_ww_eval` folder in Terminal.

```
cd ~/Documents/iLAB/nRF_SDK/14_ww_eval
```

## 1. One-time software setup (on your Mac)

```
pip3 install pyserial numpy
tools/rig.sh playlists --tts
```

The second command builds `playlists/tts/`:

| File | What it's for |
|---|---|
| `wake_50.wav` | 50 × "Hey Vision", a new one every ~3.4 s, 2 min 49 s total |
| `decoys_50.wav` | 50 decoy phrases ("Hey Vivian", "Hey Division", "Hey Siri"…), same spacing |
| `calibration_noise.wav` | 30 s of noise at the same loudness, for setting the speaker volume |
| `cues_*.csv` | the start time and text of every clip |

The voices are the Mac's text-to-speech. They are fine for shaking down the rig,
but the model was trained on real people, so **use real recordings for the study**:
record ~10 people saying "Hey Vision" 5 times each (phone voice memos are fine),
put the files in one folder, the decoys in another, and run

```
tools/rig.sh playlists --wake-dir recordings/wake --decoy-dir recordings/decoys --name people
```

## 2. Flash the board

1. Plug a USB cable into the DK's USB port labelled **IMCU** and switch **POWER** on.
2. Build and flash:
   ```
   tools/rig.sh build
   tools/rig.sh flash
   ```
   This replaces the prototype firmware (13_finalProto). To go back later, flash that project from VS Code as before.

## 3. First power-on check

```
tools/rig.sh monitor
```

Press the DK's **RESET** button. You should see, within a couple of seconds:

```
=== wake-word beamforming evaluation (14_ww_eval) ===
... wake word: window=160 samples, threshold=700/1000, vote 4 of 6, cooldown 1200 ms
... capture: 16000 Hz, 2 ch, 32-bit slots, 256 samples/frame (16 ms), queue 8
... ready: 16000 Hz, 2 ch, 256-sample frames; BTN1 start, BTN2 stop
```

Then check the microphones:

```
levels on
```

One line prints every second. **Gently rub a fingertip over the LEFT microphone's sound hole**:
only the `left` numbers should jump. Then the RIGHT one. If they're swapped, the
two INMP441s' L/R pins are wired the other way round; swap those two wires (L/R
to GND = left, L/R to 3V3 = right). With the room quiet, both `rms` values should
be similar and small; a mic stuck at 0 or at the maximum is miswired or dead.

```
levels off
```

The wake-word model only runs while a test is running, so check it with a short one:

```
start
```

Say "Hey Vision" a few times, a couple of seconds apart, from about 30 cm. Each
detection prints a gold `>>> wake word #1 on bf_lr ...` line and blinks LED2. The
test stops by itself after 30 s and prints a summary; the `Det` column is the count.
If nothing ever triggers, send me the terminal output.

## 4. Build the physical rig

1. **Centre platform**: base → scissor jack → fixed plate → lazy Susan → top plate. Fix the breadboard and DK to the top plate with Dual Lock so nothing slides.
2. **Origin**: the midpoint between the two microphone capsules, directly over the lazy Susan's axis. Measure and write down the capsule spacing.
3. **Markings**: forward arrow on the top plate. On the table or floor, tape four arcs centred on the origin at **15, 30, 45 and 60 cm** (measured from the origin to the front of the speaker's driver) and mark 0°, ±15°, ±30°, ±45°, ±60°, ±90° and 180° on the 60 cm arc. (Right of the array is positive.)
4. **Speaker**: on its own stand, driver pointing at the origin, driver centre at the same height as the microphone capsules. Bubble-level the top plate.
5. **Cables**: tape the DK's USB cable to the plate and the table so turning the platform can't tug it.

## 5. Set the playback loudness (every session)

1. Put the speaker at 0°, 60 cm (the farthest position).
2. Play `calibration_noise.wav` on loop.
3. Hold a sound-level meter (a phone app like NIOSH SLM or Decibel X is fine) at the microphones, pointed at the speaker.
4. Turn the speaker volume until the meter reads **65 dB** (about a conversational voice). Never touch the volume knob again that session; tape it. At 45, 30 and 15 cm the speech will be about 2.5, 6 and 12 dB louder; that's expected, just as a real person gets louder the closer they stand.
5. Put the reading and the volume setting in the first test's notes.

## 6. Run one test

A "test" is one configuration at one angle and distance, played twice: once with the wake words, once with the decoys.

In the monitor:

```
set_preset bf_lr            # or: left   right   lr_mix
set_angle 45
set_distance 60
set_label bf_lr_45deg_60cm
set_env quiet               # or: medium   loud
set_duration 174
start
```

Press play on `wake_50.wav` straight after `start` (there are 3 s of silence at the start, so a second's delay doesn't matter). When it finishes, the monitor prints:

```
RESULT  bf_lr  at 45° / 60 cm / quiet  ->  47 detections   (VALID FOR COMPARISON)
```

That number is **Detected**. Then:

```
set_duration 173
start
```

and play `decoys_50.wav`. That RESULT number is **Falsely triggered**.

Log both in the dashboard with the same **Environment**. If the quality says `DEGRADED`, redo the test.

You can also start with **BTN1** and stop early with **BTN2**; LED1 is on while a test runs.

## 7. Background noise: quiet, medium, loud

The whole experiment is repeated three times, once per environment. Only the
background noise changes; the speech level stays at 65 dB every time.

| Environment | Noise at the microphones | Speech-to-noise |
|---|---|---|
| Quiet  | none (room as it is, write down its dB) | — |
| Medium | **55 dB** | speech 10 dB louder |
| Loud   | **65 dB** | speech and noise equally loud |

**Noise speaker setup**

1. Use a second speaker for the noise, never the speech speaker.
2. Put it at **−90° (the array's left), 60 cm**, same height as the microphones, and
   leave it there for every test in every environment. Turning the platform moves
   the speech angle; the noise always comes from the same place in the room.
   (If you ever test negative angles, move it to +90° for those so the two
   speakers never stand in the same spot.)
3. Play your noise track on loop. It needs no gaps or silent parts, or some wake
   words land in quiet moments and others don't.

**Setting the noise level (start of each medium/loud block)**

1. Speech speaker silent. Play the noise on loop.
2. Hold the sound-level meter at the microphones, as in section 5.
3. Turn the **noise** speaker's volume until the meter reads 55 dB (medium) or 65 dB
   (loud). Tape that knob too.
4. Check the speech speaker still gives 65 dB with the noise off (section 5).

**Running a test with noise**

1. Start the noise loop and let it play.
2. In the monitor: `set_env medium` (or `loud`), then the usual commands from section 6.
3. `start`, play `wake_50.wav`; then `start` again, play `decoys_50.wav`.
4. Leave the noise running through both halves and between tests.
5. In the dashboard pick the same Environment, and put the meter reading in
   **Noise dB**.

For the quiet block use `set_env quiet`, no noise, and put the room's meter
reading in Noise dB.

## 8. Plan the sessions

| | |
|---|---|
| One test (wake + decoys) | ~6 min |
| One position, 4 configurations | ~25 min |
| 0°, 45°, 90° at 60 cm, one environment | ~75 min |
| Same, all three environments | ~4 hours |
| 15, 30, 45, 60 cm at 0°, one environment | ~100 min |
| 7 angles at 60 cm, one environment | ~3 hours |

Suggested plan: 0°, 45°, 90° at 60 cm, all four configurations, done as three
separate blocks: quiet, then medium, then loud. 60 cm is the hardest distance,
so it shows the differences best. If there's time, add a distance sweep (15, 30,
45, 60 cm at 0°) in the loud environment. Do one block per sitting if you
can. Within each block change the order of configurations at each position so
tiredness or drift doesn't favour one. Keep every position identical across the
three blocks so the environments compare fairly.

In the dashboard, the **By environment** chart shows how each configuration
holds up as the noise gets louder. The **in …** menu next to the metric switch
limits the other charts to one environment.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `No development kit found` | Use the IMCU USB port, POWER switch on, try another cable (some are charge-only). |
| Monitor says no port answered | Press RESET, then run the monitor again. Check the flash step finished. |
| Garbled characters | The USB adapter isn't keeping up with 1 Mbaud. In `boards/nrf54lm20dk_nrf54lm20b_cpuapp.overlay` change `current-speed = <1000000>` to `<115200>`, rebuild, reflash, and run `tools/rig.sh monitor --baud 115200`. |
| Zero detections with TTS clips but it triggers on your voice | The model doesn't like that synthetic voice. Use real recordings (section 1). |
| `err: stop the run first` | A test is still running. Type `stop`, or wait for its duration. |
| Anything else | Send me the newest file in `sessions/`. |
