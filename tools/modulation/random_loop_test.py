#!/usr/bin/env python3
"""Random steps at 100 BPM through 8-beat loops: coverage and seed semantics."""
from pathlib import Path
import argparse,subprocess,sys
import numpy as np
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
bundled=ROOT.parents[1]/'apps/Playmodes/OceanAudio/bin/data/Supercollider/Scsynth/bin/scsynth'
ap=argparse.ArgumentParser();ap.add_argument('--server',type=Path,default=bundled if bundled.exists() else Path('/Applications/SuperCollider.app/Contents/Resources/scsynth'));server=ap.parse_args().server
w=Path('/tmp/random-loop-coverage');w.mkdir(exist_ok=True);(w/'home').mkdir(exist_ok=True)
for name in ['modulation.scd','modulationRandom.scd']:
 (w/name).write_text((ROOT/'synthdefs/Defaults/Modulation'/name).read_text().replace('(1..32).do','(1..2).do'))
(w/'config.yaml').write_text('includePaths:\n  - '+str(ROOT/'sclang/SCClassLibrary')+'\nexcludeDefaultPaths: true\n')
probe='SynthDef(\\LoopProbe,{Out.ar(0,K2A.ar([In.kr(700),In.kr(710),In.kr(720),In.kr(730)]))}).writeDefFile("'+str(w)+'");'
(w/'compile.scd').write_text('try{'+''.join('"'+str(w/name)+'".load;'for name in ['modulation.scd','modulationRandom.scd'])+probe+'"COMPILED".postln;0.exit;} {|e|e.reportError;1.exit;}')
r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l',str(w/'config.yaml'),str(w/'compile.scd')],capture_output=True,text=True,timeout=60,env={'HOME':str(w/'home'),'PATH':'/usr/bin:/bin'},stdin=subprocess.DEVNULL)
assert r.returncode==0 and 'COMPILED'in r.stdout and 'ERROR'not in r.stdout,r.stdout+r.stderr
defs=list((w/'CompiledSynthdefs/Modulation').glob('*.scsyndef'))+list(w.glob('*.scsyndef'))
def render(name):
 score=bundle(0,[message('/d_recv',p.read_bytes())for p in defs]+[message('/g_new',1,0,0)])
 setup=[message('/s_new','ModTransportClockV2',100,1,1,'out',600.,'bpm',100.,'run',1.,'anchorId',1.,'anchorHard',1.,'anchorPos',0.,'anchorRandomPos',0.)]
 for i,(seed,repeat)in enumerate([(42,0),(42,1),(0,0),(0,0)]):
  setup.append(message('/s_new','ModLfoX1',101+i,1,1,'out',700.+10*i,'transportbus',600.,'transport',1.,'repeatloop',float(repeat),'cpb',1.,'freq',100./60,'rtype',1.,'rshare',1.,'seed',float(seed),'steps',9.))
 setup.append(message('/s_new','LoopProbe',110,1,1));score+=bundle(.01,setup)
 for loop in range(1,8):
  score+=bundle(.01+loop*8*.6,[message('/n_set',100,'anchorPos',0.,'anchorRandomPos',float(loop*8),'anchorId',float(loop+1),'anchorHard',float(loop+1))])
 score+=bundle(.01+64*.6,[message('/c_set',0,0.)]);osc=w/(name+'.osc');osc.write_bytes(score);wav=w/(name+'.wav')
 r=subprocess.run([str(server),'-N',str(osc),'_',str(wav),'48000','WAV','float','-o','4','-i','0','-c','2048','-a','256','-D','0','-U',str(server.parent/'plugins')],capture_output=True,text=True,timeout=30)
 assert r.returncode==0,r.stdout+r.stderr
 data,ch,sr=read_wav(wav);x=np.array(data).reshape(-1,ch)
 assert np.all(np.isfinite(x))
 draws=np.array([x[int((.01+(i+.5)*.6)*sr),:]for i in range(64)])*8
 assert np.max(abs(draws-np.round(draws)))<1e-5
 # New targets stay constant between adjacent beat boundaries.
 for i in range(64):
  a=int((.01+(i+.2)*.6)*sr);b=int((.01+(i+.8)*.6)*sr)
  assert np.max(np.ptp(x[a:b],axis=0))<1e-6
 return draws
x=render('first');y=render('second')
assert set(np.round(x[:,0]).astype(int))==set(range(9)),np.unique(x[:,0])
assert np.array_equal(x[:,1].reshape(8,8),np.tile(x[:8,1],(8,1)))
assert len(np.unique(x[:,1]))<9
assert np.array_equal(x[:,0],y[:,0]) # positive seed repeatable across new synths
assert np.mean(x[:,2]!=x[:,3])>.5 and np.mean(x[:,2]!=y[:,2])>.5 # 0 is no fixed seed
print('PASS: all nine positions reached across 8-beat loops at 100 BPM; Repeat loop preserves repeating patterns; draws stay beat-aligned')
print('PASS: positive seeds repeat across runs; seed 0 gives independent fresh sequences')
# Exercise production seed mapping and transport anchor arithmetic, not copies.
s=(ROOT/'src/nodes/boxSynths/shared/scModulation.cpp').read_text();a=s.index('float scModulation::lfoNoiseSeed(');b=s.index('\n}',a)+2
h=(ROOT/'src/timing/scTransportSync.h').read_text();ha=h.index('inline double unwrappedBeatAt(');hb=h.index('\n}',ha)+2
cpp=r'''
#include <cassert>
#include <cmath>
#include <cstdint>
struct P{int v=0;int get()const{return v;}};
struct scModulation{P lfoSeed[1];float freshSeeds[1]{123};float lfoNoiseSeed(int)const;};
METHOD
struct ofxOceanodeTransportState{bool isPlaying=true,loopEnabled=true;float bpm=100;double beatPosition=7.9,loopStartBeat=0,loopEndBeat=8;uint64_t loopCount=0;bool loopActive()const{return loopEnabled&&loopEndBeat>loopStartBeat;}};
ANCHOR
int main(){scModulation a,b;assert(a.lfoNoiseSeed(0)==123);a.freshSeeds[0]=321;assert(a.lfoNoiseSeed(0)==321);
 a.lfoSeed[0].v=b.lfoSeed[0].v=42;b.freshSeeds[0]=999;assert(a.lfoNoiseSeed(0)==b.lfoNoiseSeed(0));
 assert(a.lfoNoiseSeed(0)>0);a.lfoSeed[0].v=43;assert(a.lfoNoiseSeed(0)!=b.lfoNoiseSeed(0));
 ofxOceanodeTransportState t;assert(std::abs(unwrappedBeatAt(t,.06)-8)<1e-6); // pre-sent wrap
 t.beatPosition=.2;t.loopCount=1;assert(std::abs(unwrappedBeatAt(t,-.12)-8)<1e-6); // late NRT wrap
 t.isPlaying=false;assert(std::abs(unwrappedBeatAt(t,1)-8.2)<1e-6);
}
'''.replace('METHOD',s[a:b]).replace('ANCHOR',h[ha:hb])
(w/'native.cpp').write_text(cpp);subprocess.run(['clang++','-std=c++17',str(w/'native.cpp'),'-o',str(w/'native')],check=True);subprocess.run([str(w/'native')],check=True)
print('PASS: native seed mapping and ahead/late/stopped loop anchors')
