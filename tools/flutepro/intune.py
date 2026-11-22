#!/usr/bin/env python3
"""How often FluteNew plays in tune on a harmonic of the note, per overblow.
  intune.py <work> [reps] [key=value ...]
For each note: 3 windows (0.7-1.4 s), the harmonic heard and its error in cents."""
import sys, pathlib, math
import numpy as np
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sc, sweep
work = pathlib.Path(sys.argv[1]); reps = int(sys.argv[2]) if len(sys.argv) > 2 else 2
args = dict(a.split('=') for a in sys.argv[3:])
notes = [int(v) for v in args.pop('notes', '24,36,48,60,72,84,96').split(',')]
obs = [float(v) for v in args.pop('obs', '0,1,2,3').split(',')]
for ob in obs:
    line = []
    for n in notes:
        cells = []
        for r in range(reps):
            a = dict(args, overblow=ob)
            _, wav = sweep.render(work, 'FluteNew', [n], a, dur=1.5)
            x, _, sr = sc.read_wav(wav); x = np.array(x)
            hs = []
            for w0 in (0.7, 0.95, 1.2):
                seg = x[int(w0 * sr):int(w0 * sr) + 8192]
                if np.sqrt(np.mean(seg ** 2)) < 1e-3: hs.append('-'); continue
                p, pk, cl = sweep.pitch_of(seg, sr)
                m = pk if abs(pk - p) > 0.5 else p       # spectral peak wins on period octave errors
                ratio = 2 ** ((m - n) / 12); h = max(1, round(ratio))
                c = 1200 * math.log2(ratio / h)
                hs.append(f'{h}{c:+.0f}' if abs(c) >= 15 else f'{h}')
            cells.append('/'.join(hs))
        line.append(f'{n}:' + ' '.join(cells))
    print(f'ob {ob}: ' + '  '.join(line))
