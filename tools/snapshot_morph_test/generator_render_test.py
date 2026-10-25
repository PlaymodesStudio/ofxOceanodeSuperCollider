#!/usr/bin/env python3
"""Render production shared LFO blends and held-note envelopes on offline SC."""
from pathlib import Path
import sys,subprocess,math
r=Path(__file__).resolve().parents[2];sys.path.insert(0,str(r/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
import argparse,json
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',type=Path,default=Path('/tmp/shared-generator-morph-render'))
args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
for name in ['modulation.scd','modulationRandom.scd']:
 (out/name).write_text((r/'synthdefs/Defaults/Modulation'/name).read_text().replace('(1..32).do','[1].do'))
(out/'config.yaml').write_text('includePaths:\n  - '+str(r/'sclang/SCClassLibrary')+'\nexcludeDefaultPaths: true\n')
probe = 'SynthDef(\\GeneratorProbe,{Out.ar(0,K2A.ar(In.kr(100,6)));Out.ar(6,K2A.ar(In.kr(110,1)));Out.ar(7,In.ar(20,1))}).writeDefFile(OUT_DIR);\nSynthDef(\\GeneratorGate,{Out.ar(16,K2A.ar(\\value.kr(1)))}).writeDefFile(OUT_DIR);\n\n' .replace("OUT_DIR",json.dumps(str(out)))
(out/'compile.scd').write_text('try{'+''.join('this.executeFile('+json.dumps(str(out/name))+');' for name in ['modulation.scd','modulationRandom.scd'])+probe+'"GENERATORS COMPILED".postln;0.exit;}{|error|error.reportError;1.exit;};')
compiled=subprocess.run([str(r/'sclang/osx/sclang'),'-u','0','-l',str(out/'config.yaml'),str(out/'compile.scd')],capture_output=True,text=True,timeout=120)
assert compiled.returncode==0 and 'GENERATORS COMPILED' in compiled.stdout and 'ERROR:' not in compiled.stdout,compiled.stdout+compiled.stderr
defs=out/'CompiledSynthdefs/Modulation'
events={}
def add(t,*msg):events.setdefault(t,[]).extend(msg)
for name in ['ModLfoX1','ModEnv1','ModEnvAr1']:add(0,message('/d_recv',(defs/(name+'.scsyndef')).read_bytes()))
for name in ['GeneratorProbe','GeneratorGate']:add(0,message('/d_recv',(out/(name+'.scsyndef')).read_bytes()))
add(.01,message('/g_new',1,0,0))
for mode in range(5):add(.02,message('/s_new','ModLfoX1',1000+mode,1,1,'out',100+mode,'freq',1.3,'seed',7,'noise',1 if mode==1 else 0,'rtype',max(0,mode-1),'initphase',.13))
add(.02,message('/s_new','ModLfoX1',1005,1,1,'out',105,'freq',1.3,'seed',7,'initphase',.13))
add(.02,message('/s_new','GeneratorGate',1010,1,1),message('/s_new','ModEnv1',1011,1,1,'out',110,'gate',16,'atk',.05,'dec',.1,'sus',1.,'rel',.2),message('/s_new','ModEnvAr1',1012,1,1,'out',111,'aout',20,'gate',16,'atk',.05,'dec',.1,'sus',1.,'rel',.2),message('/s_new','GeneratorProbe',1013,1,1))
# Live blend every pair, including reversing and interrupting: same source ID and clock.
cases=[];start=.3
for a,b in [(0,1),(1,2),(2,3),(3,4),(4,0)]:
 for t in [0.,.001,.25,.499,.501,.75,.999,1.]:
  w=[0.]*6;w[a]=1-t;w[b]+=t;add(start,message('/n_setn',1005,'modeweights',6,*w));cases.append((start,w));start+=.08
# Held sustain morphs through midpoint and both endpoints, then release/retrigger.
sustains=[]
for when,value in [(0.3,1.),(.6,.75),(.9,.501),(1.2,.499),(1.5,.25),(1.8,0.),(2.1,.6)]:
 for node in [1011,1012]:add(when,message('/n_set',node,'sus',value))
 sustains.append((when,value))
add(2.5,message('/n_set',1010,'value',0.));add(3.,message('/n_set',1010,'value',1.))
add(4.,message('/g_freeAll',0));add(4.1,message('/c_set',0,0.))
(out/'score.osc').write_bytes(b''.join(bundle(t,events[t]) for t in sorted(events)))
r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(out/'score.osc'),'_',str(out/'render.wav'),'48000','WAV','float','-o','8','-i','0','-a','128','-c','4096','-m','131072','-D','0','-U','/Applications/SuperCollider.app/Contents/Resources/plugins'],capture_output=True,text=True,timeout=30)
assert r.returncode==0 and 'FAILURE' not in r.stdout+r.stderr,r.stdout+r.stderr
a,ch,sr=read_wav(out/'render.wav');assert ch==8 and all(math.isfinite(v) for v in a)
for when,w in cases:
 for i in range(round((when+.015)*sr),round((when+.065)*sr),64):
  expected=sum(w[j]*a[i*8+j] for j in range(5));assert abs(a[i*8+5]-expected)<2e-5,(when,a[i*8+5],expected)
for when,s in sustains:
 for c in [6,7]:
  i=round((when+.06)*sr);assert abs(a[i*8+c]-s)<2e-4,(when,c,a[i*8+c],s)
for c in [6,7]:
 assert abs(a[round(2.85*sr)*8+c])<1e-6
 assert abs(a[round(3.3*sr)*8+c]-.6)<2e-4
print('PASS: real SC LFO weighted blends across all algorithms at .499/.501 on one running clock; held Sustain, release and retrigger for control/audio envelopes')
