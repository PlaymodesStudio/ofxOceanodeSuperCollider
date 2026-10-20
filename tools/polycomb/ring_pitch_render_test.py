#!/usr/bin/env python3
"""Compile production ringmod and measure pitch source, transpose and ratio."""
import pathlib,sys,subprocess
import numpy as np
ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
out=pathlib.Path('/tmp/polycomb-ring-pitch');out.mkdir(exist_ok=True)
s=(ROOT/'synthdefs/Defaults/PolyComb/polyCombExtras.scd').read_text();start=s.index('(1..32).do',s.index('// PCRingR<N>'));end=s.index('"PolyComb extras written',start)
(out/'compile.scd').write_text(f'Archive.archiveDir="{out}";\n'+s[start:end].replace('.writeDefFile(dir)',f'.writeDefFile("{out}")')+f'\nSynthDef(\\RingInput,{{Out.ar(64,DC.ar(0.1)!32)}}).writeDefFile("{out}");\n0.exit;\n')
(out/'config.yaml').write_text(f'includePaths:\n    - {ROOT}/sclang/SCClassLibrary\nexcludePaths: []\npostInlineWarnings: false\nexcludeDefaultPaths: true\n')
with (out/'compile.log').open('w') as log:
 r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l',str(out/'config.yaml'),str(out/'compile.scd')],stdout=log,stderr=subprocess.STDOUT,timeout=30)
assert r.returncode==0 and (out/'PCRingR32.scsyndef').exists(),(out/'compile.log').read_text()
for n in (1,4,32):
 # Oscillator bus, independent pitch, then resonator-map pitch with transposition/ratio.
 cases=[(0,69,48,0,1,440),(1,69,57,0,1,220),(1,69,60,12,2,1046.502261)]
 events=[(0,[message('/d_recv',(out/'RingInput.scsyndef').read_bytes()),message('/d_recv',(out/f'PCRingR{n}.scsyndef').read_bytes()),message('/g_new',1,0,0)])]
 for i,(mode,bus,pitch,trans,ratio,hz) in enumerate(cases):
  t=i+0.01
  events.append((t,[message('/c_setn',0,n,*([float(bus)]*n)),message('/s_new','RingInput',100,1,1),message('/s_new',f'PCRingR{n}',101,1,1,'in',64,'out',0,'note',0,'ringmode',float(mode),'ringpitch',float(pitch),'ringtranspose',float(trans),'ringratio',float(ratio),'mrmix',40)]))
  events[-1][1].extend(message('/n_setn',101,key,n,*([float(value)]*n)) for key,value in [('ringpitch',pitch),('ringtranspose',trans),('ringratio',ratio)])
  events.append((i+.99,[message('/g_freeAll',1)]))
 events.append((3.,[message('/c_set',0,0.)]));score=out/f'{n}.osc';score.write_bytes(b''.join(bundle(t,m)for t,m in events));wav=score.with_suffix('.wav')
 r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(score),'_',str(wav),'48000','WAV','float','-o',str(n),'-i','0','-a','256','-c','128','-D','0'],capture_output=True,text=True,timeout=30)
 assert r.returncode==0 and 'FAILURE'not in r.stdout+r.stderr,r.stdout+r.stderr
 audio,ch,sr=read_wav(wav);audio=np.array(audio).reshape(-1,ch)
 for i,c in enumerate(cases):
  sig=audio[int((i+.2)*sr):int((i+.9)*sr)];assert np.isfinite(sig).all()
  for channel in range(n):
   peak=np.argmax(abs(np.fft.rfft(sig[:,channel]*np.hanning(len(sig)))))*sr/len(sig)
   assert abs(peak-c[-1])<2,(n,i,channel,peak,c[-1])
print('PASS: oscillator/external/mapped ring pitch, transposition and ratio at 1, 4 and 32 voices.')
