#!/usr/bin/env python3
"""Render production dropdown transitions offline, including legacy sampler endpoints."""
from pathlib import Path
import argparse,json,math,struct,subprocess,sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',type=Path,default=Path('/tmp/box-dropdown-render'))
args=parser.parse_args();o=args.output.resolve();o.mkdir(parents=True,exist_ok=True)
classes=o/'classes';classes.mkdir(exist_ok=True)
(classes/'oceanodeMicroSpace.sc').write_text((ROOT/'synthdefs/Oceanode/oceanodeMicroSpace.sc').read_text())
(o/'sclang.yaml').write_text('includePaths:\n  - '+str(ROOT/'sclang/SCClassLibrary')+'\n  - '+str(classes)+'\nexcludeDefaultPaths: true\n')
source=(ROOT/'synthdefs/Defaults/SampleBox/samplebox.scd').read_text()
# Historical resampling/stretch reader and timing expressions, retained as an
# independent compatibility reference; all other note/routing code stays shared.
legacy=source
start=legacy.index('step = switch(stretchVariant');end=legacy.index('                    pos = Sweep',start)
legacy=legacy[:start]+r"""step = if(stretchVariant == 0,
                        { speed * (2 ** ((pitch - 60) / 12)) },
                        { speed / vm.(28, \stretch.kr(1)).clip(0.125, 8) }) * BufRateScale.kr(buf) * fitRate;
"""+legacy[end:]
start=legacy.index('read = { |b, at|');end=legacy.index('                    left = read.',start)
legacy=legacy[:start]+r"""read = { |b, at|
                        if(stretchVariant == 0, { BufRd.ar(1, b, at, 0, 4) }, {
                            Warp1.ar(1, b, at / frames, 2 ** ((pitch - 60) / 12),
                                vm.(32, \warpwindow.kr(0.15)).clip(0.01, 1), -1, 8, 0, 4)
                        })
                    };
"""+legacy[end:]
for label,text in [('Old',legacy),('New',source)]:
 (o/(label+'.scd')).write_text(text.replace('(1..2).do','[1].do').replace('(1..16).do','[1].do').replace('"SampleBoxPad_"','"Dropdown'+label+'Pad_"'))
wavetable=(ROOT/'synthdefs/Defaults/WavetableBox/wavetableBox.scd').read_text()
warp=wavetable[wavetable.index('warp = {'):wavetable.index('// Everything the variants share:')]
(o/'warp.scd').write_text('(var warp;'+warp+r"""SynthDef(\DropdownWarpProbe,{
 var phase=DC.ar(\phase.kr(0.31));
 var first=warp.(phase,\mode1.kr(0),\amt1.kr(0.7),NamedControl.kr(\weights1,0!9));
 Out.ar(\out.kr(0),warp.(first,\mode2.kr(0),\amt2.kr(0.7),NamedControl.kr(\weights2,0!9)))
}).writeDefFile(OUT_DIR);
)""".replace('OUT_DIR',json.dumps(str(o))))
extras=(ROOT/'synthdefs/Defaults/PolyComb/polyCombExtras.scd').read_text()
start=extras.index('(1..32).do',extras.index('// PCRingR<N>'));end=extras.index('// PCBlur<C>',start)
(o/'ring.scd').write_text('(var dir='+json.dumps(str(o))+';'+extras[start:end].replace('(1..32).do','[1].do')+')')
grain=(ROOT/'synthdefs/Defaults/GrainBox/scGrainBoxPoly.scd').read_text()
start=grain.index('            rate = (pitchSum');end=grain.index(';',start)+1
(o/'probes.scd').write_text(r"""SynthDef(\DropdownProbe,{Out.ar(5,K2A.ar(In.kr(300,1)));Out.ar(6,In.ar(32,1))}).writeDefFile(OUT_DIR);
SynthDef(\DropdownRingInput,{Out.ar(128,DC.ar(1));Out.kr(256,69)}).writeDefFile(OUT_DIR);
SynthDef(\DropdownScaleProbe,{
 var g=In.kr(500,80),pitchSum=\pitch.kr(6.25),x=(pitchSum-g[13]).wrap(0,12),rate,sampleRateCorr=1;
 SCALE
 Out.ar(1,K2A.ar(rate.log2*12));
}).writeDefFile(OUT_DIR);
""".replace('OUT_DIR',json.dumps(str(o))).replace('SCALE',grain[start:end]))
# Compile the actual spreader bodies at four output channels.
spread='(var c=4,dir='+json.dumps(str(o))+',d='+json.dumps(str(o))+';'
for file,name in [('WavetableBox/wavetableBox.scd','WavetableBoxSpread'),('PhysicalBox/physicalBox.scd','PhysicalBoxSpread'),('PolyComb/polyCombParts.scd','PolyCombSpread'),('PolyComb/polyCombExtras.scd','PCSpreadWide')]:
 text=(ROOT/'synthdefs/Defaults'/file).read_text();start=text.index('SynthDef(("'+name+'"');end=text.index('}).writeDefFile',start);end=text.index(';',end)+1
 spread+=text[start:end].replace('d +/+ "WavetableBox"','dir')
