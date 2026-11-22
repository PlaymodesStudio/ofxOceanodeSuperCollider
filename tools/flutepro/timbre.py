#!/usr/bin/env python3
"""Spectral-envelope distance FluteNew vs FluteOrig at low/mid notes, with an
Orig-vs-Orig baseline (two noise draws).  timbre.py <work> [key=value ...]
Flute's own controls (fb1 fb2 lpf noise ibreath vibamp vibfreq) go to both, the rest to FluteNew."""
import sys, pathlib
import numpy as np
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sc, sweep

def spec(x, sr, a, b):
    seg = np.asarray(x[int(a * sr):int(b * sr)]); n = 4096; S = 0
    for i in range(0, len(seg) - n, n // 2):
        S = S + np.abs(np.fft.rfft(seg[i:i + n] * np.hanning(n))) ** 2
    f = np.fft.rfftfreq(n, 1 / sr)
    # 1/3-octave bands 40 Hz..16 kHz
    edges = 40 * 2 ** (np.arange(0, 28) / 3)
    return np.array([10 * np.log10(S[(f >= lo) & (f < hi)].sum() + 1e-12) for lo, hi in zip(edges[:-1], edges[1:])])

work = pathlib.Path(sys.argv[1]); args = dict(a.split('=') for a in sys.argv[2:])
notes = [int(v) for v in args.pop('notes', '24,36,48,60,66,72').split(',')]
dup = [n for n in notes for _ in (0, 1)]
common = {k: v for k, v in args.items() if k in ('fb1', 'fb2', 'lpf', 'noise', 'ibreath', 'vibamp', 'vibfreq')}
_, wo = sweep.render(work, 'FluteOrig', dup, common, dur=2.0)
_, wn = sweep.render(work, 'FluteNew', notes, args, dur=2.0)
xo, _, sr = sc.read_wav(wo); xn, _, _ = sc.read_wav(wn)
for i, n in enumerate(notes):
    o1 = spec(xo, sr, (2 * i) * 2 + 0.5, (2 * i) * 2 + 1.95); o2 = spec(xo, sr, (2 * i + 1) * 2 + 0.5, (2 * i + 1) * 2 + 1.95)
    nn = spec(xn, sr, i * 2 + 0.5, i * 2 + 1.95)
    base = np.sqrt(np.mean((o1 - o2) ** 2)); d = np.sqrt(np.mean((o1 - nn) ** 2))
    print(f'note {n}: orig/orig {base:5.2f} dB   orig/new {d:5.2f} dB   worst band {np.max(np.abs(o1 - nn)):5.1f}')
