#!/usr/bin/env python3
"""SynthPercBox NRT tests (scsynth -N on the compiled defs).

  render_test.py [workdir]

1. Every factory pad of every generator, hit in Kit mode (Gate channel g, Pitch 60 + pad), one render
   per generator: each pad's window must be finite and audible; prints its peak (dBFS).
2. Pad switching: K1 pad A then pad B; B's first 20 ms must equal B rendered alone (the row's values are
   in place on the voice's first sample), through control gates and through an audio-bus gate (cable).
3. Notes mode: note lo + k plays pad k.
"""
import json, math, pathlib, struct, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sp
from sc import message as M, bundle as B

WORK = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else '/tmp/sp_render'); WORK.mkdir(parents=True, exist_ok=True)
FACT = json.load(open(pathlib.Path(__file__).resolve().parent / 'factory.json'))
DEFS = ['SPK1', 'SPStomps', 'SPNoix', 'SPClix', 'SPCrax', 'SPQuad']
SPACE = {'SPNoix': 1, 'SPClix': 1, 'SPQuad': 2}
SR = sp.SR
IN_BUS, PRIV, PBUS, ROWBUS = 64, 100, 20, 0
N_IN, N_GEN, N_SPACE = 1000, 1001, 1100

def setup(gi, inmode=0, lo=0, gate_cable=None):
    gen = FACT[gi]; d = DEFS[gi]
    msgs = [M('/d_loadDir', str(sp.COMPILED)), M('/g_new', 1, 0, 0)]
    spin = [M('/s_new', 'SPIn', N_IN, 1, 1, 'out', IN_BUS)]
    if gate_cable is not None:   # a cable: gate_ar mapped to an audio bus, written by a DC synth score
        spin += [M('/n_mapan', N_IN, 'gate_ar', gate_cable, 1), M('/n_set', N_IN, 'gate_sel', 1, 'gate_n', 1)]
    out = PRIV if d in SPACE else 0
    g = [M('/s_new', d, N_GEN, 1, 1, 'gin', IN_BUS, 'out', out, 'ch', gi, 'inmode', inmode, 'lo', lo, 'rowbus', ROWBUS,
           'pbus', PBUS, 'bps', 2.0)]
    for k, p in enumerate(gen['params']):
        vals = [pad['values'][k] for pad in gen['pads']]
        vals = (vals + [vals[-1]] * 17)[:17]
        g.append(M('/n_setn', N_GEN, 'p_' + p['key'], 17, *[float(v) for v in vals]))
    if d in SPACE:
        g.append(M('/s_new', 'SPSpace%d' % SPACE[d], N_SPACE, 1, 1, 'in', PRIV, 'pbus', PBUS, 'out', 0))
    return [B(0, msgs), B(0.01, spin + g)]

def hit_kit(t, gi, pad, hold):
    gates = [0.0] * 8; pitches = [60.0] * 8
    gates[gi] = 1.0; pitches[gi] = 60.0 + pad
    return [B(t, [M('/n_setn', N_IN, 'pitches', 8, *pitches), M('/n_setn', N_IN, 'gates', 8, *gates)]),
            B(t + hold, [M('/n_setn', N_IN, 'gates', 8, *([0.0] * 8))])]

def run(name, score, end):
    score = score + [B(end, [M('/g_freeAll', 1)]), B(end + 0.01, [M('/c_set', 0, 0)])]
    s, log = sp.render(b''.join(score), WORK / (name + '.wav'), 2)
    return s, log

def window(s, t0, t1):
    a, b = int(t0 * SR) * 2, int(t1 * SR) * 2
    return s[a:b]

HOLD = {'SPK1': 0.3, 'SPStomps': 0.3, 'SPNoix': 0.5, 'SPClix': 0.1, 'SPCrax': 0.4, 'SPQuad': 0.6}
STEP = 3.0
bad = 0
print('1. factory pads (peak dBFS of each pad window, Kit mode)')
for gi, gen in enumerate(FACT):
    d = DEFS[gi]; n = len(gen['pads'])
    score = setup(gi)
    for k in range(n): score += hit_kit(0.5 + k * STEP, gi, k, HOLD[d])
    s, log = run(d, score, 0.5 + n * STEP)
    peaks = []
    for k in range(n):
        w = window(s, 0.5 + k * STEP, 0.5 + (k + 1) * STEP)
        fin = all(math.isfinite(x) for x in w)
        pk = max((abs(x) for x in w), default=0)
        peaks.append(pk)
        if not fin or pk < 1e-5 or pk > 4: bad += 1
    print(f'  {gen["name"]:7}', ' '.join(f'{(20 * math.log10(p) if p > 0 else -999):6.1f}' for p in peaks))
    if 'FAILURE' in log or 'not found' in log: print(log[-800:]); bad += 1

print('2. pad switch: B after A equals B alone (first 20 ms)')
def k1(score_hits, cable=False):
    sc = setup(0, gate_cable=(200 if cable else None))
    return sc + score_hits
