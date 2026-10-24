#!/usr/bin/env python3
"""Recover PolyComb 1.7 snapshots and create candidate banks for the EXISTING node.
Never edits DSP, GUI, compiled SynthDefs, installed presets or the source ensemble.
Pitch maps and DSP/model differences are explicitly audited; these are translations,
not a claim of audio-identical emulation. Original state payloads remain available.
"""
import argparse,copy,functools,hashlib,json,math,pathlib,re,struct,subprocess
import pitchmap
ROOT=pathlib.Path(__file__).resolve().parents[2]
DEST=ROOT/'synthdefs/Defaults/PolyComb/ReaktorSnapshots'
def write(path,value):path.write_text(json.dumps(value,indent=2,ensure_ascii=False,allow_nan=False)+'\n')
def clamp(v,a,b):return max(a,min(b,v))
def lfo_shape(wave):
    """Native morph positions: triangle=0, cosine=.5, square=1."""
    return 1.0 if wave==3 else 0.0 if wave in (1,2,4) else .5

def pow_amount(p):
    """The shared LFO's Pow (customPow, -1..1) closest to x^p on 0..1 (the original's power)."""
    if abs(p-1)<1e-3:return 0.0
    xs=[i/64 for i in range(65)]
    def f(x,a):
        k2=2*a*0.99999/((-a*0.999999)+1);return x*(k2+1)/max(1e-6,k2*abs(x)+1)
    best=min(((sum((x**p-f(x,a))**2 for x in xs),a) for a in [i/200-0.995 for i in range(399)]))
    return best[1]
# the LFO waveform Lists' values: 0 saw up, 1 saw down, 2 pulse, 3 triangle,
# 4 sine, 5 stepped random, 6 smooth random (bRWN2), 7 jumps (mono only);
# a value past the List's end is its last entry
LFO_RANDOM={5:1,6:2,7:3}
def pulse_duty(width):
    """The original pulse: high from the cycle's start for 0.5 + W/2 of it (W -1..1).
    The shared LFO's square keeps its width only up to 2/3 (wider, it splits)."""
    return clamp(.5+width*.5,.01,.66)
def lfo_phase(wf,phase,width):
    """The shared LFO's square is centred on the cycle (high around 0.5): the
    original's pulse starts high, so it is read 0.5 - duty/2 later."""
    return (phase+(.5-pulse_duty(width)/2 if wf==2 else 0))%1
def lfo_wave(put,L,row,count,width,smooth):
    # snapshots store the List's row from 1 (5: Sin); its value is the row - 1
    # (0 Saw, 1 SawI, 2 Pls, 3 Tri, 4 Sin, 5 Rnd, 6 bRWN2, 7 jumps); past the end: the last row
    wf=min(max(int(row)-1,0),count-1)
    put(L+'Random_Type',LFO_RANDOM.get(wf,0));put(L+'Random_Smooth',smooth if wf==5 else 0.0)
    put(L+'Noise',False);put(L+'S&H_Glide',0.0)
    put(L+'Round',{2:1.0,3:0.0,0:0.0,1:0.0}.get(wf,.5))
    put(L+'Skew',-1 if wf==0 else 1 if wf==1 else 0)
    put(L+'Width',pulse_duty(width) if wf==2 else .5);put(L+'Centered',True)
    return wf
def lfo_span(gain,offset):
    """Endpoint fit to the source's clip(gain * unipolar + offset, 0, 1).

    Exact when neither endpoint clips (or both yield the same constant).
    An asymmetric clipped interior still needs the source transfer function.
    """
    lo=clamp(offset,0,1);hi=clamp(offset+gain,0,1)
    return (lo+hi)*.5,(hi-lo)*.5
# PolyComb 1.7's Scale Mode list (1-based) -> ChordBox's dictionary
# (Supercollider/Pitchclass/scales.txt; the missing ones appended at its end)
SCALE_NAMES_17=['chromatic','ionian','aeolian','dorian','phrygian','lydian','mixolydian','locrian','majorBlues','minorBlues',
    'majPent','minPent','japan','spanish','wholetone','minorThirds','majorThirds','fourths','fifths','octave',
    'messiaen3','messiaen4','messiaen5','messiaen6','messiaen7','lydianDominant','melodicMajor']
@functools.lru_cache(None)
def library_scales():
    path=ROOT.parent.parent/'apps/Playmodes/OceanAudio/bin/data/Supercollider/Pitchclass/scales.txt'
    out={}
    for line in path.read_text().splitlines():
        first=line.split(';')[0].split(',')
        if len(first)<2:continue
        words=first[1].split()
        if words:out[words[0]]=[float(x) for x in words[1:]]
    missing=[n for n in SCALE_NAMES_17 if n not in out]
    assert not missing,f'scales.txt lacks {missing}'
    return out
@functools.lru_cache(None)
def library_chords():
    """ChordBox's chords.txt by index: the 2018 Harmony's chord List follows its order (row N = entry N)."""
    path=ROOT.parent.parent/'apps/Playmodes/OceanAudio/bin/data/Supercollider/Pitchclass/chords.txt'
    out={}
    for line in path.read_text().splitlines():
        first=line.split(';')[0].split(',')
        if len(first)<2 or not first[0].strip().isdigit():continue
        words=first[1].split()
        if words:out[int(first[0])]=(words[0],[float(x) for x in words[1:]])
    assert out[1][0]=='Fifth' and out[34][0]=='add9',path
    return out
def user_chord(raw,offset,count):
    """2018: a Harmony slot's User chord: the first `count` values of its Event Table's row 1
    (12 floats a row, from byte 753 + 48 of the module), as pitch classes."""
    base=offset+753+48
    values=[struct.unpack_from('<f',raw,base+4*i)[0] for i in range(max(1,min(12,int(round(count)))))]
    return sorted({int(round(x))%12 for x in values})
def closest_chord(pcs):
    """The dictionary chord (any root) sharing the most pitch classes with pcs: (root, name, tones)."""
    T=set(pcs);best=None
    for index,(name,tones) in library_chords().items():
        S={int(t)%12 for t in tones}
        for root in range(12):
            R={(t+root)%12 for t in S}
            key=(len(R&T)/len(R|T),-abs(len(R)-len(T)),-root,-index)
            if best is None or key>best[0]:best=(key,root,name,tones)
    return best[1],best[2],best[3]
