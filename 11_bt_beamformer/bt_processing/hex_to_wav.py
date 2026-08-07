import wave
import struct

def hex_to_wav(hex_str: str, wav_path: str, sample_rate: int = 16000, gain: float = 1.0):
    # Parse hex string into bytes
    tokens = hex_str.split()
    byte_vals = []

    for t in tokens:
        # Handle '0800' style tokens (4 hex chars) or '08' style (2 hex chars)
        if len(t) == 4:
            byte_vals.append(int(t[0:2], 16))
            byte_vals.append(int(t[2:4], 16))
        elif len(t) == 2:
            byte_vals.append(int(t, 16))
        else:
            raise ValueError(f"Unexpected token length: {t}")
    
    # Convert bytes to 16-bit little-endian signed samples (mono)
    if len(byte_vals) % 2 != 0:
        raise ValueError("Odd number of bytes: cannot form 16-bit samples")

    samples = []
    for i in range(0, len(byte_vals), 2):
        lo = byte_vals[i]
        hi = byte_vals[i + 1]
        val = (hi << 8) | lo  # little-endian
        if val >= 0x8000:
            val -= 0x10000     # convert to signed
        samples.append(val)

    # Apply gain (optional)
    if gain != 1.0:
        scaled = []
        for s in samples:
            v = int(s * gain)
            if v > 32767:
                v = 32767
            if v < -32768:
                v = -32768
            scaled.append(v)
        samples = scaled

    # Write samples to WAV file (mono, 16-bit PCM)
    with wave.open(wav_path, 'w') as wf:
        wf.setnchannels(1)          # mono (firmware already beamformed)
        wf.setsampwidth(2)          # 16-bit
        wf.setframerate(sample_rate)
        wf.writeframes(struct.pack('<' + 'h' * len(samples), *samples))

    print(f"WAV written to {wav_path} with {len(samples)} samples at {sample_rate} Hz (gain={gain})")


if __name__ == "__main__":
    # Read hex data from file
    input_filename = "ble_hex.txt"
    output_filename = "ble_recording.wav"

    with open(input_filename, "r") as f:
        hex_input = f.read().strip()

    # You can tweak gain here if needed (e.g. gain=4.0)
    hex_to_wav(hex_input, output_filename, sample_rate=16000, gain=4.0)
    print(f"Done. Open '{output_filename}' in any audio player.")