spread+=r'SynthDef(\DropdownSpreadInput,{Out.ar(128,DC.ar(0.25)!64)}).writeDefFile(dir);)'
(o/'spread.scd').write_text(spread)
(o/'compile.scd').write_text('try{'+''.join('this.executeFile('+json.dumps(str(o/name))+');'for name in ['Old.scd','New.scd','warp.scd','ring.scd','probes.scd','spread.scd'])+'"DROPDOWNS COMPILED".postln;0.exit;}{|e|e.reportError;1.exit;};')
p=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-u','0','-l',str(o/'sclang.yaml'),str(o/'compile.scd')],capture_output=True,text=True,timeout=120)
(o/'compile.log').write_text(p.stdout+p.stderr)
assert p.returncode==0 and 'DROPDOWNS COMPILED' in p.stdout and 'ERROR:' not in p.stdout+p.stderr,p.stdout+p.stderr

events={}
def add(t,*m):events.setdefault(t,[]).extend(m)
add(0,message('/d_recv',(o/'DropdownWarpProbe.scsyndef').read_bytes()),message('/g_new',1,0,0));cases=[];when=.1
# Two endpoint probes and one mixed probe, restarted at constant phase for each pair.
for stage in [1,2]:
 for a in range(9):
  for b in range(9):
   for i,mode in enumerate([a,b,a]):add(when,message('/s_new','DropdownWarpProbe',100+i,1,1,'out',i,'mode'+str(stage),mode))
   for j,t in enumerate([0.,.499,.501,1.]):
    w=[0.]*9;w[a]=1-t;w[b]+=t;now=when+j*.1;add(now,message('/n_set',102,'mode'+str(stage),a if t<.5 else b),message('/n_setn',102,'weights'+str(stage),9,*w));cases.append((now,t))
   add(when+.4,message('/g_freeAll',1));when+=.45
add(when+.1,message('/c_set',0,0.));(o/'warp-score.osc').write_bytes(b''.join(bundle(t,events[t])for t in sorted(events)))
p=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(o/'warp-score.osc'),'_',str(o/'warp-render.wav'),'8000','WAV','float','-o','3','-i','0','-a','128','-c','1024','-m','131072','-D','0','-U','/Applications/SuperCollider.app/Contents/Resources/plugins'],capture_output=True,text=True,timeout=40);assert p.returncode==0 and 'FAILURE' not in p.stdout+p.stderr,p.stdout+p.stderr
a,ch,sr=read_wav(o/'warp-render.wav');assert ch==3 and all(map(math.isfinite,a))
for when,t in cases:
 i=round((when+.08)*sr)*ch;expected=(1-t)*a[i]+t*a[i+1];assert abs(a[i+2]-expected)<1e-4,(when,t,a[i+2],expected)
print('PASS: production audio-rate warp DSP: all 81 mode pairs, both stages, actual selector switch at .5, linear settled phase blend and no enum jump')

