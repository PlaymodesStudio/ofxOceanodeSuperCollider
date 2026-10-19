#!/usr/bin/env python3
"""Render quak against user-supplied Reaktor references without playing audio.

Requires numpy and ffmpeg. Note/voice count are explicit because a recording
alone does not identify the keyboard octave or unison assignment. The two
reference takes need not be phase aligned; report frequency, DC and RMS, not
an invalid sample-by-sample null test. No normalization is applied to files.
"""
import argparse,json,pathlib,subprocess
import numpy as np
from render_test import message,bundle
ROOT=pathlib.Path(__file__).resolve().parents[2]

def load_audio(path):
    info=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-of','json',str(path)]))['streams'][0]
    raw=subprocess.check_output(['ffmpeg','-v','error','-i',str(path),'-f','f32le','-'])
    return np.frombuffer(raw,dtype='<f4').reshape(-1,info['channels']).astype(float),int(info['sample_rate'])

def measure(audio,sr,start,end):
    x=audio[round(start*sr):round(end*sr),0]
    assert np.isfinite(x).all()
    n=2**int(np.ceil(np.log2(len(x)*8)))
    mag=np.abs(np.fft.rfft((x-x.mean())*np.hanning(len(x)),n))
    freq=np.fft.rfftfreq(n,1/sr)
    k=np.argmax(mag*((freq>8)&(freq<20)))
    # Local parabola, below one FFT bin. Harmonic sidebands and evolving
    # table collisions still limit the precision of the carrier estimate.
    p=np.polyfit(freq[k-1:k+2],np.log(np.maximum(mag[k-1:k+2],1e-30)),2)
    return dict(carrier_hz=float(-p[1]/(2*p[0])),dc=float(x.mean()),rms=float(np.sqrt(np.mean(x*x))),peak=float(np.max(np.abs(x))))

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dry',required=True,type=pathlib.Path)
    parser.add_argument('--wet',required=True,type=pathlib.Path)
    parser.add_argument('--voices',type=int,default=5)
    parser.add_argument('--note',type=float,default=60)
    parser.add_argument('--defs',type=pathlib.Path,default=ROOT/'synthdefs/Defaults/AliasingBox/CompiledSynthdefs/AliasingBox')
    parser.add_argument('--legacy',action='store_true',help='Compare definitions from the first port (Pitch Bend naming)')
    parser.add_argument('--output',type=pathlib.Path,default=pathlib.Path('/tmp/aliasing-reference/current'))
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    n=args.voices;assert 1<=n<=16
    folder=ROOT/'synthdefs/Defaults/AliasingBox'
    bank=json.loads((folder/'factory.json').read_text())[0]['values'];controls=json.loads((folder/'controls.json').read_text())
    defs=[args.defs/'AliasingBoxVoice.scsyndef', ROOT/f'synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation/ModGate{n}.scsyndef',ROOT/'synthdefs/Defaults/PolyComb/CompiledSynthdefs/PolyComb/PolyCombSpread2.scsyndef']
    cases=[('dry',0,1),('wet',1,1),('range_quarter',0,.25),('range_small',0,.05)]
    report={'note':args.note,'voices':n,'legacy':args.legacy,'reference':{},'render':{}}
    for name,path in [('dry',args.dry),('wet',args.wet)]:
        a,sr=load_audio(path);report['reference'][name]=measure(a,sr,1,6)
    score=bundle(0,[message('/d_recv',p.read_bytes())for p in defs]+[message('/g_new',1,0,0)])
    for case,(name,filtered,table_range) in enumerate(cases):
        t=case*6.5+.01
        params=[]
        for c in controls:
            sc=c['sc'];v=bank[c['name']]
            if args.legacy and sc=='fine_tune':sc='pitch_bend'
            params += [sc,float(v)]
        cmds=[message('/c_set',32,0.0),message('/s_new',f'ModGate{n}',1000,0,1,'out',32,'vel',0,'rnd',16,'gate',1.0,'velocity',1.0),message('/n_setn',1000,'gate',n,*([1.0]*n))]
        comp=1/np.sqrt(max(1,n/2))
        for voice in range(n):
            cmds.append(message('/s_new','AliasingBoxVoice',1001+voice,1,1,*params,'filter_on',float(filtered),'table_range',float(table_range),
                'gatebus',32+voice,'velbus',voice,'out',48+voice,'vis',2048,'voice',voice,'voices',n,'pitch',args.note,'master',1.0,'normalization',float(1/(n*comp)),
                *sum(([p,32]for p in ['mpitch','mwrite','mread','msym','mlp','mhp','mlpres','mhpres','mlpfm','mhpfm','mdrive','mamp']),[])))
        cmds.append(message('/s_new','PolyCombSpread2',1200,1,1,'in',48,'out',0,'voices',n,'vis',700,'mpan',800))
        width=bank['Stereo'];pans=[max(0,min(1,.5+(i/(n-1)-.5)*width))if n>1 else.5 for i in range(n)]+[.5]*(16-n)
        score+=bundle(t,cmds+[message('/n_setn',1200,'pan',16,*pans)])
        score+=bundle(t+5.5,[message('/n_setn',1000,'gate',n,*([0.0]*n))])
        score+=bundle(t+6.4,[message('/n_free',1000,1200,*range(1001,1001+n))])
    # Same-timestamp, parallel A/B: an address-range change must alter the
    # sound even when gate phase and LFO initialization are identical.
    pairs=[message('/s_new',f'ModGate{n}',1000,0,1,'out',32,'vel',0,'rnd',16,'gate',1.0)]
    for out,range_value in enumerate((1.0,.05)):
        pairs.append(message('/s_new','AliasingBoxVoice',5000+out,1,1,*params,
            'filter_on',0.0,'table_range',range_value,'gatebus',32,'velbus',0,'out',out,'vis',2048,
            'voice',0,'voices',1,'pitch',args.note,'master',0.5,'normalization',1.0,
            *sum(([p,32]for p in ['mpitch','mwrite','mread','msym','mlp','mhp','mlpres','mhpres','mlpfm','mhpfm','mdrive','mamp']),[])))
    score+=bundle(26.2,pairs)
    score+=bundle(28.2,[message('/n_free',1000,5000,5001)])
    score+=bundle(28.3,[message('/c_set',32,0.0)])
    path=args.output/'comparison.osc';path.write_bytes(score);wav=args.output/'comparison.wav'
    run=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(path),'_',str(wav),'44100','WAV','float','-o','2','-i','0','-z','64','-D','0','-w','2048','-m','65536'],text=True,capture_output=True)
    (args.output/'server.log').write_text(run.stdout+run.stderr)
    assert run.returncode==0 and 'FAILURE' not in run.stdout+run.stderr,run.stdout+run.stderr
    a,sr=load_audio(wav)
    for case,(name,_,_) in enumerate(cases):report['render'][name]=measure(a,sr,case*6.5+.5,case*6.5+5.4)
    report['dry_pitch_error_cents']=float(1200*np.log2(report['render']['dry']['carrier_hz']/report['reference']['dry']['carrier_hz']))
    pair=a[round(26.5*sr):round(28*sr)]
    delta=np.sqrt(np.mean((pair[:,0]-pair[:,1])**2))
    assert delta>1e-4 or args.legacy,'Table Range did not change the sound'
    report['range_change_rms_difference']=float(delta)
    (args.output/'measurements.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
if __name__=='__main__':main()
