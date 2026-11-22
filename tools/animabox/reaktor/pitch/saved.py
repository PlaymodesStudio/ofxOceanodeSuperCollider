import json,sys,struct
S='/private/tmp/claude-501/-Users-santiagovilanova-Documents-OF-of-playmodes-openFrameworks-addons-ofxOceanodeSuperCollider/7768c92a-ac06-40a9-b1b7-5a4548cbf300/scratchpad'
r=json.load(open(S+'/anima/am.json'))
d=open('/Users/santiagovilanova/Downloads/AnimaMundi2_tips.ens','rb').read()
mods={}
def walk(n):
    mods[n['object_id']]=n
    for c in n.get('children',[]): walk(c)
for t in r['top_level']: walk(t)
for a in sys.argv[1:]:
    m=mods[int(a)]; o=int(m['offset'],16)+0x112
    f=struct.unpack('<f',d[o:o+4])[0]; i=struct.unpack('<i',d[o:o+4])[0]
    cp=m.get('control_properties')
    s='%s %s %s norm=%g int=%d'%(a,m['type_name'],m.get('name') or m.get('instance_label') or '',f,i)
    if cp: s+=' range[%g..%g] -> %g'%(cp['range_endpoint_1'],cp['range_endpoint_2'],cp['range_endpoint_1']+(cp['range_endpoint_2']-cp['range_endpoint_1'])*f)
    print(s)
