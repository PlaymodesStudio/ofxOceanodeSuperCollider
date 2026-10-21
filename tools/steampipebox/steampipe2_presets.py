#!/usr/bin/env python3
"""Capture V2 instrument snapshots and translate the supported SteamPipeBox controls.

Space Master Deluxe is approximated with R66. Voice modes, glide and the arp
remain source metadata; they do not silently configure unrelated Oceanode nodes.
"""
import argparse,hashlib,json,math,pathlib,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[2]
FOLDER=ROOT/'synthdefs/Defaults/SteamPipeBox'
IDS={'Attack':8,'Decay':9,'Sustain':10,'Release':11,'Velocity Amount':56,'Envelope Key Scaling':57,
 'DC / Noise':67,'Noise Cutoff':75,'Noise Key Track':76,'Noise Velocity Track':99,'Noise Resonance':100,'Noise One Pole':101,
 'Tune':125,'Fine':126,'Sample Correction':129,'Tune MW':132,'Polarity':146,
 'Allpass Tune':157,'Allpass Fine':158,'Allpass Sample Correction':161,'Allpass MW':164,'Allpass On':236,'Diffusion':235,
 'Offset':177,'Push':179,'Feedback RT':199,'Feedback Key Track':203,'Release Damp':219,'Soft / Hard':243,'Symmetry':244,
 'HP 0':334,'HP 1':348,'LP 0':349,'LP 1':362,'HP Key Track':371,'LP Key Track':372,'Gain':8282,'Stereo':8277,'Mod Wheel':8244}

def translate(source,controls,baseline):
    result=[]
    for preset in source['presets']:
        state=preset['controls']
        def v(mid):
            c=state[str(mid)]
            assert c.get('decode_status','resolved')=='resolved', (preset['name'],mid,c)
            value=c.get('value',c.get('selection_index'))
            assert value is not None and math.isfinite(value),(preset['name'],mid,c)
            return float(value)
        values=dict(baseline)
        for key,mid in IDS.items():values[key]=v(mid)
        for key in ('Noise One Pole','Allpass On'):values[key]=float(values[key]==1)
        values.update({'Vibrato Amount':0.,'Vibrato Frequency':0.,'Bend':0.,'Pan Random':1.,'Delay Bypass':1.})
        # Keep the replacement deliberately explicit: this is R66, not the
        # Space Master algorithm. Copy compatible quantities and scale ranges.
        values.update({'R66 Size':max(0.,min(60.,(48-v(7221))*60/72)), 'R66 RT':v(7224),
         'R66 Frequency':v(7225)*2,'R66 Spin':v(7226)*2,'R66 Position':v(7231),'R66 Dizzy':0.,
         'R66 High Damp':-v(7230)*5,'R66 Low Damp':-v(7228)*5,'R66 LP':max(24.,v(7229)),
         'R66 Predelay':v(7216)*1000,'R66 Spread':max(-1.,min(1.,v(7219))),
         'R66 Mix':v(7232),'R66 Left On':float(v(7249)==1),'R66 Right On':float(v(7249)==1)})
        for c in controls:
            value=values[c['name']]
            assert c['min']-1e-5<=value<=c['max']+1e-5,(preset['name'],c['name'],value)
        result.append(dict(name=preset['name'],original_name=preset['original_name'],bank='SteamPipe 2 - Bank 1',
          bank_slot=preset['slot'],bank_size=87,scope='instrument',source_version=2,values=values,
          extras={'SteamPipe_2':True,'Legato':bool(v(7303)),'Noise_Filter_Envelope':v(6495)},
          translation_notes=['Space Master Deluxe approximated with R66','Source voice mode, glide and arpeggiator are not reproduced'],
          source_voice_controls={str(mid):state[str(mid)] for mid in (8039,8041,8042,8052,8219,8307,8612,9533) if str(mid) in state}))
    return result

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('ensemble',type=pathlib.Path);ap.add_argument('--report',type=pathlib.Path);a=ap.parse_args()
    extractor=ROOT/'tools/ReaktorPatchExtractor/dist/Reaktor Patch Extractor.app/Contents/MacOS/ReaktorPatchExtractor'
    r=json.loads(a.report.read_text() if a.report else subprocess.check_output([str(extractor),str(a.ensemble)],text=True))
    assert r['counts']['modules']==1487 and r['counts']['decoded_core_cells']==11
    presets=[]
    for s in r['snapshots']:
        if s.get('snapshot_scope')!='instrument':continue
        state={str(c['module_object_id']):{k:v for k,v in c.items() if k in ('value','selection_index','normalized_value','source','decode_status')} for c in s['effective_controls']}
        presets.append(dict(name=s.get('container_name',s['name']),original_name=s['name'],slot=s['container_bank_slot'],controls=state))
    presets.sort(key=lambda p:p['slot']);assert len(presets)==87 and [p['slot'] for p in presets]==list(range(87))
    source=dict(source=a.ensemble.name,sha256=hashlib.sha256(a.ensemble.read_bytes()).hexdigest(),counts=r['counts'],presets=presets,
      resonator_audit={'main_delay_core':5236,'diffusion_delay_core':6222,'interpolation':'Core Interpol RT cubic Hermite',
       'phase_lp_macro':946,'phase_lp_coefficient':-.5,'phase_hp_macro':3536,'phase_hp_coefficient':0.3330000042915344,'arctan_output':'cycles'},
      assumptions=['Only native instrument snapshots are imported','Space Master Deluxe uses an R66 approximation','Voice modes, glide and arpeggiator remain unmapped'])
    (FOLDER/'steampipe2-source.json').write_text(json.dumps(source,indent=2)+'\n')
    print('Captured 87 SteamPipe 2 instrument snapshots; run import_ensemble.py to rebuild all banks and embedded data')
if __name__=='__main__':main()
