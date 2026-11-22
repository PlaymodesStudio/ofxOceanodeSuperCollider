#!/usr/bin/env python3
"""End to end: the Init sound's LFO 1 (monobrownian = ModLfoX Random Smooth + Random Same,
phase spread 6 cycles over 35 bands, Pow -0.874) on 128-wide modulation buses (AnimaBox's
busWidth), ModRoute32 into the Energy target, read by AnimaBank16_2 chunks. The bank's per-band
levels (its vis bus) are recorded as extra output channels: they must stay in 0..1, move in time
and differ from band to band."""
import sys, math, json, pathlib, tempfile
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sc
HERE = pathlib.Path(__file__).resolve().parent
def main():
    work = pathlib.Path(tempfile.mkdtemp(prefix='animalfo'))
    bank = sc.compile_script(work / 'a', 'Defaults/AnimaBox/animaBox.scd', [('(1..16).do { |n|', '[2].do { |n|')]) / 'AnimaBox'
    modd = sc.compile_script(work / 'm', 'Defaults/Modulation/modulationRandom.scd', [('(1..32).do { |n|', '[32].do { |n|')]) / 'Modulation'
    route = sc.compile_script(work / 'r', 'Defaults/Modulation/modulation.scd', [('(1..32).do { |n|', '[32].do { |n|')]) / 'Modulation'
    sc._run(work / 'p', 'SynthDef(\\VisProbe, { Out.ar(2, K2A.ar(In.kr(\\vis.kr(0), 16))) }).writeDefFile("%s");' % (work / 'p'))
    v = json.load(open(HERE / 'reaktor/factory.json'))[0]['values']
    B = v['Bands']; beats = v['LFO1_Beats'][0]; hz = 2.0 / beats; spread = v['LFO1_Spread']
    SRC, TGT, VIS, EXC, OUT = 1000, 2000, 3000, 64, 80
    defs = [bank / 'AnimaBody.scsyndef', bank / 'AnimaBank16_2.scsyndef', modd / 'ModLfoX32.scsyndef', route / 'ModRoute32.scsyndef', work / 'p/VisProbe.scsyndef']
    msgs = [sc.message('/d_recv', p.read_bytes()) for p in defs] + [sc.message('/g_new', 1, 0, 0)]
    score = sc.bundle(0, msgs)
    m = []
    for c in range(2):         # 35 bands: 2 copies of 32 voices
        voices = range(c * 32, c * 32 + 32)
        m += [sc.message('/s_new', 'ModLfoX32', 100 + c, 1, 1, 'out', SRC + c * 32, 'voffset', float(c * 32), 'rtype', 2.0,
                         'rshare', 1.0, 'seed', 77.0, 'initphase', float(v['LFO1_Phase'])),
              sc.message('/n_setn', 100 + c, 'freq', 32, *[hz] * 32),
              sc.message('/n_setn', 100 + c, 'phoff', 32, *[spread * i / B for i in voices]),
              sc.message('/n_setn', 100 + c, 'pow', 32, *[v['LFO1_Pow'][0]] * 32),
              sc.message('/n_setn', 100 + c, 'modeweights', 6, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0),
              sc.message('/s_new', 'ModRoute32', 200 + c, 1, 1, 'src', SRC + c * 32, 'out', TGT + c * 32, 'amount', 1.0, 'scale', 1.0)]
    m += [sc.message('/s_new', 'AnimaBody', 300, 1, 1, 'out', EXC, 'gate', 0.8, 'pitch', 60.0, 'drone', 1.0, 'gatebus', 400,
                     'vel', 900, 'rnd', 950, 'vis', 990)]
    for c in range(3):
        m += [sc.message('/s_new', 'AnimaBank16_2', 400 + c, 1, 1, 'first', float(c * 16), 'exc', EXC, 'out', OUT, 'menergy', TGT,
                         'mq', 2500, 'mpitch', 2700, 'vis', VIS, 'reso', 29.9, 'gain', 16.0),
              sc.message('/n_setn', 400 + c, 'pitch', 16, *[48.0 + 2 * (c * 16 + i) for i in range(16)]),
              sc.message('/n_setn', 400 + c, 'active', 16, *[1.0 if c * 16 + i < B else 0.0 for i in range(16)])]
    m += [sc.message('/s_new', 'VisProbe', 500, 1, 1, 'vis', VIS)]
    score += sc.bundle(0.01, m)
    # zero the target block each block, as the node does for unrouted targets: routes add (Out.kr)
    score += sc.bundle(6.0, [sc.message('/c_set', 0, 0.0)])
    data, ch, sr = sc.render(score, work / 'lfo.wav', 18)
    bands = [[data[i * ch + 2 + b] for i in range(0, len(data) // ch, sr // 50)] for b in range(16)]
    flat = [x for b in bands for x in b[10:]]
    lo, hi = min(flat), max(flat)
    moving = [max(b[10:]) - min(b[10:]) for b in bands]
    diff = max(abs(bands[0][k] - bands[5][k]) for k in range(10, len(bands[0])))
    print('energy range %.3f .. %.3f; per-band movement min %.3f; band 1 vs 6 max diff %.3f' % (lo, hi, min(moving), diff))
    ok = lo >= -1e-6 and hi <= 1.0001 and min(moving) > 0.05 and diff > 0.05
    print('PASS' if ok else 'FAIL'); return 0 if ok else 1
if __name__ == '__main__': sys.exit(main())
