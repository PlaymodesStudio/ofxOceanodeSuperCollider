#!/usr/bin/env python3
"""High feedback: fb1/fb2 to 1 for 2 s then back to 0.4. Does the voice come back?
  robust.py <work> <def> [key=value ...]   prints rms per 0.5 s"""
import sys, pathlib, math
import numpy as np
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sc
work = pathlib.Path(sys.argv[1]); name = sys.argv[2]; args = dict(a.split('=') for a in sys.argv[3:])
pitch = float(args.pop('pitch', 60))
a = ['pitch', pitch] + [v for kv in args.items() for v in (kv[0], float(kv[1]))]
score = sc.bundle(0, [sc.message('/d_load', str(work / 'defs' / (name + '.scsyndef')))])
score += sc.bundle(0.01, [sc.message('/s_new', name, 1000, 0, 0, *a)])
score += sc.bundle(1.0, [sc.message('/n_set', 1000, 'fb1', 1.0, 'fb2', 1.0)])
score += sc.bundle(3.0, [sc.message('/n_set', 1000, 'fb1', 0.4, 'fb2', 0.4)])
score += sc.bundle(5.0, [sc.message('/c_set', 0, 0.0)])
x, _, sr = sc.render(score, work / f'robust_{name}.wav', 1); x = np.array(x)
print(name, ' '.join(f'{math.sqrt(np.mean(x[int(t*sr):int((t+.5)*sr)]**2)) if np.all(np.isfinite(x[int(t*sr):int((t+.5)*sr)])) else float("nan"):.3g}' for t in np.arange(0, 5, 0.5)))
