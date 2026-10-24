#!/usr/bin/env python3
"""Regression for manual gate controls on existing voices after snapshot recall.

Records gate/velocity on the first sample of each note, not only their block peak.
Also checks the shared velocity source and envelope tracking, pitch steps, repeated Tarrega/PVC notes and shorter gate pulses.
"""
import argparse,importlib.util,json,math,pathlib,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('render',pathlib.Path(__file__).with_name('render_test.py'))
r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r)
m,b=r.message,r.bundle

def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',type=pathlib.Path,default=pathlib.Path('/tmp/steampipebox-retrigger'));a=ap.parse_args();a.output.mkdir(parents=True,exist_ok=True)
 controls=json.loads((r.FOLDER/'controls.json').read_text());bank=json.loads((r.FOLDER/'factory.json').read_text());byname={x['name']:x for x in bank};compiled=r.FOLDER/'CompiledSynthdefs/SteamPipeBox'
 source='''Archive.archiveDir="/tmp";
SynthDef(\\SteamPipeRecallProbe,{
 Out.ar(2,[In.ar(18),In.ar(16),In.ar(48),SteamPipeStep.ar(In.kr(2050)),SteamPipeStep.ar(In.kr(2048)),SteamPipeStep.ar(In.kr(0)),In.ar(49)]);
}).writeDefFile("'''+str(a.output)+'''");
0.exit;
'''
 (a.output/'probe.scd').write_text(source)
 config=a.output/'config.yaml';config.write_text('includePaths:\n    - '+str(ROOT/'sclang/SCClassLibrary')+'\n    - '+str(ROOT/'ugens/OceanodeSteamPipe')+'\nexcludePaths: []\nexcludeDefaultPaths: true\n')
 result=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-D','-l',str(config),str(a.output/'probe.scd')],capture_output=True,text=True,timeout=20)
 (a.output/'compile.log').write_text(result.stdout+result.stderr);assert result.returncode==0 and (a.output/'SteamPipeRecallProbe.scsyndef').exists()
 mods=['mpitch','mtune','maptune','mnoise','mnoiseres','mdiffusion','mpush','moffset','mfeedback','mhp','mlp','mmw','mamp']
 def params(p):return sum(([c['sc'],float(p['values'][c['name']])] for c in controls),[])
 init=[m('/d_recv',(ROOT/'synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation/ModEnvAr1.scsyndef').read_bytes()),*[m('/d_recv',(compiled/(n+'.scsyndef')).read_bytes()) for n in ['SteamPipeBoxVoice','SteamPipeBoxSpread2','SteamPipeBoxGate1']],m('/d_recv',(a.output/'SteamPipeRecallProbe.scsyndef').read_bytes()),m('/g_new',1,0,0),m('/c_setn',32,16,*([0.]*16))]
 results=[]
 for block in [1,64,256]:
  score=b(0,init)+b(.016,[m('/s_new','SteamPipeBoxGate1',1000,0,1,'out',16,'velout',48,'gate',0.,'velocity',1.,'vel',0,'rnd',1),m('/s_new','SteamPipeBoxVoice',1001,1,1,'gatebus',16,'velabus',48,'out',18,'vis',2048,*params(bank[0]),*sum(([name,32] for name in mods),[])),m('/s_new','SteamPipeBoxSpread2',1002,1,1,'in',18,'out',0,'voices',1,'mpan',32,'vis',2200,'vispan',2220),m('/s_new','ModEnvAr1',1004,1,1,'gate',16,'velbus',0,'veltrack',1.,'atk',.001,'dec',.001,'sus',1.,'rel',.05,'out',2,'aout',49),m('/s_new','SteamPipeRecallProbe',1003,1,1)])
  cases=[];now=.032
  for preset in [bank[0],byname['tarrega'],byname['pvc'],byname['tarrega'],bank[0],byname['pvc']]:
   score+=b(now,[m('/n_set',1001,*params(preset))]);now+=.016
   for pitch in [36,60,84]:
    for gate,vel,length in [(1,1,.128),(1,.5,.128),(.25,1,.128),(1,1,.016)]:
     score+=b(now,[m('/n_set',1001,'pitch',float(pitch)),m('/n_set',1000,'gate',float(gate),'velocity',float(vel))]);score+=b(now+length,[m('/n_set',1000,'gate',0.)]);cases.append(dict(name=preset['name'],pitch=pitch,gate=gate,velocity=vel,length=length,start=now));now+=.512
  # A mouse drag generates a series of positive gate values without an off.
  # It must not keep the noise cutoff at the first tiny slider value.
  drags=[]
  for name in ['tarrega','pvc']:
   preset=byname[name]
   score+=b(now,[m('/n_set',1001,*params(preset),'pitch',60.),m('/n_set',1000,'gate',0.,'velocity',1.)]);now+=.016
   for level in [.015625,.03125,.0625,.125,.25,.5,.75,1.]:
    score+=b(now,[m('/n_set',1000,'gate',level)]);now+=.016
   drags.append(dict(name=name,start=now-.016))
   score+=b(now+.128,[m('/n_set',1000,'gate',0.)]);now+=.512
  score+=b(now,[m('/g_freeAll',1)])
  x,ch,sr=r.render('/Applications/SuperCollider.app/Contents/Resources/scsynth',a.output/f'block-{block}.wav',score,block=block,channels=9)
  assert all(math.isfinite(v) for v in x)
  for c in cases:
   # Include the block containing the event, but exclude the previous note.
   first=max(0,round(c['start']*sr)-block);last=round((c['start']+.48)*sr)
   edge=next((i for i in range(first,last) if x[i*ch+3]>0),None);assert edge is not None,(block,c,'no gate')
   gate=x[edge*ch+3];velocity=x[edge*ch+4]
   assert abs(gate-c['gate'])<1e-6,(block,c,'ramped gate',gate)
   assert abs(velocity-c['gate']*c['velocity'])<1e-6,(block,c,'wrong onset velocity',velocity)
   # Allow the control bus conversion and fast ADSR to settle, then check
   # that both the routable Velocity source and shared envelope retain dynamics.
   settled=edge+round(.008*sr)
   for channel,label in [(7,'velocity source'),(8,'envelope velocity tracking')]:
    assert abs(x[settled*ch+channel]-c['gate']*c['velocity'])<1e-5,(block,c,label,x[settled*ch+channel])
   hz=440*2**((c['pitch']-69)/12);assert abs(x[edge*ch+6]-hz)<.002,(block,c,'ramped pitch',x[edge*ch+6],hz)
   dry=max(abs(x[i*ch+2]) for i in range(edge,last));peak=max(abs(x[i*ch+k]) for i in range(edge,last) for k in [0,1])
   if c['name'] in ['tarrega','pvc'] and c['gate']==1 and c['velocity']==1 and c['length']>.01:
    assert dry>.02,(block,c,'lost noise excitation',dry)
   c.update(block=block,first_velocity=velocity,dry_peak=dry,output_peak=peak);results.append(c)
  for c in drags:
   start=round(c['start']*sr);last=round((c['start']+.128)*sr)
   dry=max(abs(x[i*ch+2]) for i in range(start,last))
   assert dry>.02,(block,c,'slider drag retained tiny velocity',dry)
   if c['name']=='pvc':
    envelope=max(x[i*ch+5] for i in range(start,last));assert envelope>.8,(block,c,'slider drag retained tiny envelope peak',envelope)
   results.append(dict(name=c['name'],block=block,manual_drag=True,dry_peak=dry))
 (a.output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
 print(json.dumps({'notes_checked':sum(not x.get('manual_drag',False) for x in results),'slider_drags_checked':sum(x.get('manual_drag',False) for x in results),'blocks':[1,64,256],'onset_velocity':'exact','modulation_velocity_and_envelope_tracking':'passed','pitch_steps':'passed','tarrega_and_pvc_full_velocity_notes':'audible'},indent=2))
if __name__=='__main__':main()
