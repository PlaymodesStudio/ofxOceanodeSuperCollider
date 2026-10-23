#!/usr/bin/env python3
"""Render the real folded scope: ordered history, channel choice, read-only audio and free."""
import pathlib
import subprocess
import sys
import tempfile
ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
with tempfile.TemporaryDirectory(prefix='folded-waveform-audio-') as folder:
    work=pathlib.Path(folder)
    conf=work/'sclang.yaml'
    conf.write_text('includePaths:\n    - "'+str(ROOT/'sclang/SCClassLibrary')+'"\nexcludeDefaultPaths: true\n')
    source=work/'test.scd'
    source.write_text('''
SynthDef(\\scopeSource, {Out.ar(256, [Line.ar(0, 1, 1), DC.ar(-0.75)])}).writeDefFile("%s");
SynthDef(\\scopeTap, {Out.ar(0, [In.ar(256)] ++ K2A.ar(In.kr(128, 64)))}).writeDefFile("%s");
0.exit;
'''%(work,work))
    r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l',str(conf),str(source)],capture_output=True,text=True,timeout=60)
    assert r.returncode==0 and (work/'scopeTap.scsyndef').exists(),r.stdout+r.stderr
    definition=ROOT/'data/Supercollider/Synthdefs/Defaults/Analysis/foldedWaveform/oceanodeFoldedWaveform.scsyndef'
    score=bundle(0,[message('/d_recv',definition.read_bytes()),message('/d_load',str(work/'scopeSource.scsyndef')),
                    message('/d_load',str(work/'scopeTap.scsyndef')),message('/g_new',1,0,0)])
    score+=bundle(.01,[message('/s_new','scopeSource',1000,1,1),
                       message('/s_new','oceanodeFoldedWaveform',1001,1,1,'in',256,'out',128),
                       message('/s_new','scopeTap',1002,1,1)])
    score+=bundle(.4,[message('/n_free',1001),message('/c_fill',128,64,0.)])
    score+=bundle(.55,[message('/g_freeAll',1)])
    osc=work/'test.osc';osc.write_bytes(score);wav=work/'test.wav'
    r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(osc),'_',str(wav),
                      '48000','WAV','float','-o','65','-i','0','-a','1024','-w','8192','-D','0','-U',
                      '/Applications/SuperCollider.app/Contents/Resources/plugins'],capture_output=True,text=True,timeout=60)
    assert r.returncode==0 and 'FAILURE' not in r.stdout+r.stderr,r.stdout+r.stderr
    audio,channels,rate=read_wav(wav);assert channels==65
    for t in (.15,.25,.35):
        frame=round(t*rate);raw=audio[frame*channels];history=audio[frame*channels+1:frame*channels+65]
        assert abs(raw-(t-.01))<.002,(t,raw)
        assert all(b>=a-1e-6 for a,b in zip(history,history[1:])),history
        assert abs((history[-1]-history[0])-.05)<.002,history
        assert abs(history[-1]-raw)<.003,(t,history[-1],raw)
        assert history[0]>0 # channel two is negative and must not be mixed in
    frame=round(.48*rate)
    assert abs(audio[frame*65]-.47)<.002
    assert max(abs(v) for v in audio[frame*65+1:frame*65+65])<1e-6
print('PASS: scrolling 64-point history, 50 ms span, first channel, no audio changes and clean removal.')
