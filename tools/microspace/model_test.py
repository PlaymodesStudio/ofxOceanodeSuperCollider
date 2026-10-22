#!/usr/bin/env python3
"""MicroSpace SynthDef against a sample-by-sample model of the Reaktor graph.

The model follows the decoded "Space Master Deluxe" Core cell literally
(diffuser = z^-1 Shelf(Delay(x + g y)) - g x, integer early delays, Hermite late
delays, cross-coupled tank), so it checks the LocalIn/LocalOut restructuring of
synthdefs/Effect/Time/Reverb/microSpace.scd. Mod = 0 cases must match to
float32 precision (better than -50 dB); a modulated case is only checked for stability and level.
Compiles with the bundled sclang and renders with scsynth -N (no audio device).
"""
import argparse, math, pathlib, subprocess, sys
import numpy as np
ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/aliasingbox'))
from render_test import message, bundle, read_wav

SR = 48000; BLOCK = 64; PREROLL = 1.5; TAIL = 2.0

def p2t(p): return 1.0 / (8.17741966 * 2 ** (p / 12))

class Shelf:
    """Core Shelf: BiLin 1P HS then LS (gain dB, corner scaled by 10^(dB/40))."""
    def __init__(self, hf, hb, lf, lb):
        def warp(f):
            x = min(max(f, SR / 24576), SR / 2.125) * math.pi / SR
            return x + x ** 3 * 0.333333343 + x ** 5 * 0.13333334
        e = 10 ** (hb / 40); a = e * e; w = warp(hf * e)
        a1 = (1 - w) / (1 + w); k = (a - 1) / (1 + w)
        self.h = (1 + k, -(a1 + k), a1)
        e = 10 ** (lb / 40); a = e * e; w = warp(lf / e)
        a1 = (1 - w) / (1 + w); k = w / (1 + w) * (a - 1)
        self.l = (1 + k, k - a1, a1)
        self.s = [0.0] * 4
    def __call__(self, x):
        s = self.s; b0, b1, a1 = self.h
        y = b0 * x + b1 * s[0] + a1 * s[1]; s[0], s[1] = x, y
        b0, b1, a1 = self.l
        z = b0 * y + b1 * s[2] + a1 * s[3]; s[2], s[3] = y, z
        return z

class Delay:
    def __init__(self, n): self.b = np.zeros(n); self.n = n; self.p = 0
    def write(self, x): self.p = (self.p + 1) % self.n; self.b[self.p] = x
    def tap(self, i): return self.b[(self.p - i) % self.n]
    def integer(self, d): return self.tap(min(max(round(d), 0), self.n - 1))
    def hermite(self, d):
        d = min(max(d, 0), self.n - 2); i = math.floor(d); f = d - i
        ym, y0, y1, y2 = self.tap(max(i - 1, 0)), self.tap(i), self.tap(i + 1), self.tap(i + 2)
        c1 = 0.5 * (y1 - ym); c2 = ym - 2.5 * y0 + 2 * y1 - 0.5 * y2
        c3 = 0.5 * (y2 - ym) + 1.5 * (y0 - y1)
        return y0 + f * (c1 + f * (c2 + f * c3))

class DiffDel:
    def __init__(self, n, hermite, damp):
        self.d = Delay(n); self.hermite = hermite; self.damp = damp; self.z = 0.0
    def __call__(self, x, t, g=0.67):
        y = self.z - g * x
        self.d.write(x + g * y)
        r = self.d.hermite(t * SR) if self.hermite else self.d.integer(t * SR)
        self.z = self.damp(r)
        return y

def smoother():
    c = math.log(2) / (0.05 * SR); state = [0.0]
    def step(x): state[0] += (x - state[0]) * c; return state[0]
    return step

