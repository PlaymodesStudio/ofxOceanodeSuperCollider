#!/usr/bin/env python3
"""Measure dry pluck tails for Tarrega/Coolharp, without delay or reverb.

A fresh gate is sent to an existing voice, then closed after 100 ms. Tail RMS
and periodicity are measured after the steam envelope has substantially decayed.
"""
import argparse,importlib.util,json,math,pathlib
ROOT=pathlib.Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('render',pathlib.Path(__file__).with_name('render_test.py'));r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r)
m,b=r.message,r.bundle

def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',type=pathlib.Path,default=pathlib.Path('/tmp/steampipebox-resonance'));a=ap.parse_args();a.output.mkdir(parents=True,exist_ok=True)
 bank={x['name']:x for x in json.loads((r.FOLDER/'factory.json').read_text())};controls=json.loads((r.FOLDER/'controls.json').read_text());compiled=r.FOLDER/'CompiledSynthdefs/SteamPipeBox'
 mods=['mpitch','mtune','maptune','mnoise','mnoiseres','mdiffusion','mpush','moffset','mfeedback','mhp','mlp','mmw','mamp']
 results=[]
 for name in ['tarrega','coolharp']:
  params=sum(([c['sc'],float(bank[name]['values'][c['name']])] for c in controls),[])
  score=b(0,[*[m('/d_recv',(compiled/(n+'.scsyndef')).read_bytes()) for n in ['SteamPipeBoxVoice','SteamPipeBoxGate1']],m('/g_new',1,0,0),m('/c_setn',32,16,*([0.]*16))])
  score+=b(.016,[m('/s_new','SteamPipeBoxGate1',1000,0,1,'out',16,'velout',48,'gate',0.,'velocity',1.,'vel',0,'rnd',1),m('/s_new','SteamPipeBoxVoice',1001,1,1,'gatebus',16,'velabus',48,'out',0,'vis',2048,'pitch',60.,*params,*sum(([n,32] for n in mods),[]))])
  score+=b(.032,[m('/n_set',1000,'gate',1.)])+b(.132,[m('/n_set',1000,'gate',0.)])+b(4.032,[m('/g_freeAll',1)])
  x,ch,sr=r.render('/Applications/SuperCollider.app/Contents/Resources/scsynth',a.output/(name+'.wav'),score,channels=1)
  assert all(math.isfinite(v) for v in x)
  def rms(t0,t1):
   y=x[round((.032+t0)*sr):round((.032+t1)*sr)];return math.sqrt(sum(v*v for v in y)/len(y))
  tail_rms=rms(.5,.7);assert tail_rms>1e-4,(name,'resonance died with excitation',tail_rms)
  # A normalized autocorrelation peak at the fundamental, measured well after
  # excitation; short broadband noise bursts fail both this and the tail test.
  window=x[round(.532*sr):round(.532*sr)+4096];best=(-1,0)
  for lag in range(round(sr/200),round(sr/90)):
   left=window[:-lag];right=window[lag:];energy=math.sqrt(sum(v*v for v in left)*sum(v*v for v in right))
   corr=sum(l*q for l,q in zip(left,right))/max(1e-30,energy)
   if corr>best[0]:best=(corr,lag)
  assert best[0]>.8,(name,'tail is not tonal',best)
  results.append(dict(name=name,pitch=60,gate_seconds=.1,dry_peak=max(map(abs,x)),tail_rms_500_700ms=tail_rms,tail_rms_1000_1200ms=rms(1.,1.2),periodicity=best[0],fundamental_hz=sr/best[1]))
 (a.output/'results.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results,indent=2))
if __name__=='__main__':main()
