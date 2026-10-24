#!/usr/bin/env python3
"""Convert custom SuperFM/SuperFMDrone patches to FMBox and collect a matching bank.

Dry-run by default. --apply backs up every changed original before writing.
Retains live per-voice drone controls through FMBox Ext routes; Vector Getter
adapters retain packed six-operator inputs. Explicit envelope seconds and curve preserve the old gated envelopes.
"""
from pathlib import Path
import argparse, copy, hashlib, json, math, re, shutil, time
ROOT = Path(__file__).resolve().parents[1]
MODELS = {'SC SuperFM': False, 'SC SuperFMDrone': True}
def esc(s): return re.sub(r'[ <>\{\}\[\],()/\\.#]', '_', s)
def vec(v, default=0):
    if isinstance(v, list): return [float(x) for x in v]
    if isinstance(v, (int,float)): return [float(v)]
    if v is None: return [float(default)]
    return [float(x.strip()) for x in str(v).split(',') if x.strip()]
def one(d,k,default=0): return vec(d.get(k), default)[0]
def serial(v): return ', '.join(format(x,'.12g') for x in v) if isinstance(v,list) else str(v)
def load(p): return json.loads(p.read_text())
def dump(d): return json.dumps(d,indent=4,ensure_ascii=False)+'\n'
def digest(p): return hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else None

def native_fields():
    s=(ROOT/'src/nodes/boxSynths/fmBox/scDX7Data.h').read_text()
    def fields(name):
        section=s.split(' '+name+' = {{',1)[1].split('}};',1)[0]
        return [(a,b,int(c),int(d)) for a,b,c,d in re.findall(r'\{"([^"]+)","([^"]+)",(\d+),(\d+)\}',section)]
    return fields('globalFields'),fields('operatorFields')

TABLES = {k:json.loads(v) for k,v in re.findall(r'(\w+):\s*(\[[^\]]*\])',(ROOT/'synthdefs/Defaults/FMBox/dx7Data.inc').read_text())}
def env_level(amplitude):
    return max(0,min(99,99+20*math.log10(max(amplitude,1e-8))*4/3))
def env_duration(start,end,rate):
    # Same unscaled envelope timing as FMBoxVoice, with neutral key/velocity scaling.
    def emap(level,nxt=False):
        return 64*(2*level+(5 if nxt else 0)) if level<=5 else 64*(5+level) if level<=16 else 64*(4+level) if level<=20 else 64*(14+math.floor(level/2))
    speed=99-rate;a,b=emap(start),emap(end,True);vr=32*(99+28)
    decay=(max(1,min(9000,a+vr-4064))-max(1,min(9000,b+vr-4064)))*.0235/(.2819*2**(speed*.16))
    percent=TABLES['dx7_voice_eg_rate_rise_percent']
    rise=TABLES['voice_eg_rate_rise_duration'][int(speed+.5)]*(percent[int(end)]-percent[int(start)])
    return max(.00001,decay if a>b else rise)
def fit_rate(start,end,seconds):
    return min(range(100),key=lambda rate:abs(math.log(env_duration(start,end,rate)/max(seconds,.00001))))


