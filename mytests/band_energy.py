#!/usr/bin/env python3
"""Measure relative energy in a frequency band of a WAV vs a reference WAV.

Pure-stdlib implementation (wave + math), safe under "python3 -I": the WAVs
are read as data only and no imports are taken from their directories.

A 6th-order Butterworth band-pass (cascade of three high-pass and three
low-pass RBJ biquads per side, 36 dB/octave skirts) isolates
[lo_hz, hi_hz); energy is the mean square of the filtered PCM. Both files
are cropped to their common length, and stereo is downmixed to mono. The
same filter is applied to test and reference, so relative ratios are
unaffected by the Butterworth edge attenuation.

Usage: band_energy.py low.wav ref.wav [lo_hz] [hi_hz]
Prints reference/test band energies, the linear ratio and the dB ratio.
"""
import math
import sys
import wave


def read_pcm16(path):
    with wave.open(path, "rb") as w:
        nch = w.getnchannels()
        sw = w.getsampwidth()
        sr = w.getframerate()
        raw = w.readframes(w.getnframes())
    if sw != 2:
        sys.exit(f"{path}: only PCM16 is supported")
    vals = []
    # Little-endian signed 16 bit.
    for i in range(0, len(raw) - 1, 2):
        v = raw[i] | (raw[i + 1] << 8)
        if v >= 32768:
            v -= 65536
        vals.append(float(v))
    if nch > 1:
        vals = [sum(vals[i:i + nch]) / nch for i in range(0, len(vals), nch)]
    return sr, vals


def biquad_highpass_coeffs(f0, sr, q):
    w0 = 2.0 * math.pi * f0 / sr
    cw, sw = math.cos(w0), math.sin(w0)
    alpha = sw / (2.0 * q)
    b0 = (1.0 + cw) / 2.0
    b1 = -(1.0 + cw)
    b2 = (1.0 + cw) / 2.0
    a0 = 1.0 + alpha
    a1 = -2.0 * cw
    a2 = 1.0 - alpha
    return (b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)


def biquad_lowpass_coeffs(f0, sr, q):
    w0 = 2.0 * math.pi * f0 / sr
    cw, sw = math.cos(w0), math.sin(w0)
    alpha = sw / (2.0 * q)
    b0 = (1.0 - cw) / 2.0
    b1 = 1.0 - cw
    b2 = (1.0 - cw) / 2.0
    a0 = 1.0 + alpha
    a1 = -2.0 * cw
    a2 = 1.0 - alpha
    return (b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)


def apply_biquad(x, c):
    b0, b1, b2, a1, a2 = c
    y = [0.0] * len(x)
    x1 = x2 = y1 = y2 = 0.0
    for i, xi in enumerate(x):
        yi = b0 * xi + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        y[i] = yi
        x2, x1, y2, y1 = x1, xi, y1, yi
    return y


def band_energy(x, sr, lo, hi):
    # Q for a 2nd-order Butterworth biquad; three sections per side give a
    # 6th-order response (36 dB/octave skirts).
    q = 1.0 / math.sqrt(2.0)
    out = x
    for _ in range(3):
        out = apply_biquad(out, biquad_highpass_coeffs(lo, sr, q))
    for _ in range(3):
        out = apply_biquad(out, biquad_lowpass_coeffs(hi, sr, q))
    return sum(v * v for v in out) / len(out)


def main():
    test_path, ref_path = sys.argv[1], sys.argv[2]
    lo = float(sys.argv[3]) if len(sys.argv) > 3 else 4000.0
    hi = float(sys.argv[4]) if len(sys.argv) > 4 else 8000.0

    sr_t, t = read_pcm16(test_path)
    sr_r, r = read_pcm16(ref_path)
    if sr_t != sr_r:
        sys.exit(f"sample rate mismatch: {sr_t} vs {sr_r}")

    n = min(len(t), len(r))
    t, r = t[:n], r[:n]

    e_t = band_energy(t, sr_t, lo, hi)
    e_r = band_energy(r, sr_r, lo, hi)
    ratio = e_t / e_r if e_r > 0.0 else float("nan")
    db = 10.0 * math.log10(ratio) if ratio > 0.0 else float("-inf")
    print(f"ref_band_energy={e_r:.6e} test_band_energy={e_t:.6e} "
          f"ratio={ratio:.6f} ratio_db={db:.3f}")


if __name__ == "__main__":
    main()
