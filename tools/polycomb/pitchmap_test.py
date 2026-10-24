#!/usr/bin/env python3
"""pitchmap.py (the importer's) against scPolyCombPitchMap.h (the node's): random maps, every mode."""
import json,pathlib,random,subprocess,tempfile
import pitchmap
ROOT=pathlib.Path(__file__).resolve().parents[2]
rng=random.Random(7);cases=[]
for k in range(400):
    m=dict(shape=rng.randint(0,2),first=float(rng.choice([0,1,2])),interval=round(rng.uniform(-12,12),2),transpose=round(rng.uniform(-24,24),2),
           bend=rng.choice([1,1,2.5,3.2]),offset=round(rng.uniform(-3,3),2),mode=rng.randint(0,3),quantize=rng.randint(0,2),root=float(rng.randint(0,11)),
           tones=sorted(rng.sample(range(12),rng.randint(1,8))),table=[rng.randint(0,11) for _ in range(12)],harmonic_base=round(rng.uniform(24,96),1),span=rng.choice([0,0,6,12,24]))
    m['octave_jump']=rng.choice([0,.25,.5,1,1.25,2])
    n=rng.choice([1,4,16,24,32]);shift=[round(rng.uniform(-12,12))] if rng.random()<.5 else []
    cases.append((m,n,shift))
src=r'''
#include "nodes/boxSynths/polyComb/scPolyCombPitchMap.h"
#include <cstdio>
#include <vector>
int main(){ int k; while(std::scanf("%d",&k)==1){
  pcmap::Map m; int nt,n,ns; float x;
  std::scanf("%d %f %f %f %f %f %d %d %f %f %f",&m.shape,&m.first,&m.interval,&m.transpose,&m.bend,&m.offset,&m.mode,&m.quantize,&m.root,&m.harmonicBase,&m.span);
  std::scanf("%f",&m.octaveJump);
  std::scanf("%d",&nt); m.tones.clear(); for(int i=0;i<nt;i++){std::scanf("%f",&x);m.tones.push_back(x);}
  m.table.clear(); for(int i=0;i<12;i++){std::scanf("%f",&x);m.table.push_back(x);}
  std::scanf("%d %d",&n,&ns); std::vector<float> s; for(int i=0;i<ns;i++){std::scanf("%f",&x);s.push_back(x);}
  for(float p: pcmap::generate(m,n,s)) std::printf("%.4f ",p); std::printf("\n"); } }
'''
with tempfile.TemporaryDirectory() as d:
    c=pathlib.Path(d)/'t.cpp';c.write_text(src);exe=pathlib.Path(d)/'t'
    subprocess.run(['clang++','-std=c++17','-I'+str(ROOT/'src'),str(c),'-o',str(exe)],check=True)
    lines=[]
    for k,(m,n,s) in enumerate(cases):
        lines.append(f"{k} {m['shape']} {m['first']} {m['interval']} {m['transpose']} {m['bend']} {m['offset']} {m['mode']} {m['quantize']} {m['root']} {m['harmonic_base']} {m['span']} {m['octave_jump']} {len(m['tones'])} {' '.join(map(str,m['tones']))} {' '.join(map(str,m['table']))} {n} {len(s)} {' '.join(map(str,s))}")
    out=subprocess.run([str(exe)],input='\n'.join(lines)+'\n',capture_output=True,text=True,check=True).stdout.splitlines()
for (m,n,s),line in zip(cases,out):
    c=[float(x) for x in line.split()];p=pitchmap.generate(m,n,s)
    assert len(c)==n and all(abs(a-b)<2e-3 for a,b in zip(c,p)),(m,n,s,c[:4],p[:4])
print(f'PASS: pitchmap.py == scPolyCombPitchMap.h on {len(cases)} random maps')
