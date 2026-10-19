#!/usr/bin/env python3
"""Render the imported factory bank offline (no sound device or running server).

Compile aliasingBox.scd and Defaults/Modulation/modulation.scd first.
Writes the score, audio and measurements beneath --output (default /tmp).
"""
import argparse, json, math, pathlib, struct, subprocess
ROOT = pathlib.Path(__file__).resolve().parents[2]

def string(s):
    b=s.encode()+b'\0';return b+b'\0'*((-len(b))%4)
def message(address,*args):
    tags=',';data=b''
    for v in args:
        if isinstance(v,str):tags+='s';data+=string(v)
        elif isinstance(v,bytes):tags+='b';data+=struct.pack('>I',len(v))+v+b'\0'*((-len(v))%4)
        elif isinstance(v,int):tags+='i';data+=struct.pack('>i',v)
        else:tags+='f';data+=struct.pack('>f',v)
    return string(address)+string(tags)+data

def bundle(t,messages):
    b=b'#bundle\0'+struct.pack('>Q',round(t*2**32))
    for m in messages:b+=struct.pack('>I',len(m))+m
    return struct.pack('>I',len(b))+b

def read_wav(path):
    b=path.read_bytes();p=12
    while p<len(b)-8:
        tag,size=struct.unpack_from('<4sI',b,p);p+=8
        if tag==b'fmt ':fmt,channels,rate=struct.unpack_from('<HHI',b,p)
        if tag==b'data':
            assert fmt==3,'Expected IEEE float WAV'
            return struct.unpack_from('<'+'f'*(size//4),b,p),channels,rate
        p+=size+(size%2)
    raise AssertionError('No WAV data')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server',default='/Applications/SuperCollider.app/Contents/Resources/scsynth')
    parser.add_argument('--output',type=pathlib.Path,default=pathlib.Path('/tmp/aliasingbox-render'))
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    folder=ROOT/'synthdefs/Defaults/AliasingBox'
    bank=json.loads((folder/'factory.json').read_text());controls=json.loads((folder/'controls.json').read_text())
    defs=[folder/'CompiledSynthdefs/AliasingBox/AliasingBoxVoice.scsyndef', ROOT/'synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation/ModGate1.scsyndef']
    score=bundle(0,[message('/d_recv',p.read_bytes()) for p in defs]+[message('/g_new',1,0,0)])
    duration=1.5
    for i,preset in enumerate(bank):
        t=i*duration+0.01
        params=[]
        for c in controls:params += [c['sc'],float(preset['values'][c['name']])]
        score+=bundle(t,[message('/c_set',32,0.0), # all modulation targets read this zero bus
            message('/s_new','ModGate1',1000,0,1,'out',16,'gate',1.0,'velocity',1.0,'vel',0,'rnd',1),
            message('/s_new','AliasingBoxVoice',1001,1,1,'gatebus',16,'velbus',0,'out',0,'vis',2048,'voice',0,'voices',1,'pitch',60.0,*params,
                    *sum(([n,32] for n in ['mpitch','mwrite','mread','msym','mlp','mhp','mlpres','mhpres','mlpfm','mhpfm','mdrive','mamp']),[]))])
        score+=bundle(t+1.2,[message('/n_set',1000,'gate',0.0)])
        score+=bundle(t+duration-0.02,[message('/n_free',1000,1001)])
    # Exercise the shared sources/routes and voice spreader at 1, 4 and 16
    # voices. Group order is the production order: gates, sources, routes,
    # voices, spreader. Gate closure must release every channel.
    extra=[]
    for count in (1,4,16):
        moddir=ROOT/'synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation'
        paths=[moddir/f'{name}{count}.scsyndef' for name in ('ModGate','ModLfo','ModEnv','ModRoute')]
        paths += [ROOT/'synthdefs/Defaults/PolyComb/CompiledSynthdefs/PolyComb/PolyCombSpread2.scsyndef']
        extra += [message('/d_recv',p.read_bytes()) for p in paths]
    score=bundle(0,extra)+score
    for test,count in enumerate((1,4,16)):
        t=len(bank)*duration+test*2.0+0.01
        add=[message('/c_setn',592,16,*([0.0]*16)),
            message('/s_new',f'ModGate{count}',1000,1,1,'out',32,'gate',1.0,'velocity',1.0,'vel',0,'rnd',16),
            message('/n_setn',1000,'gate',count,*([1.0]*count)),
            message('/s_new',f'ModLfo{count}',1100,1,1,'out',512,'freq',2.0),
            message('/s_new',f'ModEnv{count}',1101,1,1,'out',528,'gate',32,'atk',0.01,'dec',0.1,'sus',0.8,'rel',0.1),
            message('/s_new',f'ModRoute{count}',1102,1,1,'src',512,'out',560,'amount',0.5,'scale',24.0),
            message('/s_new',f'ModRoute{count}',1103,1,1,'src',528,'out',576,'amount',1.0,'scale',1.0)]
        params=[]
        for c in controls:params += [c['sc'],float(bank[0]['values'][c['name']])]
        for voice in range(count):
            add += [message('/s_new','AliasingBoxVoice',1001+voice,1,1,
                'gatebus',32+voice,'velbus',voice,'out',48+voice,'vis',2048,'voice',voice,'voices',count,
                'pitch',60.0+voice,*params,*sum(([n,32] for n in ['mpitch','mread','msym','mlp','mhp','mlpres','mhpres','mlpfm','mhpfm','mdrive']),[]),'mwrite',560+voice,'mamp',576+voice,'vca',1.0)]
        add += [message('/s_new','PolyCombSpread2',1200,1,1,'in',48,'out',0,'voices',count,'mpan',592,'vis',700)]
        score+=bundle(t,add)
        score+=bundle(t+1.0,[message('/n_setn',1000,'gate',count,*([0.0]*count))])
        score+=bundle(t+1.9,[message('/n_free',1000,1100,1101,1102,1103,1200,*range(1001,1001+count))])
    score+=bundle(len(bank)*duration+6.1,[message('/c_set',32,0.0)])
    scorepath=args.output/'factory.osc';scorepath.write_bytes(score)
    wav=args.output/'factory.wav'
    run=subprocess.run([args.server,'-N',str(scorepath),'_',str(wav),'48000','WAV','float','-o','2','-i','0','-z','64','-D','0','-w','2048','-m','65536'],text=True,capture_output=True)
    (args.output/'server.log').write_text(run.stdout+run.stderr)
    assert run.returncode==0,run.stderr
    assert 'FAILURE' not in run.stdout+run.stderr,run.stdout+run.stderr
    audio,channels,rate=read_wav(wav);report=[]
    for i,preset in enumerate(bank):
        start=round((i*duration+0.1)*rate)*channels;end=round((i*duration+1.1)*rate)*channels
        samples=audio[start:end:channels]
        assert all(math.isfinite(x) for x in samples),preset['name']
        peak=max(map(abs,samples));rms=math.sqrt(sum(x*x for x in samples)/len(samples))
        report.append(dict(name=preset['name'],peak=peak,rms=rms))
    (args.output/'measurements.json').write_text(json.dumps(report,indent=2)+'\n')
    assert all(x['peak']<=1 for x in report),'Output clips'
    for test,count in enumerate((1,4,16)):
        t=len(bank)*duration+test*2.0
        playing=audio[round((t+0.2)*rate)*channels:round((t+0.9)*rate)*channels]
        released=audio[round((t+1.5)*rate)*channels:round((t+1.8)*rate)*channels]
        assert all(math.isfinite(x) for x in playing+released),f'{count} voices: non-finite audio'
        assert max(map(abs,playing))>1e-5,f'{count} voices: modulation path silent'
        assert max(map(abs,released))<1e-5,f'{count} voices: gates did not release'
    print('PASS: LFO + envelope + matrix + stereo spread at 1, 4 and 16 voices; gates release to silence.')
    audible=sum(x['rms']>1e-5 for x in report)
    print(f"PASS: {len(report)} factory snapshots render finite, unclipped audio; {audible} above -100 dB RMS.")
    print('Output:',args.output)
    for r in report:
        if r['rms']<=1e-5:print('Quiet:',r['name'])

if __name__=='__main__':main()
