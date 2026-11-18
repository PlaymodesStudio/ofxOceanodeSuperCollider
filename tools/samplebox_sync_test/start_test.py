#!/usr/bin/env python3
"""A synced looping pad through transport start, stop, seek, start: where the
read position is right after each start, against the anchor's."""
import math, pathlib, subprocess, sys
sys.argv = [sys.argv[0], sys.argv[1], sys.argv[2], 'x']
import importlib.util
spec = importlib.util.spec_from_file_location('a', pathlib.Path(__file__).with_name('anchor_test.py')); a = importlib.util.module_from_spec(spec)
src = open(spec.origin).read().split("if __name__ == '__main__':")[0]; exec(src, a.__dict__)
OUT, DEF, SR, FRAMES, PERIOD, FIT, BPM = a.OUT, a.DEF, a.SR, a.FRAMES, a.PERIOD, a.FIT, a.BPM
m, b = a.message, a.bundle
starts = [(0.5003, 5.0), (4.0007, 20.0), (7.0011, 31.5)]     # (time, beat): the last one wraps the loop soon after
stops = [3.0, 6.0]
score = b(0, [m('/d_recv', DEF.read_bytes()), m('/b_allocRead', 0, str(OUT / 'sine.wav')), m('/g_new', 1, 0, 0)])
score += b(0.1, [m('/s_new', DEF.stem, 1001, 0, 1, 'buf0', 0, 'loaded', 1.0, 'loop', 1.0, 'sync', 1.0, 'fitbeats', FIT, 'fitrate', 1.0,
    'tbpm', BPM, 'run', 0.0, 'anchorPos', 0.0, 'anchorId', 1.0, 'modbus', 1000, 'vis', 2000, 'velbus', 2100, 'rndbus', 2200,
    'gatebus', 100, 'chokeblock', 2300, 'latestbus', 2400, 'out', 0, 'gain', 1.0, 'velocity', 1.0, 'pitch', 60.0, 'poly', 1.0, 'voices', 8.0)])
ev = []; aid = 1; fire = 0; stop = 0
for t, beat in starts:
    aid += 1; fire += 1
    ev.append((t, [m('/n_set', 1001, 'tbpm', BPM, 'run', 1.0, 'anchorPos', beat, 'anchorId', float(aid)), m('/n_set', 1001, 'fire', float(fire))]))
for t in stops:
    aid += 1; stop += 1
    ev.append((t, [m('/n_set', 1001, 'run', 0.0, 'anchorId', float(aid)), m('/n_set', 1001, 'stop', float(stop))]))
for t, msgs in sorted(ev, key=lambda e: e[0]): score += b(t, msgs)
score += b(9.0, [m('/c_set', 0, 0.0)])
osc = OUT / 'start.osc'; wav = OUT / 'start.wav'; osc.write_bytes(score)
subprocess.run([a.SCSYNTH, '-N', str(osc), '_', str(wav), str(SR), 'WAV', 'float', '-o', '1', '-c', '4096'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
y = a.read_wav(wav)
rate = FRAMES / (FIT * 60 / BPM) / SR
for t, beat in starts:
    for label, d0 in (('20 ms after', 0.02), ('1.5 s after', 1.5)):
        n0 = int((t + d0) * SR); n1 = n0 + 1000
        ss = cc = sc = ys = yc = 0.0
        for n in range(n0, n1):
            fr = ((beat + (n / SR - t) * BPM / 60) / FIT % 1.0) * FRAMES      # where the transport says the loop is
            ph = 2 * math.pi * fr / PERIOD
            s = math.sin(ph); c = math.cos(ph); v = y[n]
            ss += s * s; cc += c * c; sc += s * c; ys += v * s; yc += v * c
        det = ss * cc - sc * sc; A = (ys * cc - yc * sc) / det; B = (yc * ss - ys * sc) / det
        off = math.atan2(B, A) * PERIOD / (2 * math.pi) / rate
        print(f'start at beat {beat:5.1f}, {label}: level {math.hypot(A, B):.3f}, position {off:+7.2f} samples from the transport\'s')
for t in stops:
    n = int((t + 0.5) * SR); print(f'stopped at {t}: level 0.5 s later {max(abs(v) for v in y[n:n + 2000]):.5f}')
