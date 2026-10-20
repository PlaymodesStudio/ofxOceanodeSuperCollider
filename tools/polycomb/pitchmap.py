"""Python twin of src/nodes/scPolyCombPitchMap.h (PolyComb's internal pitch maps).
The importer uses it for what depends on the played notes (FM depth in Hz, ring
pitch, audit); pitchmap_test.py checks it against the C++ engine."""
import math
OFF,TABLE,TONES,HARMONIC=0,1,2,3
NEAREST,DOWN,UP=0,1,2
def snap_integer(x):
    integer=math.floor(x+.5) if x>=0 else math.ceil(x-.5)
    return integer if abs(x-integer)<1e-4 else x
def normalized_pow(x,amount):
    x=max(0,min(1,x));amount=max(-1,min(1,amount))
    if x in (0,1) or abs(amount)<1e-6:return x
    strength=6*abs(amount);k=math.expm1(strength)
    return math.log1p(k*x)/strength if amount>0 else math.expm1(strength*x)/k

def morph(phase,roundness,skew,pw,centered=False):
    def mapval(x,a,b,c,d):return c+(d-c)*max(0,min(1,(x-a)/(b-a)))
    lin=phase%1;pw=max(.001,min(1,pw))
    if pw<.5:
        lin=mapval(lin,.5-pw,.5+pw,0,1) if centered else mapval(lin,0,max(pw*2,1e-7),0,1)
        if skew<0 and lin>=1-1e-6:lin=0
    elif pw>=1-1e-6:lin=mapval(skew,-1,1,1,0)
    else:
        mid=mapval(skew,-1,1,1,0)
        if lin<mid:lin=mapval(lin,0,max(mapval(1-pw,0,.5,0,mid),1e-7),0,mid)
        else:lin=mapval(lin,min(mapval(pw,.5,1,mid,1),1-1e-7),1,mid,1)
    threshold=max(0,min(1,.5-skew*.5))
    lin=mapval(lin,0,max(threshold,1e-7),0,.5) if lin<threshold else mapval(lin,min(threshold,1-1e-7),1,.5,1)
    tri=max(0,1-abs(2*lin-1));cosraw=math.cos(2*math.pi*lin+math.pi)
    if roundness>=1:return float(.25<=lin<.75)
    if roundness>=.5:
        power=(roundness-.5)*2;k1=2*power*.99999;k2=k1/(1-power*.999999)
        v=cosraw*(k2+1)/max(k2*abs(cosraw)+1,1e-6)
        return max(0,min(1,(v+1)*.5))
    return max(0,min(1,tri+((cosraw+1)*.5-tri)*roundness*2))

def raw_voice_index(m,i,n):
    n=max(1,n);v=m['first']+i
    if m.get('morph_geometry',False):
        position=(v-1)*m.get('waves',1)
        phase=position%n/n
        v=1+n*morph(phase,m.get('roundness',0),m.get('skew',-1),m.get('pulse_width',.5))
        shift=(math.floor(position/n)*m.get('cycle_shift',0))%n
        if shift!=0:v=1+(max(0,min(n-1,v-1))+shift)%n
    else:
        waves=m.get('waves',1)
        if abs(waves-1)>1e-6:v=1+((v-1)*waves)%n
        if m['shape']==2:v=1+(n-1)*(.5-.5*math.cos(2*math.pi*(v-1)/n))
        if m['shape']==1:v=n*.5-abs((v-1)-n*.5)+1
        if abs(m['bend']-1)>1e-6:
            width=max(1,n-1);x=(v-1)/width;v=1+(-((-x)**m['bend']) if x<0 else x**m['bend'])*width
    if m.get('invert',False):v=n+1-v
    symmetry=m.get('symmetry',0)
    if symmetry>0:
        width=max(1,n/(symmetry+1));v=width-abs((v-1)%(width*2)-width)+1
    v=1+snap_integer(v-1)
    comb=m.get('comb',0)
    if comb>0 and math.floor(v-1)%2:v+=(n+1-2*v)*comb
    quant=m.get('index_quantize',0)
    if quant>1e-6:v=1+math.floor(snap_integer((v-1)/quant))*quant
    amount=m.get('index_random',0)
    if amount>0:
        h=(i*747796405+m.get('seed',0)*2891336453+277803737)&0xffffffff
        h=(((h>>((h>>28)+4))^h)*277803737)&0xffffffff;h=(h>>22)^h
        v+=(h/4294967295*2-1)*amount
    return max(1,min(n,v)) if m.get('morph_geometry',False) else v
def shaped_voice_index(m,i,n):
    v=raw_voice_index(m,i,n)
    if not m.get('morph_geometry',False) or abs(m.get('pow',0))<1e-6:return v
    values=[raw_voice_index(m,j,n)for j in range(n)];lo=min(values);hi=max(values)
    return v if hi-lo<1e-6 else lo+(hi-lo)*normalized_pow((v-lo)/(hi-lo),m['pow'])
def voice_index(m,i,n):
    v=shaped_voice_index(m,i,n)
    half=(math.floor(v*2+.5)if v>=0 else math.ceil(v*2-.5))*.5
    if abs(v-half)<1e-5:v=half
    return math.floor(v+.5) if v>=0 else math.ceil(v-.5)
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
            if abs(d)<distance-1e-4 or (abs(abs(d)-distance)<=1e-4 and c+root<best):distance=abs(d);best=c+root
    return best if distance<1e8 else p
def legacy_voice_index(m,i,n):
    n=max(1,n);v=m['first']+i
    if m['shape']==1:v=n*.5-abs(v-n*.5)+1
    if abs(m['bend']-1)>1e-6:
        x=v/n;v=(-((-x)**m['bend'])if x<0 else x**m['bend'])*n
    return v
def generate_legacy(m,n,shift=None):
    out=[]
    for i in range(n):
        s=0 if not shift else shift[min(i,len(shift)-1)];index=legacy_voice_index(m,i,n)
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

def user_scale(notes):
    out=[]
    for x in notes:
        note=max(0,min(11,math.floor(x+.5)))
        if note not in out:out.append(note)
    return out or [0]
def generate(m,n,shift=None):
    if m['mode']!=TABLE:
        out=[]
        for i in range(n):
            s=0 if not shift else shift[min(i,len(shift)-1)];index=voice_index(m,i,n)
            if m['mode']==HARMONIC:
                h=m['interval']*index;base=m['harmonic_base']+m['transpose']+s
                p=base+12*math.log2(h) if h>1e-6 else base-12*math.log2(-h) if h<-1e-6 else base
                p=max(-127,min(127,p))
            else:
                rel=m['interval']*index
                if m.get('span',0)>1e-6:rel%=m['span']
                p=60+m['transpose']+rel+s
                if m['mode']==TONES:p=quantize_tones(p,m['tones'],round(m['root']),m['quantize'])
            out.append(p+m['offset'])
        return out
    scale=user_scale(m['table']);out=[]
    for i in range(n):
        index=voice_index(m,i,n)
        degree=m['interval']*(index-1)+m.get('degree_offset',0)
        if m.get('span',0)>1e-6:degree%=m['span']
        degree=math.floor(degree+1e-5)
        s=0 if not shift else shift[min(i,len(shift)-1)]
        p=60+m['transpose']+m['root']+12*m.get('octave_jump',1)*(degree//len(scale))+scale[degree%len(scale)]
        if abs(s)>1e-6:p=quantize_tones(p+s,scale,m['root']+m['transpose'],m['quantize'])
        out.append(p+m['offset'])
    return out