# 2018 presets whose Harmony slot plays its User chord (the slot's mode is not in the snapshots)
USER_CHORD_2018={'xespills'}
def controls_from_source():
    """Literal defaults/ranges from the current production sources, no app execution."""
    cpp=(ROOT/'src/nodes/boxSynths/polyComb/scPolyComb.cpp').read_text();header=(ROOT/'src/nodes/boxSynths/polyComb/scPolyComb.h').read_text()
    types={}
    for typ,names in re.findall(r'ofParameter<([^<>]+)>\s+([^;]+);',header):
        for name in names.split(','):
            name=name.strip().split('[')[0];types[name]=typ
    out={}
    def number(s):return float(s.rstrip('fF'))
    for var,name,d,lo,hi in re.findall(r'(\w+)\.set\("([^"]+)",\s*([-\d.]+f?),\s*([-\d.]+f?),\s*([-\d.]+f?)\)',cpp):
        if var not in types or name in ('Voices','Channels','Master'):continue
        typ=types[var]
        if typ not in ('float','int'):continue
        out[name.replace(' ','_')]=dict(name=name,default=int(number(d)) if typ=='int' else number(d),min=number(lo),max=number(hi),type=typ)
    for var,name,val in re.findall(r'(\w+)\.set\("([^"]+)",\s*(true|false)\)',cpp):
        if var in types and name not in ('Show','Scramble'):out[name.replace(' ','_')]=dict(name=name,default=val=='true',type='bool')
    out['Ring_Pitch_Source']=dict(name='Ring Pitch Source',default=1,min=0,max=2,type='int')
    out['Scramble']=dict(name='Scramble',default=False,type='bool')
    for var,name,d,lo,hi in re.findall(r'vec\((\w+),\s*"([^"]+)",\s*([-\d.]+f?),\s*([-\d.]+f?),\s*([-\d.]+f?)\)',cpp):
        out[name.replace(' ','_')]=dict(name=name,default=[number(d)],min=number(lo),max=number(hi),type='vector')
    out['Comb_Pitch']=dict(name='Comb Pitch',default=[0.0],min=-48,max=127,type='vector')
    # Pitch & Harmony maps and Delta: set in loops / with named limits in the source
    for pre in ('Osc_Map_','Comb_Map_'):
        for k,(d,lo,hi,typ) in dict(Shape=(0,0,2,'int'),First=(1.0,0,32,'float'),Interval=(3.0,-24,24,'float'),Transpose=(0.0,-72,72,'float'),
                Bend=(1.0,1,6,'float'),Mode=(0,0,3,'int'),Quantize=(0,0,2,'int'),Root=(0.0,0,11,'float'),Harmonic_Base=(36.0,0,127,'float'),
                Fine_Tune=(0.0,-1,1,'float'),Span=(0.0,0,48,'float')).items():out[pre+k]=dict(name=(pre+k).replace('_',' '),default=d,min=lo,max=hi,type=typ)
        out[pre+'Tones']=dict(name=pre.replace('_',' ')+'Tones',default=[0.0,2,4,5,7,9,11],min=0,max=24,type='vector')
        for k,d,lo,hi,typ in [('New_Seed_On_Gate',False,0,1,'bool'),('Cycle_Shift',0,-31,31,'int'),('Scale',0,0,1024,'int'),('Skew',-1.,-1,1,'float'),('Roundness',0.,0,1,'float'),('PW',.5,.001,1,'float'),('Pow',0.,-1,1,'float'),('Octave_Jump',1.0,0,4,'float'),('Num_Waves',1.0,0,16,'float'),('Version',2,2,2,'int'),('Degree_Offset',0.,-256,256,'float'),('Invert',False,0,1,'bool'),('Symmetry',0,0,32,'int'),('Comb',0.,0,1,'float'),('Index_Quantize',0.,0,16,'float'),('Index_Random',0.,0,16,'float'),('Seed',0,0,9999,'int')]:
            out[pre+k]=dict(name=(pre+k).replace('_',' '),default=d,min=lo,max=hi,type=typ)
        out[pre+'Table']=dict(name=pre.replace('_',' ')+'Table',default=[float(i) for i in range(12)],min=0,max=11,type='vector')
    for k,d in (('Delta_LFO_A',0),('Delta_LFO_B',1)):out[k]=dict(name=k.replace('_',' '),default=d,min=0,max=7,type='int')
    out['Pan']=dict(name='Pan',default=[.5],min=0,max=1,type='vector')
    for b,f in zip(('Low','Lo_Mid','Mid','Hi_Mid','High'),(80,250,1000,4000,12000)):
        for suffix,d,lo,hi in (('',0,-24,24),('_Freq',f,20,20000),('_Slope' if b in ('Low','High') else '_Q',1,.1,4 if b in ('Low','High') else 20)):
            key='EQ_'+b+suffix;out[key]=dict(name=key.replace('_',' '),default=float(d),min=lo,max=hi,type='float')
    mod=(ROOT/'src/nodes/boxSynths/shared/scModulation.cpp').read_text()
    # Defaults are the shared source setup's literal values, repeated per source.
    vectors={'Beats':(1,.125,512),'Hz':(1,0,50),'S&H':(0,0,32),'Detune':(0,0,8),'Round':(.5,0,1),'Skew':(0,-1,1),'Width':(.5,0,1),'Gain':(1,1,16),'Pow':(0,-1,1),'BiPow':(0,-1,1)}
    scalars={'Phase':(0,0,1),'S&H_Glide':(0,0,10),'Noise_Scale':(1,0,10),'Spread':(1,0,16),'Invert':(0,0,1),'Shuffle':(0,0,1),'Random':(0,0,1),'Offset':(0,-8,8),'Combination':(0,0,1)}
    for i in range(1,9):
        for suffix,(d,lo,hi) in vectors.items():
            if suffix=='Beats':d=2**min(i-1,4)
            out[f'LFO{i}_{suffix}']=dict(default=[float(d)],min=lo,max=hi,type='vector')
        for suffix,(d,lo,hi) in scalars.items():out[f'LFO{i}_{suffix}']=dict(default=float(d),min=lo,max=hi,type='float')
        for suffix,d in [('Sync',True),('Centered',True),('Noise',False),('Normalize',False)]:out[f'LFO{i}_{suffix}']=dict(default=d,type='bool')
        for suffix,d,hi in [('Symmetry',0,8),('Quantize',32,32),('Modulo',32,32),('Random_Type',0,3),('Steps',0,32)]:out[f'LFO{i}_{suffix}']=dict(default=d,min=0 if suffix in ('Symmetry','Random_Type','Steps') else 1,max=hi,type='int')
        out[f'LFO{i}_Model']=dict(default=1,min=0,max=3,type='int')
        out[f'LFO{i}_Value']=dict(default=0.0,min=0,max=1,type='float')
        out[f'LFO{i}_Random_Smooth']=dict(default=0.0,min=0,max=10,type='float')
    for e in range(1,5):
        for suffix,d,lo,hi in [('Attack',.01,.001,10),('Decay',.5,.001,10),('Sustain',0,0,1),('Release',.5,.001,20),('Attack_Curve',-4,-12,12),('Decay_Curve',-4,-12,12),('Release_Curve',-4,-12,12),('Vel',0,0,1)]:
            out[f'Env{e}_{suffix}']=dict(default=float(d),min=lo,max=hi,type='float')
    return out

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('ensemble',type=pathlib.Path)
    ap.add_argument('--extractor',type=pathlib.Path,default=ROOT/'tools/ReaktorPatchExtractor/dist/Reaktor Patch Extractor.app/Contents/MacOS/ReaktorPatchExtractor')
    ap.add_argument('--voices',type=int,choices=range(1,33),help='default: the source\'s own (1.7: 24, later: 32)');ap.add_argument('--output',type=pathlib.Path,default=DEST)
    ap.add_argument('--profile',choices=('1.7','2018','2025'),default='1.7')
    ap.add_argument('--bank',help='Import only this named source bank')
    ap.add_argument('--chord',help='2025: the Harmony instrument\'s first chord, as root,tone,tone,... in semitones (e.g. 2,0,4,7): '
                    'both maps quantized to it (Tones). The Harmony is not in the snapshots: one chord for every preset')
    args=ap.parse_args()
    if args.voices is None:args.voices=24 if args.profile=='1.7' else 32
    raw=args.ensemble.read_bytes();report=json.loads(subprocess.check_output([str(args.extractor.resolve()),str(args.ensemble.resolve())],text=True))
    modules={}
    def walk(ms):
        for m in ms:modules[m['object_id']]=m;walk(m['children'])
    walk(report['top_level'])
    candidates={}
    for m in modules.values():
        if 'snapshot_control_id'in m:candidates.setdefault(m['snapshot_control_id'],[]).append(m)
    panels={sid:ms[0]for sid,ms in candidates.items()if len(ms)==1}
    if args.profile=='2025':
        # Snapshot IDs are local to instruments. Harmony reuses some low IDs;
        # these inner TEST-CHANS states belong to the main PolyComb instrument.
        for sid,ms in candidates.items():
            scoped=[m for m in ms if m.get('hierarchy_path','').startswith('/Instrument#143572/')]
            if len(scoped)==1:panels[sid]=scoped[0]
    if args.profile=='1.7':assert len(panels)==len(candidates),'Ambiguous panel identities'
    expected={'1.7':237,'2018':472,'2025':373}[args.profile]
    snapshots=[s for s in report['snapshots']if not args.bank or s.get('bank_name')==args.bank]
    assert snapshots,'Requested bank not found'
    assert all(s['parameter_count']==expected and s['object_id_mapping_method']=='stable_panel_reference' for s in snapshots), 'Incomplete or unvalidated extraction'
    descriptors=[]
    for sid,m in sorted(panels.items()):
        descriptors.append(dict(snapshot_id=sid,module_id=m['object_id'],path=m.get('hierarchy_path'),kind=m['kind'],properties=m.get('control_properties'),saved_raw=m.get('saved_control_raw_u32')))
    schema=controls_from_source();defaults={k:v['default'] for k,v in schema.items()}
    recovered=[];translated=[];audits=[]
    # Named scales are only a pitch-grid approximation: NI's exact quantizer,
    # tie behavior and scale degree indexing have not yet been ported.
    scales=[list(range(12)),[0,2,4,5,7,9,11],[0,2,3,5,7,8,10],[0,2,3,5,7,9,10],[0,1,3,5,7,8,10],[0,2,4,6,7,9,11],[0,2,4,5,7,9,10],[0,1,3,5,6,8,10],[0,2,3,4,7,9],[0,3,5,6,7,10],[0,2,4,7,9],[0,3,5,7,10],[0,1,5,7,8],[0,1,4,5,7,8,10],[0,2,4,6,8,10],[0,3,6,9],[0,4,8],[0,5,10],[0,7],[0],[0,2,3,4,6,7,8,10,11],[0,1,2,5,6,7,8,11],[0,1,5,6,7,11],[0,2,4,5,6,8,10,11],[0,1,2,3,5,6,7,8,9,11],[0,2,4,6,7,9,10]]
    for ordinal,snap in enumerate(snapshots,1):
        values={};records=[];tables={}
        name_of_snapshot=snap.get('container_name',snap['name']) if args.profile!='1.7' else snap['name']
        cursor=int(snap['offset'],16)+4+len(snap['name'].encode())+108+snap['metadata_extension_bytes']
        for r in snap['parameters']:
            sid=struct.unpack_from('<I',raw,cursor-4)[0];size=struct.unpack_from('<I',raw,cursor)[0]
            assert sid==r['local_module_object_id'] and size==r['record_size']
            if 'value'in r:values[sid]=r['value']
            if size==548 and r['value_type_code']==4:
                tables[sid]=[struct.unpack_from('<f',raw,cursor+40+i*8)[0] for i in range(64)]
            records.append(dict(snapshot_id=sid,type=r['value_type'],type_code=r['value_type_code'],size=size,value=r.get('value'),payload_words=r.get('payload_words')))
            cursor+=size+8
        effective={};origins={}
        for sid,m in panels.items():
            if sid in values:
                v=values[sid];origins[str(sid)]='snapshot'
                r=next(r for r in records if r['snapshot_id']==sid)
                if r['type']=='normalized_float':
                    props=m.get('control_properties',{});lo=props.get('range_endpoint_1',0);hi=props.get('range_endpoint_2',1);v=lo+(hi-lo)*v
            else:
                origins[str(sid)]='saved global state (empty/isolated in snapshot)'
                if m['kind'] in (23,24):v=m.get('saved_control_raw_u32',1)
                elif 'saved_control_value'in m:v=m['saved_control_value']
                else:
                    bits=m.get('saved_control_raw_u32',0);v=struct.unpack('<f',struct.pack('<I',bits))[0]
            effective[sid]=v
        # Switches without an active panel reference still have explicit integer
        # snapshot records. Do not interpret unknown normalized records as units.
        for r in records:
            if r['type']=='integer' and r['value'] is not None:effective.setdefault(r['snapshot_id'],r['value'])
        if args.profile!='1.7':
            from later_profiles import projection
            projected,profile_issues,static_notes=projection(args.profile,effective,args.voices)
        else:projected=effective;profile_issues=[];static_notes=None
        def v(sid,default=0):return projected.get(sid,default)
        state=copy.deepcopy(defaults);state.update({'Osc_Map_Offset':0.,'Comb_Map_Offset':0.});matrix=[0.0]*(24*14);issues=[];clips=[];mapped=set();params=[]   # 23 shared sources + Delta
        issues.extend(profile_issues)
        def put(key,value):
            if key in ('Osc_Map_Offset','Comb_Map_Offset'):
                state[key]=float(clamp(value,-24,24));return  # legacy post-map shift; migrated after translation
            assert key in schema,key
            if key.endswith('Map_Table'):
                state[key]=[float(x) for x in value];return
            meta=schema[key];typ=meta['type']
            if typ=='bool':state[key]=bool(value);return
            arr=value if isinstance(value,list) else [value];checked=[]
            for x in arr:
                assert math.isfinite(x),(ordinal,key,x)
                y=clamp(x,meta['min'],meta['max'])
                if abs(y-x)>1e-5:clips.append(dict(control=key,source_value=x,used_value=y))
                checked.append(int(round(y)) if typ=='int' else float(y))
            state[key]=checked if typ=='vector' else checked[0]
        def direct(key,sid):put(key,v(sid));mapped.add(sid)
        def route(source,target,amount):matrix[source*14+target]+=amount
        put('Drone',True);put('Scramble',False)   # Drone: the converted sounds hold (no gate needed);put('Placement',0);put('Spread',1)
        shape=int(round(v(11,1)));shape_names={1:'Saw',2:'SuperSaw',3:'Pulse',4:'Tri',5:'Sine',6:'White',7:'Impulse',8:'Dust'}
        for key in ('Saw','SuperSaw','Pulse','Tri','Sine','White','Pink','Impulse','Dust'):put(key,1.0 if shape_names.get(shape)==key else 0.0)
        if shape in (2,8):issues.append('Two-saw/geiger oscillator approximated by SuperSaw/Dust')
        put('Pulse_Width',(v(348)+1)/2);mapped.update((11,348))
        def pitches(main):
            ids=(44,45,124,495,128,219,216,220,215) if main else (29,30,125,496,127,232,231,233,230)
            interval,trans,circular,bend,fine,mode,scale,base,table=ids
            notes=[]
            for i in range(1,args.voices+1):
                pos=args.voices/2-abs(i-args.voices/2)+1 if v(circular)>0 else i
                pos=(pos/args.voices)**v(bend,1)*args.voices
                note=60+v(trans)+pos*v(interval)
                choice=int(v(mode,1))
                if choice==2:
                    degree=int(v(scale,1))-1;allowed=scales[degree] if 0<=degree<len(scales) else list(range(12))
                    note=min((12*octave+pc for octave in range(-10,30) for pc in allowed),key=lambda x:(abs(x-note),x))
                elif choice==1:
                    tablevals=tables.get(table,list(range(64)));octave=math.floor(note/12);pc=int(math.floor(note%12));note=octave*12+tablevals[pc]
                elif choice==3:
                    # Static harmonic-grid approximation at MIDI 60, not NI's dynamic quantizer.
                    baseHz=440*2**((v(base,60)-69)/12);hz=440*2**((note-69)/12);harmonic=max(1,round(hz/baseHz));note=69+12*math.log2(baseHz*harmonic/440)
                notes.append(note+v(fine)+v(37)+v(351)+v(380))
            mapped.update((interval,trans,circular,bend,fine,mode,scale,base));return notes
        pitch_mean,pitch_depth=lfo_span(v(207),v(208))
        if args.profile=='1.7':
            # The original's pitch maps, live (Pitch & Harmony tab, External off):
            # 60 + Transpose (+ the global transpose, before quantizing) + Interval x
            # index, Tonal Mode, then the post-quantization offsets (fine, the global
            # pitch, LFO2's resting offset). The played Pitch moves it around 60.
            maps={}
            for main in (True,False):
                interval,trans,circular,bend,fine,mode,scale,base,table=(44,45,124,495,128,219,216,220,215) if main else (29,30,125,496,127,232,231,233,230)
                pre='Osc_Map_' if main else 'Comb_Map_'
                choice=int(v(mode,1));tones=list(range(12))
                if choice==2:
                    name=SCALE_NAMES_17[int(v(scale,1))-1] if 1<=int(v(scale,1))<=len(SCALE_NAMES_17) else 'chromatic'
                    tones=library_scales()[name]
                tab=[float(x) for x in tables.get(table,list(range(12)))[:12]]
                m=dict(shape=1 if v(circular)>0 else 0,first=1.0,interval=v(interval),transpose=v(trans)+v(351),bend=v(bend,1),
                       offset=v(fine)+v(37)+v(380)+(v(55) if main else v(56))*pitch_mean,mode={1:pitchmap.TABLE,2:pitchmap.TONES,3:pitchmap.HARMONIC}.get(choice,pitchmap.OFF),
                       quantize=pitchmap.NEAREST,root=0.0,tones=tones,table=tab,harmonic_base=v(base,60))
                maps[main]=m
                for k,x in (('Shape',m['shape']),('First',m['first']),('Interval',m['interval']),('Transpose',m['transpose']),('Bend',m['bend']),('Offset',m['offset']),
                            ('Mode',m['mode']),('Quantize',m['quantize']),('Root',m['root']),('Tones',[float(x) for x in tones]),('Table',tab),('Harmonic_Base',m['harmonic_base'])):put(pre+k,x)
                mapped.update((interval,trans,circular,bend,fine,mode,scale,base))
            mapped.update((37,351,380))
            put('Pitch_External',False);put('Pitch_Shift',1 if v(356)>0 else 0);put('Comb_Map_On',True)
            put('Transpose',0.0);put('Comb_Pitch',0.0);put('Comb_Transpose',0.0);put('Comb_Mode',1)
            mainnotes,combnotes=pitchmap.generate_legacy(maps[True],args.voices),pitchmap.generate_legacy(maps[False],args.voices)
            issues.append('Live pitch maps (Pitch & Harmony): NI\'s scale quantizer tables are unrecoverable from the file; scales use the dictionary with nearest-note quantization')
        else:
            # The later versions' pitch grids as live maps (External off):
            # 2018: 60 + transpose + interval x voice (0..31), fine and global after.
            # 2025: 60 + transpose + ((i/(N-1) x length + offset) mod 1) x step x 12:
            #       Interval x index wrapping every step x 12 (Span), the offset as First.
            N=args.voices;maps={}
            for main in (True,False):
                pre='Osc_Map_' if main else 'Comb_Map_'
                m=dict(shape=0,first=0.0,interval=0.0,transpose=0.0,bend=1.0,offset=0.0,mode=pitchmap.OFF,quantize=pitchmap.NEAREST,root=0.0,
                       tones=[0.0,2,4,5,7,9,11],table=[float(i) for i in range(12)],harmonic_base=36.0,span=0.0)
                if args.profile=='2018':
                    # the Pitch Set panel: Step per voice; Invert: the set descends from its base
                    m['interval']=effective.get(1244 if main else 1275,0)*32/N
                    if effective.get(1247 if main else 1278,0)>.25:m['interval']=-m['interval']
                    m['transpose']=v(45) if main else v(30);m['offset']=v(37)+(v(128) if main else v(127))
                    # the Harmony sequences (Sequence OSC / RESO): their first slot's chord (a row of
                    # ChordBox's chords.txt) and its root fader, the map quantized to its tones
                    row,root=(1078,1079) if main else (1148,1149)
                    chords=library_chords();row=min(max(int(effective.get(row,34)),1),max(chords))
                    name,tones=chords.get(row,chords[34]);key=float(effective.get(root,0))
                    if name_of_snapshot in USER_CHORD_2018:
                        # its User chord, as the dictionary's closest chord
                        table=modules[83117 if main else 84245];count=effective.get(1085 if main else 1155,5)
                        shift,name,tones=closest_chord(user_chord(raw,int(table['offset'],16),count));key+=shift
                        issues.append(f'2018: {"oscillator" if main else "resonator"} User chord {user_chord(raw,int(table["offset"],16),count)} as {name} on {shift}')
                    m['mode']=pitchmap.TONES;m['root']=key%12;m['tones']=sorted({t%12 for t in tones})
                else:
                    step,length,off,trans=(2332,2333,2334,2565) if main else (2604,2605,2606,2624)
                    step,length,off=effective.get(step,1),effective.get(length,1),effective.get(off,0)
                    span=step*12
                    m['transpose']=effective.get(trans,0);m['offset']=v(37)+(0 if main else effective.get(1737,0))
                    m['bend']=effective.get(2335 if main else 2607,1)
                    if abs(length)>1e-6 and N>1:
                        m['interval']=span*length/(N-1);m['first']=(off*(N-1)/length)%((N-1)/abs(length))
                    else:m['transpose']+=(off%1)*span
                    m['span']=span if abs(length)>1e-6 else 0.0
                    if m['first']>128:issues.append('2025 grid offset beyond the map\'s First range: clipped');m['first']=128.0
                    if args.chord:
                        chord=[float(x) for x in args.chord.split(',')]
                        m['mode']=pitchmap.TONES;m['root']=chord[0]%12;m['tones']=sorted({t%12 for t in chord[1:]}) or [0.0]
                maps[main]=m
                for k,x in (('Shape',m['shape']),('First',m['first']),('Interval',m['interval']),('Transpose',m['transpose']),('Bend',m['bend']),('Offset',m['offset']),
                            ('Mode',m['mode']),('Quantize',m['quantize']),('Root',m['root']),('Tones',m['tones']),('Table',m['table']),('Harmonic_Base',m['harmonic_base']),('Span',m['span'])):put(pre+k,x)
            put('Pitch_External',False);put('Pitch_Shift',0);put('Comb_Map_On',True)
            put('Transpose',0.0);put('Comb_Pitch',0.0);put('Comb_Transpose',0.0);put('Comb_Mode',1)
            mainnotes,combnotes=pitchmap.generate_legacy(maps[True],N),pitchmap.generate_legacy(maps[False],N)
            put('Osc_Map_Offset',state['Osc_Map_Offset']+v(55)*pitch_mean);put('Comb_Map_Offset',state['Comb_Map_Offset']+v(56)*pitch_mean)
            mainnotes=[p+v(55)*pitch_mean for p in mainnotes];combnotes=[p+v(56)*pitch_mean for p in combnotes]
            issues.append('Live pitch grid (Pitch & Harmony); '+('the Harmony sequence approximated by its first slot\'s chord (Tones)' if args.profile=='2018' or (args.profile=='2025' and args.chord)
                          else 'this version\'s Harmony instrument (chords / scales / overtones) is not translated'))
        issues.append('Original stereo voice randomization is approximated by uniform speaker spread')
        direct('Detune',49);direct('Comb_Detune',36);put('Vib_Amount',v(38)+v(272));put('Vib_Rate',v(39));mapped.update((38,39,272))
        if abs(v(272))>1e-6:issues.append('Separate global and oscillator vibratos combined; oscillator vibrato rate omitted')
        if args.profile=='2018':
            # the oscillators' Pitch Set vibrato (Tr-Poly): depth vibA st, each voice's rate
            # uniformly random in 0 .. vibF (the node: Rate x (1 +- Random)); added to the
            # global one, the deeper one's rate kept
            vib_a,vib_f=effective.get(873,0),effective.get(874,0)
            if vib_a>1e-6 and vib_f>1e-6:
                put('Vib_Amount',v(38)+vib_a)
                if vib_a>=v(38):put('Vib_Rate',vib_f/2);put('Vib_Rate_Random',1.0)
            # Rndm recalls one of the Pitch Set's stored random orders: a random voice order instead
            put('Scramble',effective.get(1249,0)>0 or effective.get(1280,0)>0)
            if effective.get(871,0)>1e-6:issues.append('2018: the resonators\' Pitch Set vibrato has no counterpart (their mapped notes are fixed)')
        put('FM_Ratio',2**(v(344)/12));put('FM_Ratio_Snap',False)
        put('FM_Amount',[v(347)/(8*440*2**((note-69)/12)) for note in mainnotes]);put('FM_On',v(347)>1e-6);mapped.update((344,347))
        issues.append('FM depth translated from Hz at MIDI 60; the new ratio/amount ranges may clip and vary under pitch modulation')
        # Reaktor is the only native comb; preserve each snapshot's saturation level.
        put('Comb_Model',1);put('Comb_Saturation',False);direct('Comb_Sat_Level',7)
        if args.profile!='1.7':
            issues.append('Comb2 resonator uses the Reaktor PolyComb 1.7 comb; its loop damping/high-pass differ')
        direct('Comb_Feedback',18);direct('Comb_Mix',19);put('Comb_On',True);put('Comb_Tone',1)
        fmap={1:6,2:8,3:4,4:9,5:10,6:5}
        for key,sid in [('Filter_Type',10),('Post_Type',13)]:
            val=int(v(sid,3));put(key,fmap.get(val,0))
        # 1.7 / 2018: Reaktor's Multi/LP 4-Pole, the ladder model (its outputs as the types)
        if args.profile in ('1.7','2018'):put('Filter_Model',1)
        if args.profile=='2025':
            for key,family,poles in [('Filter_Type',2092,2094),('Post_Type',2101,2103)]:
                four=int(effective.get(poles,2))>=3
                put(key,{1:5 if four else 4,2:7 if four else 6,3:9 if four else 8,4:11 if four else 3}.get(int(effective.get(family,1)),4))
        put('Filter_On',True);put('Post_Filter_On',True);direct('Cutoff',8);direct('Resonance',9);direct('Post_Cutoff',21);direct('Post_Resonance',12)
        put('Post_Cutoff',v(21)+v(58)*pitch_mean)
        put('Key_Track',0)
        for key,sid in [('Amp_Attack',226),('Amp_Decay',227),('Amp_Release',229),('Env1_Attack',154),('Env1_Decay',155),('Env1_Release',157)]:put(key,.001*10**(v(sid)/20));mapped.add(sid)
        direct('Amp_Sustain',228);direct('Env1_Sustain',156);put('Vel_Sens',1);put('Osc_Amp',clamp(v(240)/2,0,1))
        if abs(v(359,1)-1)>1e-5:issues.append('Power applied to the whole amplitude envelope is not the same as segment curves')
        put('Glide',.001*10**(v(436)/20));mapped.add(436)
        # Eight native LFOs are available; four represent the original two poly + two mono sources.
        for li,ids in enumerate(((178,179,181,182,193,194,195,200,238,242,243,268),(204,205,207,208,209,210,211,212,237,260,261,270))):
            length,power,scale,offset,wave,num,den,pwm,phase,period,sym,random=ids;L=f'LFO{li+1}_'
            put(L+'Sync',True);put(L+'Beats',v(den,1)/max(1,v(num,1)));put(L+'Phase',v(phase)%1)
            # the stepped random's lowpass: 1000 / (its Fader) Hz -> a one-pole time constant
            smooth=v(202 if li==0 else 213,9999)/(2*math.pi*1000)
            wf=lfo_wave(put,L,v(wave,5),7,v(pwm),smooth)
            put(L+'Phase',lfo_phase(wf,v(phase),v(pwm)))
            # Preserve cycle count across a chosen finite pool approximately, with built-in indexer.
            cycles=(1+v(length)) if v(length)<0 else round(1+15*v(length))
            # rndm reorders the voices' indices: Shuffle (not Random, not noise)
            put(L+'Spread',abs(cycles)*args.voices/max(1,v(period,80)));put(L+'Random',0.0);put(L+'Shuffle',clamp(v(random)/40,0,1))
            put(L+'Quantize',args.voices);put(L+'Modulo',args.voices)
            put(L+'Pow',[pow_amount(v(power,1))]);put(L+'BiPow',0)
            # its quantizer: LFO1 a step size (0..1), LFO2 a step count (0..12): levels = 1/step + 1
            q=v(180) if li==0 else v(206);levels=(round(1/q)+1 if q>1e-6 else 0) if li==0 else (int(round(q))+1 if q>=1 else 0)
            put(L+'Steps',min(32,levels))
            if abs(v(power,1)-1)>.001 or v(random)>0 or v(sym)!=80:issues.append(f'Poly LFO {li+1}: power, voice-index symmetry/randomization and phase distribution approximated')
            if li==0:
                # Native Amp is additive around Osc Amp: source is bipolar.
                gain=v(181,1);off=v(182);mean,depth=lfo_span(gain,off)
                put('Osc_Amp',mean*v(240)/2);route(0,12,depth/max(.0001,mean))
                if min(off,off+gain)<0 or max(off,off+gain)>1:
                    issues.append('Amplitude LFO clipped transfer fitted at its endpoints; interior clipping differs')
            else:
                # the comb reads the note before modulation: its own depth, not the difference
                route(1,0,v(55)*pitch_depth/24);route(1,5,(v(56) if args.profile=='1.7' else v(56)-v(55))*pitch_depth/24);route(1,9,v(58)*pitch_depth/48)
                if min(v(208),v(208)+v(207))<0 or max(v(208),v(208)+v(207))>1:
                    issues.append('Pitch LFO clipped transfer fitted at its endpoints; interior clipping differs')
            mapped.update(ids)
        envamt=v(159);route(8,9,envamt/48);mapped.add(159)
        if args.profile!='1.7':
            # The later versions move both filters with the poly LFOs: Pre-Cut
            # + 1345 x LFO1 + 986 x LFO2, Post-Cut + 1343 x LFO1 (+ 58 x LFO2, above).
            # Each LFO's resting value goes into the cutoff, its swing into a route.
            m1,d1=lfo_span(v(181,1),v(182));m2,d2=lfo_span(v(207,1),v(208))
            pre1,pre2,post1=effective.get(1345,0),effective.get(986,0),effective.get(1343,0)
            put('Cutoff',v(8)+pre1*m1+pre2*m2);route(0,3,pre1*d1/48);route(1,3,pre2*d2/48)
            put('Post_Cutoff',state['Post_Cutoff'][0]+post1*m1);route(0,9,post1*d1/48)
            if abs(pre1)>1e-6 or abs(pre2)>1e-6 or abs(post1)>1e-6:issues.append('Filter cutoffs follow the poly LFOs: resting value in the cutoff, swing routed (clipped LFO transfer fitted at its endpoints)')
        issues.append('Poly LFO offsets and transfer curves use existing bipolar routing approximations')
        for li,ids in enumerate(((360,361,362,364,365,369,376,377,378),(385,386,387,389,390,391,392,393,394)),3):
            width,wave,amount,phase,quant,ease,dest,num,den=ids;L=f'LFO{li}_';wf=int(v(wave,5))
            put(L+'Sync',True);put(L+'Beats',v(den,1)/max(1,v(num,1)));put(L+'Phase',v(phase)%1);put(L+'Spread',0)
            # ease: a lowpass at 3 .. 1 .. 0.01 Hz (scanned by the ease Fader; 0: none)
            e=v(ease);hz=3+(1-3)*min(e,1)+((0.01-1)*(e-1) if e>1 else 0)
            wf=lfo_wave(put,L,v(wave,5),8,v(width),1/(2*math.pi*hz) if e>1e-6 else 0.0)
            put(L+'Phase',lfo_phase(wf,v(phase),v(width)))
            put(L+'Quantize',args.voices);put(L+'Modulo',args.voices)
            put(L+'Steps',min(32,int(round(v(quant)))+1) if v(quant)>=1 else 0)    # step 1 / quant
            destination=int(v(dest,1))-1;amt=v(amount)   # the row from 1 -> the List's value
            # The "send to" List's own values (not its row): 1 post-cut x60 st, 2 FM x5000 Hz,
            # 3 resonator mix, 4 transport x24 st and 5 interval x12 (both BEFORE the Tonal
            # Mode: quantized into the scale), 6 pitch x12 st (after), 7 ring amount, 8 pwm x2,
            # 9/10 the poly LFOs' AM phase, 11 pre-cut x100 st.
            routes={1:(9,60/48),2:(1,5000/(440*8)),3:(7,1),6:(0,12/24),7:(8,1),8:(11,1),11:(3,100/48)}
            if destination in routes and amt>1e-6:
                target,scale=routes[destination];route(li-1,target,amt*scale)
                issues.append(f'Mono LFO {li-2}: unipolar output translated to bipolar routing; baseline/shape differ')
            elif destination in (4,5) and amt>1e-6:
                # the map's Transpose / Interval, moved by the LFO (Modulate): the scale snaps it
                key,depth,span=('Osc_Map_Transpose',24,96) if destination==4 else ('Osc_Map_Interval',12,48)
                if key in state:
                    put(key,state[key]+depth*amt/2)
                    params.append(dict(key=key,base=[state[key]],amounts={str(li-1):clamp(depth*amt/span,-1,1)}))
                    issues.append(f'Mono LFO {li-2}: {"transport" if destination==4 else "interval"} modulation moves the oscillators map before its quantization (Modulate, frame rate)')
            elif destination not in (0,1) and amt>1e-6:issues.append(f'Mono LFO {li-2} destination {destination} (ring ratio / LFO AM phase) has no equivalent target')
            if v(quant)>0 or v(ease)>0 or wf in (7,8,10):issues.append(f'Mono LFO {li-2}: quantization/easing/random algorithm approximated')
            mapped.update(ids)
        # the original's ring modulator (Ring Model 1: PCRingR): Ratio x each voice's note, Width
        # shaping the modulator, Amount from dry to fully modulated, a random detune per voice
        put('Ring_Model',1);put('Ring_Ratio',[v(460,1)]);put('Ring_Width',v(457));direct('Ring_Mix',461);direct('Ring_Detune',592)
        put('Ring_On',v(461)>1e-6 or abs(matrix[2*14+8])>1e-6 or abs(matrix[3*14+8])>1e-6);mapped.update((457,460))
        if v(461)>0:issues.append('Original pulse-width/ratio ring oscillator approximated by existing sine ring modulator')
        # Existing effects used as translations; originals are different algorithms.
        direct('Reverb_Mix',75);put('Reverb_On',v(75)>1e-6);put('Reverb_Decay',.1+9.9*v(84));put('Reverb_Size',60*v(83));put('Reverb_Mod_Rate',v(85)*10);direct('Reverb_Wander',86);put('Reverb_Lowpass',440*2**((v(87)-69)/12))
        direct('Echo_Mix',450);direct('Echo_Feed',443);put('Echo_On',v(450)>1e-6);put('Echo_Sync',False);put('Echo_Delay',max(.005,1.5*v(444)**2));direct('Echo_Cutoff',446);direct('Echo_Resonance',456)
        if v(75)>0:issues.append('Original modulated reverb approximated by shared MixerPro reverb')
        if args.profile=='1.7':
            # the original's delay itself (Echo Model 1): its loop lowpass / resonance /
            # highpass and L / R lines; Feed and Mix as they are (the model halves / squares them)
            hz=lambda p:440*2**((p-69)/12)
            put('Echo_Model',1);put('Echo_Hi',hz(v(446,118)));put('Echo_Lo',hz(v(447,30)));put('Echo_Res',v(456));put('Echo_Spread',v(445))
            put('Echo_Feed',v(443))
            if v(450)>0:issues.append('Delay: the original loop (PolyComb 1.7 echo model); its tempo-synced time quantization is not translated')
        elif v(450)>0:issues.append('Delay timing/filter/LR controls approximated by shared echo; original quantized sync not fully translated')
        put('EQ_On',False);put('Shaper_On',False)
        # NI's Core gain computer multiplies excess dB by -Ratio (0..1).
        # MixerPro uses a conventional input/output ratio and milliseconds.
        put('Comp_On',int(v(493,1))==1)
        direct('Comp_Threshold',481);direct('Comp_Knee',486);direct('Comp_Makeup',484)
        put('Comp_Ratio',1/max(.0001,1-v(483)))
        put('Comp_Attack',10**(v(480)/20));put('Comp_Release',10**(v(482)/20))
        put('Comp_Auto_Makeup',False);put('Comp_RMS',0);put('Comp_SC_HPF',20);put('Comp_Mix',1)
        mapped.update((480,482,483,493))
        issues.append('Original compressor translated to MixerPro: ballistics/detector, stereo-link switch and headroom saturation differ; source saturation not recreated')
        if v(442)>0:issues.append('Cabinet tilt/drive/mix has no equivalent insert in current PolyComb')
        # NI sub dynamics follow audio, not MIDI gate. The independent native
        # sine uses an ADSR approximation; do not synthesize a level for inactive
        # saved layers. 2025 signed modulation is frozen at its midpoint.
        if args.profile=='2025':
            base=effective.get(2011,0);amod=effective.get(2740,0)
            subgain=abs(base+0.5*amod)
            # as in 1.7: absolute over the global pitch offset (octave 2 + 12 st = MIDI 36), around 60 here
            subtrans=12*effective.get(2008,0)+effective.get(2009,0)+effective.get(2706,0)+0.5*effective.get(2708,0)+v(37)-60
            subrel=effective.get(2010,0)
            if amod or effective.get(2708,0):issues.append('2025 sub pitch/amplitude modulation frozen at unipolar midpoint; signed amplitude represented by magnitude (phase differs)')
        else:
            subgain=effective.get(589,0)/16
            # the original sub plays 12 x octave + 12 + semitones over the global pitch
            # offset (0 at the played note 60), not over the note: around 60 here
            subtrans=12*effective.get(590,0)+12+effective.get(591,0)+v(37)+v(380)-60
            subrel=effective.get(653,0)
        put('Sub_On',subgain>1e-9);put('Sub_Level',subgain);put('Sub_Transpose',subtrans)
        put('Sub_Attack',state['Amp_Attack']);put('Sub_Decay',state['Amp_Decay']);put('Sub_Sustain',state['Amp_Sustain'])
        put('Sub_Release',max(state['Amp_Release'],.001*10**(subrel/20)))
        if subgain>1e-9:
            issues.append('Independent sine sub: gate ADSR replaces source audio follower; source tremolo/shaper and additive sends omitted; pitch is a static input-relative approximation')
            if args.profile!='2025':issues.append('Sub gain normalized from source 0..16 to 0..1 because source follower amplitude is unavailable')
        # DELTA: how much LFO1 x mix + LFO2 x mix moved over the window (the matrix's
        # Delta row), to the oscillators' and the resonators' pitch in semitones.
        # Depths over the Pitch target's 24 st go into the source's gain.
        d1,d2=v(572),v(577);mapped.update((564,565,572,577,578,579))
        put('Delta_LFO_A',0);put('Delta_LFO_B',1);put('Delta_Mix_A',v(579));put('Delta_Mix_B',v(578));put('Delta_Window',max(1,v(564,1000)))
        g=max(1.0,abs(d1)/24,abs(d2)/24);put('Delta_Gain',v(565,3)*g)
        if abs(d1)>1e-6:route(23,0,d1/(24*g))
        if abs(d2)>1e-6:route(23,5,d2/(24*g))
        if (abs(d1)>1e-6 or abs(d2)>1e-6) and v(565,3)*g>10:issues.append('Delta gain over the native 10 is clipped')
        # Isolated/global stages are retained in raw recovery, not silently claimed as imported.
        issues.append('Original four-band parametric EQ is not recreated; saved global settings remain in audit')
        for i,x in enumerate(matrix):
            if abs(x)>1:clips.append(dict(control=f'matrix[{i}]',source_value=x,used_value=clamp(x,-1,1)));matrix[i]=clamp(x,-1,1)
        name=snap.get('container_name',snap['name']) if args.profile!='1.7' else snap['name']
        recovered.append(dict(index=ordinal,name=name,inner_name=snap['name'],bank=snap.get('bank_name'),bank_index=snap.get('bank_index'),bank_slot=snap.get('bank_slot'),offset=snap['offset'],effective_values={str(k):x for k,x in effective.items()},origins=origins,tables={str(k):x for k,x in tables.items()},records=records))
        slot=dict(name=name,controls=state,matrix=matrix,params=params,lfos=4,envs=1,exts=0,voices=args.voices)
        if max(state['Osc_Amp'])<=1e-9 and not state['Sub_On']:
            slot['importNote']='The main sound layer is muted in this translation; additional source layers are incomplete.'
            issues.append('Main oscillator bank is muted; unported independent layers can leave this translation silent')
        translated.append(slot)
        audit=dict(index=ordinal,name=name,bank=snap.get('bank_name'),limitations=sorted(set(issues)),clipped=clips,source_main_notes=mainnotes,source_comb_notes=combnotes)
        audit['mapped_panel_ids' if args.profile=='1.7' else 'projected_legacy_control_ids']=sorted(mapped)
        audits.append(audit)
    from migration import migrate
    for slot in translated:
        for pre in ('Osc_Map_','Comb_Map_'):slot['controls'][pre+'Version']=1
        for i in range(1,9):slot['controls'].pop(f'LFO{i}_Model',None)
    for slot,controls in zip(translated,migrate([s['controls'] for s in translated])):slot['controls']=controls
    args.output.mkdir(parents=True,exist_ok=True)
    write(args.output/'source-controls.json',descriptors)
    write(args.output/'native-controls.json',schema)
    write(args.output/'recovered-snapshots.json',dict(source=args.ensemble.name,sha256=hashlib.sha256(raw).hexdigest(),snapshot_count=len(recovered),snapshots=recovered))
    identical={}
    for slot in translated:
        key=json.dumps({k:v for k,v in slot.items()if k!='name'},sort_keys=True)
        identical.setdefault(key,[]).append(slot['name'])
    write(args.output/'translation-audit.json',dict(status='candidate approximation; not an audio-identical bank',profile=args.profile,voices=args.voices,pitch_reference=60,ambiguous_panel_ids=sorted(set(candidates)-set(panels)),equivalent_translation_groups=[names for names in identical.values()if len(names)>1],snapshots=audits))
    groups={}
    for slot,source in zip(translated,recovered):groups.setdefault(source['bank'] if args.profile!='1.7' else '1.7',[]).append(slot)
    files=[]
    for bank,slots in groups.items():
        for start in range(0,len(slots),128):
            end=min(len(slots),start+128)
            title=f'Reaktor PolyComb {args.profile}'+(f' - {bank}' if args.profile!='1.7' else '')
            if len(slots)>128:title+=f' - {start+1:03}-{end:03}'
            path=args.output/(re.sub(r'[/\\:]','_',title)+'.json')
            write(path,dict(snapshotBank=1,sourceVersion=args.profile,sourceBank=bank,translation='live pitch maps' if args.profile=='1.7' else 'static approximation',slots=slots[start:end]));files.append(path.name)
    write(args.output/'bank-manifest.json',dict(profile=args.profile,files=files,snapshot_count=len(recovered)))
    print(f'Recovered {len(recovered)} x {expected} records; created {len(files)} candidate banks for {args.voices} voices. DSP/GUI untouched.')
if __name__=='__main__':main()
