"""Reference model of AnimaMundi band pitch (Pitch Index -> Harmony layer -> Pitch Modifiers).
Deterministic part only (random sources set to 0). See spec_pitch.md.
Functions only. Per-snapshot printout: python3 -I pitch/print_ref.py (uses pitch/run_ref.py + pitch/snapvals.json)"""
import json, math, os
HERE=os.path.dirname(os.path.abspath(__file__))
def rnd(x): return math.floor(x+0.5)          # Reaktor Quantize St=1 (assumed round-half-up)
def fmod(a,b): return a-b*math.floor(a/b)      # Reaktor Modulo (floor based)
def det_curve(ph, skew, rnd_=0.0, a=0.0):
    x=min(max(ph,a),1-a); x=(x-a)/(1-2*a)                     # cell 12453 (Fader12454 = a)
    skew=max(-0.999999,min(0.999999,skew)); c=(skew+1)/2      # cell 12415 (fader range clamp)
    w=0.5*x/c if x<=c else 0.5+0.5*(x-c)/(1-c)
    tri=2*w if w<0.5 else 2-2*w                                # cell 12416
    g=2-abs(skew)
    s0=tri/g; s1=(1-math.cos(2*math.pi*w))/2/g                  # Selector 12418 (round 0..1 crossfade)
    y=s0+(s1-s0)*min(max(rnd_,0),1)
    Y=min(max(2-rnd_,0),1)                                     # x^y macro exponent
    u=2*y-1; return (math.copysign(abs(u)**Y,u)+1)/2
def pitch_index(V,B,step=1,f1=1,f2=0,rept=1,comb=0,invert=0,symm=1,rdev=0,shape=0,q=0,skew=0.999999,rround=0,a=0):
    k=rnd(V/rept)
    ph=fmod(k/(B+1)*f1+f2,1.0)
    v=rnd((B+1)*det_curve(ph,skew,rround,a))
    if invert: v=1+(B+1)-v
    cm=rnd(abs((B+1)*fmod(k,2)*comb - v))
    st=step/f1 if f1 else 0
    val=cm*st
    P=(st*(B+1))/symm
    if P: m=fmod(val,P); n=math.floor(val/P); val=abs(fmod(n,2)*P-m)*symm
    else: val=0.0                                               # UNCERTAIN: Reaktor x%0
    val=rnd(abs(val+0))                                         # rnd dev with deviation 0
    if shape:
        A=0.999*shape; kk=2*A/(1-A); x=val/(B+1); val=(B+1)*x*(kk+1)/(kk*abs(x)+1)
    if q: val=q*rnd(val/q)
    return val
def layer_degree(j, cat, table=None, N=None, ov_mode=0, ov_quant=0):
    if cat in ('Chords','Scales','User'):
        n=len(table); return table[int(fmod(j,n))]+12*math.floor(j/n)
    if cat=='Micro': return j*12.0/N
    h={0:2*j+2,1:2*j+1,2:j+1}[ov_mode]
    p=1+12*math.log2(h) if h>0 else float('nan')               # UNCERTAIN for h<=0
    return rnd(p) if ov_quant else p
