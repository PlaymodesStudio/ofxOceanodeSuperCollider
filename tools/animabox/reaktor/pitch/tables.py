import json,struct,sys,bisect
S='/private/tmp/claude-501/-Users-santiagovilanova-Documents-OF-of-playmodes-openFrameworks-addons-ofxOceanodeSuperCollider/7768c92a-ac06-40a9-b1b7-5a4548cbf300/scratchpad'
r=json.load(open(S+'/anima/am.json'))
d=open('/Users/santiagovilanova/Downloads/AnimaMundi2_tips.ens','rb').read()
mods={};offs=[]
def walk(n):
    mods[n['object_id']]=n; offs.append(int(n['offset'],16))
    for c in n.get('children',[]): walk(c)
for t in r['top_level']: walk(t)
offs.sort()
PAT=bytes.fromhex('0000f042 0000803f 18000000 04000000'.replace(' ',''))
def table(oid):
    o=int(mods[oid]['offset'],16); end=offs[bisect.bisect_right(offs,o)]
    seg=d[o:end]
    a0=seg.find(PAT); a=seg.find(PAT,a0+1); b=seg.find(PAT,a+1)
    st=b+len(PAT)
    # DX,DY: search backwards for int pair before 0, 12.0
    k=seg.rfind(b'\x00\x00\x40\x41',0,a)  # float 12.0? fallback
    dx,dy=struct.unpack('<ii',seg[k-12:k-4])
    if dx>200 or dy<=0: dx,dy=struct.unpack('<ii',seg[k-8:k])
    data=[struct.unpack('<f',seg[st+12+4*j:st+16+4*j])[0] for j in range(dx*dy)]
    hdr=[struct.unpack('<f',seg[st+4*j:st+4*j+4])[0] for j in range(3)]
    return dx,dy,hdr,[data[y*dx:(y+1)*dx] for y in range(dy)]
if __name__=='__main__':
    for a in sys.argv[1:]:
        dx,dy,hdr,rows=table(int(a))
        print(a,mods[int(a)]['hierarchy_path'][60:],'DX=%d DY=%d hdr=%s'%(dx,dy,hdr))
        for y,row in enumerate(rows): print('  %3d'%y,' '.join('%g'%v for v in row))
