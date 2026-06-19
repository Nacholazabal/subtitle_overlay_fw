#!/usr/bin/env python3
"""Analyze frequency spectrum of noise to identify filterable components."""

import numpy as np
import wave
import struct
import sys
from pathlib import Path

def analyze_spectrum(wav_path):
    """Analyze frequency spectrum and identify dominant noise frequencies."""

    # Read WAV file
    with wave.open(str(wav_path), 'rb') as wf:
        nchannels = wf.getnchannels()
        sampwidth = wf.getsampwidth()
        framerate = wf.getframerate()
        nframes = wf.getnframes()
        raw = wf.readframes(nframes)

    # Decode S16_LE
    if sampwidth != 2:
        print(f"ERROR: expected 16-bit samples, got {sampwidth*8}-bit")
        return

    n_samples = len(raw) // 2
    samples = np.array(struct.unpack(f"<{n_samples}h", raw), dtype=np.float32)

    # Flatten to mono if stereo
    if nchannels > 1:
        samples = samples[::nchannels]

    # Normalize to [-1, 1]
    samples = samples / 32768.0

    print(f"=== SPECTRUM ANALYSIS: {wav_path.name} ===")
    print(f"Duration: {len(samples)/framerate:.1f}s")
    print(f"Sample rate: {framerate} Hz")
    print(f"RMS level: {np.sqrt(np.mean(samples**2)):.6f} ({20*np.log10(np.sqrt(np.mean(samples**2))):.1f} dBFS)")
    print()

    # FFT analysis (use first 4 seconds or full file if shorter)
    analysis_samples = min(len(samples), framerate * 4)
    fft_data = np.fft.rfft(samples[:analysis_samples])
    fft_magnitude = np.abs(fft_data)
    fft_freqs = np.fft.rfftfreq(analysis_samples, 1.0/framerate)

    # Convert to dB
    fft_db = 20 * np.log10(fft_magnitude + 1e-10)

    # Find peaks (frequencies with significant energy)
    # Focus on 0-8000 Hz (relevant for speech)
    speech_range = fft_freqs <= 8000
    fft_db_speech = fft_db[speech_range]
    fft_freqs_speech = fft_freqs[speech_range]

    # Find top 10 frequency components
    sorted_indices = np.argsort(fft_db_speech)[::-1]

    print("=== TOP 10 FREQUENCY COMPONENTS (0-8000 Hz) ===")
    print(f"{'Freq (Hz)':<12} {'Magnitude (dB)':<18} {'Type'}")
    print("-" * 60)

    for i in range(min(10, len(sorted_indices))):
        idx = sorted_indices[i]
        freq = fft_freqs_speech[idx]
        mag = fft_db_speech[idx]

        # Classify frequency
        freq_type = ""
        if 48 <= freq <= 52:
            freq_type = "← 50Hz MAINS HUM"
        elif 58 <= freq <= 62:
            freq_type = "← 60Hz MAINS HUM"
        elif 98 <= freq <= 102 or 148 <= freq <= 152:
            freq_type = "← 50Hz HARMONIC"
        elif 118 <= freq <= 122 or 178 <= freq <= 182:
            freq_type = "← 60Hz HARMONIC"
        elif freq < 10:
            freq_type = "← DC offset / very low freq"
        elif 200 <= freq <= 300:
            freq_type = "← male speech fundamental"
        elif 300 <= freq <= 500:
            freq_type = "← female speech fundamental"
        elif freq > 4000:
            freq_type = "← high freq / sibilants"

        print(f"{freq:<12.1f} {mag:<18.1f} {freq_type}")

    print()

    # Analyze noise floor by frequency bands
    print("=== NOISE DISTRIBUTION BY FREQUENCY BAND ===")
    bands = [
        (0, 100, "Sub-bass (DC/hum)"),
        (100, 300, "Bass (low freq noise)"),
        (300, 1000, "Low-mid (speech fundamentals)"),
        (1000, 3000, "Mid (speech clarity)"),
        (3000, 8000, "High (sibilants/consonants)"),
    ]

    for f_low, f_high, label in bands:
        band_mask = (fft_freqs_speech >= f_low) & (fft_freqs_speech < f_high)
        band_energy = np.mean(fft_magnitude[speech_range][band_mask]**2)
        band_db = 10 * np.log10(band_energy + 1e-10)
        print(f"{label:<35} {band_db:>8.1f} dB")

    print()

    # Detect specific noise signatures
    print("=== FILTERABLE NOISE SIGNATURES ===")

    hum_50 = np.max(fft_db_speech[(fft_freqs_speech >= 48) & (fft_freqs_speech <= 52)])
    hum_60 = np.max(fft_db_speech[(fft_freqs_speech >= 58) & (fft_freqs_speech <= 62)])

    if hum_50 > -40:
        print(f"✓ 50Hz hum detected: {hum_50:.1f} dB → FILTERABLE with notch filter")
    if hum_60 > -40:
        print(f"✓ 60Hz hum detected: {hum_60:.1f} dB → FILTERABLE with notch filter")

    # Check for harmonics
    harmonics_50 = [100, 150, 200, 250]
    harmonics_60 = [120, 180, 240, 300]

    for h in harmonics_50:
        h_mag = np.max(fft_db_speech[(fft_freqs_speech >= h-2) & (fft_freqs_speech <= h+2)])
        if h_mag > -40:
            print(f"✓ {h}Hz (50Hz harmonic) detected: {h_mag:.1f} dB → FILTERABLE")

    for h in harmonics_60:
        h_mag = np.max(fft_db_speech[(fft_freqs_speech >= h-2) & (fft_freqs_speech <= h+2)])
        if h_mag > -40:
            print(f"✓ {h}Hz (60Hz harmonic) detected: {h_mag:.1f} dB → FILTERABLE")

    # Check if noise is broadband (white/pink)
    # Calculate spectral flatness
    geometric_mean = np.exp(np.mean(np.log(fft_magnitude[speech_range] + 1e-10)))
    arithmetic_mean = np.mean(fft_magnitude[speech_range])
    spectral_flatness = geometric_mean / (arithmetic_mean + 1e-10)

    print()
    print(f"Spectral flatness: {spectral_flatness:.3f}")
    if spectral_flatness > 0.5:
        print("→ BROADBAND noise (white/pink) — HARD to filter without affecting speech")
    else:
        print("→ TONAL noise (specific frequencies) — EASIER to filter")

    print()
    print("=== RECOMMENDATION ===")
    if hum_50 > -30 or hum_60 > -30:
        print("Strong mains hum detected → implement 50/60Hz notch filter + high-pass at 80Hz")
    elif spectral_flatness > 0.6:
        print("Broadband noise → filtering will damage speech quality")
        print("BETTER FIX: increase signal level at source (laptop volume, better cable, line out)")
    else:
        print("Mixed noise → analyze specific peaks and consider targeted notch filters")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python analyze_noise_spectrum.py <wav_file>")
        sys.exit(1)

    wav_path = Path(sys.argv[1])
    if not wav_path.exists():
        print(f"ERROR: {wav_path} not found")
        sys.exit(1)

    analyze_spectrum(wav_path)
