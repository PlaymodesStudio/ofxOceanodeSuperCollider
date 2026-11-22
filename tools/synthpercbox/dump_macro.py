"""Print a macro's modules and internal wires. usage: dump_macro.py report.json decoded.json OBJECT_ID [depth]"""
import json,sys
rep=json.load(open(sys.argv[1])); dec=json.load(open(sys.argv[2])); root=int(sys.argv[3]); depth=int(sys.argv[4]) if len(sys.argv)>4 else 0
mods={};par={}
def walk(n,p):
  mods[n['object_id']]=n; par[n['object_id']]=p
  for c in n.get('children',[]): walk(c,n['object_id'])
for t in rep['top_level']: walk(t,None)
lab={}
for ins,cs in dec['controls'].items():
  for sid,c in cs.items(): lab[c['oid']]=f"{c['label']} sid{sid} {c['range']}"
def nm(o):
  m=mods.get(o)
  if not m: return f'?{o}'
  s=m.get('name') or m.get('instance_label') or ''
  extra=m.get('constant_value') if m['kind']==100 else lab.get(o,'')
  return f"{m['type_name']}#{o}{'['+s+']' if s else ''}{' ='+str(extra) if extra else ''}"
def inside(o,root,d):
  k=0
  while o is not None and k<=d:
    if o==root: return True
    o=par[o]; k+=1
  return False
pn={}
for p in rep['ports']:
  if p['class_name']=='KInPort' and 'name' in p: pn[(p.get('source_object_id'),p.get('source_port_record_index'))]=p['name']
onames={}
for p in rep['ports']:
  if p['class_name']=='KOutPort' and 'name' in p: onames[(p.get('source_object_id'),p.get('source_port_record_index'))]=p['name']
for c in mods[root].get('children',[]):
  print('  ',nm(c['object_id']), c.get('child_count') or '')
print('wires:')
for w in rep['connections']:
  s,t=w.get('source_object_id'),w.get('target_object_id')
  if s in mods and t in mods and (inside(s,root,depth+1) or inside(t,root,depth+1)):
    print(f"  {nm(s)}.{onames.get((s,w['source_output_index']),w['source_output_index'])} -> {nm(t)}.{pn.get((t,w['target_input_index']),w['target_input_index'])}")
