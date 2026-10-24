#!/usr/bin/env python3
"""Validate and offline-render the staged BIENAL FMBox migration (no audio device)."""
import argparse, importlib.util, json, math, pathlib, subprocess, sys
import numpy as np
ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
spec=importlib.util.spec_from_file_location('migration',ROOT/'tools/migrate_superfm_to_fmbox.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('stage',type=pathlib.Path);parser.add_argument('--data',type=pathlib.Path,required=True)
parser.add_argument('--originals',type=pathlib.Path,help='Migration backup after applying; unchanged source nodes still come from --data')
parser.add_argument('--report',type=pathlib.Path,help='Backup report.json when testing installed patches')
args=parser.parse_args()
originals=args.originals or args.data
report=json.loads((args.report or args.stage/'migration-report.json').read_text())['converted']
work=pathlib.Path('/tmp/bienal-fmbox-render');work.mkdir(exist_ok=True)
events=[]
def event(t,*a):events.append((t,message(*a)))
for f in [ROOT/'synthdefs/Defaults/FMBox/CompiledSynthdefs/FMBox'/n for n in ('FMBoxVoice.scsyndef','FMBoxDX7LFO.scsyndef')]:event(0,'/d_recv',f.read_bytes())
event(0,'/d_recv',(ROOT/'synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation/ModGate1.scsyndef').read_bytes())
for name in ('SuperFMDrone','SuperFM7'):
 path=args.data/'Supercollider/Synthdefs/Defaults/KR'/name/(name+'1.scsyndef')
 event(0,'/d_recv',path.read_bytes())
event(0,'/g_new',1,0,0)
globals_,ops=m.native_fields();segments=[]
for i,item in enumerate(report):
 t=i*2+.01;state=json.loads((args.stage/item['folder']/(item['new']+'.json')).read_text());s=state['fmBox']
 # A saved drone is intentionally muted by its live Amp input. Exercise that
 # input open for the render; leave the staged patch and snapshot unchanged.
 for source in range(14,22):
  if s['fmModMatrix'][source*23+3]>0 and s[f'Ext_{source-13}'][0]==0:
   s[f'Ext_{source-13}']=[1.]
 values={sc:s[m.esc(name)] for name,sc,_,_ in globals_}
 values.update(env_model=s['Envelope_Model'],env_curve=s['Envelope_Curve'],matrix_model=s['Matrix_Model'])
 for op in range(6):
  for name,sc,_,_ in ops:values[f'op{op}_{sc}']=s[f'Op_{op+1}_'+m.esc(name)]
  values[f'mix{op}']=s[f'Op_{op+1}_Mix']
  for k in range(4):values[f'op{op}_seconds{k}']=s.get(f'Op_{op+1}_Seconds_{k+1}',.1)
  for source in range(6):values[f'fm{op}_{source}']=s[m.esc(f'FM {source+1} > {op+1}')]
 for name,sc in [('FM_Amount','mod_scale'),('Envelope_Time','env_time')]:values[sc]=s[name]
 for p in s['fmModParams']:
  op=int(p['key'].split('_')[1])-1
  values[f'mix{op}']=p['base'][0]+sum(s[f'Ext_{int(src)-13}'][0]*a*.5 for src,a in p['amounts'].items())
 target_values=[sum(s['fmModMatrix'][src*23+target]*s.get(f'Ext_{src-13}',[0])[0] for src in range(14,22)) for target in range(23)]
 scales=[24,4,7,1,1]+[16]*6+[7]*6+[99]*6
 for k,v in enumerate(target_values):event(t,'/c_set',100+k*16,v*scales[k])
 event(t,'/s_new','ModGate1',10,1,1,'out',32,'vel',0,'rnd',16,'gate',1.,'velocity',1.)
 flat=lambda d:sum(([k,float(v)] for k,v in d.items()),[])
 event(t,'/s_new','FMBoxDX7LFO',11,1,1,'gatebus',32,'out',200,*flat({k:v for k,v in values.items() if k.startswith('lfo_')}))
 buses=sum(([name,100+k*16] for k,name in enumerate(['mpitch','mscale','mfeedback','mamp','mpan']+[f'mratio{x}'for x in range(6)]+[f'mdetune{x}'for x in range(6)]+[f'mlevel{x}'for x in range(6)])),[])
 event(t,'/s_new','FMBoxVoice',12,1,1,'gatebus',32,'velbus',0,'nativebus',200,'out',1,'vis',1024,
       'pitch',m.vec(state['Pitch'])[0],'master',float(state['Master'])*item['gain_compensation'],
       'normalization',1/item['voices'],*buses,*flat(values))
 # Original production SynthDef, same first voice and effective live inputs.
 original=json.loads((originals/item['folder']/(item['old']+'.json')).read_text())
 old=dict(original)
 connections=json.loads((originals/item['folder']/'connections.json').read_text())
 for source,ports in connections.items():
  for port,targets in ports.items():
   for name in targets.get(item['old'],{}):
    old[m.esc(name)]=m.source_values(args.data/item['folder'],source,port,m.vec(old.get(m.esc(name)),1))
 drone='Drone' in item['old']
 amp=m.one(old,'Amp',1)
 if amp==0:amp=1. # same live control exercised open in both versions
 scale=m.one(old,'Scale',1)
 if m.one(old,'Amp',1)==0 and any('Scale' in targets.get(item['old'],{}) and 'Amp' in targets.get(item['old'],{}) for ports in connections.values() for targets in ports.values()):scale=1.
 event(t,'/s_new','SuperFMDrone1' if drone else 'SuperFM71',13,1,1,'out',0,'pitch',m.vec(state['Pitch'])[0],'amp',amp,'modScale',scale,'gate',1.,'velocity',1.)
 def setn(name,values):event(t,'/n_setn',13,name,len(values),*values)
 setn('mod_matrix',m.vec(old['Matrix']))
 event(t,'/n_set',13,'feedback',m.one(old,'Feedback',1))
 if drone:
  for op in range(6):
   for port,key,default in [('op_amp','Op_Amp',int(op==0)),('op_ratio','Op_Ratio',1),('op_detune','Op_Detune',0)]:
    event(t,'/n_set',13,f'{port}_{op+1}',m.one(old,f'{key}_{op+1}',default))
 else:
  for port,key,default in [('op_amps','Op_Amps',0),('op_ratios','Op_Ratios',1),('op_detunes','Op_Detunes',0)]:
   v=m.vec(old.get(key),default);setn(port,(v+[default]*6)[:6])
  setn('eg_levels',m.vec(old['EgLevels']))
  times=m.vec(old['EgTimes']);duration=m.one(old,'Duration',2)
  setn('eg_rates',[1/(max(.001,times[op*4+k]-(times[op*4+k-1] if k else 0))*duration) for op in range(6) for k in range(4)])
 event(t+1.2,'/n_set',10,'gate',0.)
 event(t+1.2,'/n_set',13,'gate',0.)
 event(t+1.98,'/g_freeAll',1);segments.append((item['folder'],t+.05,t+1.1))
event(len(report)*2+.01,'/c_set',0,0.)
score=work/'score.osc';score.write_bytes(b''.join(bundle(t,[msg])for t,msg in sorted(events,key=lambda x:x[0])))
result=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(score),'_',str(work/'preview.wav'),'48000','WAV','float','-o','2','-i','0','-a','512','-c','4096','-w','4096','-D','0','-m','65536'],capture_output=True,text=True,timeout=45)
log=result.stdout+result.stderr;(work/'render.log').write_text(log)
assert result.returncode==0 and 'FAILURE' not in log and 'exception' not in log.lower(),log
samples,channels,rate=read_wav(work/'preview.wav');assert all(math.isfinite(v) for v in samples)
measurements=[]
for folder,start,stop in segments:
 v=samples[int(start*rate)*channels+1:int(stop*rate)*channels:channels]
 rms=math.sqrt(sum(x*x for x in v)/len(v));peak=max(abs(x) for x in v)
 assert rms>1e-5,(folder,'silent',rms)
 assert peak<12,(folder,'unexpected gain',peak)
 a=np.asarray(samples[int(start*rate)*channels:int(stop*rate)*channels:channels]);b=np.asarray(v)
 # Average FFT power in semitone-width bands, insensitive to chaotic FM phase.
 def spectrum(x):
  frames=np.array([x[k:k+4096] for k in range(0,len(x)-4096,1024)])
  power=np.mean(abs(np.fft.rfft(frames*np.hanning(4096)))**2,axis=0)
  hz=np.fft.rfftfreq(4096,1/rate);edges=np.geomspace(20,20000,73)
  return np.array([power[(hz>=lo)&(hz<hi)].sum() for lo,hi in zip(edges[:-1],edges[1:])])
 pa,pb=spectrum(a),spectrum(b)
 cosine=float(np.dot(pa,pb)/(np.linalg.norm(pa)*np.linalg.norm(pb)))
 level_db=float(20*np.log10(np.sqrt(np.mean(b*b))/np.sqrt(np.mean(a*a))))
 assert cosine>.9,(folder,'spectrum mismatch',cosine)
 item=next(item for item in report if item['folder']==folder)
 assert abs(level_db)<(.05 if item.get('render_gain_correction_db') else 3.1),(folder,'level mismatch',level_db)
 if 'Drone' not in item['old']:
  tail=np.asarray(samples[int((stop+.8)*rate)*channels+1:int((stop+.85)*rate)*channels:channels])
  assert np.sqrt(np.mean(tail*tail))<.001,(folder,'release did not reach silence')
 measurements.append({'folder':folder,'rms':rms,'peak':peak,'spectrum_cosine':cosine,'level_difference_db':level_db})
(work/'measurements.json').write_text(json.dumps(measurements,indent=2))
print('PASS:',len(report),'staged FMBox states render finite, audible output with their stored routing and gain compensation')
print('A/B (original left, FMBox right):',work/'preview.wav')
print(json.dumps(measurements,indent=2))
