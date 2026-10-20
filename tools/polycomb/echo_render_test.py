#!/usr/bin/env python3
"""Exercise the production 1.7 echo: cutoff edits, resonance, feedback and decay."""
import argparse,json,pathlib,subprocess,sys,struct
import numpy as np
ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
ap=argparse.ArgumentParser();ap.add_argument('--source',type=pathlib.Path,default=ROOT/'synthdefs/Defaults/PolyComb/polyCombExtras.scd');ap.add_argument('--baseline',action='store_true');a=ap.parse_args()
out=pathlib.Path('/tmp/polycomb-echo-'+('baseline' if a.baseline else 'fixed'));out.mkdir(exist_ok=True)
source=a.source.read_text();start=source.index('(1..16).do { |c|',source.index('// PCEchoR<C>'));end=source.index('// PCCombR<N>',start)
compile_source=out/'compile.scd'
compile_source.write_text(f'Archive.archiveDir="{out}";\n'+source[start:end].replace('.writeDefFile(dir)',f'.writeDefFile("{out}")')+f'\nSynthDef(\\EchoTestInput,{{Out.ar(32,((SinOsc.ar(\\freq.kr(1000))*\\amp.kr(0.01))+(Impulse.ar(0)*\\imp.kr(0.01)))!16)}}).writeDefFile("{out}");\n0.exit;\n')
config=out/'config.yaml';config.write_text(f'includePaths:\n    - {ROOT}/sclang/SCClassLibrary\nexcludePaths: []\npostInlineWarnings: false\nexcludeDefaultPaths: true\n')
with (out/'compile.log').open('w') as log:
 r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l',str(config),str(compile_source)],stdout=log,stderr=subprocess.STDOUT,text=True,timeout=30)
assert r.returncode==0 and (out/'PCEchoR16.scsyndef').exists(),(out/'compile.log').read_text()
cases=[dict(name='Hi sweep, no feedback',res=0.,feed=0.,hi=6000.,changes=[(0.6,'hi',20000.)]),
       dict(name='Negative resonance, Hi sweep',res=-1.,feed=.8,hi=1000.,changes=[(.6,'hi',12000.),(1.,'hi',20000.),(1.4,'hi',20.)]),
       dict(name='Resonant feedback',res=.98,feed=.98,hi=1000.,freq=1000.,changes=[]),
       dict(name='Fast Hi/Lo/Res edits',res=0.,feed=.98,hi=6000.,delay=.005,spread=.33,changes=[(.2+i*.015,['hi','res','lo'][i%3],([20.,20000.][i%2] if i%3==0 else [-1.,.98][i%2] if i%3==1 else [1.,4000.][i%2]))for i in range(100)])]
reports=[]
for sr in ([48000]if a.baseline else [32000,44100,48000,96000]):
 for channels in ([2]if a.baseline else [1,2,16]):
  length=8.;events=[(0,message('/g_new',1,0,0)),(0,message('/d_recv',(out/f'PCEchoR{channels}.scsyndef').read_bytes())),(0,message('/d_recv',(out/'EchoTestInput.scsyndef').read_bytes()))]
  for i,c in enumerate(cases):
   t=i*length+.01
   events += [(t,message('/s_new','EchoTestInput',100,1,1,'amp',.01,'freq',c.get('freq',1000.),'imp',.01)),(t,message('/s_new',f'PCEchoR{channels}',101,1,1,'in',32,'out',0,'hi',c['hi'],'lo',40.,'res',c['res'],'feed',c['feed'],'delay',c.get('delay',.05),'spread',c.get('spread',0.),'mix',1.,'on',1.))]
   for dt,key,value in c['changes']:events.append((t+dt,message('/n_set',101,key,value)))
   events += [(t+2.,message('/n_set',100,'amp',0.)),((i+1)*length-.01,message('/g_freeAll',1))]
  events.append((len(cases)*length,message('/c_set',0,0.)))
  osc=out/f'{sr}-{channels}.osc';osc.write_bytes(b''.join(bundle(t,[m])for t,m in sorted(events,key=lambda e:e[0])))
  wav=osc.with_suffix('.wav')
  r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(osc),'_',str(wav),str(sr),'WAV','float','-o',str(channels),'-i','0','-a','256','-c','128','-z','64','-m','65536','-D','0'],capture_output=True,text=True,timeout=60)
  assert r.returncode==0 and 'FAILURE'not in r.stdout+r.stderr,r.stdout+r.stderr
  with wav.open('rb') as f:
   f.seek(12)
   while True:
    tag,size=struct.unpack('<4sI',f.read(8));pos=f.tell()
    if tag==b'fmt ':fmt,ch,rate=struct.unpack('<HHI',f.read(8));assert fmt==3
    if tag==b'data':offset=pos;count=size//4;break
    f.seek(pos+size+(size%2))
  audio=np.memmap(wav,dtype='<f4',mode='r',offset=offset,shape=(count,)).reshape(-1,ch)
  for i,c in enumerate(cases):
   sig=audio[int(i*length*sr):int((i+1)*length*sr)]
   peak=float(np.max(np.abs(sig)));finite=bool(np.isfinite(sig).all())
   early=float(np.max(np.abs(sig[int(2.5*sr):int(3.5*sr)])))
   tail=float(np.max(np.abs(sig[int(6*sr):int(7*sr)])))
   reports.append(dict(sr=sr,channels=channels,case=c['name'],peak=peak,finite=finite,early_tail=early,late_tail=tail))
   if not a.baseline:
    assert finite and peak<1.,reports[-1]
    assert tail<max(1e-6,early*.8),reports[-1]
  print(f'{sr} Hz / {channels} channels:',[(c["case"],round(c["peak"],5))for c in reports[-4:]])
(out/'measurements.json').write_text(json.dumps(reports,indent=2)+'\n')
if a.baseline:assert any(not r['finite'] or r['peak']>1 for r in reports), 'Expected the old echo to reproduce the runaway'
if not a.baseline:print('PASS: Hi/Lo/Res edits, max resonance/feedback, 5ms delay, four sample rates and 1/2/16 channels remain bounded and decay after input stops.')
