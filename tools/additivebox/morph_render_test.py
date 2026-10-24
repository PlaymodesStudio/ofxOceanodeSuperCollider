#!/usr/bin/env python3
"""Render AdditiveBox's production voice amp path across count-morph gains."""
from pathlib import Path
import json
import math
import re
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav

with tempfile.TemporaryDirectory(prefix='additive-morph-render-') as directory:
    work=Path(directory)
    source=(ROOT/'synthdefs/Defaults/AdditiveBox/additiveBox.scd').read_text()
    source=source.replace('caps = [8, 16, 32, 64, 128, 256]','caps = [8]').replace('chunks = [8, 16, 32, 64]','chunks = [8]').replace('maxOut = 16','maxOut = 1')
    # Identical deterministic phases isolate the voice gain from phase beating.
    source,n=re.subn(r'\{ Rand\(0, 2pi\) \} ! nc','(0 ! nc)',source)
    assert n==1
    (work/'additiveBox.scd').write_text(source)
    (work/'run.scd').write_text('Archive.archiveDir='+json.dumps(str(work))+';this.executeFile('+json.dumps(str(work/'additiveBox.scd'))+');"MORPH COMPILED".postln;0.exit;')
    (work/'sclang.yaml').write_text('includePaths:\n  - '+str(ROOT/'sclang/SCClassLibrary')+'\nexcludeDefaultPaths: true\n')
    r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-D','-l',str(work/'sclang.yaml'),str(work/'run.scd')],capture_output=True,text=True,timeout=60)
    assert r.returncode==0 and 'MORPH COMPILED' in r.stdout,r.stdout+r.stderr
    definition=work/'CompiledSynthdefs/AdditiveBox/AdditiveBoxCore8_1.scsyndef'
    events=bundle(0,[message('/d_recv',definition.read_bytes()),message('/g_new',1,0,0)])
    setup=[message('/c_setn',1000,24,*[1.0]+[0.0]*7,*[float(i+1) for i in range(8)],*[.5]*8)]
    for v in range(9):
        setup.append(message('/s_new','AdditiveBoxCore8_1',100+v,1,1,'bus',1000,'np',8,'first',0,'gate',1.0,'pitch',60.0,'amp',.1 if v in (0,8) else 0.0,'silbus',4096+v,'out',v))
    events+=bundle(.01,setup)
    cases=[]
    for j,t in enumerate((0.0,.001,.1,.499,.501,.9,1.0,.5,0.0)):
        when=.05+j*.25
        events+=bundle(when,[message('/n_set',100+v,'amp',.1*t) for v in range(1,8)])
        cases.append((when,t))
    events+=bundle(2.4,[message('/g_freeAll',1)])+bundle(2.41,[message('/c_set',0,0.0)])
    score=work/'test.osc';score.write_bytes(events);wav=work/'test.wav'
    r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(score),'_',str(wav),'48000','WAV','float','-o','9','-i','0','-z','64','-D','0','-m','131072','-U','/Applications/SuperCollider.app/Contents/Resources/plugins'],capture_output=True,text=True,timeout=30)
    assert r.returncode==0 and 'FAILURE' not in r.stdout+r.stderr,(r.returncode,r.stdout+r.stderr)
    audio,channels,sr=read_wav(wav)
    assert channels==9 and all(math.isfinite(x) for x in audio)
    for when,t in cases:
        frames=range(round((when+.17)*sr),round((when+.23)*sr))
        ref=sum(audio[f*channels+8]**2 for f in frames)
        assert ref>1e-4
        for v in range(8):
            expected=1 if v==0 else t
            error=sum((audio[f*channels+v]-expected*audio[f*channels+8])**2 for f in frames)
            assert math.sqrt(error/ref)<2e-5,(t,v,math.sqrt(error/ref))
    print('PASS: real AdditiveBox core renders count-presence gains at .001 and across .5; common voice unchanged, forward/reverse endpoint gains exact')