def convert(state, drone, label):
    globals_,operators=native_fields();sound={esc(name):initial for name,_,_,initial in globals_}
    n=int(one(state,'N_Chan',1))
    if not 1<=n<=16: raise ValueError(f'{label}: {n} voices exceeds FMBox support')
    sound.update(Gate_Mode=2 if drone else 1,FM_Amount=one(state,'Scale',1),Envelope_Time=1,
                 Width=1,Placement=1,Speaker_Layout=0,Feedback=one(state,'Feedback',1),Osc_Sync=1,LFO_Sync=0,Matrix_Model=1,
                 Envelope_Model=0 if drone else 1,Envelope_Curve=-4,
                 Pitch_Depth=0,Amp_Depth=0,EQ_On=False,Echo_On=False,Reverb_On=False,Sat_On=False)
    levels=vec(state.get('EgLevels'),1);times=vec(state.get('EgTimes'),.25);duration=one(state,'Duration',2)
    matrix=vec(state.get('Matrix'),0);matrix=(matrix+[0]*36)[:36]
    amps=vec(state.get('Op_Amps'),0);ratios=vec(state.get('Op_Ratios'),1);detunes=vec(state.get('Op_Detunes'),0)
    for op in range(6):
        pre=f'Op_{op+1}_'
        for name,_,_,initial in operators:sound[pre+esc(name)]=initial
        ratio=one(state,f'Op_Ratio_{op+1}',1) if drone else ratios[op] if op<len(ratios) else 1
        detune=one(state,f'Op_Detune_{op+1}',0) if drone else detunes[op] if op<len(detunes) else 0
        hz=440*2**((one(state,'Pitch',60)-69)/12)
        sound[pre+'Coarse']=max(.5,min(31,ratio+detune/max(hz,.001)))
        sound[pre+'Fine']=0;sound[pre+'Detune']=7;sound[pre+'Output']=99
        amp=one(state,f'Op_Amp_{op+1}',1 if op==0 else 0) if drone else amps[op] if op<len(amps) else (1 if op==0 else 0)
        sound[pre+'Mix']=amp
        if drone:
            for k in range(4):sound[pre+f'Level_{k+1}']=99;sound[pre+f'Rate_{k+1}']=0
        else:
            old=[levels[op*4+k] if op*4+k<len(levels) else (1 if k<3 else 0) for k in range(4)]
            native=[env_level(old[k]) if k<3 else 0 for k in range(4)]
            previous=0;durations=[]
            for k in range(4):
                fraction=times[op*4+k] if op*4+k<len(times) else (k+1)*.25
                seconds=max(.001,fraction-previous)*duration*.5 # old engine: myTimes = 0.5 / derivedRate
                durations.append(seconds)
                sound[pre+f'Level_{k+1}']=native[k]
                previous=fraction
            for k,seconds in enumerate(durations):
                sound[pre+f'Rate_{k+1}']=fit_rate(native[k-1],native[k],seconds)
                sound[pre+f'Seconds_{k+1}']=seconds
        for source in range(6):
            sound[esc(f'FM {source+1} > {op+1}')]=matrix[op*6+source]
    sound.update(fmModLfos=0,fmModEnvs=0,fmModExts=0,fmModParams=[],fmModMatrix=[0.]*(23*23))
    sound['name']=label
    new={'Show':'0','Voices':str(n),'Channels':str(n),'Pitch':serial(vec(state.get('Pitch'),60)),
         'Gate':serial(vec(state.get('Gate'),0)),'Velocity':serial(vec(state.get('Levels'),1)),
         'Master':str(1 if drone else one(state,'Amp',1)), 'expanded':state.get('expanded',True),
         'fmBox':sound,'fmBoxPublished':[],'fmBoxTimeline':[]}
    return new

def source_values(folder,node,port,fallback):
    p=folder/(node+'.json')
    if not p.exists():return fallback
    d=load(p)
    for k in (esc(port),'Value','CSV_Input','Output','Out'):
        if k in d:
            try:return vec(d[k])
            except (ValueError,TypeError):pass
    return fallback

