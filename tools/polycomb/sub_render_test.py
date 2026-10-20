#!/usr/bin/env python3
"""Real optional sub DSP: muted main, gated release, tuning and dry bypass."""
import json, pathlib, subprocess, sys
import numpy as np
ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
out=pathlib.Path('/tmp/polycomb-sub-render');out.mkdir(exist_ok=True)
probe=out/'probe.scd';probe.write_text('Archive.archiveDir="/tmp/polycomb-sub-render";\nSynthDef(\\PCSubProbe,{Out.ar(0,SinOsc.ar(220,0,\\amp.kr(0)));Out.ar(32,K2A.ar(\\gate.kr(1)))}).writeDefFile("/tmp/polycomb-sub-render");\n0.exit;\n')
config=out/'config.yaml';config.write_text(f'includePaths:\n    - {ROOT}/sclang/SCClassLibrary\nexcludePaths: []\npostInlineWarnings: false\nexcludeDefaultPaths: true\n')
r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l',str(config),str(probe)],capture_output=True,text=True,timeout=30)
assert r.returncode==0 and (out/'PCSubProbe.scsyndef').exists(),r.stdout+r.stderr
folder=ROOT/'synthdefs/Defaults/PolyComb/CompiledSynthdefs/PolyComb'
score=bundle(0,[message('/d_recv',p.read_bytes())for p in (out/'PCSubProbe.scsyndef',folder/'PCSub1.scsyndef')]+[message('/g_new',1,0,0),message('/c_set',100,1.0)])
cases=[('dry',.1,None,0,1,1),('off',.1,0,0,1,1),('sub',0,1,0,1,1),('octave',0,1,12,1,1),('master0',0,1,0,0,1),('velocity0',0,1,0,1,0),('toggle',0,1,0,1,1)]
for i,(name,amp,on,transpose,master,vel) in enumerate(cases):
 msgs=[message('/c_set',100,float(vel)),message('/s_new','PCSubProbe',100,1,1,'amp',float(amp))]
 if on is not None:msgs+=[message('/s_new','PCSub1',101,1,1,'out',0,'gate',32,'vel',100,'vis',200,'velsens',1.,'pitch',60.,'transpose',float(transpose),'level',.2,'master',float(master),'on',float(on),'atk',.01,'dec',.1,'sus',1.,'rel',.1)]
 score+=bundle(i+0.01,msgs)
 if name=='toggle':score+=bundle(i+.3,[message('/n_set',101,'on',0.)])
 score+=bundle(i+.5,[message('/n_set',100,'gate',0.)])+bundle(i+.99,[message('/g_freeAll',1)])
score+=bundle(len(cases)+.01,[message('/c_set',0,0.)]);p=out/'test.osc';p.write_bytes(score);wav=out/'test.wav'
r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(p),'_',str(wav),'48000','WAV','float','-o','1','-i','0','-a','128','-c','512','-D','0'],capture_output=True,text=True,timeout=30)
(out/'server.log').write_text(r.stdout+r.stderr);assert r.returncode==0 and 'FAILURE' not in r.stdout+r.stderr,r.stdout+r.stderr
samples,ch,sr=read_wav(wav);audio=np.asarray(samples);assert np.isfinite(audio).all()
parts={name:audio[int((i+.1)*sr):int((i+.4)*sr)]for i,(name,*_)in enumerate(cases)}
assert np.array_equal(parts['dry'],parts['off']),'Disabled sub must preserve dry samples'
assert np.max(np.abs(parts['sub']))>.19,'Sub must sound with muted main oscillator'
assert np.max(np.abs(parts['master0']))==0 and np.max(np.abs(parts['velocity0']))==0
for i in (2,3):assert np.max(np.abs(audio[int((i+.8)*sr):int((i+.95)*sr)]))<1e-7,'Release must finish'
assert np.max(np.abs(audio[int(6.4*sr):int(6.49*sr)]))<1e-7,'Disabling sub must fade to silence before gate release'
def hz(x):return np.count_nonzero((x[:-1]<=0)&(x[1:]>0))/(len(x)/sr)
assert abs(hz(parts['sub'])-261.626)<5 and abs(hz(parts['octave'])-523.251)<5
(out/'measurements.json').write_text(json.dumps({name:dict(peak=float(np.max(np.abs(x))),hz=hz(x))for name,x in parts.items()},indent=2)+'\n')
print('PASS: muted-main sub sounds; octave tuning, release, master/velocity silence, toggle fade and sample-identical disabled bypass.')

