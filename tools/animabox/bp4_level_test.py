#!/usr/bin/env python3
"""Level calibration of the band filter: ReaktorMulti4 BP4 (type 9) at 1 kHz for several
resonances, sine at the centre and white noise, times AnimaMundi's gain correction
gc(P, Res) * 0.1585 * dbamp(-ResL * s3(ResL/100)) (spec_bank.md 3b). Prints dB."""
import sys, math, pathlib, tempfile
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sc
RESL = [0, 10, 20, 30, 40, 60, 80, 100]
def res_of(r): return min(0.995, 1 - 10 ** (-r / 40))
def s3(x):
    pts = [(0, .40), (.25, .57), (.5, .71), (.75, .71), (1, .71)]
    x = min(max(x, 0), 1)
    for (a, ya), (b, yb) in zip(pts, pts[1:]):
        if x <= b: return ya + (yb - ya) * (x - a) / (b - a)
    return .71
def gcorr(r, hz=1000.0, sr=48000.0):
    res = res_of(r)
    return 10 ** (-73 * (1 - res) * hz / sr / 20) * 0.1585 * 10 ** (-r * s3(r / 100) / 20)
def main():
    work = pathlib.Path(tempfile.mkdtemp(prefix='bp4'))
    for src, name in (('SinOsc.ar(1000) * 0.5', 'sine'), ('WhiteNoise.ar * 0.5', 'noise')):
        body = 'SynthDef(\\BP4Probe, { var x = %s; Out.ar(0, [%s] ++ [x]); }).writeDefFile("%s");' % (
            src, ', '.join('ReaktorMulti4.ar(x, 1000, %f, 9)' % res_of(r) for r in RESL), work)
        sc._run(work, body)
        score = sc.bundle(0, [sc.message('/d_recv', (work / 'BP4Probe.scsyndef').read_bytes()),
                              sc.message('/s_new', 'BP4Probe', 1000, 0, 0)])
        score += sc.bundle(6.0, [sc.message('/c_set', 0, 0.0)])
        data, ch, sr = sc.render(score, work / (name + '.wav'), len(RESL) + 1)
        start = 4 * sr * ch
        rms = [math.sqrt(sum(v * v for v in data[start + c::ch]) / ((len(data) - start) // ch)) for c in range(ch)]
        ref = rms[-1]
        print(name)
        for r, v in zip(RESL, rms):
            g = 20 * math.log10(v / ref); c = 20 * math.log10(gcorr(r))
            print('  ResL %3d dB  Res %.4f  filter %+6.1f dB  corr %+6.1f dB  total %+6.1f dB' % (r, res_of(r), g, c, g + c))
if __name__ == '__main__': main()
