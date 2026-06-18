#!/usr/bin/env python3
"""
Analyze noise floor and SNR from WAV files.
Usage: python analyze_noise.py <noise.wav> <signal.wav>
"""

import sys
import wave
import struct
import math

def read_wav_samples(filepath):
    """Read WAV file and return samples as list of floats [-1.0, 1.0]"""
    with wave.open(filepath, 'rb') as wf:
        nchannels = wf.getnchannels()
        sampwidth = wf.getsampwidth()
        framerate = wf.getframerate()
        nframes = wf.getnframes()

        print(f"Reading {filepath}:")
        print(f"  Channels: {nchannels}, Sample width: {sampwidth} bytes, Rate: {framerate} Hz, Frames: {nframes}")

        raw_data = wf.readframes(nframes)

        # Unpack S16_LE samples
        if sampwidth != 2:
            raise ValueError(f"Expected 16-bit samples, got {sampwidth*8}-bit")

        samples = struct.unpack(f'<{nframes * nchannels}h', raw_data)

        # Normalize to [-1.0, 1.0]
        normalized = [s / 32768.0 for s in samples]

        return normalized, framerate

def calculate_rms(samples):
    """Calculate RMS (root mean square) of samples"""
    if not samples:
        return 0.0

    sum_squares = sum(s * s for s in samples)
    mean_square = sum_squares / len(samples)
    return math.sqrt(mean_square)

def calculate_peak(samples):
    """Calculate absolute peak value"""
    if not samples:
        return 0.0
    return max(abs(s) for s in samples)

def linear_to_dbfs(linear_value):
    """Convert linear amplitude to dBFS (decibels relative to full scale)"""
    if linear_value <= 0:
        return -float('inf')
    return 20 * math.log10(linear_value)

def detect_speech_segments(samples, framerate, threshold_rms):
    """
    Detect segments with speech by finding where RMS is significantly above noise floor.
    Returns indices of speech samples.
    """
    # Window size: 100ms
    window_samples = int(0.1 * framerate)

    speech_indices = []

    for i in range(0, len(samples), window_samples):
        window = samples[i:i+window_samples]
        window_rms = calculate_rms(window)

        # If window RMS is >3dB above threshold, consider it speech
        if window_rms > threshold_rms * 1.41:  # ~3dB = sqrt(2)
            speech_indices.extend(range(i, min(i+window_samples, len(samples))))

    return speech_indices

def main():
    if len(sys.argv) != 3:
        print("Usage: python analyze_noise.py <noise.wav> <signal.wav>")
        sys.exit(1)

    noise_file = sys.argv[1]
    signal_file = sys.argv[2]

    print("=" * 70)
    print("NOISE FLOOR ANALYSIS")
    print("=" * 70)
    print()

    # Analyze noise file
    print("STEP 1: Analyzing NOISE file (silence + background noise)")
    print("-" * 70)
    noise_samples, noise_rate = read_wav_samples(noise_file)

    noise_rms = calculate_rms(noise_samples)
    noise_peak = calculate_peak(noise_samples)
    noise_rms_dbfs = linear_to_dbfs(noise_rms)
    noise_peak_dbfs = linear_to_dbfs(noise_peak)

    print(f"\nNoise statistics:")
    print(f"  RMS:  {noise_rms:.6f} linear = {noise_rms_dbfs:.2f} dBFS")
    print(f"  Peak: {noise_peak:.6f} linear = {noise_peak_dbfs:.2f} dBFS")
    print()

    # Analyze signal file
    print("STEP 2: Analyzing SIGNAL file (with voice)")
    print("-" * 70)
    signal_samples, signal_rate = read_wav_samples(signal_file)

    signal_rms_total = calculate_rms(signal_samples)
    signal_peak = calculate_peak(signal_samples)
    signal_rms_dbfs_total = linear_to_dbfs(signal_rms_total)
    signal_peak_dbfs = linear_to_dbfs(signal_peak)

    print(f"\nSignal statistics (entire file):")
    print(f"  RMS:  {signal_rms_total:.6f} linear = {signal_rms_dbfs_total:.2f} dBFS")
    print(f"  Peak: {signal_peak:.6f} linear = {signal_peak_dbfs:.2f} dBFS")
    print()

    # Detect speech segments
    print("STEP 3: Detecting speech segments")
    print("-" * 70)
    speech_indices = detect_speech_segments(signal_samples, signal_rate, noise_rms * 2)

    if speech_indices:
        speech_samples = [signal_samples[i] for i in speech_indices]
        speech_rms = calculate_rms(speech_samples)
        speech_rms_dbfs = linear_to_dbfs(speech_rms)

        speech_duration = len(speech_indices) / signal_rate
        total_duration = len(signal_samples) / signal_rate

        print(f"Detected speech: {speech_duration:.2f}s / {total_duration:.2f}s ({100*speech_duration/total_duration:.1f}%)")
        print(f"\nSpeech segment statistics:")
        print(f"  RMS:  {speech_rms:.6f} linear = {speech_rms_dbfs:.2f} dBFS")
    else:
        print("WARNING: No speech segments detected!")
        speech_rms_dbfs = signal_rms_dbfs_total
        speech_rms = signal_rms_total

    print()

    # Calculate SNR
    print("STEP 4: SNR Calculation")
    print("-" * 70)
    snr_db = speech_rms_dbfs - noise_rms_dbfs

    print(f"Signal level (speech RMS): {speech_rms_dbfs:.2f} dBFS")
    print(f"Noise floor (noise RMS):   {noise_rms_dbfs:.2f} dBFS")
    print(f"SNR:                       {snr_db:.2f} dB")
    print()

    # Verdict
    print("=" * 70)
    print("FINAL VERDICT")
    print("=" * 70)
    print(f"Noise floor:   {noise_rms_dbfs:.2f} dBFS")
    print(f"Signal level:  {speech_rms_dbfs:.2f} dBFS")
    print(f"SNR:           {snr_db:.2f} dB")
    print()

    if snr_db < 20:
        verdict = "MAL - ruido excesivo, dificulta reconocimiento"
        status = "FAIL"
    elif snr_db < 40:
        verdict = "ACEPTABLE - ruido presente pero manejable"
        status = "OK"
    else:
        verdict = "EXCELENTE - ruido minimo"
        status = "EXCELLENT"

    print(f"Status: [{status}] {verdict}")
    print()

    # Additional context
    print("Context:")
    print(f"  - Typical good microphone SNR: 60-70 dB")
    print(f"  - Typical speech recognition minimum: 20-25 dB")
    print(f"  - Your analog capture: {snr_db:.1f} dB")
    print()

if __name__ == "__main__":
    main()
