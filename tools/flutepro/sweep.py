#!/usr/bin/env python3
"""Pitch / level of FluteOrig and FluteNew across notes and settings.
  sweep.py <work> [def] [key=value ...]   (defs compiled by null_test.py into <work>/defs)
"""
import sys, pathlib, math
import numpy as np
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sc

def pitch_of(x, sr):
    x = np.asarray(x) - np.mean(x)
    n = len(x); X = np.fft.rfft(x * np.hanning(n), 4 * n)
    mag = np.abs(X); f = np.fft.rfftfreq(4 * n, 1 / sr)
    lo = int(30 / f[1]); k = lo + np.argmax(mag[lo:])          # strongest partial
    # autocorrelation for the period
    ac = np.fft.irfft(np.abs(np.fft.rfft(x, 2 * n)) ** 2)[:n]
    ac /= ac[0] + 1e-20
    mn = int(sr / 5000); best = mn + np.argmax(ac[mn:int(sr / 25)])
    # first peak above 0.85 of max (avoid octave errors)
    thr = 0.85 * ac[best]
    for lag in range(mn, int(sr / 25)):
        if ac[lag] >= thr and ac[lag] >= ac[lag - 1] and ac[lag] >= ac[lag + 1]: best = lag; break
    a, b, c = ac[best - 1], ac[best], ac[best + 1]
    lag = best + float(np.clip(0.5 * (a - c) / (a - 2 * b + c + 1e-20), -0.5, 0.5))
    return 69 + 12 * math.log2(sr / lag / 440), 69 + 12 * math.log2(f[k] / 440), ac[best]

def render(work, name, notes, args, dur=1.5):
    score = sc.bundle(0, [sc.message('/d_load', str(work / 'defs' / (name + '.scsyndef')))])
    t = 0.01
    for i, n in enumerate(notes):
        a = ['pitch', float(n)] + [v for kv in args.items() for v in (kv[0], float(kv[1]))]
        score += sc.bundle(t, [sc.message('/s_new', name, 1000 + i, 0, 0, *a)])
        score += sc.bundle(t + dur - 0.01, [sc.message('/n_free', 1000 + i)])
        t += dur
    score += sc.bundle(t, [sc.message('/c_set', 0, 0.0)])
    tag = name + '_' + '_'.join(f'{k}{v}' for k, v in args.items())
    x, ch, sr = sc.render(score, work / (tag + '.wav'), 1)
    x = np.array(x); res = []
    for i, n in enumerate(notes):
        seg = x[int((i * dur + 0.8) * sr): int((i * dur + 0.8) * sr) + 8192]
        rms = math.sqrt(np.mean(seg ** 2))
        p, pk, cl = pitch_of(seg, sr) if rms > 1e-4 else (float('nan'),) * 3
        res.append((n, rms, p, pk, cl))
    return res, work / (tag + '.wav')

if __name__ == '__main__':
    work = pathlib.Path(sys.argv[1]); name = sys.argv[2] if len(sys.argv) > 2 else 'FluteOrig'
    args = dict(a.split('=') for a in sys.argv[3:] if '=' in a)
    notes = [int(v) for v in args.pop('notes', '24,36,48,60,66,72,76,80,84,90,96,102,108').split(',')]
    res, wav = render(work, name, notes, args)
    print(name, args)
    for n, rms, p, pk, cl in res:
        print(f'  note {n:3d}  rms {rms:.3f}  period->{p:6.2f} ({p - n:+5.2f})  peak->{pk:6.2f}  clar {cl:.2f}')
