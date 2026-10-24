#!/usr/bin/env python3
"""Check production noise for collective crossings, continuity and voice spacing."""
import pathlib,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[2]
s=(ROOT/'src/nodes/boxSynths/shared/scModulation.cpp').read_text();a=s.index('float scModulation::noise2(');b=s.index('\n}',a)+2;method=s[a:b]
# All server-backed noise paths must use exactly the same algorithm.
bodies=[]
for f in ['Modulation/modulation.scd','Modulation/modulationRandom.scd','AdditiveBox/additiveBox.scd','GrainBox/scGrainBoxPoly.scd']:
 s=(ROOT/'synthdefs/Defaults'/f).read_text();a=s.index('valueNoise2 = {');b=s.index('\n};',a)+3;bodies.append(s[a:b])
assert len(set(bodies))==1,'Noise implementations diverged between synths'
cpp=r'''
#include <cmath>
#include <algorithm>
#include <cassert>
#include <iostream>
template<class T>T ofClamp(T x,T a,T b){return std::clamp(x,a,b);}
struct scModulation {static float noise2(float,float,float=0);};
METHOD
int main(){
 for(float scale:{.25f,1.f,2.f})for(float salt:{0.f,17.31f}){
  for(int cycle=-16;cycle<=128;++cycle){
   for(float phase:{0.f,.125f,.5f,.999f}){
    float lo=1,hi=0,changes=0;
    for(int v=0;v<32;++v){
     float t=cycle+phase,y=v*scale,n=scModulation::noise2(t,y,salt);
     assert(std::isfinite(n)&&n>=0&&n<=1);assert(n==scModulation::noise2(t,y,salt));
     lo=std::min(lo,n);hi=std::max(hi,n);
     float next=scModulation::noise2(t+.001f,y,salt);
     assert(std::abs(next-n)<.006f);changes+=std::abs(next-n);
    }
    assert(hi-lo>.05f); // no collective middle-value collapse, including integer cycles
    assert(changes>.0001f); // no collective stop at the interpolation knots
   }
  }
 }
 for(int t=0;t<100;++t)for(int v=1;v<32;++v)
  assert(scModulation::noise2(t*.13f,0)==scModulation::noise2(t*.13f,v*0.f)); // Scale 0 deliberately links
 float fieldDifference=0,timeDifference=0;
 for(int v=0;v<32;++v){
  fieldDifference+=std::abs(scModulation::noise2(.23f,v,0)-scModulation::noise2(.23f,v,17.31f));
  timeDifference+=std::abs(scModulation::noise2(.23f,v)-scModulation::noise2(1.23f,v));
 }
 assert(fieldDifference>1&&timeDifference>1);
 std::cout<<"PASS: 3 scales x 2 fields x 580 phases x 32 voices: no collective midpoint crossing or stop, bounded continuous motion, independent fields and intentional Scale 0 linking; all four SC implementations identical.\n";
}
'''.replace('METHOD',method)
with tempfile.TemporaryDirectory()as d:
 p=pathlib.Path(d);(p/'t.cpp').write_text(cpp)
 subprocess.run(['c++','-std=c++17',str(p/'t.cpp'),'-o',str(p/'t')],check=True)
 subprocess.run([str(p/'t')],check=True)
