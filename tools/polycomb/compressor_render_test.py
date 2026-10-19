#!/usr/bin/env python3
"""Verify the reused MixerPro compressor's bypass, ratio and GR routing."""
import math,pathlib,subprocess,sys
ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
out=pathlib.Path('/tmp/polycomb-compressor-test');out.mkdir(exist_ok=True)
src=out/'probe.scd'
src.write_text('Archive.archiveDir="/tmp/polycomb-compressor-test";\nSynthDef(\\PCCompInput,{Out.ar(32,SinOsc.ar(1000,0,\\amp.kr(1))!2)}).writeDefFile("/tmp/polycomb-compressor-test");\nSynthDef(\\PCCompMeter,{Out.ar(2,K2A.ar(In.kr(100)))}).writeDefFile("/tmp/polycomb-compressor-test");\n0.exit;\n')
config=out/'config.yaml';config.write_text(f'includePaths:\n    - {ROOT}/sclang/SCClassLibrary\nexcludePaths: []\npostInlineWarnings: false\nexcludeDefaultPaths: true\n')
r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l',str(config),str(src)],capture_output=True,text=True,timeout=30)
assert r.returncode==0,r.stdout+r.stderr
paths=[out/'PCCompInput.scsyndef',out/'PCCompMeter.scsyndef',ROOT/'synthdefs/Defaults/MixerPro/CompiledSynthdefs/mixerpro/mixerProComp2.scsyndef']
score=bundle(0,[message('/d_recv',p.read_bytes())for p in paths]+[message('/g_new',1,0,0)])
cases=[('dry',1,0,1),('mix-dry',1,1,0),('compressed',1,1,1),('quiet',.01,1,1)]
for i,(name,amp,on,mix)in enumerate(cases):
    score+=bundle(i+.01,[message('/s_new','PCCompInput',100,1,1,'amp',float(amp)),message('/s_new','mixerProComp2',101,1,1,'in',32,'out',0,'grBus',100,'threshold',-20.0,'ratio',4.0,'knee',0.0,'attack',.1,'release',5.0,'makeup',0.0,'rms',0.0,'schpf',20.0,'on',float(on),'mix',float(mix)),message('/s_new','PCCompMeter',102,1,1)])
    score+=bundle(i+.99,[message('/g_freeAll',1)])
score+=bundle(4.1,[message('/c_set',0,0.0)])
osc=out/'test.osc';osc.write_bytes(score);wav=out/'test.wav'
r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(osc),'_',str(wav),'48000','WAV','float','-o','3','-i','0','-a','128','-c','4096','-D','0'],capture_output=True,text=True)
assert r.returncode==0 and 'FAILURE' not in r.stdout+r.stderr,r.stdout+r.stderr
audio,ch,rate=read_wav(wav);assert ch==3
rendered=[];meters=[]
for i in range(4):
    values=audio[int((i+.6)*rate)*ch:int((i+.8)*rate)*ch]
    assert all(math.isfinite(x)for x in values)
    rendered.append(values[::ch]);meters.append(sum(values[2::ch])/len(values[2::ch]))
assert max(abs(a-b)for a,b in zip(rendered[0],rendered[1]))<1e-6
rms=lambda a:math.sqrt(sum(x*x for x in a)/len(a))
assert .1<rms(rendered[2])/rms(rendered[0])<.35
assert 10<meters[2]<20 and abs(meters[3])<.1,meters
assert .005<rms(rendered[3])<.01
print('PASS: shared MixerPro compressor bypass is dry, loud input compresses, quiet input passes, and live GR bus reports reduction.')
