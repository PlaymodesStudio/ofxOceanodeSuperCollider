#!/usr/bin/env python3
"""Compare the adaptation with the original converters and synthesis graph.

Native LFO is neutralized, oscillator sync enabled and the same DC blocker
is applied to both outputs for repeatable comparison.
Fixed-mode patches are excluded because the adaptation fixes upstream overrides.
"""
from pathlib import Path
import re,json,math,subprocess,sys
root=Path(__file__).resolve().parents[2];s=(root/'synthdefs/Defaults/FMBox/upstream/DX7.scd.txt').read_text();out=Path('/tmp/fmbox-reference');out.mkdir(exist_ok=True)
# Compile upstream converters and synth without booting or modifying a server.
start=s.index('coarseArrR =');end=s.index('/*\nbuffers and busses')
vars=s[start:end].strip().rstrip(',')
synth=s[s.index('SynthDef(\\DX7,'):s.index('scale_vrv = {')]
synth=synth.replace('\\DX7,','\\DX7Reference,').replace('FreeSelfWhenDone.kr(Line.kr(0, 1, 10));','').replace('kilnod = DetectSilence.ar(out, 0.01, 0.2, doneAction:2);','').replace('Out.ar(outBus, out.dup);','Out.ar(outBus, LeakDC.ar(out));').replace('}).add;','}).writeDefFile("/tmp/fmbox-reference");')
conv=s[s.index('scale_vrv = {'):s.index('noteParser = { arg vel, note;')]
# defjamHead0 is not used; remove because it depends on the original buffers.
a=conv.index('defjamHead0 = {');b=conv.index('defPitchEnv = {');conv=conv[:a]+conv[b:]
script='(\nvar '+vars+',busMePitch=200,busMeAmp=201;\n'+synth+conv+r'''
{
    var f=File("/tmp/fmbox-reference/params.tsv","w");
    var bank=File("BANK","r"),line,rows=List.new;
    while({line=bank.getLine;line.notNil},{rows.add(line)});bank.close;
    [0,1,2,5,10,30,100,1000,16383].do { |index|
        var row=rows[index];
        [[60,127],[36,64],[84,100]].do { |notevel|
            var note=notevel[0],vel=notevel[1],params;
            145.do { |k| var pair=cirklonCCparse[k],value=row.copyRange(k*2,k*2+1).asInteger;
                vr[pair[1]+if(pair[0]==4,128,0)]=value;
            };
            params=defme.(note,vel);
            f.write(index.asString++"|"++note.asString++"|"++vel.asString);
            params.pairsDo { |key,value| f.write("\t"++key.asString++":"++value.asString) };f.write("\n");
        };
    };f.close;
}.value;
)
'''.replace('BANK',str(root/'data/Supercollider/Presets/FMBox/DX7.afx'))
(out/'reference.scd').write_text(script)
(out/'compile.scd').write_text('Archive.archiveDir="/tmp/fmbox-reference";try {"/tmp/fmbox-reference/reference.scd".load;} { |e| e.reportError; 1.exit; };0.exit;\n')

