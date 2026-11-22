#!/usr/bin/env python3
"""AnimaBox chain offline: AnimaBody (saw, gate) -> AnimaBank16_<n> x chunks -> AnimaPost<n>
-> AnimaMetaVerb<n>. Checks finite output, a sane level, and how fast NRT renders a full bank
(realtime factor = audio seconds / wall seconds).
  chain_test.py [channels=2] [bands=120]"""
import sys, math, time, pathlib, tempfile
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sc
def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 2
    bands = int(sys.argv[2]) if len(sys.argv) > 2 else 120
    work = pathlib.Path(tempfile.mkdtemp(prefix='animachain'))
    comp = sc.compile_script(work, 'Defaults/AnimaBox/animaBox.scd', [('(1..16).do { |n|', '[%d].do { |n|' % n)]) / 'AnimaBox'
    defs = [sc.message('/d_recv', p.read_bytes()) for p in sorted(comp.glob('*.scsyndef'))]
    EXC, BANK, POST, CTL = 64, 80, 100, 500     # audio buses / control bus base
    scale = [0, 2, 4, 7, 9]
    chunks = (bands + 15) // 16
    t0 = defs + [sc.message('/g_new', 1, 0, 0)]
    msgs = [sc.message('/s_new', 'AnimaBody', 1000, 1, 1, 'out', EXC, 'gate', 0.0, 'pitch', 48.0, 'oct', 0.0,
                       'gatebus', 400, 'vel', CTL + 300, 'rnd', CTL + 440, 'vis', CTL + 600)]
    msgs.append(sc.message('/n_setn', 1000, 'osc', 2, 1.0, 1.0))
    for c in range(chunks):
        first = c * 16
        pitches = [36 + scale[(first + b) % 5] + 12 * ((first + b) // 5) for b in range(16)]
        energy = [0.5 + 0.5 * math.sin(0.4 * (first + b)) for b in range(16)]
        pana = [((first + b) * 0.618) % 1 for b in range(16)]
        panb = [((first + b) * 0.382 + 0.5) % 1 for b in range(16)]
        active = [1.0 if first + b < bands else 0.0 for b in range(16)]
        msgs += [sc.message('/s_new', 'AnimaBank16_%d' % n, 1100 + c, 1, 1, 'first', float(first), 'exc', EXC, 'out', BANK,
                            'menergy', CTL, 'mq', CTL + 128, 'mpitch', CTL + 256, 'vis', CTL + 700, 'reso', 40.0, 'gain', 16.0),
                 sc.message('/n_setn', 1100 + c, 'pitch', 16, *map(float, pitches)),
                 sc.message('/n_setn', 1100 + c, 'energy', 16, *energy),
                 sc.message('/n_setn', 1100 + c, 'pana', 16, *pana),
                 sc.message('/n_setn', 1100 + c, 'panb', 16, *panb),
                 sc.message('/n_setn', 1100 + c, 'active', 16, *active)]
    msgs += [sc.message('/s_new', 'AnimaPost%d' % n, 1200, 1, 1, 'in', BANK, 'out', POST, 'type', 0.0, 'cutoff', 127.0),
             sc.message('/s_new', 'AnimaMetaVerb%d' % n, 1300, 1, 1, 'in', POST, 'out', 0, 'mix', 0.4)]
    score = sc.bundle(0, t0) + sc.bundle(0.01, msgs)
    score += sc.bundle(0.5, [sc.message('/n_set', 1000, 'gate', 0.8)])
    score += sc.bundle(4.5, [sc.message('/n_set', 1000, 'gate', 0.0)])
    score += sc.bundle(8.0, [sc.message('/c_set', 0, 0.0)])
    start = time.time()
    data, ch, sr = sc.render(score, work / 'chain.wav', n)
    wall = time.time() - start
    finite = all(x == x and abs(x) < 100 for x in data)
    held = data[int(1.5 * sr) * ch:int(4.5 * sr) * ch]
    rms = math.sqrt(sum(x * x for x in held) / len(held)); peak = max(abs(x) for x in data)
    print('channels %d bands %d: finite %s, held rms %.4f (%.1f dBFS), peak %.3f, NRT %.1fx realtime -> %s'
          % (n, bands, finite, rms, 20 * math.log10(max(rms, 1e-9)), peak, 8.0 / wall, work / 'chain.wav'))
    return 0 if finite and 1e-4 < rms and peak < 20 else 1
if __name__ == '__main__': sys.exit(main())
