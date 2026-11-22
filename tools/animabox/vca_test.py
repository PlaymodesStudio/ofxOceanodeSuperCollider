#!/usr/bin/env python3
"""AnimaBody's Amp VCA: with \\vca 1 the exciter follows the Amp target bus (an envelope routed to
Amp) instead of its ADSR, so it keeps sounding after the gate closes while the envelope releases.
Gate on 0.2..1.0 s; the Amp bus is 1 until 2.0 s (a long release), then 0. Without the VCA the
Body stops ~30 ms after the gate (its own release)."""
import sys, math, pathlib, tempfile
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sc
def main():
    work = pathlib.Path(tempfile.mkdtemp(prefix='animavca'))
    comp = sc.compile_script(work, 'Defaults/AnimaBox/animaBox.scd', [('(1..16).do { |n|', '[2].do { |n|')]) / 'AnimaBox'
    res = {}
    for vca in (0.0, 1.0):
        score = sc.bundle(0, [sc.message('/d_recv', (comp / 'AnimaBody.scsyndef').read_bytes()), sc.message('/c_set', 700, 1.0),
                              sc.message('/s_new', 'AnimaBody', 1000, 0, 0, 'out', 0, 'pitch', 60.0, 'oct', 0.0, 'gatebus', 400,
                                         'vel', 800, 'rnd', 900, 'vis', 950, 'vca', vca, 'mamp', 700)])
        score += sc.bundle(0.2, [sc.message('/n_set', 1000, 'gate', 1.0)])
        score += sc.bundle(1.0, [sc.message('/n_set', 1000, 'gate', 0.0)])
        score += sc.bundle(2.0, [sc.message('/c_set', 700, 0.0)])
        score += sc.bundle(3.0, [sc.message('/c_set', 0, 0.0)])
        data, ch, sr = sc.render(score, work / ('vca%d.wav' % vca), 2)
        rms = lambda a, b: math.sqrt(sum(x * x for x in data[int(a * sr) * ch:int(b * sr) * ch]) / max(1, int((b - a) * sr) * ch))
        res[vca] = (rms(0.4, 0.9), rms(1.2, 1.9), rms(2.2, 2.9))
        print('vca %d: held %.3f, after gate-off %.4f, after the envelope %.4f' % ((vca,) + res[vca]))
    ok = res[0][0] > 0.05 and res[0][1] < 0.01 and res[1][0] > 0.05 and res[1][1] > 0.05 and res[1][2] < 0.001
    print('PASS' if ok else 'FAIL'); return 0 if ok else 1
if __name__ == '__main__': sys.exit(main())
