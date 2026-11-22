#!/usr/bin/env python3
"""FluteVoice mode 1 against flute.scd's own loop, from the same breath, notes 24..72."""
import sys, pathlib, math
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sc
work = pathlib.Path(sys.argv[1]); work.mkdir(parents=True, exist_ok=True)
defs = work / 'defs'; defs.mkdir(exist_ok=True)
sc._run(work, f'd = "{defs}";\n"{pathlib.Path(__file__).resolve().parent / "test_defs.scd"}".load;')
for pitch in [24, 32, 44, 56, 64, 72]:
    score = sc.bundle(0, [sc.message('/d_load', str(defs / 'FluteAB.scsyndef'))])
    score += sc.bundle(0.01, [sc.message('/s_new', 'FluteAB', 1000, 0, 0, 'pitch', float(pitch))])
    score += sc.bundle(3.0, [sc.message('/c_set', 0, 0.0)])
    x, ch, sr = sc.render(score, work / f'ab{pitch}.wav', 2)
    a, b = x[0::2][sr:], x[1::2][sr:]
    ra = math.sqrt(sum(v * v for v in a) / len(a)); d = math.sqrt(sum((p - q) ** 2 for p, q in zip(a, b)) / len(a))
    print(f'pitch {pitch}: rms {ra:.4f}  diff {20 * math.log10(d / ra + 1e-12):.1f} dB')
