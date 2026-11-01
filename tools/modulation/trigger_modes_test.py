#!/usr/bin/env python3
"""Exercise shared per-voice gate clocks, one-shot ramps and beat envelopes."""
import pathlib,subprocess,sys
import numpy as np
ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
work=pathlib.Path('/tmp/shared-modulation-triggers');work.mkdir(exist_ok=True)
for name in ['modulation.scd','modulationRandom.scd']:
 s=(ROOT/'synthdefs/Defaults/Modulation'/name).read_text().replace('(1..32).do','(1..2).do')
 (work/name).write_text(s)
# Compile AdditiveBox's production clock too: its partial lanes use these controls.
s=(ROOT/'synthdefs/Defaults/AdditiveBox/additiveBox.scd').read_text()
a=s.index('SynthDef(\\AdditiveBoxLaneClock,');b=s.index('}).writeDefFile(dir);',a)+len('}).writeDefFile(dir);')
clock='(var dir="'+str(work)+'";\n'+s[a:b]+'\n)'
(work/'clock.scd').write_text(clock)
probe=r'''
SynthDef(\TestGates,{Out.ar(32,K2A.ar(\gate.kr(0 ! 2)))}).writeDefFile("WORK");
SynthDef(\TestModProbe,{Out.ar(0,K2A.ar(In.kr(700,2)))}).writeDefFile("WORK");
'''.replace('WORK',str(work))
(work/'run.scd').write_text('"'+str(work/'modulation.scd')+'".load;\n"'+str(work/'modulationRandom.scd')+'".load;\n"'+str(work/'clock.scd')+'".load;\n'+probe+'\n"COMPILED".postln;0.exit;')
(work/'config.yaml').write_text(f'includePaths:\n  - {ROOT}/sclang/SCClassLibrary\nexcludePaths: []\nexcludeDefaultPaths: true\n')
(work/'home').mkdir(exist_ok=True)
r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l',str(work/'config.yaml'),str(work/'run.scd')],capture_output=True,text=True,
                 timeout=120,stdin=subprocess.DEVNULL,env={'HOME':str(work/'home'),'PATH':'/usr/bin:/bin'})
assert 'COMPILED' in r.stdout and 'ERROR' not in r.stdout,r.stdout[-5000:]
defs=list((work/'CompiledSynthdefs/Modulation').glob('*.scsyndef'))+list(work.glob('*.scsyndef'))
SR=48000

def render(name,kind,mode=0,power=1,restart=0,noise=False):
 score=bundle(0,[message('/d_recv',p.read_bytes())for p in defs]+[message('/g_new',1,0,0)])
 score+=bundle(.01,[message('/s_new','TestGates',100,1,1),
                     message('/s_new',kind,101,1,1,'out',700.0,'gate',32.0,'freq',1.0,'round',0.0,'skew',-1.0,
                             'trigmode',float(mode),'phasepower',float(power),'restartsec',float(restart)),
                     message('/n_setn',101,'round',2,0.0,0.0),message('/n_setn',101,'skew',2,-1.0,-1.0),
                     message('/n_set',101,'noise',1.0 if noise else 0.0),message('/s_new','TestModProbe',102,1,1)])
 if kind=='AdditiveBoxLaneClock':
  score+=bundle(.02,[message('/n_set',101,'bus',673.0,'np',8.0,'l0rate',1.0,'l0trigmode',float(mode),'l0restartsec',float(restart),'gate',0.0)])
 for t,g in [(.5,[1.,0.]),(1.0,[1.,1.]),(2.0,[0.,1.]),(2.5,[1.,1.])]:
  score+=bundle(t,[message('/n_setn',100,'gate',2,*g)]+([message('/n_set',101,'gate',g[0])] if kind=='AdditiveBoxLaneClock' else []))
 score+=bundle(3,[message('/c_set',0,0.)])
 osc=work/(name+'.osc');osc.write_bytes(score);wav=work/(name+'.wav')
 result=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(osc),'_',str(wav),str(SR),
                        'WAV','float','-o','2','-i','0','-c','4096','-a','1024'],capture_output=True,text=True,timeout=60)
 assert wav.exists(),result.stdout+result.stderr
 data,ch,_=read_wav(wav);return np.array(data).reshape(-1,ch)

for kind in ['ModLfo2','ModLfoX2']:
 x=render(kind+'-shot',kind,2)
 assert np.max(np.abs(x[int(.1*SR):int(.4*SR)]))<1e-7
 assert abs(x[int(1.25*SR),0]-.75)<.01 and abs(x[int(1.25*SR),1]-.25)<.01
 assert x[int(1.75*SR),0]>.99 and x[int(2.25*SR),0]==0
 assert abs(x[int(2.75*SR),0]-.25)<.01
 q=render(kind+'-quadratic',kind,2,2)
 assert abs(q[int(.75*SR),0]-.0625)<.01
 loop=render(kind+'-loop',kind,1,1,.75)
 assert abs(loop[int(1.4*SR),0]-.15)<.01 and abs(loop[int(1.4*SR),1]-.4)<.01
 free=render(kind+'-free',kind,0)
 assert np.max(abs(free[:,0]-free[:,1]))<1e-7 and free[int(.3*SR),0]>.25
 print('PASS:',kind,'independent per-voice Gate, one-shot end hold, retrigger, quadratic ramp, periodic reset, free-running compatibility',flush=True)
noise=render('noise-shot','ModLfoX2',2,noise=True)
assert np.ptp(noise[int(1.6*SR):int(1.9*SR),0])<1e-6
print('PASS: one-shot noise also holds its terminal value',flush=True)
clock=render('partial-shot','AdditiveBoxLaneClock',2)
assert abs(clock[int(1.25*SR),0]-.75)<.01 and clock[int(1.75*SR),0]>.99
clock=render('partial-loop','AdditiveBoxLaneClock',1,1,.75)
assert abs(clock[int(1.4*SR),0]-.15)<.01
print('PASS: AdditiveBox partial-lane one-shot hold and beat-period reset',flush=True)
# Check actual C++ envelope values consumed by custom/shared hosts.
source=(ROOT/'src/nodes/boxSynths/shared/scModulation.cpp').read_text()
a=source.index('scModulation::EnvValues scModulation::envValues');b=source.index('\n}',a)+2
method=source[a:b]
cpp=r'''
#include <algorithm>
#include <cassert>
template<class T>struct P{T v;T get()const{return v;}};
struct scModulation {
 struct EnvValues{float a,d,s,r,ca,cd,cr;};
 struct E{P<float>a{1},d{2},s{0.5},r{3},ca{0},cd{0},cr{0};P<bool>beats{false};}envs[1];
 float currentBpm=120;EnvValues envValues(int)const;
};
METHOD
int main(){scModulation m;assert(m.envValues(0).a==1);m.envs[0].beats.v=true;
 assert(m.envValues(0).a==0.5 && m.envValues(0).d==1 && m.envValues(0).r==1.5);
 m.currentBpm=60;assert(m.envValues(0).a==1 && m.envValues(0).r==3);
}
'''.replace('METHOD',method)
(work/'env.cpp').write_text(cpp)
subprocess.run(['clang++','-std=c++17',str(work/'env.cpp'),'-o',str(work/'env')],check=True)
subprocess.run([str(work/'env')],check=True)
print('PASS: production envelope beat/second conversion at 60 and 120 BPM',flush=True)
print('ALL PASS',flush=True)