# A neutral native LFO lets this test isolate the upstream voice equations.
wrapper=out/'compile.scd'
wrapper.write_text('Archive.archiveDir="/tmp/fmbox-reference";try {"/tmp/fmbox-reference/reference.scd".load;SynthDef(\\FMBoxNeutral,{Out.ar(200,[DC.ar(1),DC.ar(0)])}).writeDefFile("/tmp/fmbox-reference");} { |e| e.reportError; 1.exit; };0.exit;\n')
(out/'sclang.yaml').write_text('includePaths:\n'+''.join('    - \"'+str(p)+'\"\n'for p in(root/'sclang/SCClassLibrary',root/'sclang/Extensions/user/SC3plugins/SkUGens'))+'excludeDefaultPaths: true\n')
result=subprocess.run([str(root/'sclang/osx/sclang'),'-l',str(out/'sclang.yaml'),str(wrapper)],capture_output=True,text=True,timeout=30)
log=result.stdout+result.stderr;(out/'compile.log').write_text(log)
assert result.returncode==0 and 'ERROR:' not in log.split('*** Welcome to SuperCollider')[-1],log
sys.path.insert(0,str(root/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
patches={p['index']:p['values'] for p in json.loads(Path('/tmp/fmbox-dx7-render/patches.json').read_text())}
paths=[root/'synthdefs/Defaults/FMBox/CompiledSynthdefs/FMBox/FMBoxVoice.scsyndef',out/'DX7Reference.scsyndef',out/'FMBoxNeutral.scsyndef',root/'synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation/ModGate1.scsyndef']
score=bundle(0,[message('/d_load',str(p))for p in paths]+[message('/g_new',1,0,0),message('/c_setn',32,16,*([0.0]*16)),message('/s_new','FMBoxNeutral',500,1,1)])
quiet=sum(([k,32]for k in ['mpitch','mscale','mfeedback','mamp']+['m'+kind+str(i)for kind in('ratio','detune','level')for i in range(6)]),[])
cases=[]
for line in(out/'params.tsv').read_text().splitlines():
    parts=line.split('\t');index,note,vel=map(int,parts[0].split('|'))
    if index not in patches:continue
    v=patches[index]
    if any(v[f'op{op}_mode'] for op in range(6)):continue
    params={k:float(value)for k,value in (part.split(':')for part in parts[1:])}
    # Deterministic phases; the source stores the inverted osc_sync switch.
    params['osc_sync']=0
    prod=dict(v,osc_sync=1)
    t=len(cases)*2.1+.01
    score+=bundle(t,[message('/s_new','ModGate1',100,1,1,'out',64,'vel',0,'rnd',16,'gate',1.0,'velocity',vel/127),
        message('/s_new','FMBoxVoice',101,1,1,'gatebus',64,'velbus',0,'nativebus',200,'out',0,'vis',2048,'pitch',float(note),'master',1.0,'normalization',1.0,*quiet,*sum(([k,float(v)]for k,v in prod.items()if k!='stereo'),[])),
        message('/s_new','DX7Reference',102,1,1,'outBus',1,*sum(([k,float(v)]for k,v in params.items()),[]),'gate',0.0)])
    score+=bundle(t+128/48000,[message('/n_set',102,'gate',1.0)])
    score+=bundle(t+1.2,[message('/n_set',100,'gate',0.0)])
    score+=bundle(t+1.2+128/48000,[message('/n_set',102,'gate',0.0)])
    score+=bundle(t+2,[message('/n_free',100,101,102)])
    cases.append((index,note,vel,t))
score+=bundle(len(cases)*2.1+.01,[message('/n_free',500)])
osc=out/'reference.osc';osc.write_bytes(score);wav=out/'reference.wav'
r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(osc),'_',str(wav),'48000','WAV','float','-o','2','-i','0','-z','64','-D','0','-w','4096','-m','262144'],capture_output=True,text=True,timeout=60)
log=r.stdout+r.stderr;(out/'server.log').write_text(log);assert r.returncode==0 and not any(x in log for x in('FAILURE','ERROR')),log
samples,ch,sr=read_wav(wav);report=[]
for index,note,vel,t in cases:
    a=round((t+.15)*sr)*2;b=round((t+1.1)*sr)*2
    prod=samples[a:b:2];ref=samples[a+1:b:2]
    rms=lambda x:math.sqrt(sum(v*v for v in x)/len(x))
    x,y=rms(prod),rms(ref)
    report.append(dict(preset=index+1,note=note,velocity=vel,adaptation=x,reference=y,ratio=x/y if y>1e-8 else None))
(out/'measurements.json').write_text(json.dumps(report,indent=2))
audible=[p for p in report if p['reference']>1e-5]
assert len(audible)>=12,report
assert all(.95<p['ratio']<1.05 for p in audible),report
print(f"PASS: {len(audible)} audible preset/note/velocity cases agree with upstream voice RMS within 5%; full measurements in {out}")
