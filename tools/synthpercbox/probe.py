#!/usr/bin/env python3
"""Render one generator pad with overrides: probe.py GEN PAD key=value ... (peak dBFS)"""
import json, math, pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sp
from sc import message as M, bundle as B
FACT = json.load(open(pathlib.Path(__file__).resolve().parent / 'factory.json'))
DEFS = {'K1': 'SPK1', 'STOMPS': 'SPStomps', 'NOIX': 'SPNoix', 'CLIX': 'SPClix', 'CRAX': 'SPCrax', 'QUAD': 'SPQuad'}
SPACE = {'SPNoix': 1, 'SPClix': 1, 'SPQuad': 2}

def render(gen, pad, over, hold=0.4, dur=2.5, wav='/tmp/sp_probe.wav'):
    gi = [g['name'] for g in FACT].index(gen); g = FACT[gi]; d = DEFS[gen]
    vals = dict(zip([p['key'] for p in g['params']], g['pads'][pad]['values'])); vals.update(over)
    out = 100 if d in SPACE else 0
    m = [M('/s_new', 'SPIn', 1000, 1, 1, 'out', 64), M('/s_new', d, 1001, 1, 1, 'gin', 64, 'out', out, 'ch', gi, 'pbus', 20, 'bps', 2.0)]
    m += [M('/n_setn', 1001, 'p_' + k, 17, *([float(v)] * 17)) for k, v in vals.items()]
    if d in SPACE: m.append(M('/s_new', 'SPSpace%d' % SPACE[d], 1002, 1, 1, 'in', 100, 'pbus', 20, 'out', 0))
    gates = [0.0] * 8; gates[gi] = 1.0
    sc = [B(0, [M('/d_loadDir', str(sp.COMPILED)), M('/g_new', 1, 0, 0)]), B(0.01, m),
          B(0.2, [M('/n_setn', 1000, 'gates', 8, *gates)]), B(0.2 + hold, [M('/n_setn', 1000, 'gates', 8, *[0.0] * 8)]),
          B(dur, [M('/g_freeAll', 1)]), B(dur + 0.01, [M('/c_set', 0, 0)])]
    s, log = sp.render(b''.join(sc), wav, 2)
    pk = max(abs(x) for x in s)
    return 20 * math.log10(pk) if pk > 0 else -999

if __name__ == '__main__':
    gen, pad = sys.argv[1], int(sys.argv[2]) - 1
    over = dict((kv.split('=')[0], float(kv.split('=')[1])) for kv in sys.argv[3:])
    print(round(render(gen, pad, over), 1))
