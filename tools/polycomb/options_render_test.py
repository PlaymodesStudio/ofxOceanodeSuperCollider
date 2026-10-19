#!/usr/bin/env python3
"""Offline checks for appended filter types and optional nonlinear feedback.
Uses the real compiled optional SynthDefs and existing AnalogFilter plugin.
"""
import json,math,pathlib,subprocess,sys
ROOT=pathlib.Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
out=pathlib.Path('/tmp/polycomb-options-render');out.mkdir(exist_ok=True)
sc=out/'probe.scd';sc.write_text('Archive.archiveDir="/tmp/polycomb-options-render";\nSynthDef(\\PCOptionsInput,{Out.ar(\\out.kr(32),SinOsc.ar(\\freq.kr(4000),0,\\amp.kr(0.1))+(Impulse.ar(0)*\\imp.kr(0)))}).writeDefFile("/tmp/polycomb-options-render");\n0.exit;\n')
config=out/'config.yaml';config.write_text(f'includePaths:\n    - {ROOT}/sclang/SCClassLibrary\nexcludePaths: []\npostInlineWarnings: false\nexcludeDefaultPaths: true\n')
r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l',str(config),str(sc)],capture_output=True,text=True);assert r.returncode==0 and (out/'PCOptionsInput.scsyndef').exists(),r.stdout+r.stderr
folder=ROOT/'synthdefs/Defaults/PolyComb/CompiledSynthdefs/PolyComb'
paths=[out/'PCOptionsInput.scsyndef']+[folder/f'{name}1.scsyndef'for name in ('PCFilterSlopes','PCCombSat')]
score=bundle(0,[message('/d_recv',p.read_bytes())for p in paths]+[message('/g_new',1,0,0),message('/c_setn',2000,32,*([0.0]*32)),message('/c_setn',512,2,60.0,60.0)])
cases=[];length=.5
for typ,freq in [(0,4000),(1,100),(2,4000),(3,1000)]:
    for slope in (1,2,3):cases.append(dict(name=f'filter-{typ}-{slope}',kind='filter',type=typ,slope=slope,freq=freq))
for fb in (.8,-.8):
    for note in (33,69,95):
        for level in (0,48):cases.append(dict(name=f'comb-{fb}-{note}-{level}',kind='comb',feed=fb,note=note,level=level))
for i,c in enumerate(cases):
    inp=message('/s_new','PCOptionsInput',100,1,1,'out',32,'freq',float(c.get('freq',1000)),'amp',.1 if c['kind']=='filter' else 0.0,'imp',0.0 if c['kind']=='filter' else 2.0)
    if c['kind']=='filter':
        fx=message('/s_new','PCFilterSlopes1',101,1,1,'in',32,'out',0,'on',1.0,'type',float(c['type']),'slope',float(c['slope']),'cutoff',69+12*math.log2(1000/440),'res',0.0,'note',512,'mcut',2000,'mres',2000)
    else:
        fx=message('/s_new','PCCombSat1',101,1,1,'in',32,'out',0,'on',1.0,'combmode',1.0,'combpitch',float(c['note']),'combfeed',c['feed'],'combtone',1.0,'combmix',1.0,'satlevel',float(c['level']),'note',512,'mcpitch',2000,'mcfb',2000,'mcmix',2000)
    score+=bundle(i*length+.01,[inp,fx]);score+=bundle((i+1)*length-.01,[message('/g_freeAll',1)])
score+=bundle(len(cases)*length+.1,[message('/c_set',0,0.0)])
p=out/'test.osc';p.write_bytes(score);wav=out/'test.wav'
r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(p),'_',str(wav),'48000','WAV','float','-U',str(ROOT/'ugens/OceanodeAnalog')+':/Applications/SuperCollider.app/Contents/Resources/plugins','-o','1','-i','0','-a','128','-c','4096','-z','64','-D','0','-w','2048','-m','65536'],capture_output=True,text=True)
(out/'server.log').write_text(r.stdout+r.stderr);assert r.returncode==0 and 'FAILURE' not in r.stdout+r.stderr,r.stdout+r.stderr
samples,ch,sr=read_wav(wav);results=[];rendered={}
for i,c in enumerate(cases):
    values=samples[int((i*length+.01)*sr):int(((i+1)*length-.02)*sr)]
    assert all(math.isfinite(x)for x in values),c['name']
    rendered[c['name']]=values
    tail=values[int(.2*sr):]
    results.append(dict(name=c['name'],peak=max(map(abs,values)),rms=math.sqrt(sum(x*x for x in tail)/len(tail))))
levels={r['name']:r['rms']for r in results}
assert levels['filter-0-2']<levels['filter-0-1']*.4,'LP4 must roll off more than LP2'
assert levels['filter-1-2']<levels['filter-1-1']*.2,'HP4 must roll off more than HP2'
assert levels['filter-2-2']<levels['filter-2-1']*.4,'BP4 must roll off more than BP2'
for fb in (.8,-.8):
    for note in (33,69,95):
        a=rendered[f'comb-{fb}-{note}-0'];b=rendered[f'comb-{fb}-{note}-48']
        assert max(abs(x-y)for x,y in zip(a,b))>1e-4,'Sat Level must change actual feedback audio'
(out/'measurements.json').write_text(json.dumps(results,indent=2)+'\n')
print('PASS: appended filter slopes roll off correctly; saturated positive/negative feedback at 55/440/1975 Hz is finite and responds to Sat Level.')
