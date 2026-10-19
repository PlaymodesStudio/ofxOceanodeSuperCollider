#!/usr/bin/env python3
"""Recover PolyComb 1.7 snapshots and create candidate banks for the EXISTING node.
Never edits DSP, GUI, compiled SynthDefs, installed presets or the source ensemble.
Pitch maps and DSP/model differences are explicitly audited; these are translations,
not a claim of audio-identical emulation. Original state payloads remain available.
"""
import argparse,copy,hashlib,json,math,pathlib,re,struct,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[2]
DEST=ROOT/'synthdefs/Defaults/PolyComb/ReaktorSnapshots'
def write(path,value):path.write_text(json.dumps(value,indent=2,ensure_ascii=False,allow_nan=False)+'\n')
def clamp(v,a,b):return max(a,min(b,v))
def lfo_shape(wave):
    """Native morph positions: triangle=0, cosine=.5, square=1."""
    return 1.0 if wave==3 else 0.0 if wave in (1,2,4) else .5

def lfo_span(gain,offset):
    """Endpoint fit to the source's clip(gain * unipolar + offset, 0, 1).

    Exact when neither endpoint clips (or both yield the same constant).
    An asymmetric clipped interior still needs the source transfer function.
    """
    lo=clamp(offset,0,1);hi=clamp(offset+gain,0,1)
    return (lo+hi)*.5,(hi-lo)*.5
def controls_from_source():
    """Literal defaults/ranges from the current production sources, no app execution."""
    cpp=(ROOT/'src/nodes/scPolyComb.cpp').read_text();header=(ROOT/'src/nodes/scPolyComb.h').read_text()
    types={}
    for typ,names in re.findall(r'ofParameter<([^<>]+)>\s+([^;]+);',header):
        for name in names.split(','):
            name=name.strip().split('[')[0];types[name]=typ
    out={}
    def number(s):return float(s.rstrip('fF'))
    for var,name,d,lo,hi in re.findall(r'(\w+(?:\[\d+\])?)\.set\("([^"]+)",\s*([-\d.]+f?),\s*([-\d.]+f?),\s*([-\d.]+f?)\)',cpp):
        if var.split('[')[0] not in types or name in ('Voices','Channels','Master'):continue
        typ=types[var.split('[')[0]]
        if typ not in ('float','int'):continue
        out[name.replace(' ','_')]=dict(name=name,default=int(number(d)) if typ=='int' else number(d),min=number(lo),max=number(hi),type=typ)
    for var,name,val in re.findall(r'(\w+(?:\[\d+\])?)\.set\("([^"]+)",\s*(true|false)\)',cpp):
        if var.split('[')[0] in types and name not in ('Show','Scramble'):out[name.replace(' ','_')]=dict(name=name,default=val=='true',type='bool')
    out['Scramble']=dict(name='Scramble',default=False,type='bool')
    for var,name,d,lo,hi in re.findall(r'vec\((\w+(?:\[\d+\])?),\s*"([^"]+)",\s*([-\d.]+f?),\s*([-\d.]+f?),\s*([-\d.]+f?)\)',cpp):
        out[name.replace(' ','_')]=dict(name=name,default=[number(d)],min=number(lo),max=number(hi),type='vector')
    out['Comb_Pitch']=dict(name='Comb Pitch',default=[0.0],min=-48,max=127,type='vector')
    out['Pan']=dict(name='Pan',default=[.5],min=0,max=1,type='vector')
    for b,f in zip(('Low','Lo_Mid','Mid','Hi_Mid','High'),(80,250,1000,4000,12000)):
        for suffix,d,lo,hi in (('',0,-24,24),('_Freq',f,20,20000),('_Slope' if b in ('Low','High') else '_Q',1,.1,4 if b in ('Low','High') else 20)):
            key='EQ_'+b+suffix;out[key]=dict(name=key.replace('_',' '),default=float(d),min=lo,max=hi,type='float')
    mod=(ROOT/'src/nodes/scModulation.cpp').read_text()
    # Defaults are the shared source setup's literal values, repeated per source.
    vectors={'Beats':(1,.125,512),'Hz':(1,0,50),'S&H':(0,0,32),'Detune':(0,0,8),'Round':(.5,0,1),'Skew':(0,-1,1),'Width':(.5,0,1),'Gain':(1,1,16),'Pow':(0,-1,1),'BiPow':(0,-1,1)}
    scalars={'Phase':(0,0,1),'S&H_Glide':(0,0,10),'Noise_Scale':(1,0,10),'Spread':(1,0,16),'Invert':(0,0,1),'Shuffle':(0,0,1),'Random':(0,0,1),'Offset':(0,-8,8),'Combination':(0,0,1)}
    for i in range(1,9):
        for suffix,(d,lo,hi) in vectors.items():
            if suffix=='Beats':d=2**min(i-1,4)
            out[f'LFO{i}_{suffix}']=dict(default=[float(d)],min=lo,max=hi,type='vector')
        for suffix,(d,lo,hi) in scalars.items():out[f'LFO{i}_{suffix}']=dict(default=float(d),min=lo,max=hi,type='float')
        for suffix,d in [('Sync',True),('Centered',True),('Noise',False),('Normalize',False)]:out[f'LFO{i}_{suffix}']=dict(default=d,type='bool')
        for suffix,d,hi in [('Symmetry',0,8),('Quantize',16,16),('Modulo',16,16)]:out[f'LFO{i}_{suffix}']=dict(default=d,min=0 if suffix=='Symmetry' else 1,max=hi,type='int')
    for e in range(1,5):
        for suffix,d,lo,hi in [('Attack',.01,.001,10),('Decay',.5,.001,10),('Sustain',0,0,1),('Release',.5,.001,20),('Attack_Curve',-4,-12,12),('Decay_Curve',-4,-12,12),('Release_Curve',-4,-12,12),('Vel',0,0,1)]:
            out[f'Env{e}_{suffix}']=dict(default=float(d),min=lo,max=hi,type='float')
    libs=pitch_libraries()
    for key,name in (('Pitch_Scale','major'),('Pitch_Chord','M')):
        lib=libs[0 if key=='Pitch_Scale' else 1]
        out[key]=dict(name=key.replace('_',' '),default=next((i for i,x in enumerate(lib)if x[0]==name),0),min=0,max=len(lib)-1,type='int')
    return out

