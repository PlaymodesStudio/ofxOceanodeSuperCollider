import json,sys,math
exec(open('/private/tmp/claude-501/-Users-santiagovilanova-Documents-OF-of-playmodes-openFrameworks-addons-ofxOceanodeSuperCollider/7768c92a-ac06-40a9-b1b7-5a4548cbf300/scratchpad/anima/pitch_ref.py').read())
j=json.load(open('/private/tmp/claude-501/-Users-santiagovilanova-Documents-OF-of-playmodes-openFrameworks-addons-ofxOceanodeSuperCollider/7768c92a-ac06-40a9-b1b7-5a4548cbf300/scratchpad/anima/pitch/snapvals.json')); V=j['V']; names=j['names']
rows={}
for l in open('/private/tmp/claude-501/-Users-santiagovilanova-Documents-OF-of-playmodes-openFrameworks-addons-ofxOceanodeSuperCollider/7768c92a-ac06-40a9-b1b7-5a4548cbf300/scratchpad/anima/pitch/chords_table.txt').read().splitlines()[1:]:
    p=l.split(); rows[('C',int(p[0]))]=[float(x) for x in p[1:]]
for l in open('/private/tmp/claude-501/-Users-santiagovilanova-Documents-OF-of-playmodes-openFrameworks-addons-ofxOceanodeSuperCollider/7768c92a-ac06-40a9-b1b7-5a4548cbf300/scratchpad/anima/pitch/scales_table.txt').read().splitlines()[1:]:
    p=l.split(); rows[('S',int(p[0]))]=[float(x) for x in p[1:]]
cn={};sn={}
import re
for l in open('/private/tmp/claude-501/-Users-santiagovilanova-Documents-OF-of-playmodes-openFrameworks-addons-ofxOceanodeSuperCollider/7768c92a-ac06-40a9-b1b7-5a4548cbf300/scratchpad/anima/pitch/chords_list.txt').read().splitlines()[1:]:
    m=re.match(r'\s*(\d+) (.*?)\s+([\d.]+) id=(\d+)',l); cn[int(m.group(4))]=(m.group(2).strip(),int(float(m.group(3))))
for l in open('/private/tmp/claude-501/-Users-santiagovilanova-Documents-OF-of-playmodes-openFrameworks-addons-ofxOceanodeSuperCollider/7768c92a-ac06-40a9-b1b7-5a4548cbf300/scratchpad/anima/pitch/scales_list.txt').read().splitlines()[1:]:
    m=re.match(r'\s*(\d+) (.*?)\s+([\d.]+) id=(\d+)',l); sn[int(m.group(4))]=(m.group(2).strip(),int(float(m.group(3))))
def csize(v): return 0 if v<1 else 2 if v==1 else 3 if v<=14 else 4 if v<=41 else 5 if v<=82 else 6 if v<=108 else 7
def ssize(v): return 5 if v<=18 else 6 if v<=28 else 7 if v<=60 else 8 if v<=63 else 12
user=[[0,3,7,8,7,8,10,0,0,0,0,0]]+[[0,3,7,8,10,0,0,0,0,0,0,0]]*3
LY=[dict(chord=10966,scale=11076,ov=11222,user=11183,mode=11188,root=11193,oct=11197,ovQ=11226,cat=11231,mic=11238),
    dict(chord=11250,scale=11360,ov=11506,user=11467,mode=11472,root=11477,oct=11481,ovQ=11510,cat=11515,mic=11522),
    dict(chord=11534,scale=11644,ov=11790,user=11751,mode=11756,root=11761,oct=11765,ovQ=11794,cat=11799,mic=11806),
    dict(chord=11818,scale=11928,ov=12074,user=12035,mode=12040,root=12045,oct=12049,ovQ=12078,cat=12083,mic=12090)]
CAT={1:'Micro',2:'Overtones',3:'User',4:'Scales',5:'Chords'}
NN=['C','C#','D','D#','E','F','F#','G','G#','A','A#','B']
OVM=['Even','Odd','All']
def g(i,s): return V[str(i)][s]
def layer(li,s):
    L=LY[li]; cat=CAT[int(g(L['cat'],s))]
    nm=''; tab=None
    if cat=='Chords':
        nm,cv=cn[int(g(L['chord'],s))]; tab=rows[('C',min(cv,114))][:csize(cv)]; nm='%s (row %d)'%(nm,cv)
    elif cat=='Scales':
        nm,sv=sn[int(g(L['scale'],s))]; tab=rows[('S',min(sv,64))][:ssize(sv)]; nm='%s (row %d)'%(nm,sv)
    elif cat=='User': n=int(round(g(L['user'],s))); tab=user[li][:n]; nm='User n=%d'%n
    elif cat=='Micro': nm='%d-EDO'%int(round(g(L['mic'],s)))
    else: nm='Overtones %s, size %d'%(OVM[int(g(L['ov'],s))-1],int(round(g(11216,s))))
    return dict(cat=cat,name=nm,tab=[int(x) for x in tab] if tab else None,N=int(round(g(L['mic'],s))),ovm=int(g(L['ov'],s))-1,ovq=int(round(g(L['ovQ'],s))),
                root=int(round(g(L['root'],s))),oct=int(round(g(L['oct'],s))),mode=int(round(g(L['mode'],s))))
