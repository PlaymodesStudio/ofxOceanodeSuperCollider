#!/usr/bin/env python3
"""Check PhysicalVoice family coefficients and native audio without global installation."""
from pathlib import Path
import argparse,json,math,subprocess,sys,tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',type=Path,default=Path('/tmp/physical-family-morph'))
parser.add_argument('--plugin-dir',type=Path,default=ROOT/'ugens/OceanodePhysical')
args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'ugens/OceanodePhysical'),str(ROOT/'tools/physicalbox/family_morph_test.cpp'),'-o',str(out/'coefficients')],check=True)
subprocess.run([str(out/'coefficients')],check=True)
(out/'config.yaml').write_text('includePaths:\n  - '+str(ROOT/'sclang/SCClassLibrary')+'\n  - '+str(ROOT/'ugens/OceanodePhysical')+'\nexcludeDefaultPaths: true\n')
(out/'compile.scd').write_text(r'''try{SynthDef(\FamilyMorph,{
 var seed=RandSeed.ir(1,1234);
 var pairs=PhysicalVoice.names.collect{|name|[name,NamedControl.kr(name,PhysicalVoice.defaults[PhysicalVoice.names.indexOf(name)])]}.flatten;
 Out.ar(\out.kr(0),PhysicalVoice.ar(SinOsc.ar(110)*0.03,pairs,NamedControl.kr(\bfweights,0!6).lag(0.03))*0.2);
}).writeDefFile(OUT_DIR);"FAMILY COMPILED".postln;0.exit;}{|e|e.reportError;1.exit;};'''.replace('OUT_DIR',json.dumps(str(out))))
p=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-u','0','-l',str(out/'config.yaml'),str(out/'compile.scd')],capture_output=True,text=True,timeout=60)
assert p.returncode==0 and 'FAMILY COMPILED' in p.stdout and 'ERROR:' not in p.stdout+p.stderr,p.stdout+p.stderr
events={}
def add(t,*messages):events.setdefault(t,[]).extend(messages)
add(0,message('/d_recv',(out/'FamilyMorph.scsyndef').read_bytes()),message('/g_new',1,0,0))
def create(when,family):
 for i in range(2):add(when,message('/s_new','FamilyMorph',100+i,1,1,'out',2*i,'freq',110.,'gate',1.,'strike',0.,'ext',1.,'excite',1.,'listen',1.,'bfamily',float(family),'bshape',.37,'bmodes',32.))
# Same native state, explicit one-hot weights versus the legacy family argument.
for family in range(6):
 when=.02+.5*family;create(when,family);w=[0.]*6;w[family]=1.
 add(when,message('/n_setn',101,'bfweights',6,*w));add(when+.45,message('/g_freeAll',1))
# A/B interpolation must ignore the editor's categorical change at .5.
create(3.1,1)
for i,t in enumerate([0.,.25,.499,.501,.75,1.]):
 when=3.1+.15*i;w=[0.]*6;w[1]=1-t;w[5]=t
 for node in [100,101]:add(when,message('/n_setn',node,'bfweights',6,*w))
 add(when,message('/n_set',101,'bfamily',1. if t<.5 else 5.))
add(4.15,message('/g_freeAll',1));add(4.2,message('/c_set',0,0.))
(out/'score.osc').write_bytes(b''.join(bundle(t,events[t])for t in sorted(events)))
p=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(out/'score.osc'),'_',str(out/'audio.wav'),'48000','WAV','float','-o','4','-i','0','-m','131072','-D','0','-U','/Applications/SuperCollider.app/Contents/Resources/plugins:'+str(args.plugin_dir.resolve())],capture_output=True,text=True,timeout=45)
(out/'render.log').write_text(p.stdout+p.stderr)
assert p.returncode==0 and 'FAILURE' not in p.stdout+p.stderr and 'not found' not in p.stdout+p.stderr,p.stdout+p.stderr
a,ch,sr=read_wav(out/'audio.wav');assert ch==4 and all(map(math.isfinite,a)) and max(map(abs,a))>1e-6
error=max(abs(a[i+j]-a[i+j+2])for i in range(0,len(a),4)for j in range(2))
assert error<1e-7,error
print('PASS: native body-family audio ignores the .5 editor switch, retains legacy one-hot endpoints and continuous resonator state; max reference error:',error)
