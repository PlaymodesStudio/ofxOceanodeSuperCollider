#!/usr/bin/env python3
"""Sample-for-sample audio comparison of old/new SpaceSystem SynthDef names."""
import json,pathlib,subprocess,sys
ROOT=pathlib.Path(__file__).resolve().parents[2];work=pathlib.Path(sys.argv[1]).resolve()
sys.path.insert(0,str(ROOT/'tools/aliasingbox'));from render_test import message,bundle,read_wav
script=work/'rename_probe.scd'
script.write_text('''SynthDef(\\renameInput, { Out.ar(32, SinOsc.ar([71,217,983,3011])*6) }).writeDefFile(thisProcess.argv[0]);
SynthDef(\\renameTap, { Out.ar(0, In.ar(40,4) ++ In.ar(44,4)) }).writeDefFile(thisProcess.argv[0]); 0.exit;
''')
r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l',str(work/'sclang_conf.yaml'),str(script),str(work)],capture_output=True,text=True,timeout=60)
assert (work/'renameInput.scsyndef').exists() and (work/'renameTap.scsyndef').exists(),r.stdout+r.stderr
report=[]
for old,new in [('ReaktorFilter4','LadderMultimode'),('ReaktorSaturator','ParabolicSaturator')]:
 defs=[work/'renameInput.scsyndef',work/'renameTap.scsyndef',work/'defs'/old/(old+'4.scsyndef'),work/'defs'/new/(new+'4.scsyndef')]
 setup=[message('/g_new',1,0,0),message('/s_new','renameInput',1000,1,1)]
 for i,name in enumerate([old,new]):
  setup.append(message('/s_new',name+'4',1001+i,1,1,'in',32,'out',40+4*i))
  if 'Filter' in old:
   for key,values in [('pitch',[40.,72.,96.,120.]),('res',[0.,.2,.7,.95]),('type',[0.,1.,2.,5.]),('drywet',[0.,.3,.6,1.])]:setup.append(message('/n_setn',1001+i,key,4,*values))
 setup.append(message('/s_new','renameTap',1003,1,1))
 osc=work/(new+'-compare.osc');wav=osc.with_suffix('.wav')
 osc.write_bytes(bundle(0,[message('/d_load',str(p)) for p in defs])+bundle(.01,setup)+bundle(2.,[message('/g_freeAll',1)])+bundle(2.01,[message('/c_set',0,0)]))
 plugins='/Applications/SuperCollider.app/Contents/Resources/plugins:'+str(ROOT/'ugens/OceanodeAnalog')
 r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(osc),'_',str(wav),'48000','WAV','float','-o','8','-i','0','-D','0','-U',plugins],capture_output=True,text=True,timeout=60)
 assert r.returncode==0 and 'FAILURE' not in r.stdout+r.stderr,r.stdout+r.stderr
 audio,ch,sr=read_wav(wav);assert ch==8
 for c in range(4):assert audio[c::8]==audio[c+4::8],(old,new,c)
 report.append(dict(old=old,new=new,channels=4,samples_per_channel=len(audio)//8,max_difference=0))
(work/'rename-audio-validation.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