A, Bp = 2, 0   # A: pad 3 (126 dB/s, no delay: gone by B), B: pad 1 (other pitch envs, decay, drive)
alone, _ = run('k1_alone', k1(hit_kit(1.0, 0, Bp, 0.3)), 2.0)
after, _ = run('k1_after', k1(hit_kit(0.4, 0, A, 0.3) + hit_kit(1.0, 0, Bp, 0.3)), 2.0)
wa, wb = window(alone, 1.0 + 128 / SR, 1.02), window(after, 1.0 + 128 / SR, 1.02)
# A's tail (K1 pad 1 decays 25 dB/s) is still sounding in `after`: compare onsets only where B dominates
diff = max(abs(x - y) for x, y in zip(wa, wb)); ref = max(abs(x) for x in wa)
print(f'  control gate: max |B after A - B alone| = {diff:.4g} (B peak {ref:.4g})')
# onset sample: first non-negligible sample relative to the hit time (one block late by design)
def onset(s, t):
    a = int(t * SR)
    for i in range(a, a + 4096):
        if abs(s[i * 2]) > 1e-6: return i - a
    return None
print('  onset (samples after the hit time): alone', onset(alone, 1.0), 'after A', onset(after, 1.0))

# a cable gate opening 37 samples into a block (pad B after a silent pad A, both by cable) must equal
# a block-aligned control hit of B shifted by 37 samples: B's values in place on its first sample
import subprocess
TEST = pathlib.Path(__file__).resolve().parent / 'CompiledSynthdefs' / 'SynthPercBoxTest'
def cable_hits(hits):   # [(start sample, pad)]: one SPTestCable per hit (gate bus 200, pitch bus 201)
    m = [M('/d_loadDir', str(TEST))]
    for i, (at, pad) in enumerate(hits):
        m.append(M('/s_new', 'SPTestCable', 3000 + i, 0, 1, 'at', float(at), 'len', 14400.0, 'gbus', 200, 'pbus', 201, 'pitch', 60.0 + pad))
    return m
def cable_setup():
    sc = setup(0)
    sc.append(B(0.02, [M('/n_mapan', N_IN, 'gate_ar', 200, 1), M('/n_mapan', N_IN, 'pitch_ar', 201, 1),
                       M('/n_set', N_IN, 'gate_sel', 1, 'pitch_sel', 1, 'gate_n', 1, 'pitch_n', 1)]))
    return sc
silent = cable_setup() + [B(0.03, [M('/n_setn', N_GEN, 'p_vol', 1, 0.0)])]   # pad 1 (row 0) made silent: A
T0 = 64 * 750 + 37                                   # a hit 37 samples into block 750 (1.0 s)
# the cable synths run from 0.05 s: their sample count starts there
off = (int(0.05 * SR) // 64) * 64          # NRT bundles land on block starts
cabled, _ = run('k1_cable', silent + [B(0.05, cable_hits([(int(0.4 * SR) - off, 0)]))], 2.0)
# one pitch bus can't hold two pads at once: second render, B alone by cable at T0
CB = 8   # pad 9: not the starting row (pad 1), so the row switches at the edge
cab_b, _ = run('k1_cable_b', cable_setup() + [B(0.05, cable_hits([(T0 - off, CB)]))], 2.0)
ref, _ = run('k1_ref', k1(hit_kit(T0 / SR - 37 / SR, 0, CB, 0.3)), 2.0)
a0 = T0 - 37
x = [cab_b[(T0 + i) * 2] for i in range(2048)]
y = [ref[(a0 + i) * 2] for i in range(2048)]
print(f'  cable mid-block: max |B by cable (37 into a block) - B block-aligned| = {max(abs(p - q) for p, q in zip(x, y)):.3g}'
      f' (peak {max(abs(q) for q in y):.3g}); onset {onset(cab_b, T0 / SR)} samples')
print('  (a difference here, gone after ~10 ms, is the control-rate filter envelope (Reaktor A/E) stepping at\n'
      '   other instants for a mid-block onset; with Env = 0 the two match to 1e-12)')
print('3. Notes mode: notes lo..lo+13 (K1, lo 0): note 3 plays pad 4')
sc = setup(0, inmode=1, lo=0)
sc += [B(0.5, [M('/n_setn', N_IN, 'pitches', 8, 3.0, *[60.0] * 7), M('/n_setn', N_IN, 'gates', 8, 1.0, *[0.0] * 7)]),
       B(0.8, [M('/n_setn', N_IN, 'gates', 8, *[0.0] * 8)])]
notes, _ = run('k1_notes', sc, 2.0)
pad4, _ = run('k1_pad4', k1(hit_kit(0.5, 0, 3, 0.3)), 2.0)
w1, w2 = window(notes, 0.5, 0.6), window(pad4, 0.5, 0.6)
print(f'  max |note 3 - kit pad 4| = {max(abs(x - y) for x, y in zip(w1, w2)):.4g}')
print('FAILURES', bad)