sr=48000
samples=[.3*math.sin(2*math.pi*220*i/sr)+.15*math.sin(2*math.pi*371*i/sr) for i in range(sr*4)];data=struct.pack('<'+'f'*len(samples),*samples);(o/'sample.wav').write_bytes(b'RIFF'+struct.pack('<I',36+len(data))+b'WAVEfmt '+struct.pack('<IHHIIHH',16,3,1,sr,sr*4,4,32)+b'data'+struct.pack('<I',len(data))+data)
e={}
def add(t,*m):e.setdefault(t,[]).extend(m)
for n in ['DropdownOldPad_1_1_R','DropdownOldPad_1_1_T','DropdownNewPad_1_1_M']:
 add(0,message('/d_recv',(o/'CompiledSynthdefs/SampleBox'/(n+'.scsyndef')).read_bytes()))
add(0,message('/d_recv',(o/'DropdownProbe.scsyndef').read_bytes()),message('/b_allocRead',0,str(o/'sample.wav')),message('/g_new',1,0,0))
for i,(name,mix) in enumerate([('DropdownOldPad_1_1_R',0.),('DropdownNewPad_1_1_M',0.),('DropdownOldPad_1_1_T',1.),('DropdownNewPad_1_1_M',1.),('DropdownNewPad_1_1_M',.499)]):
 add(.05,message('/s_new',name,100+i,1,1,'buf0',0,'out',i,'loaded',1,'loop',1,'voices',1,'modbus',1024,'gatebus',32+i*16,'velbus',700+i*16,'rndbus',800+i*16,'latestbus',950+i,'vis',300+i*16,'stretchmix',mix))
 add(.1,message('/n_set',100+i,'gate',1.,'fire',1))
add(.1,message('/s_new','DropdownProbe',200,1,1))
add(1.,message('/n_set',104,'stretchmix',.501))
for i in range(5):add(2.,message('/n_set',100+i,'gate',0.,'stop',1))
add(2.3,message('/g_freeAll',0));add(2.4,message('/c_set',0,0.))
(o/'sample-score.osc').write_bytes(b''.join(bundle(t,e[t])for t in sorted(e)))
p=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(o/'sample-score.osc'),'_',str(o/'sample-render.wav'),str(sr),'WAV','float','-o','7','-i','0','-a','256','-c','4096','-m','262144','-D','0','-U','/Applications/SuperCollider.app/Contents/Resources/plugins'],capture_output=True,text=True,timeout=40);assert p.returncode==0 and 'FAILURE' not in p.stdout+p.stderr,p.stdout+p.stderr
a,ch,sr=read_wav(o/'sample-render.wav');assert all(map(math.isfinite,a));assert max(map(abs,a))>.01
for i,j in [(0,1),(2,3)]:
 diff=max(abs(a[k*ch+i]-a[k*ch+j])for k in range(round(.3*sr),round(1.9*sr)));print('endpoint',i,j,diff);assert diff<1e-5
for start,end,t in [(.4,.9,.499),(1.3,1.9,.501)]:
 diff=max(abs(a[k*ch+4]-((1-t)*a[k*ch+1]+t*a[k*ch+3]))for k in range(round(start*sr),round(end*sr)));print('blend',t,diff);assert diff<2e-4
assert min(a[round(.9*sr)*ch+6:round(1.1*sr)*ch:ch])>.99,'held gate must survive mode transition'
assert abs(a[round(2.2*sr)*ch+6])<1e-5
print('PASS: Resample/Stretch preserve legacy endpoints, linear settled audio blend at .499/.501, held gate and release on one running sampler')


events={}
def add(t,*messages):events.setdefault(t,[]).extend(messages)
for name in ['PCRingR1','DropdownRingInput','DropdownScaleProbe']:add(0,message('/d_recv',(o/(name+'.scsyndef')).read_bytes()))
add(.01,message('/g_new',1,0,0));add(.02,message('/s_new','DropdownRingInput',10,1,1),message('/s_new','PCRingR1',11,1,1,'in',128,'out',0,'note',256,'ringpitch',51,'ringmode',0.),message('/s_new','DropdownScaleProbe',12,1,1))
add(.02,message('/c_setn',518,24,*([2.]*24)))
for i,t in enumerate([0.,.001,.25,.499,.501,.75,.999,1.]):
 when=.1+.7*i;add(when,message('/n_set',11,'ringmode',t),message('/c_set',512,t))
