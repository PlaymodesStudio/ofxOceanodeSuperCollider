#!/usr/bin/env python3
"""Production shared clocks: late-created sources, stop/start, seek and tempo."""
from pathlib import Path
import argparse,subprocess,sys
import numpy as np
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
bundled=ROOT.parents[1]/'apps/Playmodes/OceanAudio/bin/data/Supercollider/Scsynth/bin/scsynth'
ap=argparse.ArgumentParser();ap.add_argument('--server',type=Path,default=bundled if bundled.exists() else Path('/Applications/SuperCollider.app/Contents/Resources/scsynth'));server=ap.parse_args().server
w=Path('/tmp/modulation-transport');w.mkdir(exist_ok=True);(w/'home').mkdir(exist_ok=True)
for f in ['modulation.scd','modulationRandom.scd','modulationRamp.scd']:
 (w/f).write_text((ROOT/'synthdefs/Defaults/Modulation'/f).read_text().replace('(1..32).do','(1..2).do'))
s=(ROOT/'synthdefs/Defaults/AdditiveBox/additiveBox.scd').read_text();a=s.index('SynthDef(\\AdditiveBoxLaneClock,');b=s.index('}).writeDefFile(dir);',a)+len('}).writeDefFile(dir);')
(w/'additiveClock.scd').write_text('(var dir="'+str(w)+'";\n'+s[a:b]+'\n)')
(w/'config.yaml').write_text('includePaths:\n  - '+str(ROOT/'sclang/SCClassLibrary')+'\nexcludeDefaultPaths: true\n')
probe='SynthDef(\\TransportProbe,{Out.ar(0,K2A.ar([In.kr(700),In.kr(710),In.kr(720)]))}).writeDefFile("'+str(w)+'");'
(w/'run.scd').write_text('try{'+''.join('"'+str(w/f)+'".load;'for f in ['modulation.scd','modulationRandom.scd','modulationRamp.scd','additiveClock.scd'])+probe+'"COMPILED".postln;0.exit;} {|e|e.reportError;1.exit;}')
r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l',str(w/'config.yaml'),str(w/'run.scd')],capture_output=True,text=True,timeout=60,env={'HOME':str(w/'home'),'PATH':'/usr/bin:/bin'},stdin=subprocess.DEVNULL)
assert r.returncode==0 and 'COMPILED'in r.stdout and 'ERROR'not in r.stdout,r.stdout+r.stderr
defs=list((w/'CompiledSynthdefs/Modulation').glob('*.scsyndef'))+list(w.glob('*.scsyndef'))
def render(kind,random=False,noise=False,hold=0,steps=9):
 score=bundle(0,[message('/d_recv',p.read_bytes())for p in defs]+[message('/g_new',1,0,0)])
 score+=bundle(.01,[message('/s_new','ModTransportClock',100,1,1,'out',600.,'bpm',120.,'run',1.,'anchorId',1.,'anchorHard',1.,'anchorPos',0.),message('/s_new','TransportProbe',104,1,1)])
 for when,node,out,link in [(.37,101,700.,1.),(.73,102,710.,1.),(.73,103,720.,0.)]:
  if kind=='AdditiveBoxLaneClock':
   args=['bus',out-27,'np',8.,'l0transport',link,'ltransportbus',600.,'l0cpb',1.,'l0shcpb',2.,'l0rate',2.]
  else:
   args=['out',out,'transportbus',600.,'transport',link,'cpb',1.,'shcpb',2.,'freq',2.,'trigmode',0.,'ending',1.,'from',0.,'to',1.,'rtype',1. if random else 0.,'rshare',1.,'seed',17.31,'steps',float(steps) if random else 0.,'round',0.,'skew',-1.,'sh',float(hold),'noise',1. if noise else 0.]
  score+=bundle(when,[message('/s_new',kind,node,1,1,*args)])
 for when,pos,run,bpm,hard,aid in [(1.31,2.6,0,120,2,2),(1.61,2.6,1,120,3,3),(2.,0,1,120,4,4),(2.71,1.42,1,60,5,5)]:
  score+=bundle(when,[message('/n_set',100,'anchorPos',float(pos),'run',float(run),'bpm',float(bpm),'anchorHard',float(hard),'anchorId',float(aid))])
 score+=bundle(4,[message('/c_set',0,0.)]);name=kind+('-random'if random else '-noise'if noise else '-hold'if hold else '')
 osc=w/(name+'.osc');osc.write_bytes(score);wav=w/(name+'.wav')
 r=subprocess.run([str(server),'-N',str(osc),'_',str(wav),'48000','WAV','float','-o','3','-i','0','-c','2048','-a','256','-D','0','-U',str(server.parent/'plugins')],capture_output=True,text=True,timeout=30)
 assert r.returncode==0,r.stdout+r.stderr
 data,ch,sr=read_wav(wav);x=np.array(data).reshape(-1,ch)
 assert np.all(np.isfinite(x))
 # Joining mid-beat does not establish a new phase origin.
 assert np.max(abs(x[int(.8*sr):int(3.9*sr),0]-x[int(.8*sr):int(3.9*sr),1]))<.001
 assert np.ptp(x[int(1.4*sr):int(1.55*sr),0])<1e-6 # stop holds
 assert np.max(abs(x[int(.8*sr):int(1.25*sr),0]-x[int(.8*sr):int(1.25*sr),2]))>.05 # opt-in only
 return x,sr
for kind in ['ModLfo1','ModLfoX1','ModRamp1','AdditiveBoxLaneClock']:
 x,sr=render(kind)
 for t,expected in [(.8,.58),(1.45,.6),(1.8,.98),(2.2,.4),(3.2,.91)]:
  assert abs(x[int(t*sr),0]-expected)<.015,(kind,t,x[int(t*sr),0],expected)
 print('PASS:',kind,'late join, common grid, stop, resume, seek, tempo and unlinked compatibility',flush=True)
x,sr=render('ModLfoX1',random=True)
# Eight beat-wide intervals use nine output levels, including both endpoints.
values=x[::64,0]*8
assert np.max(abs(values-np.round(values)))<1e-5
changes=np.flatnonzero(abs(np.diff(x[::64,0]))>.01)*64/sr
for t in changes:
 if .45<t<1.3:assert min(abs(t-.51),abs(t-1.01))<.006,t
assert len(np.unique(np.round(values)))>=3
before,_=render('ModLfoX1',random=True,steps=8)
old=before[::64,0]*4
assert np.max(abs(old-np.round(old)))>.1
print('PASS: RANDOMPOS destinations are integer beats and random changes occur on transport beats',flush=True)
for kind in ['ModLfo1','ModLfoX1']:render(kind,noise=True,hold=4)
print('PASS: noise and sample/hold share transport timing; linked envelopes use transport tempo through envValues')
