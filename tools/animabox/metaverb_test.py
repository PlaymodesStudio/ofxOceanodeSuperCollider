#!/usr/bin/env python3
"""MetaVerb (synthdefs/Effect/Time/Reverb/metaVerb.scd): compile 1/2/4 channels, render an impulse
through each, check finite output, energy on every channel and the decay time against the decoded
Reaktor numbers (spec: Schroeder RT60 ~1.9 s at Size 0.5, Hi/Lo Cut 0 -> flat)."""
import sys, math, pathlib, tempfile
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sc

def rt60(x, sr):
    # Schroeder backward integration, RT60 from the -5..-35 dB slope (T30 x 2)
    e = [0.0] * len(x); acc = 0.0
    for i in range(len(x) - 1, -1, -1): acc += x[i] * x[i]; e[i] = acc
    def t(db):
        thr = e[0] * 10 ** (db / 10)
        for i, v in enumerate(e):
            if v < thr: return i / sr
        return None
    a, b = t(-5), t(-35)
    return None if a is None or b is None else 2 * (b - a)

def main():
    work = pathlib.Path(tempfile.mkdtemp(prefix='metaverb'))
    ok = True
    for n in (1, 2, 4):
        out = sc.compile_exact(work / str(n), 'Effect/Time/Reverb/metaVerb.scd', n)
        d = (out / 'MetaVerb' / ('MetaVerb%d.scsyndef' % n)).read_bytes()
        data, ch, sr = render_with_impulse(work, n, d)
        chans = [data[c::ch] for c in range(ch)]
        finite = all(x == x and abs(x) < 1e3 for x in data)
        energies = [math.sqrt(sum(x * x for x in c) / len(c)) for c in chans]
        t = rt60([sum(c[i] for c in chans) for i in range(len(chans[0]))], sr)
        print('MetaVerb%d: finite %s, rms per channel %s, RT60 %.2f s' % (n, finite, ['%.4f' % e for e in energies], t or -1))
        ok &= finite and min(energies) > 1e-4 and t is not None and 1.6 < t < 2.3
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1

def render_with_impulse(work, n, mvdef):
    # impulse source: a SynthDef compiled once with the reverb run
    probe = work / 'probe'
    probe.mkdir(exist_ok=True)
    if not (probe / 'MVImpulse.scsyndef').exists():
        sc._run(probe, 'SynthDef(\\MVImpulse, { Out.ar(64, Impulse.ar(0) ! %d); Line.kr(0,1,0.1,doneAction:2) }).writeDefFile("%s");' % (4, probe))
    score = sc.bundle(0, [sc.message('/d_recv', mvdef), sc.message('/d_recv', (probe / 'MVImpulse.scsyndef').read_bytes()),
                          sc.message('/g_new', 1, 0, 0)])
    score += sc.bundle(0.01, [sc.message('/s_new', 'MetaVerb%d' % n, 1000, 1, 1, 'in', 64, 'out', 0,
                                         'size', 0.5, 'locut', 0.0, 'hicut', 1.0, 'mix', 1.0)])
    score += sc.bundle(0.5, [sc.message('/s_new', 'MVImpulse', 1001, 0, 1)])
    score += sc.bundle(8.0, [sc.message('/c_set', 0, 0.0)])
    return sc.render(score, work / ('mv%d.wav' % n), n)

if __name__ == '__main__': sys.exit(main())
