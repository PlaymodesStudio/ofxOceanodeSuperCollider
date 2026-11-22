#!/usr/bin/env python3
"""Render every AnimaBox factory sound offline (reaktor/factory.json): the band pitches as
scAnimaBox::computeBands (default ordering paths only: Step, Key, Mode, Root/Octave, Scale or
Overtones, Band Octave), each band's energy a fixed draw shaped by LFO 1's Pow (the LFO's mean
level, no movement), Reso / Gain / Stereo Detune / Post Filter from the sound, the Body a saw note
(C4, gate 0.8) through MetaVerb 0.25. Prints per sound: pitch range, RMS / peak, NaN.
  factory_render_test.py [out.wav]"""
import sys, math, json, random, pathlib, tempfile
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sc
HERE = pathlib.Path(__file__).resolve().parent
def tension(x, p):
    a = 0.999 * max(-1, min(1, p)); k = 2 * a / (1 - a); return x * (k + 1) / (k * abs(x) + 1)
def lcg(seed): return ((1103515245 * seed + 12345) & 0x7FFFFFFF) / 2147483648.0
def pitches(v):
    B = int(v['Bands']); out = []
    sc_ = v['Scale'] or [0]; n = len(sc_); absolute = any(x >= 12 for x in sc_)
    base = 0 if absolute else round(v['Root']) + 12 * round(v['Octave'])
    key = round(v['Key']) + round(v['Mode'])
    for V in range(1, B + 1):
        skew = v['Index_Skew']; step = v['Step']
        if skew > 0.5: idx = round(V * step)
        elif skew < -0.5: idx = round((B + 1 - V) * step)
        else: idx = round(min(V, B + 1 - V) * step)
        deg = idx + key
        if v['Tuning'] > 0.5:
            h = {0: 2 * deg + 2, 1: 2 * deg + 1, 2: deg + 1}[round(v['Overtones'])]
            if h <= 0: out.append(None); continue
            p = 1 + 12 * math.log2(h) + round(v['Root']) + 12 * round(v['Octave'])
        else:
            m = math.floor(deg / n); p = sc_[deg - m * n] + 12 * m + base
        out.append(p + 12 * round(v['Band_Octave']) + v['Semitone'] + v['Fine'])
    return out
def main():
    wav = sys.argv[1] if len(sys.argv) > 1 else None
    snaps = json.load(open(HERE / 'reaktor/factory.json'))
    work = pathlib.Path(tempfile.mkdtemp(prefix='animafactory'))
    comp = sc.compile_script(work, 'Defaults/AnimaBox/animaBox.scd', [('(1..16).do { |n|', '[2].do { |n|')]) / 'AnimaBox'
    defs = [sc.message('/d_recv', p.read_bytes()) for p in sorted(comp.glob('*.scsyndef'))]
    EXC, BANK, POST, CTL = 64, 80, 100, 500
    dur = 5.0; score = sc.bundle(0, defs + [sc.message('/g_new', 1, 0, 0)])
    info = []
    for k, s in enumerate(snaps):
        v = s['values']; t = k * dur + 0.05; nid = 2000 + k * 100
        P = pitches(v); B = len(P)
        rng = random.Random(k)
        msgs = [sc.message('/s_new', 'AnimaBody', nid, 1, 1, 'out', EXC, 'gate', 0.0, 'pitch', 60.0, 'oct', -3.0,
                           'gatebus', 400, 'vel', CTL + 300, 'rnd', CTL + 440, 'vis', CTL + 600)]
        for c in range((B + 15) // 16):
            f = c * 16; ids = range(f, f + 16)
            pv = [float(P[i]) if i < B and P[i] is not None else 60.0 for i in ids]
            act = [1.0 if i < B and P[i] is not None else 0.0 for i in ids]
            en = [tension(rng.random(), v['LFO1_Pow'][0]) for _ in ids]
            msgs += [sc.message('/s_new', 'AnimaBank16_2', nid + 1 + c, 1, 1, 'first', float(f), 'exc', EXC, 'out', BANK,
                                'menergy', CTL, 'mq', CTL + 128, 'mpitch', CTL + 256, 'vis', CTL + 700,
                                'reso', float(v['Reso']), 'gain', float(v['Gain']), 'sdet', float(v['Stereo_Detune']),
                                'fine', float(v['Resonance_Fine'])),
                     sc.message('/n_setn', nid + 1 + c, 'pitch', 16, *pv), sc.message('/n_setn', nid + 1 + c, 'energy', 16, *en),
                     sc.message('/n_setn', nid + 1 + c, 'active', 16, *act),
                     sc.message('/n_setn', nid + 1 + c, 'pana', 16, *[lcg(i + 1) for i in ids]),
                     sc.message('/n_setn', nid + 1 + c, 'panb', 16, *[1 - lcg(i + 121) for i in ids])]
        msgs += [sc.message('/s_new', 'AnimaPost2', nid + 50, 1, 1, 'in', BANK, 'out', POST, 'type', float(v['Post_Type']),
                            'cutoff', float(v['Post_Cutoff']), 'reson', float(v['Post_Reson']), 'gainc', float(v['Post_Gain_Correction'])),
                 sc.message('/s_new', 'AnimaMetaVerb2', nid + 51, 1, 1, 'in', POST, 'out', 0, 'mix', 0.25)]
        score += sc.bundle(t, msgs)
        score += sc.bundle(t + 0.2, [sc.message('/n_set', nid, 'gate', 0.8)])
        score += sc.bundle(t + 3.2, [sc.message('/n_set', nid, 'gate', 0.0)])
        score += sc.bundle(t + dur - 0.05, [sc.message('/n_free', nid, *[nid + 1 + c for c in range((B + 15) // 16)], nid + 50, nid + 51)])
        good = [p for p in P if p is not None]
        info.append((s['name'], B, min(good), max(good)))
    score += sc.bundle(len(snaps) * dur + 0.1, [sc.message('/c_set', 0, 0.0)])
    out = pathlib.Path(wav) if wav else work / 'factory.wav'
    data, ch, sr = sc.render(score, out, 2)
    ok = True
    for k, (name, B, lo, hi) in enumerate(info):
        seg = data[int((k * dur + 0.5) * sr) * ch:int((k * dur + 3.2) * sr) * ch]
        bad = sum(1 for x in seg if x != x)
        rms = math.sqrt(sum(x * x for x in seg if x == x) / max(1, len(seg))); peak = max(abs(x) for x in seg if x == x)
        flag = '' if 0.003 < rms and peak < 4 and not bad else '  <-- CHECK'
        ok &= not flag
        print('%-24s bands %3d  pitch %6.1f..%6.1f  rms %7.4f (%6.1f dB)  peak %6.3f%s%s' % (name, B, lo, hi, rms, 20 * math.log10(max(rms, 1e-9)), peak, ' NaN %d' % bad if bad else '', flag))
    print('wav:', out)
    return 0 if ok else 1
if __name__ == '__main__': sys.exit(main())
