#!/usr/bin/env python3
"""Morph index / final Pow invariants and independent Python/C++ comparison."""
import pathlib,subprocess,tempfile,random,math
import pitchmap
ROOT=pathlib.Path(__file__).resolve().parents[2]
source=r'''
#include "scPolyCombPitchMap.h"
#include <cassert>
#include <cstdio>
#include <cmath>
using namespace pcmap;
int main(){
 Map m;m.morphGeometry=true;m.mode=ModeTable;m.interval=1;m.table={0,4,7};m.skew=0;
 // Mirror symmetry is preserved through the complete logarithmic/exponential range.
 for(int n:{1,2,7,16,31,32})for(float round:{0.f,.25f,.5f,.75f})for(float pow:{-1.f,-.5f,0.f,.5f,1.f}){
   m.roundness=round;m.pow=pow;auto notes=generate(m,n);
   for(int i=1;i<n;++i)assert(notes[i]==notes[n-i]);
 }
 // Reported regression: twelve voices, three waves, one semitone Step.
 Map repeat;repeat.morphGeometry=true;repeat.mode=ModeOff;repeat.interval=1;repeat.waves=3;
 auto repeated=generate(repeat,12);float expected[]={61,64,67,70,61,64,67,70,61,64,67,70};
 for(int i=0;i<12;++i)assert(repeated[i]==expected[i]);
 // Cycle Shift turns the old drift into a deliberate whole-index offset.
 repeat.cycleShift=1;repeated=generate(repeat,12);
 float up[]={61,64,67,70,62,65,68,71,63,66,69,72};
 for(int i=0;i<12;++i)assert(repeated[i]==up[i]);
 repeat.cycleShift=-1;repeated=generate(repeat,12);
 float down[]={61,64,67,70,72,63,66,69,71,62,65,68};
 for(int i=0;i<12;++i)assert(repeated[i]==down[i]);
 repeat.cycleShift=4;repeated=generate(repeat,12);
 float wrap[]={61,64,67,70,65,68,71,62,69,72,63,66};
 for(int i=0;i<12;++i)assert(repeated[i]==wrap[i]);
 // Shift acts on indexation, so User scale and Harmonic Series consume it too.
 repeat.cycleShift=1;repeat.mode=ModeTable;repeat.table={0,4,7};repeated=generate(repeat,12);
 float scaleShift[]={60,72,84,96,64,76,88,100,67,79,91,103};
 for(int i=0;i<12;++i)assert(repeated[i]==scaleShift[i]);
 repeat.mode=ModeHarmonic;repeat.harmonicBase=36;repeated=generate(repeat,12);
 assert(std::abs(repeated[4]-48)<1e-5f && std::abs(repeated[8]-(36+12*std::log2(3.f)))<1e-5f);
 repeat.cycleShift=0;repeat.mode=ModeOff;repeated=generate(repeat,12);
 for(int i=0;i<12;++i)assert(repeated[i]==expected[i]);
 // Every integral cycle repeats identically when its length divides the voices.
 // Includes fractional Num Waves whose cycles still contain a whole number of voices.
 for(int n:{4,7,8,12,13,16,24,26,32})for(float waves:{.5f,1.f,1.25f,1.75f,2.f,2.5f,3.f,3.25f,4.f}){
   const float period=n/waves;const int length=(int)std::round(period);
   if(std::abs(period-length)>1e-5f || length>=n)continue;
   for(float skew:{-1.f,0.f,1.f})for(float round:{0.f,.5f,1.f})for(float pw:{.25f,.5f,.75f})for(float power:{-1.f,0.f,1.f}){
     Map cycle;cycle.morphGeometry=true;cycle.waves=waves;cycle.skew=skew;cycle.roundness=round;
     cycle.pulseWidth=pw;cycle.pow=power;
     for(int i=0;i+length<n;++i)assert(voiceIndex(cycle,i,n)==voiceIndex(cycle,i+length,n));
   }
 }
 // Voice count is the index domain, even with extreme scatter/folds/comb.
 for(int n:{1,2,4,7,8,16,32})for(float waves:{0.f,.25f,1.f,2.f,8.f})for(float pow:{-1.f,0.f,1.f}){
   Map active;active.morphGeometry=true;active.waves=waves;active.pow=pow;
   active.cycleShift=7;active.first=32;active.symmetry=3;active.comb=.8;active.indexQuantize=3;active.indexRandom=16;
   for(int i=0;i<n;++i){const float index=voiceIndex(active,i,n);assert(index>=1 && index<=n);}
   assert(generate(active,n).size()==n);
 }
 // Pow must preserve final extrema, not distort the phase of mirrored geometry.
 for(float pow:{-1.f,-.5f,0.f,.5f,1.f}){
   m.roundness=0;m.pow=pow;m.symmetry=2;m.comb=.3f;m.indexQuantize=3;m.indexRandom=.7f;
   float rawLo=1e9f,rawHi=-1e9f,lo=1e9f,hi=-1e9f;
   for(int i=0;i<32;++i){float x=rawVoiceIndex(m,i,32),y=shapedVoiceIndex(m,i,32);
     rawLo=std::min(rawLo,x);rawHi=std::max(rawHi,x);lo=std::min(lo,y);hi=std::max(hi,y);
     assert(voiceIndex(m,i,32)==std::round(voiceIndex(m,i,32)));
   }
   assert(std::abs(lo-rawLo)<1e-5f && std::abs(hi-rawHi)<1e-5f);
 }
 assert(scmorph::normalizedPow(.5f,0.f)==.5f);
 assert(scmorph::normalizedPow(.5f,1.f)>.85f && scmorph::normalizedPow(.5f,-1.f)<.05f);
 for(float x:{0.f,.1f,.3f,.5f,.8f,1.f})for(float p:{.1f,.5f,1.f})
   assert(std::abs(scmorph::normalizedPow(scmorph::normalizedPow(x,p),-p)-x)<1e-5);
 // New path: same morph oscillator as the LFO, sampled periodically without duplicating a closing endpoint.
 int n,invert,sym,seed;
 while(std::scanf("%d",&n)==1){
   Map t;t.morphGeometry=true;
   std::scanf("%f %f %f %f %f %f %d %d %f %f %f %d %d",&t.waves,&t.skew,&t.roundness,&t.pulseWidth,&t.pow,&t.first,&invert,&sym,&t.comb,&t.indexQuantize,&t.indexRandom,&seed,&t.cycleShift);
   t.invert=invert;t.symmetry=sym;t.seed=seed;
   for(int i=0;i<n;++i)std::printf("%.6f ",shapedVoiceIndex(t,i,n));std::puts("");
 }
}
'''
rng=random.Random(82);cases=[]
for _ in range(400):
 m=dict(morph_geometry=True,shape=0,bend=1,first=rng.randint(1,6),waves=rng.randint(0,16)/4,skew=rng.uniform(-1,1),roundness=rng.random(),pulse_width=rng.uniform(.001,1),pow=rng.uniform(-1,1),invert=rng.choice([0,1]),symmetry=rng.randint(0,5),comb=rng.random(),index_quantize=rng.randint(0,4),index_random=rng.random(),seed=rng.randint(0,99),cycle_shift=rng.randint(-31,31))
 cases.append((rng.choice([1,2,7,16,32]),m))
with tempfile.TemporaryDirectory()as d:
 c=pathlib.Path(d)/'test.cpp';c.write_text(source);exe=c.with_suffix('')
 subprocess.run(['clang++','-std=c++17','-I'+str(ROOT/'src/nodes'),str(c),'-o',str(exe)],check=True)
 lines=[' '.join(map(str,[n]+[m[k]for k in ('waves','skew','roundness','pulse_width','pow','first','invert','symmetry','comb','index_quantize','index_random','seed','cycle_shift')]))for n,m in cases]
 result=subprocess.run([str(exe)],input='\n'.join(lines)+'\n',capture_output=True,text=True,check=True)
 for (n,m),line in zip(cases,result.stdout.splitlines()):
  actual=list(map(float,line.split()));expected=[pitchmap.shaped_voice_index(m,i,n)for i in range(n)]
  assert all(abs(a-b)<2e-3 for a,b in zip(actual,expected)),(m,n,actual,expected)
print('PASS: Cycle Shift 0/+1/-1, positive/negative wrap, User/Harmonic mapping, exact 12-voice / 3-wave repeat, periodic morph shapes and mirrored final notes at all Pow/roundness settings, final-range preservation, logarithmic/exponential inverse, integer indices, 400 Python/C++ morph geometries.')
