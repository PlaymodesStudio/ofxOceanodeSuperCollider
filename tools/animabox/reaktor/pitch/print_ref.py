import os
H=os.path.dirname(os.path.abspath(__file__))
exec(open(os.path.join(H,'run_ref.py')).read())
for s in range(19):
    B=int(round(g(5909,s))); Ly=layer(0,s)
    key=round(g(10883,s)); pmo=round(g(5490,s))
    ps=[]
    for v in range(1,B+1):
        idx=pitch_index(v,B,step=g(12297,s),skew=g(12414,s),rdev=g(12365,s))
        ps.append(layer_degree(idx+Ly['mode']+key,Ly['cat'],Ly['tab'],Ly['N'],Ly['ovm'],Ly['ovq'])+Ly['root']+12*Ly['oct']+12*pmo)
    fin=[p for p in ps if p==p]
    print(s,names[s],'B=%d'%B,Ly['cat'],Ly['name'],Ly['tab'],'| v1..6',[round(p,2) for p in ps[:6]],'min %.1f max %.1f'%(min(fin),max(fin)),'nan:%d'%(len(ps)-len(fin)))
