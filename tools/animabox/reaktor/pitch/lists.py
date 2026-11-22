import json,struct,sys,re,bisect
S='/private/tmp/claude-501/-Users-santiagovilanova-Documents-OF-of-playmodes-openFrameworks-addons-ofxOceanodeSuperCollider/7768c92a-ac06-40a9-b1b7-5a4548cbf300/scratchpad'
r=json.load(open(S+'/anima/am.json'))
d=open('/Users/santiagovilanova/Downloads/AnimaMundi2_tips.ens','rb').read()
mods={};offs=[]
def walk(n):
    mods[n['object_id']]=n; offs.append(int(n['offset'],16))
    for c in n.get('children',[]): walk(c)
for t in r['top_level']: walk(t)
offs.sort()
def entries(oid):
    o=int(mods[oid]['offset'],16)
    end=offs[bisect.bisect_right(offs,o)]
    seg=d[o:end]
    best=None
    for p in range(0,len(seg)-8):
        cnt=struct.unpack('<i',seg[p:p+4])[0]
        if not (2<=cnt<=400): continue
        q=p+4; ents=[]; ok=True
        for k in range(cnt):
            if q+12>len(seg): ok=False;break
            L=struct.unpack('<i',seg[q:q+4])[0]
            if not (0<=L<60) or q+12+L>len(seg): ok=False;break
            name=seg[q+4:q+4+L]
            v=struct.unpack('<f',seg[q+4+L:q+8+L])[0]; tid=struct.unpack('<i',seg[q+8+L:q+12+L])[0]
            ents.append((name.decode('latin1'),v,tid)); q+=12+L
        if ok and (best is None or cnt>len(best[1])): best=(p,ents)
    return best
if __name__=='__main__':
    full='-v' in sys.argv
    for a in [x for x in sys.argv[1:] if x!='-v']:
        b=entries(int(a))
        o=int(mods[int(a)]['offset'],16)
        print(a, mods[int(a)]['hierarchy_path'][40:], 'saved@0x112=',struct.unpack('<i',d[o+0x112:o+0x116])[0], 'n=',len(b[1]) if b else None)
        if b and (full or len(b[1])<20):
            for k,(n,v,t) in enumerate(b[1]): print('  %3d %-24s %g id=%d'%(k,n,v,t))
