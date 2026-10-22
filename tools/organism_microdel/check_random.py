#!/usr/bin/env python3
"""Check sample/event/continuous-seed contracts using independent integer vectors."""
import json,pathlib,subprocess,sys
ROOT=pathlib.Path(__file__).resolve().parents[2];work=pathlib.Path(sys.argv[1]).resolve()
sys.path.insert(0,str(ROOT/'tools/aliasingbox'));from render_test import message,bundle,read_wav
script=work/'random_contract.scd'
script.write_text('''SynthDef(\\randomContract, { Out.ar(0, [CongruentialRandomKernel.ar(DC.ar(1),5,0,0), CongruentialRandomKernel.ar(0,5,0,1), CongruentialRandomKernel.ar(0,10,0,2)]) }).writeDefFile(thisProcess.argv[0]); 0.exit;''')
r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l',str(work/'sclang_conf.yaml'),str(script),str(work)],capture_output=True,text=True,timeout=60)
assert (work/'randomContract.scsyndef').exists(),r.stdout+r.stderr
results=[]
for block in [1,64]:
 osc=work/f'random-contract-{block}.osc';wav=osc.with_suffix('.wav')
 osc.write_bytes(bundle(0,[message('/d_load',str(work/'randomContract.scsyndef'))])+bundle(.01,[message('/s_new','randomContract',1000,0,0)])+bundle(.03,[message('/g_freeAll',0)])+bundle(.04,[message('/c_set',0,0)]))
 plugins='/Applications/SuperCollider.app/Contents/Resources/plugins:'+str(ROOT/'ugens/OceanodePrimitives')
 r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(osc),'_',str(wav),'48000','WAV','float','-o','3','-i','0','-D','0','-z',str(block),'-U',plugins],capture_output=True,text=True,timeout=60)
 assert r.returncode==0 and 'FAILURE' not in r.stdout+r.stderr,r.stdout+r.stderr
 a,ch,sr=read_wav(wav);start=next(i for i in range(len(a)//3) if a[i*3])
 # Reference sequence from the ensemble's signed integer arithmetic, seed=5.
 known=[0.56933804866358,-0.74192355902867,0.3240063499644,0.97284510050919,-0.68894719868854,-0.20762016580623]
 for i,x in enumerate(known):assert abs(a[(start+i)*3+1]-x)<1e-7,(block,i,a[(start+i)*3+1],x)
 for i in range(start,start+256):
  assert abs(a[i*3]-known[0])<1e-7,'Held-high trigger advanced more than once'
  # One update from seed=10; repeating this seed must give a constant output.
  assert abs(a[i*3+2]-(-0.86136707209731))<1e-7,(block,a[i*3+2])
 results.append({'block_size':block,'checked_frames':256,'known_vector_samples':len(known),'passed':True})
(work/'random-contract-validation.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results,indent=2))
