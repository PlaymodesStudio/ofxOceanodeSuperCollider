import json,sys
r=json.load(open(sys.argv[1]))
mods={}
def walk(n):
    mods[n['object_id']]=n
    for c in n.get('children',[]): walk(c)
for t in r['top_level']: walk(t)
snaps=r['snapshots']
ids=[c['module_object_id'] for c in snaps[0]['effective_controls']]
byid=[{c['module_object_id']:c for c in s['effective_controls']} for s in snaps]
print('snaps:',' | '.join('%d=%s'%(i,s['container_name']) for i,s in enumerate(snaps)))
for oid in ids:
    m=mods.get(oid,{}); c=byid[0][oid]
    lab=c.get('instance_label') or m.get('instance_label') or m.get('name') or ''
    cp=c.get('control_properties',{'range_endpoint_1':0,'range_endpoint_2':0,'step_or_resolution':0})
    vals=[byid[i][oid].get('value',byid[i][oid].get('normalized_value')) if oid in byid[i] else None for i in range(len(snaps))]
    fv=' '.join('%.3g'%v if v is not None else '-' for v in vals)
    print('%5d %-10s %-55s %-18s [%g..%g/%g] %s'%(oid,c['module_type_name'],c['hierarchy_path'][-55:],lab[:18],cp['range_endpoint_1'],cp['range_endpoint_2'],cp['step_or_resolution'],fv))
