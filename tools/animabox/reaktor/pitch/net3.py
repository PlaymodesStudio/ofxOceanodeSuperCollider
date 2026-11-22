import json,sys
r=json.load(open(sys.argv[1])); cat=json.load(open('tools/ReaktorPatchExtractor/module_catalog.json'))['modules']
want=[int(x) for x in sys.argv[2:]]
mods={}
def walk(n):
    mods[n['object_id']]=n
    for c in n.get('children',[]): walk(c)
for t in r['top_level']: walk(t)
pn={}
for p in r['ports']:
    pn.setdefault((p['source_object_id'],p['class_name']),[]).append(p.get('name'))
def plab(oid,io,idx):
    m=mods[oid]; c=cat.get(str(m['kind']),{})
    lst=c.get('inputs' if io=='i' else 'outputs')
    if lst and idx<len(lst): return lst[idx].get('label') or str(idx)
    names=pn.get((oid,'KInPort' if io=='i' else 'KOutPort'))
    if names and idx<len(names) and names[idx]: return names[idx]
    return str(idx)
ENS=open('/Users/santiagovilanova/Downloads/AnimaMundi2_tips.ens','rb').read()
import re,struct
def ioname(m):
    o=int(m['offset'],16); b=ENS[o:o+0x400]; k=b.find(b'\x00\xef\x22\x00')
    seg=b[k+4+35*4:k+4+35*4+80]
    t=struct.unpack('<i',seg[:4])[0]
    n=struct.unpack('<i',seg[4:8])[0]
    if t&0xff00==0x500 and 0<n<40: return seg[8:8+n].decode('latin1')
    if t&0xff00==0x500 and n==0: return ''
    mm=re.search(rb'[ -~]{2,}',seg); return '?'+(mm.group().decode() if mm else '')
def lab(m):
    s=m['type_name']
    if s in ('In','Out'): s+=" <%s>"%ioname(m)
    for k in ('name','instance_label','constant_label'):
        if m.get(k): s+=" '%s'"%m[k]
    if 'constant_numeric_value' in m: s+=' =%g'%m['constant_numeric_value']
    if 'control_properties' in m: cp=m['control_properties']; s+=' [%g..%g]'%(cp['range_endpoint_1'],cp['range_endpoint_2'])
    if m.get('module_properties'): s+=' props=%s'%json.dumps(m['module_properties'])
    if m.get('instance_description'): s+='  // '+m['instance_description'][:100].replace('\n',' ')
    return s
def short(oid):
    m=mods[oid]
    if m['type_name'] in ('In','Out'): return '%s<%s>#%d'%(m['type_name'],ioname(m),oid)
    return '%s#%d'%((m.get('name') or m.get('instance_label') or m['type_name'])[:16],oid)
for w in want:
    M=mods[w]; kids={c['object_id'] for c in M['children']}
    print('==',w,lab(M), M['hierarchy_path'])
    for c in M['children']: print('  %d %s'%(c['object_id'],lab(c)))
    for c in r['connections']:
        s,t=c.get('source_object_id'),c.get('target_object_id')
        if s in kids|{w} and t in kids|{w}:
            if s==w or t==w: continue
            print('   %s.%s -> %s.%s'%(short(s),plab(s,'o',c['source_output_index']),short(t),plab(t,'i',c['target_input_index'])))
