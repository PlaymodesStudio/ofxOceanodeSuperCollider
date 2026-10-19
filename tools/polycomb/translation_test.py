#!/usr/bin/env python3
"""Check imported waveform choices against the real shared LFO SynthDef."""
import json,math,pathlib,subprocess,sys
ROOT=pathlib.Path(__file__).resolve().parents[2]
from import_ensemble import lfo_shape,lfo_span
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav

out=pathlib.Path('/tmp/polycomb-translation-test');out.mkdir(exist_ok=True)
source=out/'probe.scd'
source.write_text('Archive.archiveDir="/tmp/polycomb-translation-test";\nSynthDef(\\PCLfoProbe,{Out.ar(0,K2A.ar(In.kr(\\in.kr(100))))}).writeDefFile("/tmp/polycomb-translation-test");\n0.exit;\n')
config=out/'config.yaml'
config.write_text(f'includePaths:\n    - {ROOT}/sclang/SCClassLibrary\nexcludePaths: []\npostInlineWarnings: false\nexcludeDefaultPaths: true\n')
r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l',str(config),str(source)],capture_output=True,text=True)
assert r.returncode==0,r.stdout+r.stderr
paths=[out/'PCLfoProbe.scsyndef',ROOT/'synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation/ModLfo1.scsyndef']
score=bundle(0,[message('/d_recv',p.read_bytes())for p in paths]+[message('/g_new',1,0,0)])
waves=[3,4,5];length=.1
for i,wave in enumerate(waves):
    # Freeze the LFO at phase 1/8: square=0, triangle=1/4, cosine≈.14645.
    score+=bundle(i*length+.01,[message('/s_new','ModLfo1',100,1,1,'out',100,'freq',0.0,'initphase',.125,'round',lfo_shape(wave),'skew',0.0,'pw',.5),message('/s_new','PCLfoProbe',101,1,1,'in',100)])
    score+=bundle((i+1)*length-.01,[message('/g_freeAll',1)])
score+=bundle(.31,[message('/c_set',0,0.0)])
osc=out/'test.osc';osc.write_bytes(score);wav=out/'test.wav'
r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(osc),'_',str(wav),'48000','WAV','float','-o','1','-i','0','-a','128','-c','4096','-D','0'],capture_output=True,text=True)
assert r.returncode==0 and 'FAILURE' not in r.stdout+r.stderr,r.stdout+r.stderr
audio,ch,rate=read_wav(wav)
expected=[0,.25,(1-math.cos(math.pi/4))/2]
for i,target in enumerate(expected):
    values=audio[int((i*length+.04)*rate):int((i*length+.07)*rate)]
    assert all(abs(x-target)<1e-4 for x in values),(waves[i],target,values[:5])

# In particular, offset=1 produces a constant, and a negative offset reduces
# the available excursion instead of introducing a spurious negative pitch.
assert lfo_span(1,1)==(1,0)
assert lfo_span(1,-1)==(0,0)
assert lfo_span(1,0)==(.5,.5)
assert lfo_span(.5,.25)==(.5,.25)
P=ROOT/'synthdefs/Defaults/PolyComb/ReaktorSnapshots'
raw=json.loads((P/'recovered-snapshots.json').read_text())['snapshots']
slots=[]
for p in sorted(P.glob('Reaktor PolyComb*.json')):slots+=json.loads(p.read_text())['slots']
for original,slot in zip(raw,slots):
    v=original['effective_values'];c=slot['controls']
    for li,sid in ((1,'193'),(2,'209'),(3,'361'),(4,'386')):
        assert c[f'LFO{li}_Round']==[lfo_shape(int(v[sid]))]
    mean,depth=lfo_span(v['181'],v['182'])
    assert abs(slot['matrix'][12]-depth/max(.0001,mean))<1e-6
print('PASS: pulse/triangle/sine mappings match rendered native LFO; all 210 amplitude routes respect clipped endpoints.')