def plan(root,calibration=None):
    changes={};deletes=[];hashes={};report=[];bank=[]
    for base in ('Presets','Macros','Snapshots'):
      for mp in sorted((root/base).rglob('modules.json')):
        modules=load(mp)
        if not isinstance(modules,dict) or not any(m in modules for m in MODELS):continue
        folder=mp.parent;originals={p.name:load(p) for p in folder.glob('*.json')}
        newmodules=copy.deepcopy(modules);connections=copy.deepcopy(originals.get('connections.json',{}));newfiles={}
        def add_node(model,position,state):
            group=newmodules.setdefault(model,{})
            i=1
            while f'{i:02d}' in group:i+=1
            group[f'{i:02d}']=position
            node=esc(model)+'_'+str(i);newfiles[node+'.json']=state;return node
        def wire(source,port,target,destination):
            connections.setdefault(source,{}).setdefault(port,{}).setdefault(target,{})[destination]=None
        for model,drone in MODELS.items():
          for num,position in modules.get(model,{}).items():
            old=esc(model)+'_'+str(int(num));oldfile=old+'.json'
            if oldfile not in originals:raise ValueError(f'Missing state {folder/oldfile}')
            state=originals[oldfile];label='BIENAL '+str(folder.relative_to(root)).replace('/',' - ')
            new=convert(state,drone,label);sound=new['fmBox'];n=int(new['Voices'])
            node=add_node('SC fmBox',position,new)
            outgoing=connections.pop(old,{})
            inbound=[]
            for source,ports in connections.items():
                for port,targets in ports.items():
                    if old in targets:inbound.append((source,port,targets.pop(old)))
            ext_sources={};constant=None;adapters=[]
            def ext(source,port,values):
                identity=(source,port)
                if identity not in ext_sources:
                    k=len(ext_sources);ext_sources[identity]=k
                    if k>=8:raise ValueError('External-source limit exceeded')
                    sound[f'Ext_{k+1}']=values;new['fmBoxPublished'].append(f'Ext_{k+1}')
                    if source:wire(source,port,node,f'Ext {k+1}')
                return ext_sources[identity]
            def route(k,target,amount):sound['fmModMatrix'][(14+k)*23+target]=amount
            def publish(name):
                key=esc(name)
                if key not in new['fmBoxPublished']:new['fmBoxPublished'].append(key)
            for source,port,inputs in inbound:
              for name in inputs:
                if name=='N Chan':
                    wire(source,port,node,'Voices');wire(source,port,node,'Channels')
                elif name in ('Pitch','Gate'):
                    wire(source,port,node,name)
                elif name in ('Op Amps','Op Ratios'):
                    fallback=vec(state.get(esc(name)),1 if name=='Op Ratios' else 0)
                    values=source_values(folder,source,port,fallback)
                    for op in range(min(6,len(values))):
                        getter=add_node('Vector Getter',[position[0]+400,position[1]+100+len(adapters)*55],
                                        {'Index':str(op),'Input':serial(values),'expanded':False})
                        target=f'Op {op+1} '+('Mix' if name=='Op Amps' else 'Coarse')
                        sound[esc(target)]=values[op];publish(target)
                        wire(source,port,getter,'Input');wire(getter,'Output',node,target);adapters.append(getter)
                elif name=='Amp' and not drone:wire(source,port,node,'Master')
                elif name=='Levels':wire(source,port,node,'Velocity')
                elif name=='Amp' or name=='Scale' or re.fullmatch(r'Op Amp [1-6]',name):
                    values=source_values(folder,source,port,vec(state.get(esc(name)),1))
                    if any(abs(v)>1 for v in values):raise ValueError(f'Ext range unsupported: {source}/{port}: {values}')
                    k=ext(source,port,values)
                    if name=='Amp':
                        route(k,3,1)
                        if constant is None:constant=ext('','constant',[1.])
                        route(constant,3,-1)
                    elif name=='Scale':sound['FM_Amount']=0;route(k,1,.25)
                    else:
                        target=f"Op_{name.rsplit(' ',1)[1]}_Mix";sound[target]=0
                        sound['fmModParams'].append({'key':target,'base':[0.],'amounts':{str(14+k):1.}})
                else:raise ValueError(f'Unmapped live input: {old}/{name}')
            sound['fmModExts']=len(ext_sources)
            # Generic scalar Ext routes contribute half-range: scale static carriers
            # equally, then compensate the resulting gain below.
            half=any('Op Amp ' in name for _,_,inputs in inbound for name in inputs)
            if half:
                for op in range(6):
                    key=f'Op_{op+1}_Mix'
                    if not any(p['key']==key for p in sound['fmModParams']):sound[key]*=.5
            # Match the saved mix level despite FMBox's -12 dB and voice/carrier normalization.
            mix_sum=sum(sound[f'Op_{i+1}_Mix'] for i in range(6))
            for p in sound['fmModParams']:
                mix_sum+=sum(sound[f'Ext_{int(s)-14+1}'][0]*amount*.5 for s,amount in p['amounts'].items())
            compensation=n*max(mix_sum,1)*10**(.6)*(2 if half else 1)
            correction=(calibration or {}).get(str(folder.relative_to(root)),0)
            compensation*=10**(-correction/20)
            remaining=compensation;chain=node;gain_nodes=[]
            while remaining>1.00001:
                gain=min(8.,remaining);remaining/=gain
                g=add_node('SC LinearGain*',[position[0]+140+len(gain_nodes)*140,position[1]+400],
                           {'N_Chan':str(n),'Gain':str(gain),'expanded':False})
                wire(chain,'Out',g,'In');chain=g;gain_nodes.append(g)
                for source,port,inputs in inbound:
                    if 'N Chan' in inputs:wire(source,port,g,'N Chan')
            for port,targets in outgoing.items():
                for target,inputs in targets.items():
                    for destination in inputs:wire(chain if port=='Out' else node,port,target,destination)
            newmodules[model].pop(num)
            if not newmodules[model]:del newmodules[model]
            deletes.append(folder/oldfile);bank.append(copy.deepcopy(sound))
            report.append({'folder':str(folder.relative_to(root)),'old':old,'new':node,'voices':n,
                           'vector_adapters':adapters,'gain_stages':gain_nodes,'gain_compensation':compensation,
                           'render_gain_correction_db':-correction,
                           'note':'Explicit envelope seconds and curve retain the original gated shape; gain is matched at saved settings.'})
        newfiles.update({'modules.json':newmodules,'connections.json':connections})
        for name,data in newfiles.items():changes[folder/name]=dump(data)
    if bank:
        changes[root/'nodeSnapshots/FMBoxDX7/banks/BIENAL migrated FM.json']=dump({'snapshotBank':1,'slots':bank})
        for name in ('FMBoxVoice','FMBoxDX7LFO'):
            compiled=ROOT/'synthdefs/Defaults/FMBox/CompiledSynthdefs/FMBox'/(name+'.scsyndef')
            changes[root/'Supercollider/Synthdefs/Defaults/FMBox'/compiled.name]=compiled.read_bytes()
    for p in changes.keys()|set(deletes):hashes[str(p)]=digest(p)
    return changes,deletes,hashes,report

