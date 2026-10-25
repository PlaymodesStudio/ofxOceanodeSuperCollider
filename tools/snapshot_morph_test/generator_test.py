#!/usr/bin/env python3
"""Shared effective LFO state: clocks, modes, interruptions and incoming controls."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
cpp = r'''
#include "scGeneratorMorph.h"
#include <cassert>
#include <iostream>
void near(float a,float b){assert(std::abs(a-b)<1e-5);}
int main(){
 scGeneratorMorph::LfoKeys k{"LFO1_Noise","LFO1_Random_Type","","LFO1_Sync","LFO1_Hz","LFO1_Beats","LFO1_S&H","",""};
 ofJson a={{k.noise,true},{k.random,0},{k.sync,true},{k.hz,{1.f}},{k.beats,{1.954f}},{k.sh,{0.f}}};
 ofJson b=a;b[k.noise]=false;b[k.random]=1;b[k.beats]={.5f};
 auto x=scGeneratorMorph::endpoint(a,"mGenerators",k,a),y=scGeneratorMorph::endpoint(b,"mGenerators",k,a);
 for(float t:{0.f,.001f,.499f,.5f,.501f,1.f}){
  auto state=scGeneratorMorph::blend(x,y,t);near(state["weights"][1],1-t);near(state["weights"][2],t);
  near(state["beatRate"][0],(1-t)/1.954f+t*2.f);
  ofJson held={{"mGenerators",{{k.noise,state}}}};
  auto resumed=scGeneratorMorph::endpoint(held,"mGenerators",k,a);
  assert(resumed["weights"]==state["weights"]);near(resumed["beatRate"][0],state["beatRate"][0]);
 }
 b[k.sync]=false;b[k.hz]={7.f};y=scGeneratorMorph::endpoint(b,"mGenerators",k,a);
 k.shared="LFO1_Random_Same";k.centered="LFO1_Centered";k.steps="LFO1_Steps";
 a[k.shared]=false;b[k.shared]=true;a[k.centered]=true;b[k.centered]=false;a[k.steps]=0;b[k.steps]=8;
 x=scGeneratorMorph::endpoint(a,"mGenerators",k,a);y=scGeneratorMorph::endpoint(b,"mGenerators",k,a);
 for(float t:{.499f,.501f}){
  auto mixed=scGeneratorMorph::blend(x,y,t);near(mixed["shared"],t);near(mixed["centered"],1-t);near(mixed["steps"],8*t);
  ofJson held={{"mGenerators",{{k.noise,mixed}}}};
  auto restored=scGeneratorMorph::endpoint(held,"mGenerators",k,a);assert(restored==mixed);
 }
 auto state=scGeneratorMorph::blend(x,y,.5f);near(state["hz"][0],3.5f);near(state["beatRate"][0],.5f/1.954f);
 auto blocked=scGeneratorMorph::endpoint(b,"mGenerators",k,a,[&](const std::string& key){return key==k.random;});near(blocked["weights"][0],1);
 k.model="LFO1_Model";k.manual="LFO1_Value";a[k.model]=0;a[k.manual]=.8f;b[k.model]=1;
 x=scGeneratorMorph::endpoint(a,"mGenerators",k,a);y=scGeneratorMorph::endpoint(b,"mGenerators",k,a);
 state=scGeneratorMorph::blend(x,y,.5);near(state["weights"][5],.5f);near(state["manual"],.8f);
 k.model="";k.region="LFO1_Smp";a[k.region]=true;a[k.beats]={4.f};
 x=scGeneratorMorph::endpoint(a,"mGenerators",k,a);near(x["regionRate"][0],.25f);near(x["beatRate"][0],0);
 ofJson invalid={{"mGenerators",{{k.noise,{{"weights",{0,0,0,0,0,0}}}}}}};
 assert(scGeneratorMorph::weightsValid(scGeneratorMorph::endpoint(invalid,"mGenerators",k,a)["weights"]));
 std::cout<<"PASS: shared LFO mode/clock blends, tiny steps, saved/interrupted state, manual, region clocks and connected overrides\n";
}
'''
with tempfile.TemporaryDirectory(prefix='generator-morph-') as folder:
 out=Path(folder);(out/'test.cpp').write_text(cpp)
 subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror',
 '-I'+str(ROOT/'src/nodes/boxSynths/shared'),'-I'+str(ROOT/'tools/snapshot_morph_test/stubs'),
 '-I'+str(ROOT.parents[1]/'libs/json/include'),str(out/'test.cpp'),'-o',str(out/'test')],check=True)
 subprocess.run([str(out/'test')],check=True)
