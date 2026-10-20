#!/usr/bin/env python3
"""Render the real EuclideanBox player with production follower anchors."""
import argparse
import pathlib
import subprocess
import sys
import tempfile

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/aliasingbox'))
from render_test import message, bundle, read_wav

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--report-legacy-loop-duplicates', action='store_true',
                    help='Report SynthDef loop-boundary double pulses while checking beat coverage; default is strict.')
args = parser.parse_args()

out = pathlib.Path('/tmp/transport-sync-render')
out.mkdir(exist_ok=True)
# Compile the original production player without additional transport controls.
classes = out / 'classes'
classes.mkdir(exist_ok=True)
(classes / 'oceanodeParameter.sc').write_bytes((ROOT / 'synthdefs/Oceanode/oceanodeParameter.sc').read_bytes())
config = out / 'config.yaml'
config.write_text(f'includePaths:\n    - {ROOT}/sclang/SCClassLibrary\n    - {classes}\nexcludePaths: []\npostInlineWarnings: false\nexcludeDefaultPaths: true\n')
compile_source = out / 'compile-player.scd'
compile_source.write_text(f'Archive.archiveDir="{out}";\n"{ROOT}/synthdefs/Defaults/EuclideanBox/euclideanBox.scd".load;\n0.exit;\n')
compiled = subprocess.run([str(ROOT / 'sclang/osx/sclang'), '-l', str(config), str(compile_source)], capture_output=True, text=True, timeout=30)
assert compiled.returncode == 0 and 'EuclideanBoxPlayer compiled' in compiled.stdout, compiled.stdout + compiled.stderr
with tempfile.TemporaryDirectory(prefix='transport-render-build-') as directory:
    build = pathlib.Path(directory)
    (build / 'ofxSCServer.h').write_text('''#pragma once
#include <cstdint>
struct ofxSCServer {
 static uint64_t timetagForSteadyTimeUs(uint64_t t){return t;}
 struct ScopedTimetag{ScopedTimetag(uint64_t){}};
 struct ScopedNRTOffset{ScopedNRTOffset(double){}};
};
''')
    subprocess.run(['c++', '-std=c++17', '-O2', '-I', str(build), '-I', str(ROOT / 'src'), '-I', str(ROOT.parent / 'ofxOceanode/src/Managers'), str(pathlib.Path(__file__).with_name('render_events.cpp')), '-o', str(build / 'events')], check=True)
    synthdef = ROOT / 'synthdefs/Defaults/EuclideanBox/CompiledSynthdefs/EuclideanBox/EuclideanBoxPlayer1.scsyndef'
    for label in ('loop', 'tempo', 'stop', 'loop_tempo', 'loop_off', 'nrt_loop', 'nrt_tempo', 'nrt_stop', 'nrt_loop_tempo', 'nrt_loop_off'):
        offline = label.startswith('nrt_')
        scenario = label.removeprefix('nrt_')
        lines = subprocess.check_output([str(build / 'events'), scenario] + (['offline'] if offline else []), text=True).splitlines()
        events = [(0., message('/d_recv', synthdef.read_bytes())), (0., message('/b_alloc', 0, 8 * 4 * 768, 4)), (0., message('/g_new', 1, 0, 0))]
        data = [0.] * (1536 * 4)
        for tick in range(0, 1536, 48):
            data[tick * 4:tick * 4 + 4] = [1., 12., 60., 0.]
        for offset in range(0, len(data), 2048):
            chunk = data[offset:offset + 2048]
            events.append((.01, message('/b_setn', 0, offset, len(chunk), *chunk)))
        outputs = []
        for i in range(1, 9):
            for kind in ('trig', 'gate', 'pitch'):
                outputs += [f'{kind}out{i}', 0 if i == 1 and kind == 'trig' else 128]
        events.append((.02, message('/s_new', 'EuclideanBoxPlayer1', 1000, 1, 1, 'buf', 0, 'layers', 1., 'gatesout', 128, *outputs)))
        # Preserve dispatch order for equal deadlines: queued obsolete anchor
        # first, then its complete replacement at the EXACT same OSC timetag.
        for line in lines:
            t, bpm, run, pos, fire, hard, anchor_id, base = map(float, line.split())
            events.append((.1 + t, message('/n_setn', 1000, 'bpm', 1, bpm, 'run', 1, run, 'anchorPos', 1, pos, 'anchorFire', 1, fire, 'cycleTicks', 8, *([384.] * 8), 'base', 8, *([base] * 8), 'anchorHard', 1, hard, 'anchorId', 1, anchor_id)))
        events += [(12.2, message('/g_freeAll', 1)), (12.3, message('/c_set', 0, 0.))]
        score = b''.join(bundle(t, [command]) for t, command in sorted(events, key=lambda e: e[0]))
        osc = out / f'{label}.osc'
        osc.write_bytes(score)
        wav = out / f'{label}.wav'
        result = subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth', '-N', str(osc), '_', str(wav), '48000', 'WAV', 'float', '-U', '/Applications/SuperCollider.app/Contents/Resources/plugins', '-o', '1', '-i', '0', '-a', '1024', '-c', '512', '-m', '65536', '-D', '0'], capture_output=True, text=True, timeout=30)
        assert result.returncode == 0 and 'FAILURE' not in result.stdout + result.stderr, result.stdout + result.stderr
        samples, channels, sr = read_wav(wav)
        audio = np.asarray(samples)
        onsets = np.flatnonzero(audio > .5) / sr - .1
        onsets = onsets[onsets < 719/60]
        # Compare the actual player pulses with the beat crossings, including
        # wraps and changes while a periodic/wrap anchor was already queued.
        expected = [0.]
        beat = 0.
        for i in range(1, 60 * 12):
            previous_t = (i - 1) / 60
            t = i / 60
            bpm = 135. if scenario == 'loop' else 120.
            if scenario in ('tempo', 'loop_tempo') and previous_t >= 1.97:
                bpm = 90.
            playing = not (scenario == 'stop' and 1.97 <= previous_t < 2.97)
            next_beat = beat + (bpm / 3600 if playing else 0)
            if int(np.floor(next_beat + 1e-8)) > int(np.floor(beat + 1e-8)):
                crossed = round(next_beat) if abs(next_beat - round(next_beat)) < 1e-8 else np.floor(next_beat)
                expected.append(previous_t + (crossed - beat) * 60 / bpm)
            beat = next_beat
        expected = np.asarray(expected)
        extra = np.flatnonzero(np.diff(onsets) < .004) + 1
        if args.report_legacy_loop_duplicates and len(extra):
            # Keep the strict default. Diagnostic mode accepts only the known
            # double pulse at a loop wrap, never a missing musical beat.
            assert scenario in ('loop', 'loop_tempo'), (scenario, onsets[extra])
            loop_beats = 16 if scenario == 'loop' else 4
            wrap_times = expected[np.arange(len(expected)) % loop_beats == 0]
            assert all(np.min(np.abs(wrap_times - t)) < .004 for t in onsets[extra])
            print(f'KNOWN ISSUE: {label}: {len(extra)} extra loop-boundary pulses in the unchanged SynthDef.')
            onsets = np.delete(onsets, extra)
        assert len(onsets) == len(expected), (scenario, len(onsets), len(expected), onsets)
        error = np.max(np.abs(onsets - expected))
        assert error < .004, (scenario, error, list(zip(onsets, expected)))
        status = 'COVERAGE ONLY' if len(extra) else 'PASS'
        print(f'{status}: {label}: {len(expected)} musical beats covered, max timing error {error * 1000:.3f} ms.' + (' Extra loop pulses remain.' if len(extra) else ' No duplicate/missing beats.'))
