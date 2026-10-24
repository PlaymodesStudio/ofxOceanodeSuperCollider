#!/usr/bin/env python3
"""Render production Pulsar/Grain voice gains at the start and across midpoint.

Only iteration ranges are reduced for compilation; DSP function bodies are
unchanged; Grain voices add an unscaled output tap for comparison. All generated definitions and audio remain under --output.
"""
import argparse,importlib.util,json,math,struct,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('osc',ROOT/'tools/aliasingbox/render_test.py')
osc=importlib.util.module_from_spec(spec);spec.loader.exec_module(osc)
def wav(path,values):
 data=struct.pack('<'+'f'*len(values),*values)
 path.write_bytes(b'RIFF'+struct.pack('<I',36+len(data))+b'WAVEfmt '+struct.pack('<IHHIIHH',16,3,1,48000,192000,4,32)+b'data'+struct.pack('<I',len(data))+data)
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,default=Path('/tmp/box-bank-audio'));out=parser.parse_args().output;out.mkdir(parents=True,exist_ok=True)
 config=out/'sclang.yaml';config.write_text('includePaths:\n  - '+json.dumps(str(ROOT/'sclang/SCClassLibrary'))+'\nexcludeDefaultPaths: true\n')
 paths=[]
 for box,name in [('PulsarBox','pulsarbox.scd'),('GrainBox','scGrainBoxPoly.scd')]:
  s=(ROOT/'synthdefs/Defaults'/box/name).read_text()
  s=s.replace('maxVoices = 8, maxOutputs = 16, layers = 4','maxVoices = 1, maxOutputs = 1, layers = 4')
  s=s.replace('caps = [8, 32, 128, 500]','caps = [8]').replace('(1..16).do','(1..1).do').replace('(1..2).do','(1..1).do')
  if box=='GrainBox':
   # Compare each random grain stream against itself before the gain. Different
   # voices intentionally have independent random draws, even with zero jitter.
   cursor=0
   while True:
    start=s.find('Out.ar(out,',cursor)
    if start<0:break
    i=start+len('Out.ar(');depth=1
    while depth:
     depth+=(s[i]=='(')-(s[i]==')');i+=1
    body=s[start+len('Out.ar(out,'):i-1]
    marker='* '+chr(92)+'morphgain.kr(1,0.03)'
    if marker in body:
     raw=body.replace(marker,'')
     replacement='{ var raw='+raw+'; Out.ar('+chr(92)+'testref.kr(16),raw); Out.ar(out,raw '+marker+'); }.value'
     s=s[:start]+replacement+s[i:];cursor=start+len(replacement)
    else:cursor=i
  path=out/name;path.write_text(s);paths.append(path)
 wrapper=out/'compile.scd';wrapper.write_text('Archive.archiveDir='+json.dumps(str(out))+';"BANK DSP START".postln;try {\n'+''.join('this.executeFile('+json.dumps(str(p))+');\n' for p in paths)+'"BANK DSP DONE".postln;0.exit;} { |error| error.reportError;1.exit; };\n')
 with (out/'compile.log').open('w') as log:
  compiled=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-D','-l',str(config),str(wrapper)],stdout=log,stderr=subprocess.STDOUT,timeout=90)
 log=(out/'compile.log').read_text().split('BANK DSP START')[-1]
 assert compiled.returncode==0 and 'ERROR:'not in log and 'BANK DSP DONE'in log,log[-6000:]
 wav(out/'sine.wav',[math.sin(2*math.pi*i/2048) for i in range(2048)])
 wav(out/'hann.wav',[.5-.5*math.cos(2*math.pi*i/2047) for i in range(2048)])
 wav(out/'sample.wav',[.1*math.sin(2*math.pi*220*i/48000) for i in range(48000)])
 gains=[1.,0.,.001,.499,.501]
 for kind in ('Pulsar','Grain'):
  events=[]
  def event(t,address,*args):events.append((t,osc.message(address,*args)))
  for path in (out/'CompiledSynthdefs'/('PulsarBox'if kind=='Pulsar'else'GrainBoxPoly')).glob('*.scsyndef'):event(0,'/d_recv',path.read_bytes())
  event(0,'/g_new',1,0,0)
  for i,name in enumerate(['sine','hann','sample']):event(0,'/b_allocRead',i,str(out/(name+'.wav')))
  if kind=='Grain':event(.02,'/s_new','GrainBoxPolyCtl_8',10,1,1,'ctl',300,'clk',32,'modhdr',600,'envbuf',1,'bufnum0',2,'bufnum1',2,'autotrig',1,'bpm',120,'duration',.1,'position',.1,'autotrigbeatdiv',20,'vis',0)
  for i,gain in enumerate(gains):
   if kind=='Pulsar':
    event(.03,'/s_new','PulsarBox_1_1_1',100+i,1,1,'out',i,'pbuf',0,'ebuf',1,'gated',0,'lev',.15,'morphgain',gain)
   else:event(.03,'/s_new','GrainBoxPolyV_1_1',100+i,1,1,'out',i,'ctl',300,'clk',32,'morphgain',gain,'testref',16+i)
  event(1.,'/g_freeAll',1);event(1.01,'/c_set',0,0.)
  score=out/(kind+'.osc');score.write_bytes(b''.join(osc.bundle(t,[m])for t,m in sorted(events,key=lambda x:x[0])))
  command=['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(score),'_',str(out/(kind+'.wav')),'48000','WAV','float','-o',str(21 if kind=='Grain'else len(gains)),'-i','0','-a','256','-c','2048','-m','262144','-D','0','-U','/Applications/SuperCollider.app/Contents/Resources/plugins']
  rendered=subprocess.run(command,capture_output=True,text=True,timeout=30);log=rendered.stdout+rendered.stderr;(out/(kind+'.log')).write_text(log)
  assert rendered.returncode==0 and 'FAILURE'not in log,log
  samples,channels,sr=osc.read_wav(out/(kind+'.wav'));peak=max(abs(samples[i*channels])for i in range(int(.2*sr),int(.9*sr)));assert peak>1e-5,(kind,peak)
  error=max(abs(samples[i*channels+k]-gain*samples[i*channels+(16+k if kind=='Grain'else 0)])for i in range(int(.2*sr),int(.9*sr))for k,gain in enumerate(gains))
  assert all(math.isfinite(v)for v in samples)and error<1e-6,(kind,error)
  print('PASS:',kind,'production audio presence at 0, .001, .499, .501, 1; maximum error',error,flush=True)
if __name__=='__main__':main()
