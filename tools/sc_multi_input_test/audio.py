#!/usr/bin/env python3
"""Render the standalone routing SynthDef at 128 channels; no MacroMixer or audio device."""
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/aliasingbox'))
from render_test import message, bundle, read_wav

with tempfile.TemporaryDirectory(prefix='sc-audio-routing-') as folder:
    work = pathlib.Path(folder)
    conf = work / 'sclang.yaml'
    conf.write_text('includePaths:\n    - "' + str(ROOT / 'sclang/SCClassLibrary') + '"\nexcludeDefaultPaths: true\n')
    src = work / 'sources.scd'
    src.write_text('''
SynthDef(\\routingA, {Out.ar(256, DC.ar([0.1,0.2,-0.3,0.4] ++ (0 ! 124)))}).writeDefFile("%s");
SynthDef(\\routingB, {Out.ar(512, DC.ar([0.25,0.125,0.2,-0.05] ++ (0 ! 124)))}).writeDefFile("%s");
SynthDef(\\routingTap, {Out.ar(0, In.ar(768, 128))}).writeDefFile("%s");
0.exit;
''' % (work, work, work))
    r = subprocess.run([str(ROOT / 'sclang/osx/sclang'), '-l', str(conf), str(src)],
                       env={**os.environ, 'HOME': str(work)}, capture_output=True, text=True, timeout=60)
    assert r.returncode == 0 and (work / 'routingTap.scsyndef').exists(), r.stdout + r.stderr
    definition = ROOT / 'data/Supercollider/Synthdefs/Defaults/AudioRouting/audioBusMixer.scsyndef'
    loads = [message('/d_load', str(definition)), message('/g_new', 1, 0, 0)]
    loads += [message('/d_load', str(work / (n + '.scsyndef'))) for n in ('routingA','routingB','routingTap')]
    score = bundle(0, loads)
    score += bundle(.01, [message('/s_new','routingA',1000,1,1), message('/s_new','routingB',1001,1,1),
        message('/s_new','oceanodeAudioBusMixer128',1002,1,1,'inA',256,'inB',512,'out',768,'gainA',1.,'gainB',1.,'smoothing',0.),
        message('/s_new','routingTap',1003,1,1)])
    score += bundle(.1, [message('/n_set',1002,'gainA',.5,'gainB',.5)])
    score += bundle(.2, [message('/n_set',1002,'gainA',1.,'gainB',1.)])
    score += bundle(.25, [message('/n_free',1001)])
    score += bundle(.3, [message('/g_freeAll',1)]) + bundle(.31, [message('/c_set',0,0)])
    osc = work / 'mix.osc'; osc.write_bytes(score)
    wav = work / 'mix.wav'
    r = subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(osc),'_',str(wav),
        '48000','WAV','float','-o','128','-i','0','-a','2048','-w','8192','-D','0','-U',
        '/Applications/SuperCollider.app/Contents/Resources/plugins'], capture_output=True,text=True,timeout=60)
    assert r.returncode == 0 and 'FAILURE' not in r.stdout + r.stderr, r.stdout + r.stderr
    audio, channels, rate = read_wav(wav)
    assert channels == 128 and rate == 48000
    for t, expected in ((.05,[.35,.325,-.1,.35]),(.15,[.175,.1625,-.05,.175]),(.27,[.1,.2,-.3,.4])):
        expected += [0.] * 124
        for frame in range(round(t * rate), round(t * rate) + 128):
            for c, want in enumerate(expected):
                actual = audio[frame * channels + c]
                assert abs(actual - want) < 1e-6, (t,c,actual,want)
print('Standalone routing audio passed: 128 channels, unity sum, half gains and source cleanup; no MacroMixer loaded.')
