#!/usr/bin/env python3
"""Render production Off controls, mode switching and Random smoothing on real SC."""
import json,pathlib,subprocess,sys,tempfile
import numpy as np
from migration import ROOT
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
out=pathlib.Path('/tmp/polycomb-lfo-models');out.mkdir(exist_ok=True)
# Execute the actual Off branch through a minimal OSC-recording server adapter.
code=(ROOT/'src/nodes/boxSynths/shared/scModulation.cpp').read_text();start=code.index('    s->set("hold", manualLfo(li)');end=code.index('\n    }\n}',start)+6
branch=code[start:end]
cpp=r'''
#include <nlohmann/json.hpp>
#include <vector>
#include <iostream>
using J=nlohmann::json;
struct Synth{J c;template<class T>void set(const char*k,T v){c[k]=v;}};
struct Param{float v;float get(){return v;}};
int n(){return 32;}bool manualLfo(int){return true;}
int main(){for(float value:{0.f,.25f,.5f,.75f,1.f}){
 Synth storage;auto*s=&storage;int li=0;Param lfoManual[]={{value}};
BRANCH
 std::cout<<storage.c.dump()<<'\n';}}
'''.replace('BRANCH',branch)
with tempfile.TemporaryDirectory() as d:
 p=pathlib.Path(d)/'t.cpp';p.write_text(cpp);exe=pathlib.Path(d)/'t'
 subprocess.run(['c++','-std=c++17','-I'+str(ROOT.parent.parent/'libs/json/include'),str(p),'-o',str(exe)],check=True)
 controls=[json.loads(x) for x in subprocess.check_output([str(exe)],text=True).splitlines()]
config=out/'config.yaml';config.write_text(f'includePaths:\n    - {ROOT}/sclang/SCClassLibrary\nexcludePaths: []\npostInlineWarnings: false\nexcludeDefaultPaths: true\n')
probe=out/'probe.scd';probe.write_text(f'SynthDef(\\LfoModelsProbe,{{Out.ar(0,K2A.ar(In.kr(100,32)))}}).writeDefFile("{out}");\n0.exit;\n')
r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l',str(config),str(probe)],capture_output=True,text=True,timeout=30);assert r.returncode==0,r.stdout+r.stderr
mod=ROOT/'synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation'
events=[(0,message('/d_recv',(out/'LfoModelsProbe.scsyndef').read_bytes())),(0,message('/d_recv',(mod/'ModLfo32.scsyndef').read_bytes())),(0,message('/d_recv',(mod/'ModLfoX32.scsyndef').read_bytes())),(0,message('/g_new',1,0,0))]
cases=controls+[
 {'hold':0.,'freq':[2.]*32,'initphase':0.,'round':[.5]*32,'skew':[0.]*32,'pw':[.5]*32,'phoff':[0.]*32,'noise':0.,'rtype':0.},
 {'hold':0.,'freq':[10.]*32,'rtype':1.,'rsmooth':0.,'noise':0.,'seed':4.},
 {'hold':0.,'freq':[10.]*32,'rtype':1.,'rsmooth':.2,'noise':0.,'seed':4.},
 {'hold':0.,'freq':[2.]*32,'rtype':0.,'noise':1.,'nscale':1.,'seed':3.}]
for i,c in enumerate(cases):
 start=i*1.2+.01
 # Reuse the held LFO for the Classic case, proving hold is released.
 if i!=5:
  events.append((start,message('/s_new','ModLfoX32' if i in (6,7) else 'ModLfo32',100,1,1,'out',100)))
  events.append((start,message('/s_new','LfoModelsProbe',101,1,1)))
 args=[]
 for key,value in c.items():
  values=value if isinstance(value,list) else [value];args += [key,len(values),*values]
 events.append((start,message('/n_setn',100,*args)))
 if i!=4:events.append(((i+1)*1.2-.01,message('/g_freeAll',1)))
events.append((len(cases)*1.2,message('/c_set',0,0.)))
(out/'t.osc').write_bytes(b''.join(bundle(t,[m])for t,m in sorted(events,key=lambda e:e[0])))
r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(out/'t.osc'),'_',str(out/'t.wav'),'48000','WAV','float','-o','32','-i','0','-a','256','-c','512','-D','0'],capture_output=True,text=True,timeout=30)
assert r.returncode==0 and 'FAILURE' not in r.stdout+r.stderr,r.stdout+r.stderr
samples,ch,sr=read_wav(out/'t.wav');audio=np.asarray(samples).reshape(-1,ch);assert np.isfinite(audio).all()
segments=[audio[int((i*1.2+.05)*sr):int((i*1.2+1.1)*sr)] for i in range(len(cases))]
for i,value in enumerate((0,.25,.5,.75,1)):assert np.max(np.abs(segments[i]-value))<1e-4,(value,segments[i].min(),segments[i].max())
assert np.ptp(segments[5][:,0])>.9,'Classic must move again after Off'
assert np.ptp(segments[8][:,0])>.01,'Noise must move'
rough=max(np.max(np.abs(np.diff(segments[6][:,0]))),1e-9)
smooth=np.max(np.abs(np.diff(segments[7][:,0])))
assert smooth < rough*.1,(rough,smooth)
print('PASS: exact Off values 0/.25/.5/.75/1 on all 32 voices, Off → Classic resumes, Noise moves and Random Smooth removes step discontinuities.')
