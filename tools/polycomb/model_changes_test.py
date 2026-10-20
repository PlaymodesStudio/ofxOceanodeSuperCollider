#!/usr/bin/env python3
"""Scale expansion, shared voice indexing, versioned recalls and LFO migration."""
import pathlib,subprocess,tempfile
from migration import migrate,ROOT
legacy={'Ring_Model':0,'Ring_On':True,'Ring_Pitch':[72.0],
        'Osc_Map_Mode':1,'Osc_Map_Table':[0,0,4,4,7,7,12,12,0,0,4,4],
        'Osc_Map_Interval':2.0,'Osc_Map_First':1.0,
        'LFO1_Random_Type':2,'LFO1_Sync':True,'LFO1_Beats':[2.0],
        'LFO2_Random_Type':3,'LFO3_Noise':True}
c=migrate([legacy])[0]
assert c['Ring_Model']==1 and c['Ring_Ratio']==[2.0]*32
assert migrate([{**legacy,'Transpose':[12.0]}])[0]['Ring_Ratio']==[1.0]*32
assert c['Osc_Map_Table']==[0,4,7] and c['Osc_Map_Version']==2
assert c['LFO1_Model']==c['LFO2_Model']==3 and c['LFO3_Model']==2
assert c['LFO1_Random_Smooth']==.25
assert migrate([{**c,'Osc_Map_Octave_Jump':.25}])[0]['Osc_Map_Octave_Jump']==.25
assert c['Osc_Map_New_Seed_On_Gate'] is False and c['Comb_Map_New_Seed_On_Gate'] is False
assert migrate([{**c,'Osc_Map_New_Seed_On_Gate':True}])[0]['Osc_Map_New_Seed_On_Gate'] is True
assert c['Osc_Map_Cycle_Shift']==c['Comb_Map_Cycle_Shift']==0
assert migrate([{**c,'Osc_Map_Cycle_Shift':3}])[0]['Osc_Map_Cycle_Shift']==3
assert c['Osc_Map_Skew']==-1 and c['Osc_Map_Pow']==0
shaped=migrate([{**c,'Osc_Map_Shape':2,'Osc_Map_Bend':3}])[0]
# Already migrated morph controls remain authoritative over legacy fields.
assert shaped['Osc_Map_Skew']==-1 and shaped['Osc_Map_Pow']==0
old=migrate([{'Osc_Map_Shape':2,'Osc_Map_Bend':3,'Ring_Model':1}])[0]
assert old['Osc_Map_Skew']==0 and old['Osc_Map_Roundness']==.5 and old['Osc_Map_Pow']<0
assert migrate([old])[0]==old
assert migrate([c])[0]==c,'Migration must not shift a converted scale twice'
# Legacy post-map shifts are split into whole-semitone Transpose and continuous Fine Tune.
for t,f in [(3.25,12.2),(-3.25,-12.2),(48.,24.),(.25,.137)]:
 old={'Ring_Model':1,'Osc_Map_Version':2,'Osc_Map_Transpose':t,'Osc_Map_Offset':f}
 converted=migrate([old])[0]
 assert 'Osc_Map_Offset' not in converted
 assert abs(converted['Osc_Map_Transpose']+converted['Osc_Map_Fine_Tune']-t-f)<1e-4
 assert converted['Osc_Map_Transpose']==round(converted['Osc_Map_Transpose']) and -1<=converted['Osc_Map_Fine_Tune']<=1
 assert migrate([converted])[0]==converted
fine=migrate([{'Ring_Model':1,'Osc_Map_Fine_Tune':.137,'Osc_Map_Transpose':12.}])[0]
assert abs(fine['Osc_Map_Fine_Tune']-.137)<1e-7
# Legacy sub tuning survives whole-semitone/fine splitting and repeat recalls.
for t,f in [(-12.25,0),(3.75,0),(-12.,.137),(12.75,.5)]:
 converted=migrate([{'Ring_Model':1,'Sub_Transpose':t,'Sub_Fine_Tune':f}])[0]
 assert converted['Sub_Transpose']==round(converted['Sub_Transpose'])
 assert -1<=converted['Sub_Fine_Tune']<=1
 assert abs(converted['Sub_Transpose']+converted['Sub_Fine_Tune']-t-f)<1e-5
 assert migrate([converted])[0]==converted
