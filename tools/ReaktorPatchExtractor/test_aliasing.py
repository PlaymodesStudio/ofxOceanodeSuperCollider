#!/usr/bin/env python3
"""Regression checks against an external Aliasing Synth ensemble (not bundled).

Checks byte-backed snapshot identities, stacked scope/wires, numeric constants
and extra panel link words. Also verifies ambiguity is not silently resolved.
"""
import argparse,json,pathlib,struct,subprocess,tempfile
HERE=pathlib.Path(__file__).resolve().parent
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('ensemble',type=pathlib.Path)
p.add_argument('--extractor',type=pathlib.Path,default=HERE/'dist/Reaktor Patch Extractor.app/Contents/MacOS/ReaktorPatchExtractor')
a=p.parse_args()
def report(path):return json.loads(subprocess.check_output([str(a.extractor.resolve()),str(path.resolve())],text=True))
def modules(r):
 out={}
 def walk(ms):
  for m in ms:out[m['object_id']]=m;walk(m['children'])
 walk(r['top_level']);return out
r=report(a.ensemble);m=modules(r);raw=a.ensemble.read_bytes()
assert r['schema']=='reaktor-patch-report-v5'
assert r['counts']['modules']==495 and r['counts']['snapshots']==38
assert r['counts']['connections']==r['counts']['resolved_connections']==533
assert [x['object_id']for x in r['top_level']]==[3,4,12]
assert m[394]['name']=='AT view' and m[394]['child_count']==14
assert m[434]['parent_object_id']==394 and m[504]['parent_object_id']==12
assert m[459]['constant_numeric_value']==60
assert m[411]['constant_numeric_value']==2 and m[412]['constant_numeric_value']==-1
assert m[210]['constant_numeric_value']==.5 and m[213]['constant_numeric_value']==2
assert m[192]['control_properties']['range_endpoint_1']==-64
assert m[192]['control_properties']['range_endpoint_2']==130
assert m[192]['snapshot_control_id']==115
assert m[238]['snapshot_control_id']==212 and m[314]['snapshot_control_id']==228
for snapshot in r['snapshots']:
 assert snapshot['object_id_mapping_method']=='stable_panel_reference'
 assert snapshot['resolved_parameter_count']==snapshot['parameter_count']==61
 assert len({x['module_object_id']for x in snapshot['parameters']})==61
 for state in snapshot['parameters']:
  module=m[state['module_object_id']]
  assert module['snapshot_control_id']==state['local_module_object_id']
quak=r['snapshots'][0];assert quak['name']=='quak'
effective={x['module_object_id']:x for x in quak['effective_controls']}
assert effective[14]['value']==0 and abs(effective[192]['value']+32)<1e-4
assert effective[269]['value']==-51
assert effective[231]['selection_index']==1 and 'normalized_value'not in effective[231]
# A duplicated panel reference cannot be assigned to either candidate. The
# remaining unique states still map correctly; dense traversal IDs are ignored.
changed=bytearray(raw);offset=int(m[17]['offset'],16)+0xd2
struct.pack_into('<I',changed,offset,10000+m[20]['snapshot_control_id'])
with tempfile.TemporaryDirectory(prefix='reaktor_aliasing_') as d:
 path=pathlib.Path(d)/'ambiguous.ens';path.write_bytes(changed);bad=report(path)
 for snapshot in bad['snapshots']:
  for state in snapshot['parameters']:
   if state['local_module_object_id']in [28,30]:assert 'module_object_id'not in state
print('PASS: 533 wires; 38 x 61 stable snapshot identities; constants, stacked hierarchy and ambiguous-ID handling.')
