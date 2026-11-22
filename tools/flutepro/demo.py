#!/usr/bin/env python3
"""FlutePro listening renders (48 kHz, mono) and checks.
  demo.py <work> <out dir>
work/defs must hold Flute1.scsyndef and FlutePro1.scsyndef (sc.compile_exact of flute.scd / flutepro.scd, n = 1).
  ab_low.wav       Flute then FlutePro, defaults, notes 36 48 60 72, 2 s each
  scale_high.wav   FlutePro, gated notes C5..C8 (piano-roll style), Lpf/Keytrack defaults
  overblow.wav     FlutePro held at C4 / G3, overblow 0 > 1 > 2 > 3 in steps, then a slow sweep
  feedback.wav     FlutePro feedbacks to 1 and back: it must come back (Flute stays broken)
  melody.wav       a little gated phrase with Air, Chiff, ADSR
  envmodes.wav     Env Mode Exciter / PostGain / Both, sustain 0.6, attack 0.3
  envelopes.wav    held C5 notes: sustain 0.5 / 0.25, attack 1 s, release 1 s (level and pitch checked)
"""
import sys, pathlib, math
import numpy as np
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sc
work, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
D = work / 'defs'
def load(): return sc.bundle(0, [sc.message('/d_load', str(D / 'FlutePro1.scsyndef')), sc.message('/d_load', str(D / 'Flute1.scsyndef'))])
def new(t, name, node, **kw): return sc.bundle(t, [sc.message('/s_new', name, node, 0, 0, *[x for k, v in kw.items() for x in (k, float(v))])])
def setp(t, node, **kw): return sc.bundle(t, [sc.message('/n_set', node, *[x for k, v in kw.items() for x in (k, float(v))])])
def free(t, node): return sc.bundle(t, [sc.message('/n_free', node)])
def end(t): return sc.bundle(t, [sc.message('/c_set', 0, 0.0)])
def rms(x): return math.sqrt(float(np.mean(np.square(x)))) if len(x) else 0
def render(score, name):
    x, _, sr = sc.render(score, out / name, 1)
    x = np.array(x); print(f'{name}: peak {np.max(np.abs(x)):.3f}  finite {np.all(np.isfinite(x))}'); return x, sr

# A/B low: flute.scd itself, then FlutePro, both at their defaults
s = load(); t = 0.01; node = 1000
for nt in [36, 48, 60, 72]:
    s += new(t, 'Flute1', node, pitch=nt, levels=1); s += free(t + 2, node); node += 1; t += 2.2
for nt in [36, 48, 60, 72]:
    s += new(t, 'FlutePro1', node, pitch=nt, levels=1); s += free(t + 2, node); node += 1; t += 2.2
x, sr = render(s + end(t), 'ab_low.wav')

# high scale, gated
s = load() + new(0.01, 'FlutePro1', 2000, pitch=72, gate=0, levels=1, release=0.08, attack=0.03)
t = 0.2; notes = [72, 76, 79, 84, 88, 91, 96, 100, 103, 108]
for nt in notes:
    s += setp(t, 2000, pitch=nt, gate=0.9); s += setp(t + 0.45, 2000, gate=0); t += 0.55
x, sr = render(s + end(t + 0.5), 'scale_high.wav')

# overblow
s = load(); t = 0.01
for i, nt in enumerate([60, 55]):
    s += new(t, 'FlutePro1', 3000 + i, pitch=nt, levels=1)
    for k, ob in enumerate([0, 1, 2, 3, 0]): s += setp(t + 0.1 + 1.2 * k, 3000 + i, overblow=ob)
    t += 6.2
    s += free(t, 3000 + i); t += 0.2
s += new(t, 'FlutePro1', 3010, pitch=60, levels=1)
for k in range(61): s += setp(t + k * 0.1, 3010, overblow=3 * k / 60)
t += 6.5; s += free(t, 3010)
x, sr = render(s + end(t + 0.1), 'overblow.wav')

# feedback
s = load() + new(0.01, 'FlutePro1', 4000, pitch=60, levels=1)
s += setp(1.5, 4000, ifeedbk1=1, ifeedbk2=1) + setp(3.5, 4000, ifeedbk1=0.4, ifeedbk2=0.4)
x, sr = render(s + end(5.5), 'feedback.wav')
print('   rms per second:', ' '.join(f'{rms(x[int(k*sr):int((k+1)*sr)]):.2f}' for k in range(5)))