def model(dry, size, mod, lo, hi, pos, mix):
    """Stereo Micro Space, constant controls. dry: (N, 2)."""
    assert mod == 0, 'the model leaves the 8Phase modulation out'
    damp = lambda: Shelf(4186.0094, min(0, hi) * 0.5, 65.406392, min(0, lo) * 0.72)
    eq = [Shelf(4186.0094, hi, 65.406392, lo) for _ in range(2)]
    rt = 10 ** (size / 20) * 0.001
    sides = []
    for sign in (-1, 1):
        ep = 60 - 0.42 * size + sign * -2; lp = 48 - 0.72 * size + sign * 1.5
        sides.append(dict(
            pre=Delay(4096), pre_s=smoother(),
            early=[DiffDel(4096, False, damp()) for _ in range(3)], early_s=[smoother() for _ in range(3)],
            early_t=[p2t(ep + o) for o in (8, 4, 0)], early_shelf=damp(),
            late=[DiffDel(32768, True, damp()) for _ in range(3)], late_s=[smoother() for _ in range(4)],
            late_t=[p2t(lp + o) for o in (9, 6, 3, 0)], late_shelf=damp(), tank=Delay(32768),
            gain=10 ** (max(-60 * p2t(lp) / rt, -144) / 20), fb=0.0))
    out = np.zeros_like(dry)
    xf = lambda x: x * (2 - x)
    for n in range(len(dry)):
        earlies = []
        for c, s in enumerate(sides):
            s['pre'].write(dry[n, c])
            x = s['pre'].integer(s['pre_s'](size * 0.00022 + 0.002) * SR)
            for st in range(3): x = s['early'][st](x, s['early_s'][st](s['early_t'][st]))
            earlies.append(s['early_shelf'](x))
        fbs = [s['fb'] for s in sides]   # z^-1 of last sample's tank outputs
        for c, s in enumerate(sides):
            x = earlies[c] + fbs[1 - c]
            ts = [s['late_s'][i](s['late_t'][i]) for i in range(4)]
            for st in range(3): x = s['late'][st](x, ts[st])
            late = s['late_shelf'](x)
            s['tank'].write(late)
            s['fb'] = s['tank'].hermite(ts[3] * SR) * s['gain']
            wet = eq[c](earlies[c] * xf(1 - pos) + late * xf(pos))
            out[n, c] = dry[n, c] * xf(1 - mix) + wet * xf(mix)
    return out

def compile_defs(work):
    conf = work / 'sclang_conf.yaml'
    conf.write_text('includePaths:\n' + ''.join(f'    - "{p}"\n' for p in (
        ROOT / 'sclang/SCClassLibrary', ROOT / 'sclang/Extensions', ROOT / 'synthdefs/Oceanode'))
        + 'excludePaths: []\npostInlineWarnings: false\nexcludeDefaultPaths: true\n')
    for n in (1, 2, 3):
        r = subprocess.run([str(ROOT / 'sclang/osx/sclang'), '-l', str(conf),
                            str(ROOT / 'synthdefs/Oceanode/compileExact.scd'), str(work),
                            str(ROOT / 'synthdefs/Effect/Time/Reverb/microSpace.scd'), str(n)],
                           capture_output=True, text=True, timeout=300)
        path = work / 'MicroSpace' / f'MicroSpace{n}.scsyndef'
        assert r.returncode == 0 and path.exists(), r.stdout[-3000:] + r.stderr[-3000:]
    return work / 'MicroSpace'

def render(server, folder, work, name, n, signal, controls):
    """signal: (frames, n) written to a buffer, played into bus 16 + ... ."""
    frames = len(signal)
    raw = work / f'{name}_in.wav'
    write_wav(raw, signal)
    msgs = [message('/d_recv', (folder / f'MicroSpace{n}.scsyndef').read_bytes()),
            message('/d_recv', player_def(work, n).read_bytes()),
            message('/b_allocRead', 0, str(raw)), message('/g_new', 1, 0, 0)]
    score = bundle(0, msgs)
    # Controls are vectors of n values: /s_new would only set the first one.
    setn = [message('/n_setn', 1001, k, n, *([float(v)] * n)) for k, v in controls.items()]
    score += bundle(0.01, [message('/s_new', f'MicroSpace{n}', 1001, 1, 1, 'in', 16, 'out', 0), *setn,
                           message('/s_new', f'MSPlayer{n}', 1000, 0, 1)])
    # Free before the end: scsynth -N segfaults on exit with a live delay UGen.
    score += bundle(frames / SR + 0.01, [message('/g_freeAll', 1)])
    score += bundle(frames / SR + 0.02, [message('/c_set', 0, 0.)])
    osc = work / f'{name}.osc'; osc.write_bytes(score); wav = work / f'{name}.wav'
    r = subprocess.run([server, '-N', str(osc), '_', str(wav), str(SR), 'WAV', 'float',
                        '-o', str(n), '-i', '0', '-z', str(BLOCK), '-m', '65536', '-w', '8192', '-D', '0'],
                       capture_output=True, text=True, timeout=600)
    assert r.returncode == 0 and 'FAILURE' not in r.stdout + r.stderr, r.stdout + r.stderr
    samples, ch, sr = read_wav(wav)
    audio = np.asarray(samples, dtype=np.float64).reshape(-1, ch)
    start = round(0.01 * SR)   # synths start at the 0.01 s bundle, on a block boundary
    start = start - start % BLOCK
    return audio[start:start + frames]

