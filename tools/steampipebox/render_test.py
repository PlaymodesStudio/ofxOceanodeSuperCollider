#!/usr/bin/env python3
"""NRT validation of all imported SteamPipe snapshots, both native kernels and block sizes.

Compile steampipeBox.scd first. No sound device or running server is used.
"""
import argparse,array,importlib.util,json,math,pathlib,subprocess,sys
ROOT=pathlib.Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('osc_helpers',ROOT/'tools/aliasingbox/render_test.py');osc=importlib.util.module_from_spec(spec);spec.loader.exec_module(osc)
message,bundle=osc.message,osc.bundle
FOLDER=ROOT/'synthdefs/Defaults/SteamPipeBox'
def read_float_wav(path):
 import struct
 b=path.read_bytes();pos=12
 while pos<len(b)-8:
  tag,size=struct.unpack_from('<4sI',b,pos);pos+=8
  if tag==b'fmt ':fmt,channels,rate=struct.unpack_from('<HHI',b,pos)
  if tag==b'data':
   assert fmt==3
   a=array.array('f');a.frombytes(b[pos:pos+size]);return a,channels,rate
  pos+=size+(size%2)
 raise AssertionError('No audio data')
def render(server,out,score,block=64,channels=2):
 out.parent.mkdir(parents=True,exist_ok=True);out.with_suffix('.osc').write_bytes(score)
 p=subprocess.run([server,'-N',str(out.with_suffix('.osc')),'_',str(out),'48000','WAV','float','-o',str(channels),'-i','0','-z',str(block),'-U','/Applications/SuperCollider.app/Contents/Resources/plugins:'+str(ROOT/'ugens/OceanodeSteamPipe'),'-D','0','-w','8192','-m','524288'],capture_output=True,text=True)
 log=p.stdout+p.stderr;out.with_suffix('.log').write_text(log)
 assert p.returncode==0 and not any(x in log for x in ['FAILURE','ERROR','failed','not found','exception']),(p.returncode,log[-3000:])
 return read_float_wav(out)
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--server',default='/Applications/SuperCollider.app/Contents/Resources/scsynth');ap.add_argument('--output',type=pathlib.Path,default=pathlib.Path('/tmp/steampipebox-render'));a=ap.parse_args();a.output.mkdir(parents=True,exist_ok=True)
 controls=json.loads((FOLDER/'controls.json').read_text());bank=json.loads((FOLDER/'factory.json').read_text());compiled=FOLDER/'CompiledSynthdefs'
 paths=[compiled/'SteamPipeBox/SteamPipeBoxVoice.scsyndef',compiled/'SteamPipeBox/SteamPipeBoxOriginal2.scsyndef',compiled/'SteamPipeBox/SteamPipeBoxGate1.scsyndef']
 init=[*[message('/d_recv',p.read_bytes()) for p in paths],message('/g_new',1,0,0),message('/c_setn',32,16,*([0.0]*16))]
 score=bundle(0,init)
 mods=['mpitch','mtune','maptune','mnoise','mnoiseres','mdiffusion','mpush','moffset','mfeedback','mhp','mlp','mmw','mamp']
 def notes(preset,t):
  params=[]
  for c in controls:params +=[c['sc'],float(preset['values'][c['name']])]
  return bundle(t,[message('/s_new','SteamPipeBoxGate1',1000,0,1,'out',16,'velout',48,'gate',1.0,'velocity',1.0,'vel',0,'rnd',1),message('/s_new','SteamPipeBoxVoice',1001,1,1,'gatebus',16,'velabus',48,'pitch',60.0,'master',1.0,'normalization',1.0,'vis',2048,'out',18,*params,*sum(([name,32] for name in mods),[])),message('/s_new','SteamPipeBoxOriginal2',1002,1,1,'in',18,'out',0,*params)])
 for i,preset in enumerate(bank):
  t=i*1.6+.016;score+=notes(preset,t)
  score+=bundle(t+1.024,[message('/n_set',1000,'gate',0.0)])
  score+=bundle(t+1.568,[message('/n_free',1000,1001,1002)])
 score+=bundle(len(bank)*1.6,[message('/c_set',32,0.0)])
 audio,ch,rate=render(a.server,a.output/'factory.wav',score)
 measurements=[]
 for i,p in enumerate(bank):
  x=audio[round((i*1.6+.016)*rate)*ch:round((i*1.6+1.45)*rate)*ch]
  assert all(math.isfinite(v) for v in x),p['name']
  peak=max(map(abs,x));rms=math.sqrt(sum(v*v for v in x)/len(x))
  assert peak<20,(p['name'],peak)
  measurements.append(dict(name=p['name'],peak=peak,rms=rms))
 assert all(m['rms']>1e-5 for m in measurements),'Unexpected silent factory sounds'
 (a.output/'measurements.json').write_text(json.dumps(measurements,indent=2)+'\n')
 # Isolate the kernels from the box's control-rate gate and velocity wrapper.
 import re
 def ordered(path):
  return sorted(set(re.findall(r'(?<![A-Za-z0-9_])(_[A-Za-z0-9_]+)',re.sub(r'//[^\n]*','',path.read_text()))))
 values={c['sc']:bank[0]['values'][c['name']] for c in controls}
 values.update(pitch=60,gate=1,velocity=1,bpm=120,amp_original=1,amp_curve0=-4,amp_curve1=-4,amp_curve2=-4)
 def args(path):return ','.join('DC.ar('+str(values[p[1:]])+')' for p in ordered(path))
 source='Archive.archiveDir="/tmp";\nSynthDef(\\SteamPipeBoxTimingTest,{var sig=SteamPipeVoice.ar(['+args(FOLDER/'steamPipeBox.eel2')+']);Out.ar(0,SteamPipeOriginal.ar(['+args(FOLDER/'originalEffects.eel2')+'],sig[0],sig[0]));}).writeDefFile("'+str(a.output)+'");\n0.exit;\n'
 source=source.replace('0.exit;', 'SynthDef(\\SteamPipeBoxCableInput,{var edge=DelayN.ar(Impulse.ar(0),0.01,48/SampleRate.ir);Out.ar(200,Trig1.ar(edge,0.2)*0.25);Out.ar(201,DC.ar(0.8));}).writeDefFile("'+str(a.output)+'");\n0.exit;')
 (a.output/'timing.scd').write_text(source)
 config=a.output/'config.yaml';config.write_text('includePaths:\n    - '+str(ROOT/'sclang/SCClassLibrary')+'\n    - '+str(ROOT/'ugens/OceanodeSteamPipe')+'\nexcludePaths: []\nexcludeDefaultPaths: true\n')
 (a.output/'SteamPipeBoxTimingTest.scsyndef').unlink(missing_ok=True)
 compile=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-D','-l',str(config),str(a.output/'timing.scd')],capture_output=True,text=True,timeout=20)
 (a.output/'timing-compile.log').write_text(compile.stdout+compile.stderr)
 assert compile.returncode==0 and (a.output/'SteamPipeBoxTimingTest.scsyndef').exists(),compile.stdout
 timing=message('/d_recv',(a.output/'SteamPipeBoxTimingTest.scsyndef').read_bytes())
 results={}
 for block in [1,64,256]:
  score=bundle(0,[timing,message('/g_new',1,0,0)])+bundle(.016,[message('/s_new','SteamPipeBoxTimingTest',1000,0,1)])+bundle(1.04,[message('/n_free',1000)])+bundle(1.056,[message('/c_set',32,0.0)])
  x,ch,rate=render(a.server,a.output/f'block-{block}.wav',score,block);results[block]=x[:48000*ch]
 reference=results[1];diff={str(k):max(abs(x-y) for x,y in zip(reference,v)) for k,v in results.items()}
 assert max(diff.values())<1e-6,diff
 (a.output/'block-sizes.json').write_text(json.dumps(diff,indent=2)+'\n')
 # Production graph at minimum, default and maximum voice/channel counts.
 graph_results=[]
 for voices,channels in [(1,1),(6,2),(16,16)]:
  moddir=ROOT/'synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation'
  paths=[compiled/'SteamPipeBox/SteamPipeBoxVoice.scsyndef',compiled/f'SteamPipeBox/SteamPipeBoxGate{voices}.scsyndef',compiled/f'SteamPipeBox/SteamPipeBoxOriginal{channels}.scsyndef',*[moddir/f'{n}{voices}.scsyndef' for n in ['ModLfo','ModEnv','ModRoute']],compiled/f'SteamPipeBox/SteamPipeBoxSpread{channels}.scsyndef']
  start=[message('/d_recv',p.read_bytes()) for p in paths]+[message('/g_new',1,0,0),message('/c_setn',32,16,*([0.0]*16))]
  params=[]
  for c in controls:params +=[c['sc'],float(bank[0]['values'][c['name']])]
  nodes=[message('/s_new',f'SteamPipeBoxGate{voices}',1000,1,1,'out',128,'velout',160,'gate',1.0,'velocity',1.0,'vel',0,'rnd',16),message('/n_setn',1000,'gate',voices,*([1.0]*voices)),message('/s_new',f'ModLfo{voices}',1100,1,1,'out',512,'freq',3.0),message('/s_new',f'ModEnv{voices}',1101,1,1,'out',528,'gate',128,'atk',.01,'dec',.1,'sus',.8,'rel',.1),message('/s_new',f'ModRoute{voices}',1102,1,1,'src',512,'out',560,'amount',.5,'scale',24.0),message('/s_new',f'ModRoute{voices}',1103,1,1,'src',528,'out',576,'amount',1.0,'scale',1.0)]
  for i in range(voices):nodes.append(message('/s_new','SteamPipeBoxVoice',1001+i,1,1,'gatebus',128+i,'velabus',160+i,'pitch',60.0+i*.25,'normalization',1.0,'vis',2048,'voice',i,'out',64+i,*params,*sum(([name,32] for name in mods),[]),'mpitch',560+i,'mamp',576+i,'vca',1.0))
  nodes +=[message('/s_new',f'SteamPipeBoxSpread{channels}',1200,1,1,'in',64,'out',96,'voices',voices,'mpan',32,'vis',2200),message('/s_new',f'SteamPipeBoxOriginal{channels}',1201,1,1,'in',96,'out',0,*params)]
  score=bundle(0,start)+bundle(.016,nodes)+bundle(.528,[message('/n_setn',1000,'gate',voices,*([0.0]*voices))])+bundle(1.04,[message('/g_freeAll',1)])+bundle(1.056,[message('/c_set',32,0.0)])
  audio,ch,sr=render(a.server,a.output/f'graph-{voices}-{channels}.wav',score,channels=channels)
  assert all(math.isfinite(x) for x in audio)
  peak=max(map(abs,audio));assert 1e-5<peak<20,(voices,channels,peak)
  graph_results.append(dict(voices=voices,channels=channels,peak=peak))
 # Audio cable note begins 48 samples into a 64-sample block. Its velocity
 # must be available at that exact edge, including when Velocity Amount is 1.
 params=[]
 for c in controls:params +=[c['sc'],float(bank[0]['values'][c['name']])]
 start=init+[message('/d_recv',(a.output/'SteamPipeBoxCableInput.scsyndef').read_bytes())]
 nodes=[message('/s_new','SteamPipeBoxCableInput',900,1,1),message('/s_new','SteamPipeBoxGate1',1000,1,1,'out',16,'velout',48,'vel',0,'rnd',1,'gate_sel',1.0,'vel_sel',1.0),message('/n_mapa',1000,'gate_ar',200,'vel_ar',201),message('/s_new','SteamPipeBoxVoice',1001,1,1,'gatebus',16,'velabus',48,'out',0,'vis',2048,'normalization',1.0,*params,'velocity_amount',1.0,*sum(([name,32] for name in mods),[]))]
 score=bundle(0,start)+bundle(.016,nodes)+bundle(.8,[message('/g_freeAll',1)])+bundle(.816,[message('/c_set',32,0.0)])
 audio,ch,sr=render(a.server,a.output/'audio-cable.wav',score)
 assert all(math.isfinite(x) for x in audio)
 cable_peak=max(map(abs,audio));assert cable_peak>1e-5,'Sub-block gate lost its velocity'
 (a.output/'audio-cable.json').write_text(json.dumps(dict(peak=cable_peak,edge_offset_samples=48,block_size=64),indent=2)+'\n')
 (a.output/'graphs.json').write_text(json.dumps(graph_results,indent=2)+'\n')
 print(json.dumps(dict(snapshots=len(measurements),audible=sum(m['rms']>1e-5 for m in measurements),max_peak=max(m['peak'] for m in measurements),block_differences=diff,graphs=graph_results,audio_cable_peak=cable_peak),indent=2))
if __name__=='__main__':main()
