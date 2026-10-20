#!/usr/bin/env python3
"""Validate every translated value against native control types/ranges.
Optional --render auditions the candidate banks offline through the existing
PolyComb/Modulation/MixerPro SynthDefs. No app, presets or DSP files are edited.
This is a load/render check, not a Reaktor equivalence test.
"""
import argparse,hashlib,json,math,pathlib,subprocess,sys
ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
P=ROOT/'synthdefs/Defaults/PolyComb/ReaktorSnapshots'
def filterchoice(t):
    return (t,0) if t<4 else [(0,1),(0,2),(1,1),(1,2),(2,1),(2,2),(2,3),(3,2)][t-4]

def validate(folder=P):
    schema=json.loads((folder/'native-controls.json').read_text());raw=json.loads((folder/'recovered-snapshots.json').read_text())
    manifest=json.loads((folder/'bank-manifest.json').read_text())
    banks=[folder/f for f in manifest['files']];slots=[]
    profile=manifest['profile'];count={'1.7':237,'2018':472,'2025':373}[profile]
    assert all(1<=len(json.loads(p.read_text())['slots'])<=128 for p in banks)
    for p in banks:slots+=json.loads(p.read_text())['slots']
    assert len(slots)==len(raw['snapshots'])==manifest['snapshot_count']
    for i,(slot,source)in enumerate(zip(slots,raw['snapshots'])):
        assert slot['controls']['Ring_Model']==1
        assert slot['controls']['Comb_Model']==1 and slot['controls']['Comb_Saturation'] is False
        assert slot['name']==source['name'] and len(source['records'])==count
        assert len({r['snapshot_id']for r in source['records']})==count
        assert set(slot['controls'])==set(schema)
        assert len(slot["matrix"])==24*14 and all(math.isfinite(x)and -1<=x<=1 for x in slot['matrix'])
        for key,value in slot['controls'].items():
            meta=schema[key];typ=meta['type']
            if typ=='bool':assert type(value)is bool;continue
            if typ=='vector':assert type(value)is list and 1<=len(value)<=32
            elif typ=='int':assert type(value)is int
            else:assert type(value)is float
            for v in value if typ=='vector' else [value]:assert math.isfinite(v) and meta['min']<=v<=meta['max'],(i,key,v)
    if profile=='1.7':assert raw['snapshots'][0]['tables']['215'][:12]==list(range(12))
    if profile=='2018':assert len(slots)==86 and slots[0]['name']=='Science Fiction' and slots[-1]['name']=='xespills'
    if profile=='2025':assert len(slots)==403 and slots[0]['name']=='DESFASES-NOTCH' and slots[34]['name']=='SAPIENS_MELODRAMA'
    print(f'PASS: {profile}: {len(slots)} names in source order, {count*len(slots):,} unique state records, native types/ranges and matrix dimensions.')
    return slots

