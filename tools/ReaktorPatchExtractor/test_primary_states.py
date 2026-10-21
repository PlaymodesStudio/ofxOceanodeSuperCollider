#!/usr/bin/env python3
"""Byte-backed state audit across SteamPipe, Aliasing and PolyComb ensembles.

Checks endpoints of buttons, signed/shifted switches, both XY axes, and that
invalid numeric values never become display labels or saved-state substitutes.
"""
import argparse,json,pathlib,struct,subprocess,tempfile
HERE=pathlib.Path(__file__).resolve().parent
EXE=HERE/'dist/Reaktor Patch Extractor.app/Contents/MacOS/ReaktorPatchExtractor'
ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('ensembles',nargs='+',type=pathlib.Path);a=ap.parse_args()
def extract(path):return json.loads(subprocess.check_output([str(EXE),str(path)],text=True))
def modules(j):
 out={}
 def walk(xs):
  for m in xs:out[m['object_id']]=m;walk(m['children'])
 walk(j['top_level']);return out
counts=dict(constants=0,buttons=0,switches=0,xy_axes=0,isolated_axes=0,core_structures=0,core_modules=0,core_connections=0)
def core_structure(s):
 assert s['declared_module_count']==s['module_count']
 assert s['declared_connection_count']==s['connection_count']
 counts['core_structures']+=1;counts['core_modules']+=s['module_count'];counts['core_connections']+=s['connection_count']
 for p in [s.get('properties',{})]+[m.get('properties',{}) for m in s['modules']]:
  assert p.get('declared_count')==p.get('decoded_count')
 for c in s['connections']:
  for key in ['source','target']:
   endpoint=c[key]
   if endpoint['kind']=='module_pin':assert 'module_path'in endpoint
 for m in s['modules']:
  assert not m['type_name'].startswith('core_type_')
  if 'inner_structure'in m:core_structure(m['inner_structure'])
for path in a.ensembles:
 j=extract(path);mods=modules(j);raw=path.read_bytes()
 assert j['parser_revision']=='2026-10-21-primary-state-audit'
 assert j['counts']['connections']==j['counts']['resolved_connections']
 for cell in j['core_cells']:
  assert cell['core_document']['parse_status']=='decoded'
  core_structure(cell['core_document']['structure'])
 for n in mods.values():
  if n['kind']==100:
   value=struct.unpack_from('<f',raw,int(n['offset'],16)+0x44)[0]
   assert n['constant_numeric_value']==value and n['constant_value_source']=='stored_float32';counts['constants']+=1
  if n['kind']==22 and 'saved_control_normalized'in n:
   props=n['control_properties'];value=props['range_endpoint_2' if n['saved_control_normalized']>=.5 else 'range_endpoint_1']
   assert n['saved_control_value']==value
 for s in j['snapshots']:
  params={r['module_object_id']:r for r in s['parameters'] if 'module_object_id'in r}
  for c in s['effective_controls']:
   n=mods[c['module_object_id']];p=n.get('control_properties',{})
   if n['kind']==22:
    assert c['value']==p['range_endpoint_2' if c['normalized_value']>=.5 else 'range_endpoint_1'];counts['buttons']+=1
   if n['kind']==24:
    expected=params.get(n['object_id'],{}).get('value',struct.unpack('<i',struct.pack('<I',n['saved_control_raw_u32']))[0])
    assert c['selection_index']==c['value']==expected;counts['switches']+=1
   if n['kind']==25:
    assert c['decode_status']=='resolved'
    words=params.get(n['object_id'],{}).get('payload_words')
    if words and len(words)==19:expected=[struct.unpack('<f',struct.pack('<I',words[i]))[0]for i in [8,18]]
    else:expected=n['saved_control_axes']
    for axis,e in zip(c['axes'],expected):
     props=axis['control_properties'];assert axis['normalized_value']==e
     assert abs(axis['value']-(props['range_endpoint_1']+e*(props['range_endpoint_2']-props['range_endpoint_1'])))<1e-9
     counts['xy_axes']+=1
     if 'isolated'in axis['source']:counts['isolated_axes']+=1
 if 212743 in mods:
  assert mods[212743]['snapshot_control_id']==518 and mods[212744]['snapshot_control_id']==517
  assert mods[212743]['control_properties']['range_endpoint_2']==1
  assert any(c['value']==-1 for s in j['snapshots'] for c in s['effective_controls'] if c['module_object_id']==212073)
 # Non-finite constant must not fall back to its editable numeric label.
 n=next(x for x in mods.values() if x['kind']==100);bad=bytearray(raw);struct.pack_into('<I',bad,int(n['offset'],16)+0x44,0x7fc00000)
 with tempfile.TemporaryDirectory(prefix='reaktor_state_audit_')as d:
  dst=pathlib.Path(d)/'constant_nan.ens';dst.write_bytes(bad);r=extract(dst);c=modules(r)[n['object_id']]
  assert 'constant_value'not in c and 'constant_numeric_value'not in c
  assert any('Unresolved numeric Constant #'in w for w in r['warnings'])
 # Snapshot scalar NaN retains raw data and remains explicitly unresolved.
 snap=j['snapshots'][0];cursor=int(snap['offset'],16)+4+len(snap['name'].encode())+108+snap['metadata_extension_bytes'];chosen=None
 for r in snap['parameters']:
  if r.get('module_kind')==20 and r.get('value_type')=='normalized_float':chosen=(cursor,r['module_object_id']);break
  cursor+=r['record_size']+8
 assert chosen is not None
 bad=bytearray(raw);struct.pack_into('<I',bad,chosen[0]+36,0x7fc00000)
 with tempfile.TemporaryDirectory(prefix='reaktor_state_audit_')as d:
  dst=pathlib.Path(d)/'snapshot_nan.ens';dst.write_bytes(bad);r=extract(dst);s=r['snapshots'][0]
  rec=next(p for p in s['parameters'] if p.get('module_object_id')==chosen[1]);assert rec['decode_status']=='non_finite_value' and 'value'not in rec
  c=next(c for c in s['effective_controls'] if c['module_object_id']==chosen[1]);assert c['decode_status']=='unresolved_snapshot_override' and 'value'not in c
 print('PASS:',path.name)
print(json.dumps(counts,indent=2))
