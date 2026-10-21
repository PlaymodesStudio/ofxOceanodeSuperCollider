#!/usr/bin/env python3
"""Render the real RhythmBox Click track with production transport follower anchors."""
from pathlib import Path
import math
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/aliasingbox'))
from render_test import message, bundle, read_wav

with tempfile.TemporaryDirectory(prefix='rhythmbox-render-') as directory:
    out = Path(directory)
    classes = out / 'classes'; classes.mkdir()
    (classes / 'oceanodeParameter.sc').write_bytes((ROOT / 'synthdefs/Oceanode/oceanodeParameter.sc').read_bytes())
    config = out / 'config.yaml'
    config.write_text(f'includePaths:\n    - {ROOT}/sclang/SCClassLibrary\n    - {classes}\nexcludePaths: []\npostInlineWarnings: false\nexcludeDefaultPaths: true\n')
    # Compile a copy, so the test never updates checked-in SynthDefs.
    source = out / 'tracks.scd'
    source.write_bytes((ROOT / 'synthdefs/Defaults/RhythmBox/scRhythmBoxTracks.scd').read_bytes())
    compile_script = out / 'compile.scd'
    compile_script.write_text(f'Archive.archiveDir="{out}";\n"{source}".load;\n0.exit;\n')
    result = subprocess.run([str(ROOT / 'sclang/osx/sclang'), '-l', str(config), str(compile_script)], capture_output=True, text=True, timeout=40)
    assert result.returncode == 0 and 'RhythmBox typed dry sources written' in result.stdout, result.stdout + result.stderr
    (out / 'ofxSCServer.h').write_text('''#pragma once
#include <cstdint>
struct ofxSCServer {
 static uint64_t timetagForSteadyTimeUs(uint64_t t){return t;}
 struct ScopedTimetag{ScopedTimetag(uint64_t){}};
 struct ScopedNRTOffset{ScopedNRTOffset(double){}};
};
''')
    # The same follower scenarios as the transport regression suite, in beat
    # units (RhythmBox's Steps/Beat=1), rather than the Euclidean player's PPQ.
    generator = (ROOT / 'tools/transport_sync_test/render_events.cpp').read_text()
    generator = generator.replace('clock.setPatternLength(1)', 'clock.setPatternLength(16)').replace('a.beat*48', 'a.beat')
    (out / 'events.cpp').write_text(generator)
    subprocess.run(['c++', '-std=c++17', '-O2', '-I', str(out), '-I', str(ROOT / 'src'), '-I', str(ROOT.parent / 'ofxOceanode/src/Managers'), str(out / 'events.cpp'), '-o', str(out / 'events')], check=True)
    synthdef = out / 'CompiledSynthdefs/RhythmBox/RhythmBoxClickTrack.scsyndef'
    for offline in [False, True]:
        for scenario in ['loop', 'tempo', 'stop', 'loop_tempo', 'loop_off']:
            lines = subprocess.check_output([str(out / 'events'), scenario] + (['offline'] if offline else []), text=True).splitlines()
            events = [(0., message('/d_recv', synthdef.read_bytes())), (.01, message('/g_new', 1, 0, 0)),
                      (.02, message('/s_new', 'RhythmBoxClickTrack', 1000, 1, 1,
                                    'sync', 1., 'run', 0., 'play', 0., 'out', 0, 'mixOut', 128,
                                    'stepBus', 0, 'gateBus', 1, 'stepsPerBeat', 1., 'numSteps', 16.)),
                      (.03, message('/n_setn', 1000, 'stepOn', 64, *([1.] * 64)))]
            for line in lines:
                t, bpm, run, pos, fire, hard, anchor, _ = map(float, line.split())
                events.append((t + .1, message('/n_set', 1000, 'sync', 1., 'bpm', bpm, 'run', run, 'anchorPos', pos, 'anchorFire', fire, 'anchorHard', hard, 'anchorId', anchor)))
            # Node-local Play/Reset must not restart a synced track.
            events += [(1.27, message('/n_set', 1000, 'play', 1., 'reset', 1.)), (1.28, message('/n_set', 1000, 'play', 0.))]
            events += [(12.2, message('/g_freeAll', 1)), (12.3, message('/c_set', 0, 0.))]
            osc = out / 'score.osc'
            osc.write_bytes(b''.join(bundle(t, [command]) for t, command in sorted(events, key=lambda e: e[0])))
            wav = out / 'render.wav'
            result = subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth', '-N', str(osc), '_', str(wav), '48000', 'WAV', 'float', '-U', '/Applications/SuperCollider.app/Contents/Resources/plugins', '-o', '2', '-i', '0', '-a', '1024', '-c', '512', '-m', '65536', '-w', '4096', '-D', '0'], capture_output=True, text=True, timeout=30)
            assert result.returncode == 0 and 'FAILURE' not in result.stdout + result.stderr, result.stdout + result.stderr
            samples, channels, rate = read_wav(wav)
            onsets = [i / rate - .1 for i in range(len(samples) // channels) if samples[i * channels] > .1 and i / rate - .1 < 719/60]
            expected = [0.]; beat = 0.
            for i in range(1, 720):
                previous_t = (i - 1) / 60
                bpm = 135. if scenario == 'loop' else 120.
                if scenario in ['tempo', 'loop_tempo'] and previous_t >= 1.97: bpm = 90.
                playing = not (scenario == 'stop' and 1.97 <= previous_t < 2.97)
                next_beat = beat + (bpm / 3600 if playing else 0)
                if math.floor(next_beat + 1e-8) > math.floor(beat + 1e-8):
                    crossed = round(next_beat) if abs(next_beat - round(next_beat)) < 1e-8 else math.floor(next_beat)
                    expected.append(previous_t + (crossed - beat) * 60 / bpm)
                beat = next_beat
            assert len(onsets) == len(expected), (scenario, offline, len(onsets), len(expected), onsets)
            error = max(abs(a-b) for a,b in zip(onsets, expected))
            assert error < .004, (scenario, error)
            print(f'PASS: {"NRT" if offline else "realtime"} {scenario}: {len(expected)} clicks, {error*1000:.3f} ms max error, no duplicate/missing steps', flush=True)