def render(slots,n=16,duration=12.0,output="/tmp/polycomb-bank-render",master=.8):
    import numpy as np
    out=pathlib.Path(output);out.mkdir(exist_ok=True)
    pc=ROOT/'synthdefs/Defaults/PolyComb/CompiledSynthdefs/PolyComb';mod=ROOT/'synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation';fx=ROOT/'synthdefs/Defaults/MixerPro/CompiledSynthdefs/mixerpro'
    names=['PCSub','PCFilterSlopes','PCFilterR','PCCombR','PCRingR','PCPitch','PCFm','PCOscSaw','PCOscTri','PCOscPulse','PCOscSine','PCOscSSaw','PCOscWhite','PCOscDust','PCOscImp','PCFilter','PCComb','PCRing','PCAmp']
    paths=[pc/f'{s}{n}.scsyndef' for s in names]+[pc/'PolyCombSpread2.scsyndef']+[mod/f'{s}{n}.scsyndef' for s in ('ModGate','ModEnv','ModEnvAr','ModLfo','ModLfoX','ModRoute')]+[fx/f'mixerPro{s}2.scsyndef' for s in ('Comp','Echo','Rvb')]
    score=bundle(0,[message('/d_recv',p.read_bytes())for p in paths]+[message('/g_new',1,0,0)])
    length=duration
    for i,s in enumerate(slots):
        c=s['controls'];msgs=[message('/c_setn',2000,15*16,*([0.0]*(15*16)))]
        nextid=100
        def synth(name,**params):
            nonlocal nextid
            id=nextid;nextid+=1;params0=sum(([k,float(v)]for k,v in params.items()if not isinstance(v,list)),[])
            msgs.append(message('/s_new',name,id,1,1,*params0))
            for k,v in params.items():
                if isinstance(v,list):
                    v=(v*n)[:n];msgs.append(message('/n_setn',id,k,n,*map(float,v)))
            return id
        def vec(key):return c[key]
        def mb(t):return 2000+t*16
        gate=synth(f'ModGate{n}',out=600,gate=[1.0],velocity=[1.0],vel=1192,rnd=1208)
        synth(f'ModEnvAr{n}',gate=600,out=3100,aout=616,atk=c['Amp_Attack'],dec=c['Amp_Decay'],sus=c['Amp_Sustain'],rel=c['Amp_Release'],cA=-4,cD=-4,cR=-4)
        for li in range(4):
            L=f'LFO{li+1}_';spread=c[L+'Spread']
            synth(f'ModLfoX{n}' if c.get(L+'Model',1)==3 else f'ModLfo{n}',rtype=1. if c.get(L+'Model',1)==3 else 0.,rsmooth=c.get(L+'Random_Smooth',0.),out=1000+li*16,freq=[2/x for x in vec(L+'Beats')],initphase=c[L+'Phase'],phoff=[v*spread/n for v in range(n)],round=vec(L+'Round'),skew=vec(L+'Skew'),pw=vec(L+'Width'),noise=c[L+'Noise'],shglide=c[L+'S&H_Glide'],gain=vec(L+'Gain'))
        synth(f'ModEnv{n}',gate=600,out=1128,atk=c['Env1_Attack'],dec=c['Env1_Decay'],sus=c['Env1_Sustain'],rel=c['Env1_Release'],velbus=1192)
        ranges=[24,1,4,48,1,24,1,1,1,48,10,.5,1,1]
        for src in range(23):
            for t in range(14):
                amt=s['matrix'][src*14+t]
                if abs(amt)>1e-6:synth(f'ModRoute{n}',src=1000+src*16,out=mb(t),amount=amt,scale=ranges[t],bipolar=src<8)
        synth(f'PCPitch{n}',out=512,mpitch=mb(0),pitch=[60.0],transpose=vec('Transpose'),detune=vec('Detune'),glide=c['Glide'],vibrate=c['Vib_Rate'],vibamt=c['Vib_Amount'])
        if c['FM_On']:synth(f'PCFm{n}',note=512,out=632,mamt=mb(1),mratio=mb(2),fmamt=vec('FM_Amount'),fmratio=vec('FM_Ratio'),fmfb=c['FM_Feedback'])
        for name,key in [('Saw','Saw'),('Tri','Tri'),('Pulse','Pulse'),('Sine','Sine'),('SSaw','SuperSaw'),('White','White'),('Dust','Dust'),('Imp','Impulse')]:
            if max(vec(key))>0:synth(f'PCOsc{name}{n}',note=512,out=648,fm=632,fmon=c['FM_On'],level=vec(key),oscamp=vec('Osc_Amp'),pw=vec('Pulse_Width'),mpw=mb(11),ssdetune=vec('SS_Detune'))
        synth(f'PCFilterR{n}' if c.get('Filter_Model',0)==1 else f'PCFilterSlopes{n}' if c['Filter_Type']>=4 else f'PCFilter{n}',note=512,**{'in':648},out=664,on=1,slope=filterchoice(c['Filter_Type'])[1],type=c['Filter_Type'] if c.get('Filter_Model',0)==1 else filterchoice(c['Filter_Type'])[0],cutoff=vec('Cutoff'),res=vec('Resonance'),mcut=mb(3),mres=mb(4))
        synth(f'PCCombR{n}',satlevel=c.get('Comb_Sat_Level',12),note=512,**{'in':664},out=680,on=1,combmode=c['Comb_Mode'],combpitch=vec('Comb_Pitch'),combtranspose=vec('Comb_Transpose'),combdetune=vec('Comb_Detune'),combfeed=vec('Comb_Feedback'),combtone=c['Comb_Tone'],combmix=vec('Comb_Mix'),mcpitch=mb(5),mcfb=mb(6),mcmix=mb(7))
        last=680
        if c['Ring_On']:
            synth(f'PCRingR{n}',note=512,**{'in':last},out=696,on=1,ringpitch=vec('Ring_Pitch'),ringtranspose=vec('Ring_Transpose'),ringdetune=vec('Ring_Detune'),ringmix=vec('Ring_Mix'),ringratio=c.get('Ring_Ratio',[1.0]),ringwidth=c.get('Ring_Width',0.0),mrmix=mb(8));last=696
        synth(f'PCFilterR{n}' if c.get('Filter_Model',0)==1 else f'PCFilterSlopes{n}' if c['Post_Type']>=4 else f'PCFilter{n}',note=512,**{'in':last},out=712,on=1,slope=filterchoice(c['Post_Type'])[1],type=c['Post_Type'] if c.get('Filter_Model',0)==1 else filterchoice(c['Post_Type'])[0],cutoff=vec('Post_Cutoff'),res=vec('Post_Resonance'),mcut=mb(9),mres=mb(14))
        synth(f'PCAmp{n}',**{'in':712},out=728,env=616,gate=600,vel=1192,velsens=1,level=master,mamp=mb(12))
        if c.get('Sub_On',False):
            synth(f'PCSub{n}',out=728,gate=600,vel=1192,vis=3400,pitch=[60.0],transpose=c['Sub_Transpose'],level=c['Sub_Level'],master=master,velsens=1,on=1,atk=c['Sub_Attack'],dec=c['Sub_Decay'],sus=c['Sub_Sustain'],rel=c['Sub_Release'],cA=c['Sub_Attack_Curve'],cD=c['Sub_Decay_Curve'],cR=c['Sub_Release_Curve'])
        echo=c['Echo_On'];rvb=c['Reverb_On']
        comp=c['Comp_On']
        synth('PolyCombSpread2',**{'in':728},out=744 if comp else 760 if echo or rvb else 0,voices=n,mpan=mb(13),vis=3200,pan=[v/max(1,n-1)for v in range(n)])
        if comp:synth('mixerProComp2',**{'in':744},out=760 if echo or rvb else 0,grBus=3300,threshold=c['Comp_Threshold'],ratio=c['Comp_Ratio'],knee=c['Comp_Knee'],attack=c['Comp_Attack'],release=c['Comp_Release'],makeup=c['Comp_Makeup'],automakeup=c['Comp_Auto_Makeup'],rms=c['Comp_RMS'],schpf=c['Comp_SC_HPF'],mix=c['Comp_Mix'],on=1)
        if echo:synth('mixerProEcho2',**{'in':760},out=776 if rvb else 0,on=1,delay=c['Echo_Delay'],feed=c['Echo_Feed'],mix=c['Echo_Mix'],cutoff=c['Echo_Cutoff'],resonance=c['Echo_Resonance'])
        if rvb:synth('mixerProRvb2',**{'in':776 if echo else 760},out=0,on=1,mix=c['Reverb_Mix'],decay=c['Reverb_Decay'],size=c['Reverb_Size'],lowpass=c['Reverb_Lowpass'],modfreq=c['Reverb_Mod_Rate'])
        score+=bundle(i*length+.01,msgs)
        score+=bundle(i*length+length*.9,[message('/n_setn',gate,'gate',n,*([0.0]*n))])
        score+=bundle((i+1)*length-.01,[message('/g_freeAll',1)])
    score+=bundle(len(slots)*length+.1,[message('/c_set',0,0.0)])
    path=out/'bank.osc';path.write_bytes(score);wav=out/'bank.wav'
    r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth','-N',str(path),'_',str(wav),'48000','WAV','float','-U',str(ROOT/'ugens/OceanodeAnalog')+':/Applications/SuperCollider.app/Contents/Resources/plugins','-o','2','-i','0','-a','1024','-c','4096','-z','64','-D','0','-w','4096','-m','262144'],text=True,capture_output=True)
    (out/'server.log').write_text(r.stdout+r.stderr)
    assert r.returncode==0 and 'FAILURE' not in r.stdout+r.stderr,r.stdout+r.stderr
    # Memory-map the float WAV: long attacks need a longer window, not a
    # multi-gigabyte Python tuple of the entire rendered bank.
    import struct
    with wav.open('rb') as f:
        f.seek(12)
        while True:
            tag,size=struct.unpack('<4sI',f.read(8));pos=f.tell()
            if tag==b'fmt ':fmt,ch,rate=struct.unpack('<HHI',f.read(8));assert fmt==3
            if tag==b'data':offset=pos;count=size//4;break
            f.seek(pos+size+(size%2))
    audio=np.memmap(wav,dtype='<f4',mode='r',offset=offset,shape=(count,));results=[]
    for i,s in enumerate(slots):
        full=audio[int(i*length*rate)*ch:int((i+1)*length*rate)*ch]
        values=audio[int((i*length+length*.6)*rate)*ch:int((i*length+length*.85)*rate)*ch]
        assert np.isfinite(full).all(),s['name']
        results.append(dict(index=i+1,name=s['name'],peak=float(np.max(np.abs(full))),sustained_peak=float(np.max(np.abs(values))),rms=float(np.sqrt(np.mean(values.astype('float64')**2)))))
    (out/'measurements.json').write_text(json.dumps(results,indent=2)+'\n')
    (out/'conditions.json').write_text(json.dumps(dict(master=master,voices=n,duration=duration,midi_note=60,bpm=120),indent=2)+'\n')
    print(f'PASS: {len(slots)} candidates render finite audio at {n} voices; {sum(x["rms"]>1e-7 for x in results)} audible within sustained test; reports in {out}')
    print(f'LEVEL AUDIT: {sum(x["peak"]>1 for x in results)} exceed full scale during the complete note/release window; maximum peak {max(x["peak"] for x in results):.4f}. Finite output does not imply a level-safe bank.')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--render',action='store_true');p.add_argument('--collection',choices=('1.7','2018','all'),default='1.7');a=p.parse_args()
    for version in (('1.7','2018')if a.collection=='all' else (a.collection,)):
        folder=P if version=='1.7' else P/version
        slots=validate(folder)
        if a.render:render(slots,output=f'/tmp/polycomb-bank-render-{version}')
