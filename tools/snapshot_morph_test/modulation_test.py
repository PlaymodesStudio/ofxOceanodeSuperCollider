#!/usr/bin/env python3
"""Shared production interpolation of route matrices and arbitrary parameter lanes."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
cpp=r'''
#include "nodes/boxSynths/shared/scModulationMorph.h"
#include <cassert>
#include <iostream>
void near(float a,float b){assert(std::abs(a-b)<1e-5);}
int main(){
 ofJson a={{"Depth","2, 4"},{"mLfos",1},{"mMatrix",{0.f,1.f}},
  {"mParams",ofJson::array({{{"key","Tone"},{"base",{10.f,20.f}},{"amounts",{{"0",-.5f}}}}})}};
 ofJson b={{"Depth","8, 10"},{"Tone",{30.f,40.f}},{"mLfos",3},{"mMatrix",{1.f,0.f,.5f}},
  {"mParams",ofJson::array({{{"key","Depth"},{"base",{8.f,10.f}},{"amounts",{{"2",.8f}}}}})}};
 for(float t:{0.f,.001f,.499f,.501f,1.f}){
  auto s=scModulationMorph::state(a,b,t,t>=.5,"m");
  near(s["mMatrix"][0],t);near(s["mMatrix"][1],1-t);near(s["mMatrix"][2],.5*t);
  assert(s["mLfos"]==(t<=0?1:3));
  const auto&depth=s["mParams"][0];const auto&tone=s["mParams"][1];
  assert(depth["key"]=="Depth" && tone["key"]=="Tone");
  near(depth["base"][0],2+6*t);near(depth["base"][1],4+6*t);
  near(depth["amounts"].value("2",0.f),.8*t);
  near(tone["base"][0],10+20*t);near(tone["amounts"].value("0",0.f),-.5*(1-t));
 }
 auto held=scModulationMorph::state(a,b,.4,false,"m");
 auto resumed=scModulationMorph::state(held,b,0,false,"m");
 assert(held["mParams"]==resumed["mParams"] && held["mMatrix"]==resumed["mMatrix"]);
 auto blocked=scModulationMorph::state(a,b,.5,true,"m",[](const std::string& k){return k=="Depth";});
 assert(blocked["mParams"].size()==1 && blocked["mParams"][0]["key"]=="Tone");
 std::cout<<"PASS: shared modulation matrices, serialized bases, disappearing/appearing routes, interrupted blends and connected-control precedence\n";
}
'''
with tempfile.TemporaryDirectory(prefix='modulation-morph-') as directory:
 work=Path(directory);(work/'test.cpp').write_text(cpp)
 subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src'),
 '-I'+str(ROOT/'tools/snapshot_morph_test/stubs'),'-I'+str(ROOT.parents[1]/'libs/json/include'),str(work/'test.cpp'),'-o',str(work/'test')],check=True)
 subprocess.run([str(work/'test')],check=True)
