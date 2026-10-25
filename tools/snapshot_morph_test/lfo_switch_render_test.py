#!/usr/bin/env python3
"""Render multivoice LFO mode and discrete-control morphs on production DSP."""
from pathlib import Path
import sys,subprocess,math
r=Path(__file__).resolve().parents[2];sys.path.insert(0,str(r/'tools/aliasingbox'));from render_test import message,bundle,read_wav
import argparse,json
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',type=Path,default=Path('/tmp/lfo-switch-morph-render'))
parser.add_argument('--definitions',type=Path,help='Compare against another compiled definition directory')
args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
(out/'sclang.yaml').write_text('includePaths:\n  - '+str(r/'sclang/SCClassLibrary')+'\nexcludeDefaultPaths: true\n')
(out/'modulationRandom.scd').write_text((r/'synthdefs/Defaults/Modulation/modulationRandom.scd').read_text().replace('(1..32).do','[4].do'))
(out/'compile.scd').write_text('try{this.executeFile('+json.dumps(str(out/'modulationRandom.scd'))+');SynthDef(\\MorphProbe4,{Out.ar(0,K2A.ar(In.kr(100,12)))}).writeDefFile('+json.dumps(str(out))+');"COMPILED".postln;0.exit;}{|e|e.reportError;1.exit;};')
compiled=subprocess.run([str(r/'sclang/osx/sclang'),'-u','0','-l',str(out/'sclang.yaml'),str(out/'compile.scd')],capture_output=True,text=True,timeout=90)
assert compiled.returncode==0 and 'COMPILED' in compiled.stdout and 'ERROR:' not in compiled.stdout,compiled.stdout+compiled.stderr
defs=args.definitions.resolve() if args.definitions else out/'CompiledSynthdefs/Modulation'

events={}
def add(t,*messages):events.setdefault(t,[]).extend(messages)
for p in [defs/'ModLfoX4.scsyndef',out/'MorphProbe4.scsyndef']:add(0,message('/d_recv',p.read_bytes()))
add(.01,message('/g_new',1,0,0))
for i in range(3):add(.02,message('/s_new','ModLfoX4',1000+i,1,1,'out',100+i*4,'freq',0.,'seed',7.,'initphase',.13,'rtype',1.,'rshare',float(i==1)))
for i in range(3):add(.02,message('/n_setn',1000+i,'freq',4,0.,0.,0.,0.))
add(.02,message('/s_new','MorphProbe4',1100,1,1))
for j,t in enumerate([0.,.25,.499,.501,.75,1.]):
 when=.1+.1*j;add(when,message('/n_set',1002,'rshare',t))
# Classic -> Stepped with Random Same also changing: one stable field each side.
for i in [0,1]:add(.8,message('/n_set',1000+i,'rshare',float(i),'rtype',float(i)))
for j,t in enumerate([0.,.25,.499,.501,.75,1.]):
 when=.8+.1*j;w=[1-t,0.,t,0.,0.,0.];add(when,message('/n_set',1002,'rshare',t,'rtype',float(t>=.5)),message('/n_setn',1002,'modeweights',6,*w))
# Centered and quantizer switches: no abrupt jump at halfway or integer thresholds.
for i in range(3):add(1.5,message('/n_set',1000+i,'rtype',0.,'pw',.2,'skew',.3),message('/n_setn',1000+i,'modeweights',6,1.,0.,0.,0.,0.,0.))
add(1.5,message('/n_set',1002,'centered',.499));add(1.6,message('/n_set',1002,'centered',.501))
add(1.8,message('/n_set',1002,'pw',.5,'skew',0.,'steps',1.999));add(1.9,message('/n_set',1002,'steps',2.001))
add(2.1,message('/g_freeAll',0));add(2.2,message('/c_set',0,0.))
(out/'score.osc').write_bytes(b''.join(bundle(t,events[t]) for t in sorted(events)))
p=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(out/'score.osc'),'_',str(out/'audio.wav'),'48000','WAV','float','-o','12','-i','0','-a','128','-c','4096','-m','131072','-D','0','-U','/Applications/SuperCollider.app/Contents/Resources/plugins'],capture_output=True,text=True,timeout=30);assert p.returncode==0 and 'FAILURE' not in p.stdout+p.stderr,p.stdout+p.stderr
a,ch,sr=read_wav(out/'audio.wav');assert ch==12 and all(math.isfinite(v) for v in a) and max(map(abs,a))>.01
def at(t,c):return a[round((t+.04)*sr)*ch+c]
for j,t in enumerate([0.,.25,.499,.501,.75,1.]):
 for voice in range(4):
  expected=(1-t)*at(.1+.1*j,voice)+t*at(.1+.1*j,voice+4)
  assert abs(at(.1+.1*j,voice+8)-expected)<2e-5,('sharing discontinuity',t,voice,at(.1+.1*j,voice+8),expected)
for start,end in [(1.,1.1),(1.5,1.6),(1.8,1.9)]:
 change=max(abs(at(end,c)-at(start,c)) for c in range(8,12));assert change<.01,('midpoint jump',start,change)
print('PASS: four-voice Classic/Stepped transition, stable shared/independent random fields, Centered and Steps stay continuous around .5 in actual DSP')
