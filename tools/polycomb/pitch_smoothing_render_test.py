#!/usr/bin/env python3
"""Render production pitch lag, comb retuning and independent root vibrato."""
import pathlib, subprocess, sys
import numpy as np
ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
out=pathlib.Path('/tmp/polycomb-pitch-smoothing-render');out.mkdir(exist_ok=True)
config=out/'config.yaml';config.write_text(f'includePaths:\n    - {ROOT}/sclang/SCClassLibrary\nexcludePaths: []\npostInlineWarnings: false\nexcludeDefaultPaths: true\n')
probe=out/'probe.scd';probe.write_text(f'Archive.archiveDir="{out}";\nSynthDef(\\PitchSmoothProbe,{{Out.ar(0,K2A.ar(In.kr(100,2)));Out.ar(32,DC.ar(1));Out.ar(48,SinOsc.ar(173,0,0.05));Out.kr(120,1)}}).writeDefFile("{out}");\n0.exit;\n')
r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l',str(config),str(probe)],capture_output=True,text=True,timeout=30)
assert r.returncode==0 and (out/'PitchSmoothProbe.scsyndef').exists(),r.stdout+r.stderr
folder=ROOT/'synthdefs/Defaults/PolyComb/CompiledSynthdefs/PolyComb'
def render(name,score,channels):
 p=out/(name+'.osc');p.write_bytes(score);wav=p.with_suffix('.wav')
 r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(p),'_',str(wav),'48000','WAV','float','-o',str(channels),'-i','0','-a','256','-c','512','-U',str(ROOT/'ugens/OceanodeAnalog')+':/Applications/SuperCollider.app/Contents/Resources/plugins','-D','0'],capture_output=True,text=True,timeout=30)
 (out/(name+'.log')).write_text(r.stdout+r.stderr)
 assert r.returncode==0 and 'FAILURE' not in r.stdout+r.stderr,r.stdout+r.stderr
 samples,ch,sr=read_wav(wav);audio=np.asarray(samples).reshape(-1,ch);assert np.isfinite(audio).all();return audio,sr
score=bundle(0,[message('/d_recv',p.read_bytes())for p in [out/'PitchSmoothProbe.scsyndef',folder/'PCPitch1.scsyndef',folder/'PCCombR1.scsyndef',folder/'PCOscSine1.scsyndef']]+[message('/g_new',1,0,0)])
score+=bundle(.01,[message('/s_new','PCPitch1',101,1,1,'out',100,'pitch',60.),message('/s_new','PitchSmoothProbe',100,1,1),message('/s_new','PCCombR1',102,1,1,'in',48,'out',2,'note',100,'combmode',1.,'combpitch',60.,'combfeed',.7,'satlevel',12.),message('/s_new','PCOscSine1',103,1,1,'note',100,'out',3,'oscamp',.2)])
# Exercise oscillator global transpose, a generated-map target, and comb tuning.
for t,key in [(.5,'transpose'),(1.,'pitch')]:score+=bundle(t,[message('/n_set',101,key,12. if key=='transpose' else 72.),message('/n_set',102,'combtranspose',12. if t==.5 else -12.)])
# 60 Hz mouse updates: a staircase over two octaves.
for i in range(60):score+=bundle(1.5+i/60,[message('/n_set',101,'transpose',float(i%25)),message('/n_set',102,'combtranspose',float(i%25))])
score+=bundle(2.7,[message('/g_freeAll',1)])+bundle(2.8,[message('/c_set',0,0.)])
a,sr=render('pitch',score,4)
for t,start,end in [(.5,60.,72.),(1.,72.,84.)]:
 before=a[int((t-.01)*sr):int((t-.002)*sr),0];settled=a[int((t+.05)*sr):int((t+.08)*sr),0]
 assert np.max(np.abs(before-start))<.001 and np.max(np.abs(settled-end))<.001
 # An unsmoothed octave change jumps 12 semitones in one control block.
 region=a[int((t-.005)*sr):int((t+.05)*sr),0]
 assert np.max(np.abs(np.diff(region,n=1)))<.1
 assert np.any((region>start+.1)&(region<end-.1))
assert np.max(np.abs(a[:,2]))<2 and np.max(np.abs(a[:,3]))<.21
score=bundle(0,[message('/d_recv',p.read_bytes())for p in [out/'PitchSmoothProbe.scsyndef',folder/'PCSub1.scsyndef']]+[message('/g_new',1,0,0)])
for i,(rate,amt,rnd) in enumerate([(5.,0.,0.),(5.,.5,0.),(8.,.5,0.),(5.,.5,.5)]):
 score+=bundle(i*4+.01,[message('/s_new','PitchSmoothProbe',100,1,1),message('/s_new','PCSub1',101,1,1,'out',2,'gate',32,'vel',120,'vis',130,'pitch',69.,'transpose',0.,'level',.2,'master',1.,'vibrate',rate,'vibamt',amt,'vibrand',rnd)])
 score+=bundle((i+1)*4-.01,[message('/g_freeAll',1)])
score+=bundle(16.01,[message('/c_set',0,0.)]);a,sr=render('vibrato',score,3)
for i,wanted in enumerate([0,5,8,None]):
 x=a[int((i*4+.5)*sr):int((i*4+3.5)*sr),2];n=len(x)
 # Analytic signal yields instantaneous frequency without an external DSP library.
 weights=np.zeros(n);weights[0]=1;weights[1:(n+1)//2]=2
 if n%2==0:weights[n//2]=1
 phase=np.unwrap(np.angle(np.fft.ifft(np.fft.fft(x)*weights)))
 freq=np.diff(phase)*sr/(2*np.pi);freq=freq[int(.1*sr):-int(.1*sr)]
 # Average control-block interpolation/ripple before estimating the LFO.
 block=240;freq=freq[:len(freq)//block*block].reshape(-1,block).mean(axis=1)
 cents=12*np.log2(freq/440.)
 if wanted==0:assert np.std(cents)<.001
 else:
  assert .45<np.std(cents)*2**.5<.55
  bins=np.fft.rfftfreq(len(cents),block/sr);power=np.abs(np.fft.rfft(cents-cents.mean()));observed=bins[1+np.argmax(power[1:])]
  if wanted:assert abs(observed-wanted)<.4,(wanted,observed)
  else:assert 2.1<observed<7.9,observed
print('PASS: oscillator/map pitch changes interpolate and settle; comb mouse-rate retuning stays finite; independent sub vibrato depth/rate/random range and zero-depth bypass.')
