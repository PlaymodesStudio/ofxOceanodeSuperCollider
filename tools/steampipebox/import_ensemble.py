#!/usr/bin/env python3
"""Import SteamPipe's Primary controls, XY states and all snapshot containers.

Uses the native Reaktor Patch Extractor; never modifies the input ensemble.
Object IDs are checked against hierarchy paths to fail on a different revision.
"""
import argparse, hashlib, json, math, pathlib, re, subprocess
ROOT = pathlib.Path(__file__).resolve().parents[2]
ITEMS = [
(211952,'Attack','attack','Steam envelope'),(211953,'Decay','decay','Steam envelope'),(211954,'Sustain','sustain','Steam envelope'),(211955,'Release','release','Steam envelope'),(211997,'Velocity Amount','velocity_amount','Steam envelope'),(211998,'Envelope Key Scaling','env_key_scaling','Steam envelope'),
(212002,'DC / Noise','dc_noise','Steam'),(212010,'Noise Cutoff','noise_cut','Steam'),(212011,'Noise Key Track','noise_key_track','Steam'),(212032,'Noise Velocity Track','noise_velocity_track','Steam'),(212033,'Noise Resonance','noise_resonance','Steam'),(212034,'Noise One Pole','noise_one_pole','Steam'),
(212052,'Tune','tune','Delay tune'),(212053,'Fine','fine','Delay tune'),(212056,'Sample Correction','sample_correction','Delay tune'),(212059,'Tune MW','tune_mw','Delay tune'),(212071,'Vibrato Amount','vibrato_amount','Delay tune'),(212072,'Vibrato Frequency','vibrato_frequency','Delay tune'),(212073,'Polarity','polarity','Delay tune'),
(212084,'Allpass Tune','allpass_tune','Allpass'),(212085,'Allpass Fine','allpass_fine','Allpass'),(212088,'Allpass Sample Correction','allpass_sample_correction','Allpass'),(212091,'Allpass MW','allpass_mw','Allpass'),(212149,'Diffusion','diffusion','Allpass'),(212150,'Allpass On','allpass_on','Allpass'),
(212100,'Offset','offset','Feedback'),(212102,'Push','push','Feedback'),(212119,'Feedback RT','feedback_rt','Feedback'),(212123,'Feedback Key Track','feedback_key_track','Feedback'),(212136,'Release Damp','release_damp','Feedback'),(212156,'Soft / Hard','soft_hard','Feedback'),(212157,'Symmetry','symmetry','Feedback'),
(212237,'HP 0','hp0','MW filter'),(212250,'HP 1','hp1','MW filter'),(212272,'HP Key Track','hp_key_track','MW filter'),(212251,'LP 0','lp0','MW filter'),(212263,'LP 1','lp1','MW filter'),(212273,'LP Key Track','lp_key_track','MW filter'),
(212287,'Pan Random','pan_random','Output'),(212293,'Stereo','stereo','Output'),(212303,'Gain','gain','Output'),
(212405,'R66 Size','r66_size','R66'),(212406,'R66 RT','r66_rt','R66'),(212492,'R66 Frequency','r66_frequency','R66'),(212493,'R66 High Damp','r66_high_damp','R66'),(212494,'R66 Low Damp','r66_low_damp','R66'),(212515,'R66 Spin','r66_spin','R66'),(212586,'R66 Position','r66_position','R66'),(212675,'R66 Dizzy','r66_dizzy','R66'),(212685,'R66 LP','r66_lp','R66'),(212695,'R66 Predelay','r66_predelay','R66'),(212696,'R66 Spread','r66_spread','R66'),(212707,'R66 Mix','r66_mix','R66'),
(212863,'Delay Stereo Difference','delay_stereo_difference','Delay'),(212887,'Delay Bypass','delay_bypass','Delay')]
XY = [(212874,[('Delay Cutoff','delay_cutoff',60,120),('Delay Resonance','delay_resonance',0,0.6660000085830688)]),(212875,[('Delay Right Steps','delay_right_steps',1,16),('Delay Left Steps','delay_left_steps',1,16)]),(212888,[('Delay Decay','delay_decay',46,100),('Delay Mix','delay_mix',0,0.5)])]
def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('ensemble',type=pathlib.Path);ap.add_argument('--report',type=pathlib.Path);a=ap.parse_args()
    extractor=ROOT/'tools/ReaktorPatchExtractor/dist/Reaktor Patch Extractor.app/Contents/MacOS/ReaktorPatchExtractor'
    r=json.loads(a.report.read_text() if a.report else subprocess.check_output([str(extractor),str(a.ensemble)],text=True))
    assert r.get('parser_revision'), 'Stale extractor report: regenerate it with Reaktor Patch Extractor 0.5.3 or later.'
    modules={}
    def walk(ms):
        for m in ms:modules[m['object_id']]=m;walk(m['children'])
    walk(r['top_level']);controls=[]
    # DSP constants must come from binary values, never the editable labels.
    # Old extractor reports misread the -60 keytrack offset as its label "0".
    for mid,expected in [(212126,-60),(212501,112)]:
        assert modules[mid].get('constant_numeric_value')==expected, 'Stale extractor report: regenerate it with the current Reaktor Patch Extractor.'

    for mid,name,sc,group in ITEMS:
        m=modules[mid];assert '/Instrument#211947/' in m['hierarchy_path']
        p=m['control_properties'];lo,hi=sorted([p['range_endpoint_1'],p['range_endpoint_2']])
        controls.append(dict(id=mid,name=name,sc=sc,group=group,min=lo,max=hi,mode='switch' if mid in [212034,212150,212887,212073] else 'float',path=m['hierarchy_path'],description=m.get('instance_description','')))
    for mid,axes in XY:
        for axis,(name,sc,lo,hi) in enumerate(axes):controls.append(dict(id=mid,axis=axis,name=name,sc=sc,group='Delay',min=lo,max=hi,mode='float',path=modules[mid]['hierarchy_path']))
    for mid,sid,name,sc in [(212743,518,'R66 Left On','r66_left_on'),(212744,517,'R66 Right On','r66_right_on')]:
        assert modules[mid]['snapshot_control_id']==sid
        controls.append(dict(id=mid,snapshot_id=sid,name=name,sc=sc,group='R66',min=0,max=1,mode='switch',path=modules[mid]['hierarchy_path']))
    for name,sc,lo,hi in [('Mod Wheel','mod_wheel',0,1),('Bend','bend',-24,24)]:controls.append(dict(id=None,name=name,sc=sc,group='Performance',min=lo,max=hi,mode='float'))
    presets=[]
    for s in r['snapshots']:
        effective={c['module_object_id']:c for c in s['effective_controls']};raw={p.get('module_object_id'):p for p in s['parameters']};values={}
        for c in controls:
            mid=c['id']
            if mid is None:v=0
            elif 'snapshot_id' in c:
                v=float(effective[mid]['selection_index']==1)
            elif 'axis' in c:
                control=effective[mid]
                assert control['decode_status']=='resolved', (mid,control)
                axis=control['axes'][c['axis']]
                assert axis['control_properties']['range_endpoint_1']==c['min']
                assert axis['control_properties']['range_endpoint_2']==c['max']
                v=float(axis['value'])
            else:
                v=effective[mid]['value']
                if mid in [212034,212150]:v=float(v==1) # 1=first input, 2=second
            assert math.isfinite(v) and c['min']-1e-5<=v<=c['max']+1e-5,(s['name'],c,v)
            values[c['name']]=v
        presets.append(dict(name=s.get('container_name',s['name']),original_name=s['name'],bank=s.get('bank_name','Bank'),bank_index=s.get('bank_index'),bank_slot=s.get('bank_slot'),values=values))
    assert len(presets)==172 and len(controls)==65
    dest=ROOT/'synthdefs/Defaults/SteamPipeBox'
    def save(p,j):
        p.parent.mkdir(parents=True,exist_ok=True)
        if p.exists():
            try:
                if json.loads(p.read_text())==j:return
            except (ValueError,OSError):pass
        p.write_text(json.dumps(j,indent=2)+'\n')
    save(dest/'graph-constants.json',[
        dict(id=m['object_id'],path=m['hierarchy_path'],offset=m['offset'],value=m['constant_numeric_value'])
        for m in modules.values() if m['kind']==100
    ])
    save(dest/'controls.json',controls);save(dest/'factory.json',presets)
    save(dest/'reaktor-snapshots.json',dict(source=a.ensemble.name,sha256=hashlib.sha256(a.ensemble.read_bytes()).hexdigest(),counts=r['counts'],snapshots=[{**{k:v for k,v in snap.items() if k not in ['effective_controls','parameters']},'parameters':[{k:v for k,v in rec.items() if k not in ['hierarchy_path','instance_label','module_type_name']} for rec in snap['parameters']]} for snap in r['snapshots']]))
    # Split at 128 regardless of duplicate source bank names. Keep source order.
    banks=[]
    for start in range(0,len(presets),128):
        bank=presets[start:start+128];name=f'Reaktor SteamPipe {start+1:03d}-{start+len(bank):03d}.json';banks.append(dict(file=name,start=start,count=len(bank)))
        slots=[dict(name=p['name'],**{re.sub(r'[^a-zA-Z0-9]', '_',c['name']):p['values'][c['name']] for c in controls}) for p in bank]
        # ofParameter::getEscapedName replaces spaces, slash and punctuation with '_'.
        save(ROOT/'presets/nodeSnapshots/SteamPipeBox/banks'/name,dict(snapshotBank=1,active=0,slots=slots))
    save(dest/'banks.json',banks)
    data='// Generated by tools/steampipebox/import_ensemble.py.\n#pragma once\nnamespace steamPipeBoxData {\n'
    for key,v in [('controls',controls),('factory',presets),('banks',banks)]:
        data+=f'inline constexpr const char* {key} = R"STEAM('+json.dumps(v,separators=(',',':'))+')STEAM";\n'
    header=ROOT/'src/nodes/scSteamPipeBoxData.h';text=data+'}\n'
    if not header.exists() or header.read_text()!=text:header.write_text(text)
    print(f'Imported {len(presets)} snapshots, {len(controls)} controls')
if __name__=='__main__':main()
