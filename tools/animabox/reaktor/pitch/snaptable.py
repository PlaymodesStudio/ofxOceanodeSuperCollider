import math
S='/private/tmp/claude-501/-Users-santiagovilanova-Documents-OF-of-playmodes-openFrameworks-addons-ofxOceanodeSuperCollider/7768c92a-ac06-40a9-b1b7-5a4548cbf300/scratchpad'
L=open(S+'/anima/controls.txt').read().splitlines()
names=[x.split('=',1)[1].strip() for x in L[0].split(':',1)[1].split('|')]
vals={}
for l in L[1:]:
    p=l.split()
    oid=int(p[0]); v=p[-20:]
    vals[oid]=[float(x) if x not in('-',) else None for x in v]
def g(i,s): return vals[i][s]
layers=[dict(user=11183,mode=11188,root=11193,oct=11197,ovN=11216,ovQ=11226,cat=11231,mic=11238,chord=('m9+5 (row14)',[0,2,10]),utab=[0,3,7,8,7,8,10]),
        dict(user=11467,mode=11472,root=11477,oct=11481,ovN=11500,ovQ=11510,cat=11515,mic=11522,chord=('m9 (row43)',[0,2,3,7,10]),utab=[0,3,7,8,10]),
        dict(user=11751,mode=11756,root=11761,oct=11765,ovN=11784,ovQ=11794,cat=11799,mic=11806,chord=('9 (row46)',[0,2,4,7,10]),utab=[0,3,7,8,10]),
        dict(user=12035,mode=12040,root=12045,oct=12049,ovN=12068,ovQ=12078,cat=12083,mic=12090,chord=('M13 (row44)',[0,4,7,9,11]),utab=[0,3,7,8,10])]
CAT={1:'Micro',2:'Overtones',3:'User',4:'Scales',5:'Chords'}
NN=['C','C#','D','D#','E','F','F#','G','G#','A','A#','B']
def desc(Ly,s):
    c=int(g(Ly['cat'],s)); cat=CAT[c]
    if cat=='Scales': sc='chinese [0,4,6,7,11]'
    elif cat=='Chords': sc='%s %s'%(Ly['chord'][0],Ly['chord'][1])
    elif cat=='User': n=int(g(Ly['user'],s)); sc='user n=%d %s'%(n,Ly['utab'][:n])
    elif cat=='Micro': N=int(g(Ly['mic'],s)); sc='%d-EDO (step %.1f c)'%(N,1200/N if N else float('nan'))
    else: sc='Overtones Even h=2j+2 (P=1+12log2 h), N=%d, quant=%d'%(g(Ly['ovN'],s),g(Ly['ovQ'],s))
    r=int(g(Ly['root'],s)); o=int(g(Ly['oct'],s)); m=int(g(Ly['mode'],s))
    return '%s: %s; root=%d(%s) oct=%d base=%d; mode=%d'%(cat,sc,r,NN[r%12],o,r+12*o,m)
for s in range(19):
    print('| %d %s | Len=%d | Key=%d | Tr=%+d | PMoct=%+d | %s |'%(s,names[s],g(10701,s),g(10883,s),g(10850,s),g(5490,s),
          ' || '.join('L%d %s'%(k+1,desc(layers[k],s)) for k in range(4) if k==0 or k<int(g(10701,s)))))
