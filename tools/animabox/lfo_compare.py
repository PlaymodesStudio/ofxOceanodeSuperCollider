#!/usr/bin/env python3
"""Reaktor's spatial LFO (spec_lfo.md, deterministic wave mode) against AnimaBox's port: the shared
LFO's ModLfoX maths (shape, Pow, Smooth) with the indexer's phase offsets (baseIndexer.cpp port),
fed by convert_snapshots.py's values. Per snapshot and LFO: the mean per-band correlation of the two
value(band, t) over 4 s (1 = the same motion), and the mean absolute difference.
Random modes print their mode only (compared by ear / statistics elsewhere)."""
import json, math, pathlib
HERE = pathlib.Path(__file__).resolve().parent
V = json.load(open(HERE / 'reaktor/pitch/snapvals.json'))['V']
F = json.load(open(HERE / 'reaktor/factory.json'))
IDS = {'LFO1_': dict(len=7439, bpm=7465, div=7467, ph=7507, mode=7571, skew=7525, round=7530, pwm=7561, pow=7609, inv=7400, smooth=7480, frn=7488, vibA=7595),
       'LFO2_': dict(len=7719, bpm=7745, div=7747, ph=7787, mode=7851, skew=7805, round=7810, pwm=7841, pow=7889, inv=7680, smooth=7760, frn=7768, vibA=7875)}
def tension(x, p):
    a = 0.999 * max(-1, min(1, p)); k = 2 * a / (1 - a); return x * (k + 1) / (k * abs(x) + 1)
def reaktor(s, P, nv, t):
    g = lambda k: float(V[str(P[k])][s])
    L = g('len'); ln = L + 1 if L <= 0 else math.floor(15 * L + 1 + 0.5)
    Fq = 2.0 / g('div') * g('bpm')
    out = []
    for v in range(1, nv + 1):
        u = (nv - (v - 1)) if g('inv') > 0 else v
        p = (g('ph') + Fq * t + ln * u / nv) % 1.0
        pwm = g('pwm'); x = (min(max(p, pwm), 1 - pwm) - pwm) / (1 - 2 * pwm)
        sk = max(-0.999999, min(0.999999, g('skew'))); c = (sk + 1) / 2
        y = 0.5 * x / c if x <= c else 0.5 + 0.5 * (x - c) / (1 - c)
        tri = 2 * y if y <= 0.5 else 2 - 2 * y; rc = (1 - math.cos(2 * math.pi * y)) / 2
        r = g('round'); Lv = tri + (rc - tri) * min(max(r, 0), 1); e = min(max(2 - r, 0), 1)
        w = math.copysign(abs(2 * Lv - 1) ** e, 2 * Lv - 1)
        out.append(tension((w + 1) / 2, g('pow')))
    return out
def indexer(n, spread, invert, wrap=True):
    vals = []
    for i in range(n):
        index = n - i; indexf = index
        noninv = indexf - 1.0; inv = n - indexf
        indexf = invert * inv + (1 - invert) * noninv + 0.5
        val = indexf / n * spread
        if wrap and val > 1: tr = math.trunc(val); val -= (tr - 1) if tr == val else tr
        vals.append(val)
    return vals