add(5.8,message('/g_freeAll',0));add(5.9,message('/c_set',0,0.))
(o/'pitch-score.osc').write_bytes(b''.join(bundle(t,events[t])for t in sorted(events)))
p=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(o/'pitch-score.osc'),'_',str(o/'pitch.wav'),'48000','WAV','float','-o','2','-i','0','-a','256','-c','4096','-m','131072','-D','0','-U','/Applications/SuperCollider.app/Contents/Resources/plugins'],capture_output=True,text=True,timeout=30)
assert p.returncode==0 and 'FAILURE' not in p.stdout+p.stderr,p.stdout+p.stderr
a,ch,sr=read_wav(o/'pitch.wav');assert all(map(math.isfinite,a))
for i,t in enumerate([0.,.001,.25,.499,.501,.75,.999,1.]):
 start=.1+.7*i+.1;end=start+.5
 crossings=sum(a[k*ch]<=0<a[(k+1)*ch]for k in range(round(start*sr),round(end*sr)))
 expected=440*2**(-18*t/12)
 assert abs(crossings/(end-start)-expected)<3,(t,crossings/(end-start),expected)
 value=a[round((start+.1)*sr)*ch+1];assert abs(value-(6.25*(1-t)+2*t))<1e-4,(t,value)
print('PASS: PolyComb pitch-source interpolation and Grain scale Off/on retain fractional pitches around .5')


# The editor's Ring/Line ID changes at .5, but the spatial DSP consumes its
# effective fraction. Check actual multichannel output around that crossing.
events={}
def add(t,*messages):events.setdefault(t,[]).extend(messages)
names=['WavetableBoxSpread4','PhysicalBoxSpread4','PolyCombSpread4','PCSpreadWide4']
for name in names+['DropdownSpreadInput']:add(0,message('/d_recv',(o/(name+'.scsyndef')).read_bytes()))
add(.01,message('/g_new',1,0,0),message('/s_new','DropdownSpreadInput',10,1,1))
for i,name in enumerate(names):
 add(.02,message('/s_new',name,100+i,1,1,'in',128,'out',4*i,'voices',1,'width',0.,'mpan',900,'vis',950))
 add(.02,message('/n_setn',100+i,'pos' if i==0 else 'pan',16,*([.4]*16)))
for i,t in enumerate([0.,.499,.501,1.]):
 for node in range(100,104):add(.1+.3*i,message('/n_set',node,'line',t))
add(1.4,message('/g_freeAll',0));add(1.5,message('/c_set',0,0.))
(o/'spread-score.osc').write_bytes(b''.join(bundle(t,events[t])for t in sorted(events)))
p=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(o/'spread-score.osc'),'_',str(o/'spread.wav'),'48000','WAV','float','-o','16','-i','0','-a','256','-c','4096','-m','131072','-D','0','-U','/Applications/SuperCollider.app/Contents/Resources/plugins'],capture_output=True,text=True,timeout=30)
assert p.returncode==0 and 'FAILURE' not in p.stdout+p.stderr,p.stdout+p.stderr
a,ch,sr=read_wav(o/'spread.wav');assert ch==16 and all(map(math.isfinite,a))
for i,name in enumerate(names):
 left=a[round(.6*sr)*ch+4*i:round(.6*sr)*ch+4*i+4];right=a[round(.9*sr)*ch+4*i:round(.9*sr)*ch+4*i+4]
 assert max(map(abs,left))>1e-4,(name,left)
 assert max(abs(x-y)for x,y in zip(left,right))<.005,(name,left,right)
print('PASS: four production multichannel spreaders retain continuous Ring/Line output around .5')