def pitch_libraries():
    base=ROOT.parent.parent/'apps/Playmodes/OceanAudio/bin/data/Supercollider/Pitchclass'
    def parse(name):
        lib=[]
        for line in (base/name).read_text().splitlines():
            first=line.split(';')[0].split(',')
            if len(first)<2:continue
            words=first[1].split()
            if words:lib.append((words[0],[float(v)for v in words[1:]]))
        assert lib,base/name
        return lib
    return parse('scales.txt'),parse('chords.txt')

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('ensemble',type=pathlib.Path)
    ap.add_argument('--extractor',type=pathlib.Path,default=ROOT/'tools/ReaktorPatchExtractor/dist/Reaktor Patch Extractor.app/Contents/MacOS/ReaktorPatchExtractor')
    ap.add_argument('--voices',type=int,choices=range(1,33),help='Override native voice count (default: original 1.7=24, later=32)');ap.add_argument('--output',type=pathlib.Path,default=DEST)
    ap.add_argument('--profile',choices=('1.7','2018','2025'),default='1.7')
    ap.add_argument('--bank',help='Import only this named source bank')
    args=ap.parse_args();raw=args.ensemble.read_bytes();report=json.loads(subprocess.check_output([str(args.extractor.resolve()),str(args.ensemble.resolve())],text=True))
    if args.voices is None:args.voices=24 if args.profile=='1.7' else 32
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
        state=copy.deepcopy(defaults);matrix=[0.0]*(24*14);issues=[];clips=[];mapped=set()
        issues.extend(profile_issues)
        def put(key,value):
            assert key in schema,key
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
        put('Env_Mode',1);put('Scramble',False);put('Placement',0);put('Spread',1)
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
        mainnotes,combnotes=static_notes if static_notes else (pitches(True),pitches(False))
        pitch_mean,pitch_depth=lfo_span(v(207),v(208))
        mainnotes=[p+v(55)*pitch_mean for p in mainnotes]
        combnotes=[p+v(56)*pitch_mean for p in combnotes]
        put('Transpose',0);put('Comb_Pitch',0);put('Comb_Mode',0)
        put('Pitch_External',False);put('Pitch_Custom',True);put('Pitch_Root',60)
        put('Harmony_On',False)
        for main,prefix in ((True,'Osc_Map_'),(False,'Comb_Map_')):
            if args.profile=='1.7':
                interval,trans,circular,bend,fine,mode,scale,base,table=(44,45,124,495,128,219,216,220,215) if main else (29,30,125,496,127,232,231,233,230)
                put(prefix+'Shape',1 if v(circular)>0 else 0);put(prefix+'First',1)
                put(prefix+'Interval',v(interval));put(prefix+'Transpose',v(trans));put(prefix+'Bend',v(bend,1))
                choice=int(v(mode,1));put(prefix+'Quantize',{1:1,2:2,3:3}.get(choice,0))
                allowed=scales[int(v(scale,1))-1]if 1<=int(v(scale,1))<=len(scales) else list(range(12))
                put(prefix+'Scale_Tones',allowed);put(prefix+'Table',tables.get(table,list(range(12)))[:12]);put(prefix+'Harmonic',v(base,60))
                put(prefix+'Fine',v(fine)+v(37)+v(351)+v(380)+(v(55) if main else v(56))*pitch_mean)
            elif args.profile=='2018':
                put(prefix+'Shape',0);put(prefix+'First',0)
                put(prefix+'Interval',effective.get(1244 if main else 1275,0))
                put(prefix+'Transpose',v(45 if main else 30))
                put(prefix+'Fine',v(37)+v(128 if main else 127)+(v(55) if main else v(56))*pitch_mean)
                put(prefix+'Scale_Tones',list(range(12)))
            else:
                put(prefix+'Shape',2);put(prefix+'First',0)
                step,length,offset,trans=(2332,2333,2334,2565)if main else (2604,2605,2606,2624)
                put(prefix+'Interval',effective.get(step,1)*12);put(prefix+'Length',effective.get(length,1));put(prefix+'Offset',effective.get(offset,0))
                put(prefix+'Transpose',effective.get(trans,0));put(prefix+'Fine',v(37)+(0 if main else effective.get(1737,0))+(v(55) if main else v(56))*pitch_mean)
                put(prefix+'Scale_Tones',list(range(12)))
        issues.append('Pitch maps are now live and input-relative; source 2025 harmony/probability selections still require verification against the original instrument')
        issues.append('Original stereo voice randomization is approximated by uniform speaker spread')
        issues.append('Live pitch maps replace clipped transpose vectors; NI exact indexer/randomization and later harmony/probability processing still differ')
        direct('Detune',49);direct('Comb_Detune',36);put('Vib_Amount',v(38)+v(272));put('Vib_Rate',v(39));mapped.update((38,39,272))
        if abs(v(272))>1e-6:issues.append('Separate global and oscillator vibratos combined; oscillator vibrato rate omitted')
        put('FM_Ratio',2**(v(344)/12));put('FM_Ratio_Snap',False)
        put('FM_Amount',[v(347)/(8*440*2**((note-69)/12)) for note in mainnotes]);put('FM_On',v(347)>1e-6);mapped.update((344,347))
        issues.append('FM depth translated from Hz at MIDI 60; the new ratio/amount ranges may clip and vary under pitch modulation')
        # The new optional feedback saturator leaves existing snapshots unchanged.
        put('Comb_Saturation',True);direct('Comb_Sat_Level',7)
        direct('Comb_Feedback',18);direct('Comb_Mix',19);put('Comb_On',True);put('Comb_Tone',1)
        issues.append('Saturated comb uses the existing AnalogFilter engine; NI feedback saturation/DC/damping/normalization still differ')
        fmap={1:6,2:8,3:4,4:9,5:10,6:5}
        for key,sid in [('Filter_Type',10),('Post_Type',13)]:
            val=int(v(sid,3));put(key,fmap.get(val,0))
            if val==5:issues.append(f'{key}: BL4 slope is available, but its NI resonance loop is approximated')
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
            wf=int(v(wave,5));put(L+'Round',lfo_shape(wf))
            put(L+'Skew',-1 if wf==1 else 1 if wf==2 else 0);put(L+'Width',.5+v(pwm)*.49 if wf==3 else .5)
            put(L+'Noise',wf in (6,10));put(L+'S&H_Glide',.1 if wf==10 else 0)
            # Preserve cycle count across a chosen finite pool approximately, with built-in indexer.
            cycles=(1+v(length)) if v(length)<0 else round(1+15*v(length))
            put(L+'Spread',abs(cycles)*args.voices/max(1,v(period,80)));put(L+'Random',clamp(v(random)/40,0,1))
            put(L+'Pow',0);put(L+'BiPow',0)
            if abs(v(power,1)-1)>.001 or v(random)>0 or v(sym)!=80:issues.append(f'Poly LFO {li+1}: power, voice-index symmetry/randomization and phase distribution approximated')
            if li==0:
                # Native Amp is additive around Osc Amp: source is bipolar.
                gain=v(181,1);off=v(182);mean,depth=lfo_span(gain,off)
                put('Osc_Amp',mean*v(240)/2);route(0,12,depth/max(.0001,mean))
                if min(off,off+gain)<0 or max(off,off+gain)>1:
                    issues.append('Amplitude LFO clipped transfer fitted at its endpoints; interior clipping differs')
            else:
                route(1,0,v(55)*pitch_depth/24);route(1,5,(v(56)-v(55))*pitch_depth/24);route(1,9,v(58)*pitch_depth/48)
                if min(v(208),v(208)+v(207))<0 or max(v(208),v(208)+v(207))>1:
                    issues.append('Pitch LFO clipped transfer fitted at its endpoints; interior clipping differs')
            mapped.update(ids)
        envamt=v(159);route(8,9,envamt/48);mapped.add(159)
        issues.append('Poly LFO offsets and transfer curves use existing bipolar routing approximations')
        for li,ids in enumerate(((360,361,362,364,365,369,376,377,378),(385,386,387,389,390,391,392,393,394)),3):
            width,wave,amount,phase,quant,ease,dest,num,den=ids;L=f'LFO{li}_';wf=int(v(wave,5))
            put(L+'Sync',True);put(L+'Beats',v(den,1)/max(1,v(num,1)));put(L+'Phase',v(phase)%1);put(L+'Spread',0)
            put(L+'Round',lfo_shape(wf));put(L+'Skew',-1 if wf==1 else 1 if wf==2 else 0);put(L+'Width',.5+v(width)*.49 if wf==3 else .5);put(L+'Noise',wf in (6,10));put(L+'S&H_Glide',.1 if wf==10 else 0)
            destination=int(v(dest,1));amt=v(amount)
            # Original sends have target-specific scaling. Existing fixed targets can be translated.
            routes={2:(9,1),3:(1,5000/(440*8)),4:(7,1),5:(0,1),7:(0,.5),9:(11,1),12:(3,100/48)}
            if destination in routes:
                target,scale=routes[destination];route(li-1,target,amt*scale)
                issues.append(f'Mono LFO {li-2}: unipolar output translated to bipolar routing; baseline/shape differ')
            elif destination!=1 and amt>1e-6:issues.append(f'Mono LFO {li-2} destination {destination} has no equivalent fixed matrix target (interval, ring ratio or LFO AM phase)')
            if v(quant)>0 or v(ease)>0 or wf in (7,8,10):issues.append(f'Mono LFO {li-2}: quantization/easing/random algorithm approximated')
            mapped.update(ids)
        direct('Ring_Mix',461);put('Ring_On',v(461)>1e-6);put('Ring_Pitch',[69+12*math.log2(max(.001,v(460))*440*2**((p-69)/12)/440) for p in mainnotes]);direct('Ring_Detune',592)
        if v(461)>0:issues.append('Original pulse-width/ratio ring oscillator approximated by existing sine ring modulator')
        # Existing effects used as translations; originals are different algorithms.
        direct('Reverb_Mix',75);put('Reverb_On',v(75)>1e-6);put('Reverb_Decay',.1+9.9*v(84));put('Reverb_Size',60*v(83));put('Reverb_Mod_Rate',v(85)*10);direct('Reverb_Wander',86);put('Reverb_Lowpass',440*2**((v(87)-69)/12))
        direct('Echo_Mix',450);direct('Echo_Feed',443);put('Echo_On',v(450)>1e-6);put('Echo_Sync',False);put('Echo_Delay',max(.005,1.5*v(444)**2));direct('Echo_Cutoff',446);direct('Echo_Resonance',456)
        if v(75)>0:issues.append('Original modulated reverb approximated by shared MixerPro reverb')
        if v(450)>0:issues.append('Delay timing/filter/LR controls approximated by shared echo; original quantized sync not fully translated')
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
            subtrans=12*effective.get(2008,0)+effective.get(2009,0)+effective.get(2706,0)+0.5*effective.get(2708,0)+v(37)
            subrel=effective.get(2010,0)
            if amod or effective.get(2708,0):issues.append('2025 sub pitch/amplitude modulation frozen at unipolar midpoint; signed amplitude represented by magnitude (phase differs)')
        else:
            subgain=effective.get(589,0)/16
            subtrans=12*effective.get(590,0)+12+effective.get(591,0)+v(37)
            subrel=effective.get(653,0)
        put('Sub_On',subgain>1e-9);put('Sub_Level',subgain);put('Sub_Transpose',subtrans)
        put('Sub_Attack',state['Amp_Attack']);put('Sub_Decay',state['Amp_Decay']);put('Sub_Sustain',state['Amp_Sustain'])
        put('Sub_Release',max(state['Amp_Release'],.001*10**(subrel/20)))
        if subgain>1e-9:
            issues.append('Independent sine sub: gate ADSR replaces source audio follower; source tremolo/shaper and additive sends omitted; pitch is a static input-relative approximation')
            if args.profile!='2025':issues.append('Sub gain normalized from source 0..16 to 0..1 because source follower amplitude is unavailable')
        put('Delta_Window',max(1,effective.get(564,1000)));put('Delta_Gain',effective.get(565,1))
        put('Delta_LFO_1',effective.get(579,0));put('Delta_LFO_2',effective.get(578,1))
        issues.append('DELTA finite-window absolute difference/smoothing is implemented; automatic source-selector routing is only mapped where decoded')
        # Isolated/global stages are retained in raw recovery, not silently claimed as imported.
        issues.append('Original four-band parametric EQ is not recreated; saved global settings remain in audit')
        for i,x in enumerate(matrix):
            if abs(x)>1:clips.append(dict(control=f'matrix[{i}]',source_value=x,used_value=clamp(x,-1,1)));matrix[i]=clamp(x,-1,1)
        name=snap.get('container_name',snap['name']) if args.profile!='1.7' else snap['name']
        recovered.append(dict(index=ordinal,name=name,inner_name=snap['name'],bank=snap.get('bank_name'),bank_index=snap.get('bank_index'),bank_slot=snap.get('bank_slot'),offset=snap['offset'],effective_values={str(k):x for k,x in effective.items()},origins=origins,tables={str(k):x for k,x in tables.items()},records=records))
        slot=dict(name=name,voices=args.voices,controls=state,matrix=matrix,params=[],lfos=4,envs=1,exts=0)
        if max(state['Osc_Amp'])<=1e-9 and not state['Sub_On']:
            slot['importNote']='The main sound layer is muted in this translation; additional source layers are incomplete.'
            issues.append('Main oscillator bank is muted; unported independent layers can leave this translation silent')
        translated.append(slot)
        audit=dict(index=ordinal,name=name,bank=snap.get('bank_name'),limitations=sorted(set(issues)),clipped=clips,source_main_notes=mainnotes,source_comb_notes=combnotes)
        audit['mapped_panel_ids' if args.profile=='1.7' else 'projected_legacy_control_ids']=sorted(mapped)
        audits.append(audit)
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
            write(path,dict(snapshotBank=1,sourceVersion=args.profile,sourceBank=bank,translation='static approximation',slots=slots[start:end]));files.append(path.name)
    write(args.output/'bank-manifest.json',dict(profile=args.profile,files=files,snapshot_count=len(recovered)))
    print(f'Recovered {len(recovered)} x {expected} records; created {len(files)} candidate banks for {args.voices} voices. DSP/GUI untouched.')
if __name__=='__main__':main()
