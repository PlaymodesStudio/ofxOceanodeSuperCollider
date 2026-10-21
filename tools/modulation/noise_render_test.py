#!/usr/bin/env python3
"""Render real shared noise LFOs and per-voice envelopes; compile defaults first."""
import pathlib,subprocess,sys,math,argparse
import numpy as np
ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--synthdefs',type=pathlib.Path);args=parser.parse_args()
out=pathlib.Path('/tmp/modulation-voice-noise');out.mkdir(exist_ok=True)
config=out/'config.yaml';config.write_text(f'includePaths:\n    - {ROOT}/sclang/SCClassLibrary\nexcludePaths: []\nexcludeDefaultPaths: true\n')
p=out/'probe.scd';p.write_text(f'SynthDef(\\NoiseVoiceProbe,{{Out.ar(0,K2A.ar(In.kr(100,32)))}}).writeDefFile("{out}");\nSynthDef(\\EnvVoiceGate,{{Out.ar(32,DC.ar(1)!4)}}).writeDefFile("{out}");\n0.exit;\n')
r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-D','-l',str(config),str(p)],capture_output=True,text=True,timeout=30);assert r.returncode==0,r.stdout+r.stderr
mod=args.synthdefs/'Defaults/Modulation' if args.synthdefs else ROOT/'synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation'
events=[]
for f in [out/'NoiseVoiceProbe.scsyndef',out/'EnvVoiceGate.scsyndef',mod/'ModLfo32.scsyndef',mod/'ModLfoX32.scsyndef',mod/'ModEnvAr4.scsyndef']:
 events.append((0,message('/d_recv',f.read_bytes())))
events.append((0,message('/g_new',1,0,0)))
for k,(definition,scale)in enumerate([('ModLfo32',1.),('ModLfoX32',1.),('ModLfo32',2.),('ModLfo32',0.)]):
 t=k*5.+.01
 events.extend([(t,message('/s_new',definition,100,1,1,'out',100,'noise',1.,'nscale',scale,'seed',0.)),(t,message('/n_setn',100,'freq',32,*([2.]*32))),(t,message('/s_new','NoiseVoiceProbe',101,1,1)),((k+1)*5.-.01,message('/g_freeAll',1))])
events.extend([(20.01,message('/s_new','EnvVoiceGate',99,1,1)),(20.01,message('/s_new','ModEnvAr4',100,1,1,'gate',32,'out',100,'aout',64)),(20.01,message('/n_setn',100,'atk',4,.01,.1,.3,.7,'dec',4,.3,.3,.3,.3,'sus',4,1.,1.,1.,1.,'cA',4,0.,0.,0.,0.)),(20.01,message('/s_new','NoiseVoiceProbe',101,1,1)),(20.99,message('/g_freeAll',1)),(21.,message('/c_set',0,0.))])
score=out/'test.osc';score.write_bytes(b''.join(bundle(t,[m])for t,m in sorted(events,key=lambda e:e[0])))
r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(score),'_',str(out/'test.wav'),'48000','WAV','float','-o','32','-i','0','-a','256','-c','512','-D','0','-m','65536','-U','/Applications/SuperCollider.app/Contents/Resources/plugins'],capture_output=True,text=True,timeout=40);assert r.returncode==0 and 'FAILURE' not in r.stdout+r.stderr,str(r.returncode)+r.stdout+r.stderr
raw,ch,sr=read_wav(out/'test.wav');a=np.asarray(raw).reshape(-1,ch);assert np.isfinite(a).all()
for k in range(3):
 seg=a[int((k*5+.1)*sr):int((k*5+4.8)*sr)]
 assert seg.min()>=-1e-6 and seg.max()<=1.000001
 assert np.min(np.ptp(seg,axis=1))>.05,('voices collapsed',k,np.min(np.ptp(seg,axis=1)))
 for cycle in range(1,9):
  row=a[int((k*5+.01+cycle*.5)*sr)]
  assert np.ptp(row)>.1,('integer cycle collapse',k,cycle,row)
seg=a[int(15.1*sr):int(19.8*sr)]
assert np.max(np.ptp(seg,axis=1))<1e-5,'Scale 0 must still link voices'
assert np.ptp(seg[:,0])>.1,'Noise must keep moving at Scale 0'
row=a[int(20.06*sr),:4];assert row[0]>.95 and row[1]>.4 and row[1]<.6 and row[2]>.12 and row[2]<.22 and row[3]>.04 and row[3]<.11,row
print('PASS: real ModLfo/ModLfoX noise stays independent at integer cycles (scales 1/2); Scale 0 intentionally links voices; four independent ADSR attacks render correctly.')
