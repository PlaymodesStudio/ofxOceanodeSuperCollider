#!/usr/bin/env python3
"""Render the real PolyComb VCA: tremolo depth, rate, random rates and bypass."""
import pathlib
import subprocess
import sys

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/aliasingbox'))
from render_test import message, bundle, read_wav

out = pathlib.Path('/tmp/polycomb-tremolo-render')
out.mkdir(exist_ok=True)
config = out / 'config.yaml'
config.write_text(f'includePaths:\n    - {ROOT}/sclang/SCClassLibrary\nexcludePaths: []\npostInlineWarnings: false\nexcludeDefaultPaths: true\n')
probe = out / 'probe.scd'
probe.write_text(f'Archive.archiveDir="{out}";\nSynthDef(\\PCTremProbe,{{Out.ar(32,SinOsc.ar(1000,0,0.2)!3);Out.ar(40,DC.ar(1)!3);Out.ar(48,DC.ar(1)!3)}}).writeDefFile("{out}");\n0.exit;\n')
r = subprocess.run([str(ROOT / 'sclang/osx/sclang'), '-l', str(config), str(probe)], capture_output=True, text=True, timeout=30)
assert r.returncode == 0 and (out / 'PCTremProbe.scsyndef').exists(), r.stdout + r.stderr
folder = ROOT / 'synthdefs/Defaults/PolyComb/CompiledSynthdefs/PolyComb'
score = bundle(0, [message('/d_recv', p.read_bytes()) for p in (out / 'PCTremProbe.scsyndef', folder / 'PCAmp3.scsyndef')] + [message('/g_new', 1, 0, 0), message('/c_setn', 100, 3, 1., 1., 1.)])
cases = [('default', None), ('zero', (8., 1., 0.)), ('partial', (5., 0., .4)), ('full', (5., 0., 1.)), ('faster', (8., 0., 1.)), ('random', (5., .6, 1.))]
for i, (name, controls) in enumerate(cases):
    args = [] if controls is None else ['tremrate', controls[0], 'tremrand', controls[1], 'tremdepth', controls[2]]
    score += bundle(i * 3 + .01, [message('/s_new', 'PCTremProbe', 100, 1, 1), message('/s_new', 'PCAmp3', 101, 1, 1, 'in', 32, 'out', 0, 'env', 40, 'gate', 48, 'vel', 100, 'mamp', 110, 'level', 1., *args)])
    score += bundle((i + 1) * 3 - .01, [message('/g_freeAll', 1)])
score += bundle(len(cases) * 3, [message('/c_set', 0, 0.)])
osc = out / 'test.osc'
osc.write_bytes(score)
wav = out / 'test.wav'
r = subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth', '-N', str(osc), '_', str(wav), '48000', 'WAV', 'float', '-o', '3', '-i', '0', '-a', '128', '-c', '512', '-D', '0'], capture_output=True, text=True, timeout=30)
assert r.returncode == 0 and 'FAILURE' not in r.stdout + r.stderr, r.stdout + r.stderr
samples, channels, sr = read_wav(wav)
audio = np.asarray(samples).reshape(-1, channels)
assert channels == 3 and np.isfinite(audio).all()
parts = {name: audio[int((i * 3 + .5) * sr):int((i * 3 + 2.5) * sr)] for i, (name, _) in enumerate(cases)}
assert np.array_equal(parts['default'], parts['zero']), 'Zero depth must preserve dry samples even with random rate enabled'
for name, rate, ratio in [('partial', 5, .125), ('full', 5, .5), ('faster', 8, .5)]:
    spectrum = np.abs(np.fft.rfft(parts[name], axis=0))
    carrier = spectrum[2000]
    for side in (-1, 1):
        measured = spectrum[2000 + side * rate * 2] / carrier
        assert np.all(np.abs(measured - ratio) < .005), (name, measured)
for voice in range(3):
    x = parts['random'][:, voice]
    spectrum = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    # Random=0.6 keeps the 5 Hz rate within 2..8 Hz on every voice.
    bins = np.arange(2003, 2018)
    offset = (bins[np.argmax(spectrum[bins])] - 2000) / 2
    assert 1.5 <= offset <= 8.5, offset
    assert spectrum[bins].max() > spectrum[2000] * .1, 'Random tremolo must modulate amplitude'
print('PASS: zero-depth sample-identical bypass, 40%/100% depth, 5/8 Hz rates, and per-voice random-rate modulation.')