src=r'''
#include "scPolyCombPitchMap.h"
#include <cassert>
#include <cmath>
using namespace pcmap;
int main(){
 Map m;m.mode=ModeTable;m.first=1;m.interval=1;m.table={0,4,7};
 auto p=generate(m,8);float want[]={60,64,67,72,76,79,84,88};
 for(int i=0;i<8;++i)assert(p[i]==want[i]);
 auto shifted=generate(m,3,{1});assert(shifted[0]==60 && shifted[1]==64 && shifted[2]==67);
 m.interval=.4f;p=generate(m,16);assert(p.front()==60 && p.back()==84); // fit three notes over two octaves
 m.interval=-1;p=generate(m,4);float down[]={60,55,52,48};
 for(int i=0;i<4;++i)assert(p[i]==down[i]);
 for(int count:{1,7,12}){
   m.table.clear();for(int i=0;i<count;++i)m.table.push_back(i);
   m.interval=1;p=generate(m,32);
   for(int i=0;i<32;++i)assert(p[i]==60+(i/count)*12+i%count);
 }
 m.mode=ModeHarmonic;m.first=1;m.interval=1;m.harmonicBase=36;
 p=generate(m,4);assert(p[0]==36 && p[1]==48 && p[3]==60);
 // Invert, symmetry, comb and quantization act on the same index in both modes.
 m.invert=true;m.symmetry=2;m.comb=.4f;m.indexQuantize=.25f;m.indexRandom=.1f;m.seed=91;
 auto h=generate(m,32);m.mode=ModeTable;m.table={0,2,4,5,7,9,11};p=generate(m,32);
 for(int i=0;i<32;++i){
   const float index=voiceIndex(m,i,32);
   const float degree=m.interval*(index-1)+m.degreeOffset;
   assert(p[i]==60+userDegree(degree,m.table));
   assert(std::abs(h[i]-(36+12*std::log2(index)))<1e-4);
 }
 assert(userScale({0,0,4,7,7}).size()==3);
 // Identical indices must always select identical notes, including scale expansion.
 Map pyramid;pyramid.mode=ModeTable;pyramid.shape=1;pyramid.interval=1;pyramid.table={0,4,7};
 auto mirrored=generate(pyramid,16);
 for(int i=1;i<8;++i)assert(mirrored[i]==mirrored[16-i]);
 assert(mirrored[0]==60 && mirrored[8]==91 && mirrored[15]==64);
 for(int i=1;i<=8;++i)assert(mirrored[i]>=mirrored[i-1]);
 for(int i=9;i<16;++i)assert(mirrored[i]<=mirrored[i-1]);
 // Grouping is anchored at degree zero: no accidental previous-octave first note.
 Map grouped;grouped.mode=ModeTable;grouped.interval=1;grouped.table={0,4,7};
 for(float size:{2.f,3.f,4.f,8.f}){
   grouped.indexQuantize=size;auto notes=generate(grouped,16);
   assert(notes[0]==60 && notes[1]==60);
   for(int i=0;i<16;++i)assert(voiceIndex(grouped,i,16)>=1);
 }
 // Octave jump and whole-degree Step are independent, including downward maps.
 Map jump;jump.mode=ModeTable;jump.interval=1;jump.table={0,4,7};
 for(float oct:{0.f,.25f,.5f,1.f,1.25f,2.f}){
   jump.octaveJump=oct;auto notes=generate(jump,7);
   assert(notes[0]==60 && notes[1]==64 && notes[2]==67);
   assert(notes[3]==60+12*oct && notes[6]==60+24*oct);
   jump.interval=-1;notes=generate(jump,2);assert(notes[1]==67-12*oct);jump.interval=1;
 }
 // BaseIndexer-inspired discrete geometry: Num Waves repeats an integer ramp;
 // Symmetry folds it, and arbitrary curves/scatter still resolve to integers.
 Map g;g.first=1;g.waves=2;
 for(int i=0;i<4;++i)assert(voiceIndex(g,i,8)==voiceIndex(g,i+4,8));
 g.waves=0;for(int i=0;i<8;++i)assert(voiceIndex(g,i,8)==1);
 g.waves=1;g.bend=3;assert(voiceIndex(g,0,16)==1 && voiceIndex(g,15,16)==16);g.bend=1;
 g.waves=1;g.symmetry=1;assert(voiceIndex(g,0,8)==1 && voiceIndex(g,4,8)==5 && voiceIndex(g,7,8)==2);
 for(int n:{1,7,16,32})for(int shape:{0,1,2})for(float waves:{0.f,.5f,1.f,2.f,8.f})for(int sym:{0,1,3,32}){
   g.shape=shape;g.waves=waves;g.symmetry=sym;g.bend=2.1f;g.comb=.37f;g.indexRandom=.75f;g.indexQuantize=.33f;
   for(int i=0;i<n;++i){float x=voiceIndex(g,i,n);assert(std::isfinite(x)&&x==std::round(x));}
 }

}
'''
with tempfile.TemporaryDirectory() as d:
 source=pathlib.Path(d)/'test.cpp';source.write_text(src);exe=pathlib.Path(d)/'test'
 subprocess.run(['c++','-std=c++17','-I'+str(ROOT/'src/nodes'),str(source),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: N-note octave expansion to 32 voices, negative indices, harmonics, shared index shaping, ring conversion, unified LFO migration and idempotent recalls.')
