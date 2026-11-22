"""Decode Bits_r6.ens (Reaktor 6 re-save of Bits.ens): per instrument controls + snapshot banks.
usage: decode_bits.py Bits_r6.ens report.json out.json"""
import json,sys,struct,re
data=open(sys.argv[1],'rb').read(); rep=json.load(open(sys.argv[2]))
u32=lambda o: struct.unpack_from('<I',data,o)[0]
f32=lambda o: struct.unpack_from('<f',data,o)[0]
INST={1331:'CLIX',2051:'STOMPS',2274:'NOIX',3069:'QUAD',4215:'CRAX',24044:'K1',132:'KIX'}
mods={};path={};inst={}
def walk(n,p,ins):
  mods[n['object_id']]=n
  nm=n.get('name') or n.get('instance_label') or n['type_name']
  path[n['object_id']]=p+'/'+nm
  if n['kind']==9: ins=n['object_id']
  inst[n['object_id']]=ins
  for c in n.get('children',[]): walk(c,path[n['object_id']],ins)
for t in rep['top_level']: walk(t,'',None)
offs=sorted(int(m['offset'],16) for m in mods.values())
import bisect
def label(m):
  o=int(m['offset'],16); i=bisect.bisect_right(offs,o)
  end=offs[i] if i<len(offs) else len(data)
  p=data.find(b'[\x07\x00\x00\x00KInPort',o,end); q=data.find(b'[\x08\x00\x00\x00KOutPort',o,end)
  stop=min(x for x in (p,q,end) if x>=0)
  best=None
  for k in range(o+0x40,stop-4):
    L=u32(k)
    if 1<=L<=40 and k+4+L<=stop:
      s=data[k+4:k+4+L]
      if all(32<=c<127 for c in s) and (k+4+L==stop or True):
        best=(k,s.decode())
  return best[1] if best else None
ctl={}
for oid,m in mods.items():
  if 'snapshot_control_id' in m:
    ctl.setdefault(inst[oid],{})[m['snapshot_control_id']]={'oid':oid,'kind':m['type_name'],'path':path[oid],
      'label':label(m),'range':m.get('control_properties'),'saved':m.get('saved_control_value')}
snaps=[]
for mt in re.finditer(rb'(.{4})([\x20-\x7e]{2,200})\x09\x00\x00\x00\x02\x00\x00\x00\x09\x00\x00\x00',data,re.S):
  if struct.unpack('<I',mt.group(1))[0]!=len(mt.group(2)): continue
  o=mt.end(); got=None
  for q in range(o,o+400,1):
    cnt=u32(q)
    if not (0<cnt<400): continue
    cur=q+4; recs={}; ok=True
    for i in range(cnt):
      if cur+16>len(data): ok=False;break
      rid,size=u32(cur),u32(cur+4)
      if size not in (0x20,0x28,0x4c) or u32(cur+8)!=9 or u32(cur+12)!=2: ok=False;break
      if size==0x28:
        typ=u32(cur+16)
        recs[rid]=f32(cur+40) if typ==2 else struct.unpack_from('<i',data,cur+40)[0]
      cur+=8+size
    if ok: got=recs;break
  if got is not None: snaps.append({'offset':mt.start(),'name':mt.group(2).decode(),'values':got})
json.dump({'controls':{INST.get(k,str(k)):v for k,v in ctl.items()},'snapshots':snaps},open(sys.argv[3],'w'),indent=1)
for s in snaps: print(hex(s['offset']),s['name'],len(s['values']),sorted(s['values'])[:5],max(s['values']) if s['values'] else '')
for k,v in ctl.items(): print(INST.get(k,k),len(v),sorted(v)[:3],max(v))