# A chord must produce only its first input pitch, including when that pitch
# is not the lowest note. Exercise value controls and mapped audio cables.
probe.write_text('Archive.archiveDir="/tmp/polycomb-sub-render";\nSynthDef(\\PCSubChordProbe,{Out.ar(32,K2A.ar(\\gate.kr(1))!3);Out.ar(64,DC.ar([67,60,64]))}).writeDefFile("/tmp/polycomb-sub-render");\n0.exit;\n')
r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l',str(config),str(probe)],capture_output=True,text=True,timeout=30)
assert r.returncode==0 and (out/'PCSubChordProbe.scsyndef').exists(),r.stdout+r.stderr
score=bundle(0,[message('/d_recv',p.read_bytes()) for p in (out/'PCSubChordProbe.scsyndef',folder/'PCSub3.scsyndef')]+[message('/g_new',1,0,0),message('/c_setn',100,3,1.,1.,1.)])
for i,via_cable in enumerate((False,True)):
    score+=bundle(i+.01,[message('/s_new','PCSubChordProbe',100,1,1),message('/s_new','PCSub3',101,1,1,'out',0,'gate',32,'vel',100,'vis',200,'pitch_sel',float(via_cable),'transpose',-12.,'level',.2,'master',1.,'atk',.01,'dec',.1,'sus',1.,'rel',.1),message('/n_setn',101,'pitch',3,67.,60.,64.)])
    if via_cable:score+=bundle(i+.02,[message('/n_mapan',101,'pitch_ar',64,3)])
    score+=bundle(i+.5,[message('/n_set',100,'gate',0.)])+bundle(i+.99,[message('/g_freeAll',1)])
score+=bundle(2.01,[message('/c_set',0,0.)]);p=out/'chord.osc';p.write_bytes(score);wav=out/'chord.wav'
r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(p),'_',str(wav),'48000','WAV','float','-o','3','-i','0','-a','128','-c','512','-D','0'],capture_output=True,text=True,timeout=30)
assert r.returncode==0 and 'FAILURE' not in r.stdout+r.stderr,r.stdout+r.stderr
samples,ch,sr=read_wav(wav);audio=np.asarray(samples).reshape(-1,ch)
assert ch==3 and np.isfinite(audio).all()
root_hz=440*2**((67-12-69)/12)
for i,source in enumerate(('values','audio cable')):
    x=audio[int((i+.1)*sr):int((i+.4)*sr)]
    assert np.max(np.abs(x))>.19,source
    assert np.array_equal(x[:,0],x[:,1]) and np.array_equal(x[:,0],x[:,2]),f'{source}: chord voices must share one root tone'
    t=np.arange(len(x))/sr
    fundamental=np.column_stack((np.sin(2*np.pi*root_hz*t),np.cos(2*np.pi*root_hz*t)))
    fitted=fundamental@np.linalg.lstsq(fundamental,x[:,0],rcond=None)[0]
    assert np.sqrt(np.mean((x[:,0]-fitted)**2))<1e-4,f'{source}: sub must contain only the root frequency'
    assert np.max(np.abs(audio[int((i+.8)*sr):int((i+.95)*sr)]))<1e-7,f'{source}: release must finish'
print('PASS: chord values and audio cables produce a single first-note root tone on every voice, with gated release.')
