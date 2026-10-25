#!/usr/bin/env python3
"""Verify batched /c_setn ranges against scsynth without a sound device."""
from pathlib import Path
import subprocess,sys,tempfile
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
with tempfile.TemporaryDirectory(prefix='box-bus-batch-') as folder:
    work=Path(folder)
    (work/'probe.scd').write_text('Archive.archiveDir="/tmp";\nSynthDef(\\BoxBusBatchProbe,{Out.ar(0,K2A.ar([In.kr(0),In.kr(1),In.kr(8)]))}).writeDefFile("'+str(work)+'");\n0.exit;\n')
    (work/'config.yaml').write_text('includePaths:\n    - '+str(root/'sclang/SCClassLibrary')+'\nexcludePaths: []\nexcludeDefaultPaths: true\n')
    compile_result=subprocess.run([str(root/'sclang/osx/sclang'),'-D','-l',str(work/'config.yaml'),str(work/'probe.scd')],capture_output=True,text=True,timeout=30)
    definition=work/'BoxBusBatchProbe.scsyndef'
    assert compile_result.returncode==0 and definition.exists(),compile_result.stdout+compile_result.stderr
    score=bundle(0,[message('/d_recv',definition.read_bytes()),message('/g_new',1,0,0)])
    score+=bundle(.01,[message('/c_setn',0,2,.25,.5,8,1,.75)])
    score+=bundle(.02,[message('/s_new','BoxBusBatchProbe',100,0,1)])
    score+=bundle(.2,[message('/n_free',100)])
    (work/'score.osc').write_bytes(score)
    result=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(work/'score.osc'),'_',str(work/'probe.wav'),'48000','WAV','float','-o','3','-i','0','-D','0'],capture_output=True,text=True,timeout=30)
    assert result.returncode==0 and 'FAILURE' not in result.stdout+result.stderr,result.stdout+result.stderr
    audio,channels,rate=read_wav(work/'probe.wav');assert channels==3
    start,end=int(.08*rate),int(.12*rate)
    for ch,expected in enumerate((.25,.5,.75)):
        samples=audio[start*channels+ch:end*channels:channels]
        assert samples and max(abs(x-expected) for x in samples)<1e-6,(ch,min(samples),max(samples))
    print('PASS: scsynth applies every disjoint range in one /c_setn message; three rendered bus levels are exact.')