def player_def(work, n):
    path = work / f'MSPlayer{n}.scsyndef'
    if not path.exists():
        src = work / f'player{n}.scd'
        src.write_text(f'SynthDef(\\MSPlayer{n}, {{ Out.ar(16, PlayBuf.ar({n}, 0, doneAction: 2)) }}).writeDefFile("{work}");\n0.exit;\n')
        conf = work / 'sclang_conf.yaml'
        r = subprocess.run([str(ROOT / 'sclang/osx/sclang'), '-l', str(conf), str(src)], capture_output=True, text=True, timeout=60)
        assert path.exists(), r.stdout + r.stderr
    return path

def write_wav(path, x):
    import struct
    x = np.asarray(x, dtype='<f4'); ch = x.shape[1]
    data = x.tobytes()
    path.write_bytes(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ' +
                     struct.pack('<IHHIIHH', 16, 3, ch, SR, SR * 4 * ch, 4 * ch, 32) +
                     b'data' + struct.pack('<I', len(data)) + data)

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--server', default='/Applications/SuperCollider.app/Contents/Resources/scsynth')
    ap.add_argument('--output', type=pathlib.Path, default=pathlib.Path('/tmp/microspace-render'))
    a = ap.parse_args(); a.output.mkdir(parents=True, exist_ok=True)
    folder = compile_defs(a.output)
    frames = round((PREROLL + TAIL) * SR)
    rng = np.random.default_rng(1)
    impulse = np.zeros((frames, 2)); impulse[round(PREROLL * SR), 0] = 1; impulse[round(PREROLL * SR) + 300, 1] = -0.5
    burst = np.zeros((frames, 2)); k = round(PREROLL * SR)
    burst[k:k + 4800] = rng.uniform(-0.5, 0.5, (4800, 2))
    cases = [('default', impulse, dict(size=50, mod=0, lo=0, hi=0, position=0.5, mix=0.5)),
             ('large_damped', burst, dict(size=85, mod=0, lo=-6, hi=-9, position=0.8, mix=1)),
             ('small_bright', impulse, dict(size=10, mod=0, lo=4, hi=7, position=0.2, mix=0.7))]
    for name, sig, ctl in cases:
        got = render(a.server, folder, a.output, name, 2, sig, ctl)
        want = model(sig, ctl['size'], ctl['mod'], ctl['lo'], ctl['hi'], ctl['position'], ctl['mix'])
        tail = slice(round(PREROLL * SR), frames)
        err = np.max(np.abs(got[tail] - want[tail])); peak = np.max(np.abs(want[tail]))
        energy = lambda x: 10 * np.log10(np.sum(x[tail] ** 2) + 1e-30)
        print(f'{name}: peak {peak:.4f}, max error {err:.2e} ({20 * np.log10(err / peak + 1e-30):.1f} dB), '
              f'energy SC {energy(got):.2f} dB, model {energy(want):.2f} dB')
        # float32 delay times (resolution ~0.001 sample at 0.3 s) limit the match to ~-60 dB.
        assert np.isfinite(got).all() and err < peak * 3e-3, f'{name}: SynthDef differs from the Reaktor model'
    # Modulated: stable, decays, close in energy to the unmodulated tail.
    ctl = dict(size=70, mod=1, lo=0, hi=0, position=1, mix=1)
    got = render(a.server, folder, a.output, 'modulated', 2, burst, ctl)
    ref = render(a.server, folder, a.output, 'unmodulated', 2, burst, dict(ctl, mod=0))
    tail = got[round(PREROLL * SR):]
    e_mod, e_ref = (10 * np.log10(np.sum(x[round(PREROLL * SR):] ** 2)) for x in (got, ref))
    print(f'modulated: energy {e_mod:.2f} dB vs {e_ref:.2f} dB unmodulated, last 0.1 s peak {np.max(np.abs(tail[-4800:])):.2e}')
    assert np.isfinite(got).all() and abs(e_mod - e_ref) < 1.5
    # Mono and odd channel counts build and run.
    for n in (1, 3):
        sig = np.zeros((frames, n)); sig[round(PREROLL * SR)] = 1
        got = render(a.server, folder, a.output, f'chans{n}', n, sig, dict(size=50, mod=0.5, mix=0.5))
        assert np.isfinite(got).all() and np.max(np.abs(got[round(PREROLL * SR) + 2000:])) > 1e-3
    print('PASS: MicroSpace matches the Reaktor graph model; modulation stable; 1/3 channels run.')

if __name__ == '__main__':
    main()