# melody with air / chiff
s = load() + new(0.01, 'FlutePro1', 5000, pitch=74, gate=0, levels=1, air=0.35, chiff=0.5, attack=0.06, decay=0.3, sustain=0.85, release=0.25, vibamp=0.4)
t = 0.2
for nt, d in [(74, .4), (76, .4), (77, .4), (79, .8), (81, .4), (84, .4), (86, 1.2), (84, .4), (81, .4), (79, 1.4)]:
    s += setp(t, 5000, pitch=nt, gate=0.85); s += setp(t + d * 0.85, 5000, gate=0); t += d
x, sr = render(s + end(t + 0.8), 'melody.wav')
tail = x[int((t + 0.4) * sr):int((t + 0.75) * sr)]
print('   release tail rms (should be ~0):', f'{rms(tail):.5f}')

# release level just after a gate-off, against the sustain before it
s = load() + new(0.01, 'FlutePro1', 6000, pitch=72, gate=0, levels=1, release=0.5)
s += setp(0.2, 6000, gate=0.8) + setp(1.2, 6000, gate=0)
x, sr = render(s + end(2.0), 'release.wav')
print(f'   sustain rms {rms(x[int(0.9*sr):int(1.15*sr)]):.3f}  release 20-100 ms {rms(x[int(1.22*sr):int(1.3*sr)]):.3f}  300-400 ms {rms(x[int(1.5*sr):int(1.6*sr)]):.3f}')

# envelopes: sustain below full, slow attack, slow release
s = load(); t = 0.01; cases = [dict(sustain=0.5, decay=0.3), dict(sustain=0.25, decay=0.3), dict(attack=1.0), dict(release=1.0)]
for i, c in enumerate(cases):
    s += new(t, 'FlutePro1', 7000 + i, pitch=72, gate=0, levels=1, **c)
    s += setp(t + 0.05, 7000 + i, gate=1) + setp(t + 2.05, 7000 + i, gate=0) + free(t + 3.4, 7000 + i); t += 3.5
x, sr = render(s + end(t), 'envelopes.wav')
import sweep
for i, c in enumerate(cases):
    b = 0.01 + 3.5 * i
    lv = [f'{rms(x[int((b + q) * sr):int((b + q + 0.1) * sr)]):.2f}' for q in (0.1, 0.3, 0.6, 1.0, 1.8, 2.2, 2.6, 3.0)]
    p = sweep.pitch_of(x[int((b + 1.5) * sr):int((b + 1.5) * sr) + 8192], sr)
    print(f'   {c}: rms at .1 .3 .6 1 1.8 | 2.2 2.6 3.0 s: {" ".join(lv)}  pitch {p[0]:.2f} clarity {p[2]:.2f}')

# env modes: Exciter (0), PostGain (1), Both (2), held C5, sustain 0.6, attack 0.3
s = load(); t = 0.01
for m in range(3):
    s += new(t, 'FlutePro1', 8000 + m, pitch=72, gate=0, levels=1, envmode=m, attack=0.3, sustain=0.6, release=0.6)
    s += setp(t + 0.05, 8000 + m, gate=1) + setp(t + 2.05, 8000 + m, gate=0) + free(t + 3.4, 8000 + m); t += 3.5
x, sr = render(s + end(t), 'envmodes.wav')
for m, name in enumerate(['Exciter', 'PostGain', 'Both']):
    b = 0.01 + 3.5 * m
    lv = [f'{rms(x[int((b + q) * sr):int((b + q + 0.1) * sr)]):.2f}' for q in (0.1, 0.25, 0.5, 1.0, 1.8, 2.2, 2.5, 2.9)]
    p = sweep.pitch_of(x[int((b + 1.5) * sr):int((b + 1.5) * sr) + 8192], sr)
    print(f'   {name}: rms at .1 .25 .5 1 1.8 | 2.2 2.5 2.9 s: {" ".join(lv)}  pitch {p[0]:.2f} clarity {p[2]:.2f}')
