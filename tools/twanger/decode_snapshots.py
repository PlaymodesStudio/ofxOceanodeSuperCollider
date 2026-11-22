import json,sys,struct,re
data=open(sys.argv[1],'rb').read(); rep=json.load(open(sys.argv[2]))
u32=lambda o: struct.unpack_from('<I',data,o)[0]
mods={};par={}
def walk(n,p):
  mods[n['object_id']]=n; par[n['object_id']]=p
  for c in n.get('children',[]): walk(c,(n.get('name') or n['type_name'])+'#%d'%n['object_id'])
for t in rep['top_level']: walk(t,'')
ctl={}
for oid,m in mods.items():
  if 'control_properties' in m and par[oid]!='Instrument#368':
    ctl[m.get('snapshot_control_id',0)]=m
snaps=[]
for mt in re.finditer(rb'(.{4})([\x20-\x7e]{2,200})\x09\x00\x00\x00\x02\x00\x00\x00\x09\x00\x00\x00',data,re.S):
  if struct.unpack('<I',mt.group(1))[0]!=len(mt.group(2)): continue
  o=mt.end(); found=None
  for q in range(o,o+400):
    if u32(q+8) in (0x20,0x28,0x4c) and u32(q+12)==9 and u32(q+16)==2 and u32(q+4)==0 and 10<u32(q)<200: found=q;break
  if found is None: continue
  cnt=u32(found); cur=found+4; recs={}
  for i in range(cnt):
    rid=u32(cur); size=u32(cur+4); typ=u32(cur+16)
    if size==0x28:
      raw=data[cur+40:cur+44]
      recs[rid]=('f',struct.unpack('<f',raw)[0]) if typ==2 else ('i',struct.unpack('<i',raw)[0])
    cur+=8+size
  snaps.append({'name':mt.group(2).decode(),'recs':recs})
out={'controls':[],'snapshots':[]}
for sid,m in sorted(ctl.items()):
  o=int(m['offset'],16)
  out['controls'].append({'sid':sid,'oid':m['object_id'],'macro':par[m['object_id']],'label':m.get('instance_label') or m['type_name'],
    'kind':m['type_name'],'min':m['control_properties']['range_endpoint_1'],'max':m['control_properties']['range_endpoint_2'],
    'step':m['control_properties']['step_or_resolution'],'cc':u32(o+0x44),'desc':m.get('instance_description','')})
for s in snaps:
  vals={}
  for sid,(t,v) in s['recs'].items():
    if sid in ctl: vals[sid]=v
  out['snapshots'].append({'name':s['name'],'values':vals})
json.dump(out,open(sys.argv[3],'w'),indent=1)
# correlation per cc
from collections import defaultdict
g=defaultdict(list)
for c in out['controls']:
  if c['kind']=='Fader': g[c['cc']].append(c['sid'])
for cc,sids in sorted(g.items()):
  if len(sids)<2: print(cc,sids,'single'); continue
  base=sids[0]
  for s2 in sids[1:]:
    eq=sum(1 for s in out['snapshots'] if abs(s['values'].get(base,9)-s['values'].get(s2,8))<1e-4)
    print(cc, base, s2, eq,'/',len(out['snapshots']))
