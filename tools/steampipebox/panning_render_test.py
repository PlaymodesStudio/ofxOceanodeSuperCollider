#!/usr/bin/env python3
"""Render the production panner, Pan telemetry and indexed shared LFO routes."""
import pathlib,subprocess,sys,argparse
import numpy as np
ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--synthdefs',type=pathlib.Path);args=parser.parse_args()
out=pathlib.Path('/tmp/steampipe-pan-render');out.mkdir(exist_ok=True)
config=out/'config.yaml';config.write_text(f'includePaths:\n    - {ROOT}/sclang/SCClassLibrary\nexcludePaths: []\nexcludeDefaultPaths: true\n')
p=out/'probe.scd';p.write_text(f'''
SynthDef(\\PanInput,{{Out.ar(32,DC.ar([1,2,3,4]++(0!12)))}}).writeDefFile("{out}");
SynthDef(\\PanOffsets,{{Out.kr(100,NamedControl.kr(\\offset,[-0.4,-0.1,0.1,0.4]++(0!12)))}}).writeDefFile("{out}");
SynthDef(\\PanProbe,{{Out.ar(0,In.ar(64,4)++K2A.ar(In.kr(200,4))++K2A.ar(In.kr(150,4)))}}).writeDefFile("{out}");
0.exit;
''')
r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-D','-l',str(config),str(p)],capture_output=True,text=True,timeout=30);assert r.returncode==0,r.stdout+r.stderr
mod=args.synthdefs/'Defaults/Modulation' if args.synthdefs else ROOT/'synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation'
steam=args.synthdefs/'Defaults/SteamPipeBox' if args.synthdefs else ROOT/'synthdefs/Defaults/SteamPipeBox/CompiledSynthdefs/SteamPipeBox'
events=[]
for file in [out/'PanInput.scsyndef',out/'PanOffsets.scsyndef',out/'PanProbe.scsyndef',steam/'SteamPipeBoxSpread4.scsyndef',mod/'ModLfo4.scsyndef',mod/'ModRoute4.scsyndef']:
 events.append((0,message('/d_recv',file.read_bytes())))
events += [(0,message('/g_new',1,0,0)),(.01,message('/s_new','PanInput',90,1,1)),(.01,message('/s_new','PanOffsets',91,1,1)),(.01,message('/s_new','SteamPipeBoxSpread4',92,1,1,'in',32,'out',64,'voices',4,'mpan',100,'vis',300,'vispan',200,'line',1.)),(.01,message('/s_new','PanProbe',93,1,1)),(1.,message('/n_setn',91,'offset',16,-.4,-.1,-.2,.4,*([0.]*12))), (2.,message('/n_free',91)),(2.,message('/c_setn',100,16,*([0.]*16))),(2.,message('/n_set',92,'line',0.)),(2.,message('/n_setn',92,'pan',16,-.2,.25,.8,1.2,*([.5]*12)))]
# Insert LFO/route before the panner. Its four distinct phase indices must
# appear in the corresponding four telemetry channels, while bases stay .5.
events += [(3.,message('/n_set',92,'line',1.)),(3.,message('/n_setn',92,'pan',16,*([.5]*16))),(3.,message('/s_new','ModLfo4',94,2,92,'out',150)),(3.,message('/n_setn',94,'freq',4,1.,1.,1.,1.,'phoff',4,0.,.25,.5,.75)),(3.,message('/s_new','ModRoute4',95,3,94,'src',150,'out',100,'bipolar',1.,'amount',.5,'scale',1.)),(4.99,message('/g_freeAll',1)),(5.,message('/c_set',0,0.))]
score=out/'test.osc';score.write_bytes(b''.join(bundle(t,[m])for t,m in sorted(events,key=lambda e:e[0])))
r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(score),'_',str(out/'test.wav'),'48000','WAV','float','-o','12','-i','0','-a','256','-c','512','-D','0','-m','65536','-U','/Applications/SuperCollider.app/Contents/Resources/plugins'],capture_output=True,text=True,timeout=30);assert r.returncode==0 and 'FAILURE' not in r.stdout+r.stderr,r.stdout+r.stderr
raw,ch,sr=read_wav(out/'test.wav');audio=np.asarray(raw).reshape(-1,ch);assert np.isfinite(audio).all()
def row(t):return np.median(audio[int((t-.02)*sr):int((t+.02)*sr)],axis=0)
def weights(pans,ring):
 result=np.zeros(4)
 for level,pan in zip([1,2,3,4],pans):
  pos=pan*(4 if ring else 3)
  for speaker in range(4):
   d=abs(pos-speaker);d=min(d,4-d)if ring else d
   result[speaker]+=level*max(0,1-d)
 return result
for t,pans,ring in [(.5,[.1,.4,.6,.9],False),(1.5,[.1,.4,.3,.9],False),(2.5,[.8,.25,.8,.2],True)]:
 r=row(t);assert np.max(abs(r[4:8]-pans))<1e-5,(t,r)
 assert np.max(abs(r[:4]-weights(pans,ring)))<1e-4,(t,r[:4],weights(pans,ring))
a=audio[int(3.1*sr):int(4.9*sr)];assert np.max(abs(a[:,4:8]-a[:,8:12]))<1e-5,'Pan index differs from its LFO index'
assert np.min(np.ptp(a[:,4:8],axis=0))>.9,'Every voice must move'
assert np.max(abs(a[:,4]-a[:,5]))>.4,'Independent phase indices were collapsed'
print('PASS: production speaker gains and live Pan telemetry, index-only edits, ring wrapping and all four independently phased LFO indices reaching their own voice.')
