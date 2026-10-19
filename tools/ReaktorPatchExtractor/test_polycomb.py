#!/usr/bin/env python3
"""Read-only regression against an external PolyComb_1.7.ens (not bundled)."""
import argparse,json,pathlib,struct,subprocess
here=pathlib.Path(__file__).resolve().parent
p=argparse.ArgumentParser(description=__doc__);p.add_argument('ensemble',type=pathlib.Path);p.add_argument('--extractor',type=pathlib.Path,default=here/'dist/Reaktor Patch Extractor.app/Contents/MacOS/ReaktorPatchExtractor');a=p.parse_args()
j=json.loads(subprocess.check_output([str(a.extractor.resolve()),str(a.ensemble.resolve())],text=True));b=a.ensemble.read_bytes();mods={}
def walk(ms):
    for m in ms:mods[m['object_id']]=m;walk(m['children'])
walk(j['top_level'])
assert j['counts']['snapshots']==210
assert j['counts']['snapshot_parameter_records']==210*237
assert j['counts']['connections']==j['counts']['resolved_connections']==2004
for s in j['snapshots']:
    assert s['object_id_mapping_method']=='stable_panel_reference'
    assert s['parameter_count']==237 and s['resolved_parameter_count']==193
    cursor=int(s['offset'],16)+4+len(s['name'].encode())+108+s['metadata_extension_bytes']
    ids=set()
    for r in s['parameters']:
        sid=struct.unpack_from('<I',b,cursor-4)[0];size=struct.unpack_from('<I',b,cursor)[0]
        assert sid==r['local_module_object_id'] and sid not in ids;ids.add(sid)
        assert size==r['record_size']
        if 'module_object_id'in r:assert mods[r['module_object_id']]['snapshot_control_id']==sid
        if sid in (215,230):
            assert size==548 and r['value_type_code']==4
            assert len(r['payload_words'])==137
        cursor+=size+8
assert j['snapshots'][0]['name']=='pendulun' and j['snapshots'][-1]['name']=='magic'
print('PASS: 210 complete snapshots x 237 states; 193 panel identities per snapshot; 548-byte scale-table states; all 2,004 Primary wires.')
