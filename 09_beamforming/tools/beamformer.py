import numpy as np
from scipy.signal import lfilter
from scipy.io import wavfile

# config
MIC_SPACING_M = 0.1508
SPEED_OF_SOUND_MS = 343.0
STEER_ANGLE_DEG = -37.0
FIR_TAPS = 31

def make_fir(frac_delay, num_taps = FIR_TAPS):
    center = num_taps // 2
    taps = np.zeros(num_taps)
    for n in range (num_taps):
        x = (n - center) - frac_delay
        sinc_val = 1.0 if abs(x) < 1e-6 else np.sin(np.pi*x) / (np.pi*x)
        window = 0.54 - 0.46 * np.cos(2 * np.pi * n / (num_taps - 1))
        taps[n] = sinc_val * window
    return taps

def beamformer_init(fs):
    angle_rad = np.radians(STEER_ANGLE_DEG)
    tau_seconds = (MIC_SPACING_M * np.sin(angle_rad)) / SPEED_OF_SOUND_MS
    delay_samples = abs(tau_seconds) * fs
    int_delay = int(np.floor(delay_samples)
    frac_delay = delay_samples - int_delay)
    h = make_fir(frac_delay)
    print(f"tau = {tau_seconds*1e6:.2f} us | int_delay = {int_delay} | frac_delay = {frac_delay:.4f}")
    return int_delay, h

def beamformer_process(mic_L, mic_R, int_delay, h):
    # integer delay
    if int_delay > 0:
        mic_R_int = np.concatenate((np.zeros(int_delay), mic_R[:-int_delay]))
    else:
        mic_R_int = mic_R
    # fractional FIR
    mic_R_frac = lfilter(fir_h, [1.0], mic_R_int)
    # delay-and-sum
    return 0.5 * (mic_L + mic_R_frac)

if __name__ == "__main__":
    fs, data = wavfile.read("dual_channel_capture.wav")
    data = data.astype(np.float32) / 2**23

    mic_L = data[:, 0]
    mic_R = data[:, 1]

    int_delay, fir_h = beamformer_init(fs)
    output = beamformer_process(mic_L, mic_R, int_delay, fir_h)

    wavfile.write("beamformed_output.wav", fs, (output * 2**23).astype(np.int32))
    print("wrote beamformed_output.wav")