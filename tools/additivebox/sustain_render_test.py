#!/usr/bin/env python3
"""Render preset 2 through real C++ recall, note writers, routes and ModEnv.

Build tools/tests/box_gate_mode_test.py with OCEANODE_TEST_BOX=additiveBox and
OCEANODE_TEST_BUILD_DIR set, then pass its regression binary to this script.
"""
from pathlib import Path
import argparse
import json
import math
import os
import subprocess
import struct
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/aliasingbox'))
from render_test import read_wav, bundle, message

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('native_binary', type=Path)
parser.add_argument('--output', type=Path, default=Path('/private/tmp/additive-sustain-audio'))
parser.add_argument('--morph-bank', type=Path, help='Render the 25-second slot 2 -> 6 performance fixture instead')
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
source = (ROOT / 'synthdefs/Defaults/AdditiveBox/additiveBox.scd').read_text().replace('maxOut = 16', 'maxOut = 1')
(out / 'additiveBox.scd').write_text(source)
(out / 'modulation.scd').write_text((ROOT / 'synthdefs/Defaults/Modulation/modulation.scd').read_text().replace('(1..32).do', '[1,13].do'))
(out / 'config.yaml').write_text('includePaths:\n  - ' + str(ROOT / 'sclang/SCClassLibrary') + '\nexcludeDefaultPaths: true\n')
(out / 'compile.scd').write_text('Archive.archiveDir=' + json.dumps(str(out)) + ';"SUSTAIN START".postln;try{'
    + ''.join('this.executeFile(' + json.dumps(str(out / name)) + ');' for name in ('additiveBox.scd', 'modulation.scd'))
    + 'SynthDef(\\SustainProbe,{Out.ar(1,K2A.ar(In.kr(\\env.kr(0))));Out.ar(2,InFeedback.ar(\\gatebus.kr(0)))}).writeDefFile('
    + json.dumps(str(out/'CompiledSynthdefs')) + ');"SUSTAIN COMPILED".postln;0.exit;}{|error|error.reportError;1.exit;};')
r = subprocess.run([str(ROOT / 'sclang/osx/sclang'), '-D', '-l', str(out / 'config.yaml'), str(out / 'compile.scd')], capture_output=True, text=True, timeout=180)
log = (r.stdout + r.stderr).split('SUSTAIN START')[-1]
assert r.returncode == 0 and 'SUSTAIN COMPILED' in log and 'ERROR:' not in log, log[-4000:]
with tempfile.TemporaryDirectory(prefix='additive-sustain-capture-') as scratch:
    capture={'BOX_SUSTAIN_SCORE':str(out/'score.osc'),'BOX_SUSTAIN_DEFS':str(out/'CompiledSynthdefs')}
    if args.morph_bank:
        capture={'BOX_MORPH_BENCH_BANK':str(args.morph_bank.resolve()),'BOX_MORPH_BENCH_SCORE':str(out/'score.osc'),
                 'BOX_MORPH_BENCH_DEFS':str(out/'CompiledSynthdefs')}
    r = subprocess.run([str(args.native_binary.resolve()), 'unused', scratch], capture_output=True, text=True,
        env={**os.environ, **capture}, timeout=120)
    (out / 'native.log').write_text(r.stdout + r.stderr)
    assert r.returncode == 0, (r.stdout + r.stderr)[-4000:]
# Keep equal-time creation, pause, parameter and resume messages in one ordered
# bundle; scsynth's event heap does not preserve ties between separate packets.
raw = (out / 'score.osc').read_bytes()
packets = {}
def collect(packet):
    hi, lo = struct.unpack_from('>II', packet, 8)
    when = hi + lo / 2**32
    offset = 16
    while offset < len(packet):
        size = struct.unpack_from('>i', packet, offset)[0]
        item = packet[offset+4:offset+4+size]
        if item.startswith(b'#bundle'): collect(item)
        else: packets.setdefault(when, []).append(item)
        offset += 4 + size
offset = 0
while offset < len(raw):
    size = struct.unpack_from('>i', raw, offset)[0]
    collect(raw[offset+4:offset+4+size])
    offset += 4 + size
# The capture's native destructor releases buffers individually. For this audio
# test stop the whole graph together; no simulated buffer-free replies are sent.
stop=35 if args.morph_bank else 14
(out / 'score.osc').write_bytes(b''.join(bundle(t, packets[t]) for t in sorted(packets) if t < stop)
    + bundle(stop, [message('/g_freeAll', 0)]) + bundle(stop+.1, [message('/c_set', 0, 0.0)]))
r = subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth', '-N', str(out / 'score.osc'), '_', str(out / 'audio.wav'),
    '48000', 'WAV', 'float', '-o', '3', '-i', '0', '-a', '1024', '-c', '65536', '-b', '1024', '-m', '262144', '-w', '4096', '-D', '0',
    '-U', '/Applications/SuperCollider.app/Contents/Resources/plugins'], capture_output=True, text=True, timeout=60)
log = r.stdout + r.stderr
(out / 'render.log').write_text(log)
assert r.returncode == 0 and 'FAILURE' not in log and 'not found' not in log, log[-4000:]
audio, channels, sr = read_wav(out / 'audio.wav')
assert channels == 3 and all(math.isfinite(v) for v in audio)
def rms(start, end, channel=0):
    window = audio[round(start * sr)*channels+channel:round(end * sr)*channels:channels]
    return math.sqrt(sum(v*v for v in window) / len(window))
if args.morph_bank:
    levels={name:rms(start,end) for name,start,end in (
        ('start',.6,1.2),('middle',12,13),('end',26.2,26.9),('released',34.5,34.9))}
    print(json.dumps(levels,indent=2))
    assert all(levels[k]>1e-5 for k in ('start','middle','end')), 'Morph must keep both banks sounding through the endpoint'
    assert levels['released']<1e-5 and max(map(abs,audio))<1, 'Morph output must remain finite and release cleanly'
    # The native fixture holds .9999, then sends exactly 1 at 25.4 seconds.
    # Check both that frame and the deferred count/bank cleanup at 26 seconds.
    for when in (25.4, 26.0):
        def slope(start, end):
            return max(abs(audio[i*channels]-audio[(i-1)*channels])
                       for i in range(round(start*sr), round(end*sr)))
        nearby=max(slope(when-.1,when-.01),slope(when+.03,when+.1))
        assert slope(when-.002,when+.02)<max(1e-5,nearby*3), ('Endpoint impulse',when)
    print('PASS: actual slot 2 -> 6 bank morph retains running sources at 1, survives deferred cleanup, and releases cleanly')
    sys.exit(0)
levels = {name: rms(start, end) for name, start, end in (
    ('attack', .5, .8), ('held', 2, 2.8), ('released', 6.5, 6.9), ('drone', 9, 9.8), ('droneOff', 13.5, 13.9))}
print(json.dumps(levels, indent=2))
print('Held envelope/gate',rms(2,2.8,1),rms(2,2.8,2))
assert levels['held'] > 1e-4, 'Preset 2 must sustain while a key is held'
assert levels['drone'] > 1e-4, 'Preset 2 must keep playing with Drone enabled'
assert levels['released'] < 1e-5 and levels['droneOff'] < 1e-5, 'Release and Drone off must reach silence'
print('PASS: preset 2 sustains held notes and Drone, and releases to silence')
