"""Python twin of src/nodes/scPolyCombPitchMap.h (PolyComb's internal pitch maps).
The importer uses it for what depends on the played notes (FM depth in Hz, ring
pitch, audit); pitchmap_test.py checks it against the C++ engine."""
import math
OFF,TABLE,TONES,HARMONIC=0,1,2,3
NEAREST,DOWN,UP=0,1,2
def voice_index(m,i,n):
    n=max(1,n);v=m['first']+i
    if m['shape']==1:v=n*.5-abs(v-n*.5)+1
    if abs(m['bend']-1)>1e-6:
        x=v/n;v=(-((-x)**m['bend']) if x<0 else x**m['bend'])*n
    return v
def apply_table(p,table,root):
    rel=p-root;octave=math.floor(rel/12);pc=min(11,max(0,math.floor(rel-octave*12)))
    return octave*12+table[min(pc,len(table)-1)]+root
def quantize_tones(p,tones,root,direction):
    if not tones:return p
    rel=p-root;octave=math.floor(rel/12);best=p;distance=1e9
    for o in (-1,0,1):
        for t in tones:
            c=(octave+o)*12+t;d=c-rel
            if direction==DOWN and d>1e-4:continue
            if direction==UP and d<-1e-4:continue
            if abs(d)<distance-1e-6 or (abs(abs(d)-distance)<=1e-6 and c+root<best):distance=abs(d);best=c+root
    return best if distance<1e8 else p
def generate(m,n,shift=None):
    out=[]
    for i in range(n):
        s=0 if not shift else shift[min(i,len(shift)-1)];index=voice_index(m,i,n)
        if m['mode']==HARMONIC:
            h=m['interval']*index;base=m['harmonic_base']+s
            p=base+12*math.log2(h) if h>1e-6 else base-12*math.log2(-h) if h<-1e-6 else base
            p=max(-127,min(127,p))
        else:
            rel=m['interval']*index
            if m.get('span',0)>1e-6:rel=rel%m['span']
            p=60+m['transpose']+rel+s
            if m['mode']==TABLE:p=apply_table(p,m['table'],round(m['root']))
            elif m['mode']==TONES:p=quantize_tones(p,m['tones'],round(m['root']),m['quantize'])
        out.append(p+m['offset'])
    return out
