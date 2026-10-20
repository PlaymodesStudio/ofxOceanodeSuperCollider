#!/usr/bin/env python3
"""Check imported waveform choices against the real shared LFO SynthDef."""
import json,math,pathlib,subprocess,sys
ROOT=pathlib.Path(__file__).resolve().parents[2]
from import_ensemble import lfo_span,LFO_RANDOM
ROUND={2:1.0,3:0.0,4:.5}   # the Lists' pulse / triangle / sine
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
waves=[2,3,4];length=.1
for i,wave in enumerate(waves):
    # Freeze the LFO at phase 1/8: square=0, triangle=1/4, cosine≈.14645.
    score+=bundle(i*length+.01,[message('/s_new','ModLfo1',100,1,1,'out',100,'freq',0.0,'initphase',.125,'round',ROUND[wave],'skew',0.0,'pw',.5),message('/s_new','PCLfoProbe',101,1,1,'in',100)])
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
    for li,sid,count in ((1,'193',7),(2,'209',7),(3,'361',8),(4,'386',8)):
        wf=min(max(int(v.get(sid,5))-1,0),count-1)  # the row from 1 (5: Sin) -> the List's value; past the end: its last entry
        assert c[f'LFO{li}_Random_Type']==LFO_RANDOM.get(wf,0),(slot['name'],li,wf)
        if wf in ROUND:assert c[f'LFO{li}_Round']==[ROUND[wf]]
        if wf in (0,1):assert c[f'LFO{li}_Skew']==[-1.0 if wf==0 else 1.0] and c[f'LFO{li}_Round']==[0.0]
    mean,depth=lfo_span(v['181'],v['182'])
    assert abs(slot['matrix'][12]-depth/max(.0001,mean))<1e-6
# 256: both poly LFOs on row 5 (Sin), no random; mono LFO1 on its last row (jumps)
s256=next(s for s in slots if s['name']=='256')['controls']
assert s256['LFO1_Random_Type']==0 and s256['LFO1_Round']==[.5] and s256['LFO2_Random_Type']==0 and s256['LFO3_Random_Type']==3,s256['LFO1_Random_Type']
print('PASS: pulse/triangle/sine (List values 2/3/4) match the rendered native LFO; saws and random types follow the decoded Lists (rows from 1: 5 is Sin); all 210 amplitude routes respect clipped endpoints.')
