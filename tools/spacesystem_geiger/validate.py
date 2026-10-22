#!/usr/bin/env python3
"""Check the rendered quad preset and RandomMetro's sample-accurate clock.

    python3 validate.py <prepared build directory>
"""
import json
import math
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/aliasingbox'))
from render_test import bundle, message, read_wav

work = pathlib.Path(sys.argv[1]).resolve()
audio, ch, sr = read_wav(work / 'preview.wav')
assert ch == 4 and len(audio) >= sr * 4 * 10
metrics = []
for c in range(ch):
    values = audio[c::ch]
    assert all(math.isfinite(x) for x in values), ('nonfinite audio', c)
    peak = max(map(abs, values))
    rms = math.sqrt(sum(x*x for x in values) / len(values))
    assert 1e-6 < rms and peak < 1, (c, peak, rms)
    metrics.append(dict(channel=c + 1, peak=peak, rms=rms))

# Fixed 20 ms clock: 960 samples at 48 kHz, without audio devices or
# parameter lag. This exercises the demand graph that used to crash scsynth.
sr = 48000
osc = work / 'clock.osc'
osc.write_bytes(bundle(0, [message('/d_recv', (work / 'defs/RandomMetro/RandomMetro1.scsyndef').read_bytes()),
                          message('/g_new', 1, 0, 0)]) +
    bundle(0.01, [message('/s_new', 'RandomMetro1', 1000, 1, 1, 'out', 0,
                           'range', 0.0, 'minms', 20.0, 'curve', 1.0, 'chance', 1.0, 'seed', 123)]) +
    bundle(1.0, [message('/g_freeAll', 1)]) + bundle(1.01, [message('/c_set', 0, 0)]))
plugins = '/Applications/SuperCollider.app/Contents/Resources/plugins:' + str(ROOT / 'ugens/OceanodeAnalog')
r = subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth', '-N', str(osc), '_',
                    str(work / 'clock.wav'), str(sr), 'WAV', 'float', '-o', '1', '-i', '0',
                    '-D', '0', '-U', plugins], capture_output=True, text=True, timeout=60)
assert r.returncode == 0 and 'FAILURE' not in r.stdout + r.stderr, r.stdout + r.stderr
samples, _, _ = read_wav(work / 'clock.wav')
events = [i for i, x in enumerate(samples) if x > 0.5]
assert len(events) >= 45, len(events)
# TDuty's initial zero-time impulse precedes its first interval by one
# sample. Subsequent intervals must stay exactly at 960, without drift.
assert events[1] - events[0] in (960, 961), events[:2]
assert all(b - a == 960 for a, b in zip(events[1:], events[2:])), events
result = dict(quad=metrics, clockEvents=len(events), clockIntervalSamples=960)
(work / 'validation.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
