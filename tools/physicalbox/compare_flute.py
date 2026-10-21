#!/usr/bin/env python3
"""Compares flute renders: harmonic levels (dB re the strongest), the noise
between the harmonics (dB re the harmonics: lower = purer), pitch wobble
(cents, std of 40 ms frames) and the vibrato rate found in it.
   python3 compare_flute.py a.wav b.wav ...   (pitch from the file name: _<midi>)"""
import re, sys, pathlib
import numpy as np
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from analyze import read_wav

def measure(path):
    x, r = read_wav(pathlib.Path(path)); m = x.mean(1)
    midi = float(re.findall(r'_(\d+)', pathlib.Path(path).stem)[-1])
    f0 = 440 * 2 ** ((midi - 69) / 12)
    seg = m[int(0.8 * r):int(2.8 * r)]
    n = len(seg)
    S = np.abs(np.fft.rfft(seg * np.hanning(n))) ** 2
    fr = np.fft.rfftfreq(n, 1 / r)
    # the actual fundamental near f0
    band = (fr > f0 * 0.9) & (fr < f0 * 1.1)
    f1 = fr[band][np.argmax(S[band])]
    harm, noise = [], []
    for h in range(1, 9):
        c = h * f1
        if c > r / 2 - 100: break
        w = (fr > c - f1 * 0.06) & (fr < c + f1 * 0.06)
        harm.append(S[w].sum())
        g = (fr > c + f1 * 0.25) & (fr < c + f1 * 0.75)
        noise.append(S[g].mean() * w.sum())
    harm = np.array(harm); top = harm.max()
    hdb = 10 * np.log10(harm / top + 1e-12)
    hnr = 10 * np.log10(np.sum(noise) / np.sum(harm) + 1e-12)
    # pitch wobble: zero-crossing-free, per-frame spectral peak near f1 (parabolic)
    hop = int(0.04 * r); cents = []
    for i in range(int(0.8 * r), int(2.8 * r) - 4 * hop, hop):
        fr2 = m[i:i + 4 * hop] * np.hanning(4 * hop)
        Z = np.abs(np.fft.rfft(fr2, 1 << 16)); ff = np.fft.rfftfreq(1 << 16, 1 / r)
        b2 = (ff > f1 * 0.9) & (ff < f1 * 1.1)
        k = np.nonzero(b2)[0][np.argmax(Z[b2])]
        y0, y1, y2 = np.log(Z[k - 1:k + 2] + 1e-12)
        d = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2)
        cents.append(1200 * np.log2((ff[k] + d * (ff[1] - ff[0])) / f1))
    cents = np.array(cents); cents -= cents.mean()
    spec = np.abs(np.fft.rfft(cents * np.hanning(len(cents)), 512)); vf = np.fft.rfftfreq(512, 0.04)
    vib = vf[1 + np.argmax(spec[1:])]
    env = np.array([np.sqrt((m[i:i + hop] ** 2).mean()) for i in range(int(0.8 * r), int(2.8 * r) - hop, hop)])
    amod = 20 * np.log10(env.max() / max(env.min(), 1e-9))
    print(f"{pathlib.Path(path).stem:14s} f1 {f1:7.1f} ({1200*np.log2(f1/f0):+5.0f}c)  harm dB " +
          " ".join(f"{v:5.1f}" for v in hdb) + f"   noise {hnr:6.1f} dB  wobble {cents.std():5.1f}c  vib {vib:4.1f} Hz  amp swing {amod:4.1f} dB")

for p in sys.argv[1:]: measure(p)
