#!/usr/bin/env python3
"""Render WavetableBox variants and real shared gate/LFO/envelope routes offline.

Compile Defaults/WavetableBox/wavetableBox.scd and Defaults/Modulation/modulation.scd first.
"""
import math
import pathlib
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/aliasingbox'))
from render_test import message, bundle, read_wav

out = pathlib.Path('/tmp/wavetable-box-render')
out.mkdir(exist_ok=True)
defs = ROOT / 'synthdefs/Defaults/WavetableBox/CompiledSynthdefs/WavetableBox'
mod = ROOT / 'synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation'
samples = [math.sin(2 * math.pi * i / 2048) for i in range(2048)]
frames = samples + [x * .25 for x in samples]
data = struct.pack('<' + 'f' * len(frames), *frames)
table = out / 'sine.wav'
table.write_bytes(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt '
                 + struct.pack('<IHHIIHH', 16, 3, 1, 48000, 192000, 4, 32)
                 + b'data' + struct.pack('<I', len(data)) + data)
events = []
def event(t, address, *args):
    events.append((t, message(address, *args)))
for path in sorted(defs.glob('*.scsyndef')):
    event(0, '/d_recv', path.read_bytes())
for name in ('ModGate2', 'ModEnv2', 'ModLfo2', 'ModRoute2'):
    event(0, '/d_recv', (mod / (name + '.scsyndef')).read_bytes())
event(0, '/g_new', 1, 0, 0)
event(0, '/b_allocRead', 0, str(table))
event(0, '/b_allocRead', 1, str(table))
event(0, '/c_setn', 600, 2, 1., 1.)
# Gate -> sources -> routes -> oscillator, matching the host's group order.
event(.05, '/s_new', 'ModGate2', 10, 1, 1, 'out', 32, 'vel', 600, 'rnd', 620)
event(.05, '/s_new', 'ModLfo2', 11, 1, 1, 'out', 400)
event(.05, '/n_setn', 11, 'freq', 2, 2., 2., 'phoff', 2, 0., .5)
event(.05, '/s_new', 'ModEnv2', 12, 1, 1, 'gate', 32, 'out', 500, 'sus', 1., 'rel', .2)
event(.05, '/n_setn', 12, 'atk', 2, .01, .01, 'dec', 2, .01, .01,
      'sus', 2, 1., 1., 'rel', 2, .2, .2)
event(.05, '/s_new', 'ModRoute2', 13, 1, 1, 'src', 500, 'out', 260, 'amount', 1., 'scale', 1.)
event(.05, '/s_new', 'ModRoute2', 14, 1, 1, 'src', 400, 'out', 116, 'amount', 1., 'scale', 1.)
controls = ['cycles', 2, 'buf', 0, 'bbuf', 1, 'buf2', 0, 'bbuf2', 1, 'gateBus', 32, 'velBus', 600]
mods = sum(([f'mod{i}', 100 + i * 16] for i in range(11)), [])
event(.05, '/s_new', 'WavetableOsc2', 20, 1, 1, *controls, *mods, 'vca', 1., 'drone', 0.)
event(.05, '/n_setn', 20, 'levels', 2, .4, .4)
event(.1, '/n_setn', 10, 'gate', 2, 1., 0.)
event(.3, '/n_setn', 10, 'gate', 2, 1., 1.)
event(.4, '/c_setn', 100, 2, 12., 0.)
event(1., '/n_setn', 10, 'gate', 2, 0., 0.)
event(1.7, '/g_freeAll', 1)
# Every 1D / 2D / live definition, at every supported channel count, must play.
variants = ['WavetableOsc', 'WavetableOsc2D', 'WavetableOscLive', 'WavetableOscLive2D']
smoke = []
for k, (variant, n) in enumerate((v, n) for v in variants for n in range(1, 17)):
    t = 1.8 + k * .12
    event(t, '/c_setn', 100, 176, *([0.] * 176))
    event(t, '/c_setn', 600, 16, *([1.] * 16))
    event(t, '/s_new', variant + str(n), 20, 1, 1, *controls, *mods, 'drone', 1., 'vca', 0.)
    event(t, '/n_setn', 20, 'levels', n, *([.2] * n))
    event(t + .11, '/n_free', 20)
    smoke.append((t, n, variant))
end = 1.8 + len(smoke) * .12
event(end, '/c_set', 0, 0.)
score = out / 'test.osc'
score.write_bytes(b''.join(bundle(t, [m]) for t, m in sorted(events, key=lambda e: e[0])))
plugins = '/Applications/SuperCollider.app/Contents/Resources/plugins'
grain = pathlib.Path.home() / 'Library/Application Support/SuperCollider/Extensions/GrainUtils/macOS'
result = subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth', '-N', str(score),
    '_', str(out / 'test.wav'), '48000', 'WAV', 'float', '-o', '16', '-i', '0', '-a', '256',
    '-c', '1024', '-w', '4096', '-D', '0', '-m', '65536', '-U', plugins + ':' + str(grain)],
    capture_output=True, text=True, timeout=60)
log = result.stdout + result.stderr
(out / 'render.log').write_text(log)
assert result.returncode == 0 and 'FAILURE' not in log and 'exception' not in log.lower(), log
raw, channels, rate = read_wav(out / 'test.wav')
assert all(math.isfinite(x) for x in raw)
def segment(start, stop, channel):
    return raw[int(start * rate) * channels + channel:int(stop * rate) * channels:channels]
def rms(start, stop, channel):
    values = segment(start, stop, channel)
    return math.sqrt(sum(x * x for x in values) / len(values))
assert rms(.15, .25, 0) > .05
assert rms(.15, .25, 1) < 1e-6, 'Unplayed voice must stay silent'
assert rms(.5, .8, 1) > .05
assert rms(1.5, 1.65, 0) < 1e-5 and rms(1.5, 1.65, 1) < 1e-5, 'Release must finish'
# Count upward zero crossings after matrix pitch +12 on voice 1 only.
for channel, expected in ((0, 523.25), (1, 261.63)):
    values = segment(.55, .95, channel)
    hz = sum(a <= 0 < b for a, b in zip(values, values[1:])) / .4
    assert abs(hz - expected) < 5, (channel, hz, expected)
envelopes = [rms(t, t + .025, 0) for t in (.45, .55, .65, .75, .85)]
assert max(envelopes) > min(envelopes) * 1.3, 'LFO -> Position must change the heard frame'
for t, n, variant in smoke:
    assert rms(t + .04, t + .09, n - 1) > .02, (variant, n)
print('PASS: all 64 WavetableBox variants; independent note gates, envelope release and per-voice matrix pitch routing')