def ours(v, pre, nv, t):
    hz = 2.0 / v[pre + 'Beats'][0]
    ph0 = indexer(nv, v[pre + 'Spread'], v[pre + 'Invert'])
    out = []
    for i in range(nv):
        ph = (hz * t + v[pre + 'Phase'] + ph0[i]) % 1.0
        pw = v[pre + 'Width'][0]; skew = v[pre + 'Skew'][0]; rnd = v[pre + 'Round'][0]
        cen = min(max((ph - (0.5 - pw)) / (2 * pw), 0), 1) if pw < 0.5 else ph
        th = 0.5 - skew * 0.5
        ph2 = min(0.5, cen / th * 0.5) if cen < th else 0.5 + 0.5 * (cen - th) / (1 - th)
        tri = max(0, 1 - abs(2 * ph2 - 1)); cs = (1 - math.cos(2 * math.pi * ph2)) / 2
        if rnd < 0.5: sh = tri + (cs - tri) * rnd / 0.5
        elif rnd < 1:
            raw = math.cos(2 * math.pi * ph2 + math.pi); pa = (rnd - 0.5) * 2; k2 = 2 * pa * 0.99999 / (1 - pa * 0.999999)
            sh = (raw * (k2 + 1) / (k2 * abs(raw) + 1) + 1) / 2
        else: sh = 1.0 if 0.25 <= ph2 < 0.75 else 0.0
        out.append(tension(sh, v[pre + 'Pow'][0]))
    return out
def corr(a, b):
    ma, mb = sum(a) / len(a), sum(b) / len(b)
    va = sum((x - ma) ** 2 for x in a); vb = sum((x - mb) ** 2 for x in b)
    if va < 1e-12 or vb < 1e-12: return 1.0 if abs(ma - mb) < 1e-3 else 0.0
    return sum((x - ma) * (y - mb) for x, y in zip(a, b)) / math.sqrt(va * vb)
for s, snap in enumerate(F):
    v = snap['values']; nv = v['Bands']; line = '%-24s' % snap['name']
    for pre, P in IDS.items():
        mode = int(round(float(V[str(P['mode'])][s])))
        if mode == 4:
            # monobrownian on one shared random table (both models read the same values): Reaktor's
            # band v = band 1's curve len x idx_v / nv cycles earlier; ours = the series at its phase
            import random
            rng = random.Random(5); table = [rng.random() for _ in range(4096)]
            def curve(x):
                c = math.floor(x); f = x - c
                v0, v1, v2, v3 = (table[int(c - 3 + k) % 4096] for k in range(4))
                return min(1, max(0, v1 + 0.5 * f * ((v2 - v0) + f * ((2 * v0 - 5 * v1 + 4 * v2 - v3) + f * (3 * (v1 - v2) + v3 - v0)))))
            g = lambda k: float(V[str(P[k])][s])
            L = g('len'); ln = L + 1 if L <= 0 else math.floor(15 * L + 1 + 0.5)
            Fq = 2.0 / g('div') * g('bpm'); hz = 2.0 / v[pre + 'Beats'][0]
            ph0 = indexer(nv, v[pre + 'Spread'], v[pre + 'Invert'], wrap=False)   # Random Same: unwrapped
            ts = [100 + k * 0.01 for k in range(400)]
            rarg = lambda t, b: Fq * t - ln * ((nv - (b - 1)) if g('inv') > 0 else b) / nv
            oarg = lambda t, b: hz * t + v[pre + 'Phase'] + ph0[b - 1]
            shift = oarg(0, 1) - rarg(0, 1)          # one time shift: band 1 aligned (the start is free)
            R = [[curve(rarg(t, b) + shift) for b in range(1, nv + 1)] for t in ts]
            O = [[curve(oarg(t, b)) for b in range(1, nv + 1)] for t in ts]
            cs = [corr([r[b] for r in R], [o[b] for o in O]) for b in range(nv)]
            line += '  %s mono corr %5.2f      ' % (pre[:4], sum(cs) / nv); continue
        if mode != 1: line += '  %s mode %d (random)        ' % (pre[:4], mode); continue
        ts = [k * 0.01 for k in range(400)]
        R = [reaktor(s, P, nv, t) for t in ts]; O = [ours(v, pre, nv, t) for t in ts]
        cs = [corr([r[b] for r in R], [o[b] for o in O]) for b in range(nv)]
        md = sum(abs(r[b] - o[b]) for r, o in zip(R, O) for b in range(nv)) / (len(ts) * nv)
        line += '  %s corr %5.2f diff %.3f' % (pre[:4], sum(cs) / nv, md)
    print(line)
