#!/usr/bin/env python3
"""Render AliasingBox -> shared Saturation/EQ/Echo/Reverb at 1/2/16 channels.
Checks transparent bypass, audible processing, tails after gate-off, and direct
routing after the stages stop. Uses the same MixerPro definitions as the node.
"""
import json,math,pathlib,subprocess
from render_test import ROOT,message,bundle,read_wav
out=pathlib.Path('/tmp/aliasingbox-effects');out.mkdir(exist_ok=True)
folder=ROOT/'synthdefs/Defaults/AliasingBox'
bank=json.loads((folder/'factory.json').read_text());controls=json.loads((folder/'controls.json').read_text())
report=[]
for n in (1,2,16):
    mixer=ROOT/'synthdefs/Defaults/MixerPro/CompiledSynthdefs/mixerpro'
    paths=[folder/'CompiledSynthdefs/AliasingBox/AliasingBoxVoice.scsyndef',ROOT/'synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation/ModGate1.scsyndef',ROOT/f'synthdefs/Defaults/PolyComb/CompiledSynthdefs/PolyComb/PolyCombSpread{n}.scsyndef']
    paths += [mixer/f'mixerPro{name}{n}.scsyndef' for name in ('Sat','EQ','Echo','Rvb')]
    paths += [mixer/f'mixerProAdapt{n}_{n}.scsyndef']
    score=bundle(0,[message('/d_recv',p.read_bytes()) for p in paths]+[message('/g_new',1,0,0),message('/c_setn',32,16,*([0.0]*16))])
    params=sum(([c['sc'],float(bank[0]['values'][c['name']])] for c in controls),[])
    add=[message('/s_new','ModGate1',100,1,1,'out',192,'gate',1.0,'vel',0,'rnd',1),
         message('/s_new','AliasingBoxVoice',101,1,1,'gatebus',192,'velbus',0,'out',48,'master',0.25,'normalization',1.0,*params,*sum(([k,32] for k in ('mpitch','mwrite','mread','msym','mlp','mhp','mlpres','mhpres','mlpfm','mhpfm','mdrive','mamp')),[])),
         message('/s_new',f'PolyCombSpread{n}',102,1,1,'in',48,'out',32,'voices',1,'mpan',32),
         message('/s_new',f'mixerProAdapt{n}_{n}',103,1,1,'in',32,'out',n),
         message('/s_new',f'mixerProSat{n}',104,1,1,'in',32,'out',80,'on',0.0,'drive',12.0),
         message('/s_new',f'mixerProEQ{n}',105,1,1,'in',80,'out',112),
         message('/s_new',f'mixerProEcho{n}',106,1,1,'in',112,'out',144,'on',0.0,'delay',0.15,'feed',0.45,'mix',0.4),
         message('/s_new',f'mixerProRvb{n}',107,1,1,'in',144,'out',0,'on',0.0,'decay',1.0,'mix',0.35)]
    score+=bundle(.01,add)
    score+=bundle(.8,[message('/n_set',104,'on',1.0),message('/n_set',105,'b3gain',8.0),message('/n_set',106,'on',1.0),message('/n_set',107,'on',1.0)])
    score+=bundle(1.7,[message('/n_set',100,'gate',0.0)])
    score+=bundle(1.8,[message('/n_set',106,'on',0.0),message('/n_set',107,'on',0.0)])
    score+=bundle(3.0,[message('/n_free',104,105,106,107),message('/s_new',f'mixerProAdapt{n}_{n}',108,1,1,'in',32,'out',0)])
    score+=bundle(3.2,[message('/n_set',100,'gate',1.0)])
    score+=bundle(3.8,[message('/n_free',100,101,102,103,108)])
    score+=bundle(4.0,[message('/c_set',32,0.0)])
    osc=out/f'{n}.osc';osc.write_bytes(score);wav=out/f'{n}.wav'
    r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(osc),'_',str(wav),'48000','WAV','float','-o',str(n*2),'-i','0','-z','64','-D','0','-w','2048','-m','262144'],capture_output=True,text=True)
    (out/f'{n}.log').write_text(r.stdout+r.stderr)
    assert r.returncode==0 and 'FAILURE' not in r.stdout+r.stderr,r.stdout+r.stderr
    audio,ch,sr=read_wav(wav);assert all(math.isfinite(v) for v in audio)
    def rms(a,b,difference=False,dry=False):
        values=[]
        for f in range(round(a*sr),round(b*sr)):
            for c in range(n):
                wet=audio[f*ch+c];ref=audio[f*ch+n+c]
                values.append(wet-ref if difference else ref if dry else wet)
        return math.sqrt(sum(v*v for v in values)/len(values))
    baseline=rms(.3,.6,True);wet=rms(1.0,1.5,True);tail=rms(2.1,2.5);dryTail=rms(2.1,2.5,dry=True);bypass=rms(3.4,3.7,True)
    assert baseline<1e-6,(n,'initial bypass',baseline)
    assert wet>1e-5,(n,'effects inaudible',wet)
    assert tail>1e-6 and dryTail<1e-6,(n,'no tail after gate-off',tail,dryTail)
    assert bypass<1e-6,(n,'stopped-stage routing',bypass)
    report.append(dict(channels=n,bypass=baseline,processed_difference=wet,tail=tail,dry_tail=dryTail,direct_route=bypass))
(out/'measurements.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS: shared saturation/EQ/echo/reverb at 1, 2 and 16 channels; bypass, audible processing, tails and direct routing.')
