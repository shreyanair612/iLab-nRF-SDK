# Audio Prototype for Vision-Impairment Assistive Glasses

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

