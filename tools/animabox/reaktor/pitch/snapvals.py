import json,struct
S='/private/tmp/claude-501/-Users-santiagovilanova-Documents-OF-of-playmodes-openFrameworks-addons-ofxOceanodeSuperCollider/7768c92a-ac06-40a9-b1b7-5a4548cbf300/scratchpad'
r=json.load(open(S+'/anima/am.json'))
d=open('/Users/santiagovilanova/Downloads/AnimaMundi2_tips.ens','rb').read()
mods={}
def walk(n):
    mods[n['object_id']]=n
    for c in n.get('children',[]): walk(c)
for t in r['top_level']: walk(t)
def rng(oid):
    m=mods[oid]; cp=m.get('control_properties')
    o=int(m['offset'],16); b=d[o:o+0x80]
    f=[struct.unpack('<f',b[j:j+4])[0] for j in range(0x40,0x68,4)]
    return (f[7],f[8])  # raw range from record
SN=[s['container_name'] for s in r['snapshots']]
V={}
for si,s in enumerate(r['snapshots']):
    for p in s['parameters']:
        oid=p.get('module_object_id')
        if oid is None: continue
        if p.get('value_type')=='normalized_float':
            m=mods[oid]; cp=m.get('control_properties')
            lo,hi=(cp['range_endpoint_1'],cp['range_endpoint_2']) if cp and cp['range_endpoint_1']!=cp['range_endpoint_2'] else rng(oid)
            V.setdefault(oid,[None]*20)[si]=lo+(hi-lo)*p['value']
        else:
            V.setdefault(oid,[None]*20)[si]=p.get('value')
json.dump({'names':SN,'V':V},open(S+'/anima/pitch/snapvals.json','w'))
