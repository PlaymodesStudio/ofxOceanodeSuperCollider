#!/usr/bin/env python3
"""The imported waveforms (Round / Skew / Width / Phase) against the original
PolyComb LFO core's formulas, rendered with the real shared ModLfo SynthDef.
F = phase - 0.5: saw up 2F, saw down -2F, triangle 1 - 4|F|, sine sin(2pi(0.25 - |F|)),
pulse +1 while F < W/2; all read unipolar ((x + 1) / 2)."""
import math,pathlib,subprocess,sys
ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(pathlib.Path(__file__).parent))
from import_ensemble import lfo_wave,lfo_phase
from validate_banks import message,bundle
out=pathlib.Path('/tmp/polycomb-lfo-shapes');out.mkdir(exist_ok=True)
probe=out/'probe.scd'
probe.write_text('SynthDef(\\LfoShapeProbe,{Out.ar(0,K2A.ar(In.kr(\\in.kr(100))))}).writeDefFile("%s");\n0.exit;\n'%out)
r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l','/tmp/polycomb-options-config.yaml',str(probe)],capture_output=True,text=True);assert r.returncode==0,r.stdout+r.stderr
def reference(wf,ph,w):
    F=ph%1-.5
    x={0:2*F,1:-2*F,3:1-4*abs(F),4:math.sin(2*math.pi*(.25-abs(F))),2:1 if F<min(w/2,.16) else -1}[wf]
    return (x+1)/2
cases=[(0,0,0),(1,0,0),(3,0,0),(4,0,0),(2,0,0),(2,0,.3),(2,0,-.4),(4,.3,0),(2,.2,.25)]   # pulse widths up to the native 2/3
phases=[i/20 for i in range(20)]
score=bundle(0,[message('/d_recv',(out/'LfoShapeProbe.scsyndef').read_bytes()),message('/d_recv',(ROOT/'synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation/ModLfo1.scsyndef').read_bytes()),message('/g_new',1,0,0)])
step=.05;k=0;expected=[]
for wf,phase,w in cases:
    c={};put=lambda key,v:c.__setitem__(key[2:],v)
    lfo_wave(put,'L_',wf+1,7,w,0)   # the snapshot's row (from 1)
    base=lfo_phase(wf,phase,w)
    for ph in phases:
        # frozen (freq 0) at the original's phase ph: the native LFO reads base + ph
        score+=bundle(k*step+.01,[message('/s_new','ModLfo1',100,1,1,'out',100,'freq',0.0,'initphase',(base+ph)%1,'round',float(c['Round']),
                       'skew',float(c['Skew']),'pw',float(c['Width']),'centered',1.0),message('/s_new','LfoShapeProbe',101,1,1,'in',100)])
        score+=bundle((k+1)*step-.01,[message('/g_freeAll',1)])
        expected.append((wf,phase,w,ph,reference(wf,ph+phase,w)));k+=1
score+=bundle(k*step+.1,[message('/c_set',0,0.0)])
(out/'t.osc').write_bytes(score)
r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(out/'t.osc'),'_',str(out/'t.wav'),'48000','WAV','float','-o','1','-i','0','-a','128','-c','4096','-D','0'],capture_output=True,text=True)
assert r.returncode==0 and 'FAILURE' not in r.stdout+r.stderr,r.stdout+r.stderr
import numpy as np
raw=(out/'t.wav').read_bytes();i=raw.find(b'data');a=np.frombuffer(raw[i+8:],dtype='<f4')
bad=[]
for j,(wf,phase,w,ph,ref) in enumerate(expected):
    got=float(np.median(a[int((j*step+.03)*48000):int((j*step+.04)*48000)]))
    # a pulse edge sampled exactly on it may land on either side
    edge=wf==2 and min(abs((ph+phase)%1-x) for x in (0,1,(.5+w*.5)))<1e-6
    if abs(got-ref)>2e-3 and not edge:bad.append((wf,phase,w,ph,round(ref,3),round(got,3)))
assert not bad,bad[:8]
print(f'PASS: saw up / saw down / triangle / sine / pulse (widths, phases) match the original LFO core at {len(expected)} phase points')