def validate(changes,report):
    for item in report:
        node=item['new'];folder_suffix=item['folder']
        matching=[p for p in changes if str(p.parent).endswith(folder_suffix) and p.name=='connections.json'];assert len(matching)==1
        con=json.loads(changes[matching[0]]);new=json.loads(changes[matching[0].parent/(node+'.json')])
        allowed={'Voices','Channels','Pitch','Gate','Velocity','Master','Out'}|set(new['fmBoxPublished'])
        for source,ports in con.items():
            assert item['old']!=source
            for port,targets in ports.items():
                assert item['old'] not in targets
                for target,inputs in targets.items():
                    if target==node:
                        for name in inputs:assert name in allowed or esc(name) in allowed,(node,name)
        assert len(new['fmBox']['fmModMatrix'])==23*23
        assert new['fmBox']['Gate_Mode'] in (1,2)
        for param in new['fmBox']['fmModParams']:
            assert re.fullmatch(r'Op_[1-6]_Mix',param['key']),param['key']
            assert param['key'] in new['fmBox']

def apply(root,changes,deletes,hashes,report):
    for name,expected in hashes.items():
        if digest(Path(name))!=expected:raise RuntimeError('File changed after preparation: '+name)
    backup=root/'Presets_backup_BoxSynthMigration'/time.strftime('%Y%m%d-%H%M%S')
    backup.mkdir(parents=True,exist_ok=False)
    for p in changes.keys()|set(deletes):
        if p.exists():
            target=backup/p.relative_to(root);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,target)
    (backup/'report.json').write_text(dump({'converted':report,'original_hashes':hashes,'new_files':[str(p.relative_to(root)) for p in changes if hashes[str(p)] is None]}))
    for p,text in changes.items():
        p.parent.mkdir(parents=True,exist_ok=True)
        p.write_bytes(text) if isinstance(text,bytes) else p.write_text(text)
    for p in deletes:p.unlink()
    return backup

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('data',type=Path);parser.add_argument('--apply',action='store_true');parser.add_argument('--stage',type=Path)
    parser.add_argument('--calibration',type=Path,help='First-pass A/B measurements.json; match measured output level')
    args=parser.parse_args();root=args.data.resolve()
    calibration={item['folder']:item['level_difference_db'] for item in load(args.calibration)} if args.calibration else None
    changes,deletes,hashes,report=plan(root,calibration);validate(changes,report)
    print(dump({'converted':report,'write_files':len(changes),'remove_files':len(deletes)}))
    if args.stage:
        args.stage.mkdir(parents=True,exist_ok=True)
        for p,text in changes.items():
            target=args.stage/p.relative_to(root);target.parent.mkdir(parents=True,exist_ok=True)
            target.write_bytes(text) if isinstance(text,bytes) else target.write_text(text)
        (args.stage/'migration-report.json').write_text(dump({'converted':report,'original_hashes':hashes}))
    if args.apply:print('Backup:',apply(root,changes,deletes,hashes,report))
if __name__=='__main__':main()
