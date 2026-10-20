#!/usr/bin/env python3
"""Exercise production parameter registration for edits, published inputs and recall."""
import pathlib,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[2]
s=(ROOT/'src/nodes/scPolyComb.cpp').read_text();a=s.index('template<typename T>\nvoid scPolyComb::reg(');b=s.index('\n}\n',a)+3
source=r'''
#include "scPolyCombControlSteps.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <cassert>
using ofJson=nlohmann::json;
template<class T>struct ofParameter{
 std::string name;T value,lo,hi;std::vector<std::function<void(T&)>> events;
 T get()const{return value;}T getMin()const{return lo;}T getMax()const{return hi;}
 std::string getName()const{return name;}std::string getEscapedName()const{return name;}
 void set(const T& v){if(v==value)return;value=v;for(auto&fn:events)fn(value);}
 void setWithoutEventNotifications(const T&v){value=v;}
 template<class F>int newListener(F fn){events.push_back(fn);return 0;}
};
struct scPolyComb{
 struct Control{std::string key,name;void*param;std::function<int()>add;std::function<ofJson()>save;std::function<void(const ofJson&)>load;};
 bool restoring=false;std::vector<Control>controls;struct Listeners{void push(int){}}listeners;
 template<class T>int addParameter(T&){return 1;}
 template<class T>void reg(ofParameter<T>&,std::function<void()>);
};
'''+s[a:b]+r'''
int main(){
 for(const std::string key:{"Transpose","Comb_Transpose","Ring_Transpose","Osc_Map_Num_Waves","Comb_Map_Num_Waves","Osc_Map_Octave_Jump","Comb_Map_Octave_Jump"}){
   scPolyComb node;ofParameter<float> p{key,0,-48,48};int sends=0;
   node.reg(p,[&](){++sends;assert(p.get()*4==std::round(p.get()*4));});
   // A published connection calls the same parameter setter as an editor edit.
   for(float value:{1.13f,1.37f,-1.13f,-1.37f}){
     p.set(value);assert(p.get()==std::round(value*4)*.25f);
     assert(node.controls[0].save().get<float>()==p.get());
   }
   assert(sends==4);
   node.restoring=true;node.controls[0].load(2.13f);
   assert(p.get()==2.25f && sends==4);
   node.restoring=false;node.controls[0].load(3.13f);
   assert(p.get()==3.25f && sends==5);
 }
 for(const std::string key:{"Sub_Transpose","Osc_Map_Transpose","Comb_Map_Transpose","Osc_Map_Interval","Comb_Map_Interval","Osc_Map_First","Osc_Map_Degree_Offset","Comb_Map_Index_Quantize"}){
   scPolyComb node;ofParameter<float> p{key,0,-24,24};node.reg(p,[](){});
   p.set(1.7f);assert(p.get()==2);node.restoring=true;node.controls[0].load(-1.7f);assert(p.get()==-2);
 }
 scPolyComb node;ofParameter<std::vector<float>> p{"Ring_Transpose",{0},{-48},{48}};node.reg(p,[](){});
 p.set({1.13f,-1.13f,2.37f});assert(p.get()==std::vector<float>({1.25f,-1.25f,2.25f}));
 node.restoring=true;node.controls[0].load(ofJson::array({.13,-.13,90}));
 assert(p.get()==std::vector<float>({.25f,-.25f,48.f}));
 ofParameter<int> cycle{"Osc_Map_Cycle_Shift",0,-11,11};node.reg(cycle,[](){});
 cycle.set(3);assert(cycle.get()==3);node.controls.back().load(100);assert(cycle.get()==11);
 node.controls.back().load(-100);assert(cycle.get()==-11);
 for(const std::string key:{"Comb_Map_Fine_Tune","Sub_Fine_Tune"}){
 ofParameter<float> fine{key,0,-1,1};node.reg(fine,[](){});
 fine.set(.137f);assert(fine.get()==.137f);node.controls.back().load(-.273f);assert(fine.get()==-.273f);
 fine.set(3);assert(fine.get()==1);
 }
 // Start index is bounded by the active parameter limit, not a fixed 32.
 ofParameter<float> first{"Osc_Map_First",1,1,4};node.reg(first,[](){});first.set(32);assert(first.get()==4);
}
'''
with tempfile.TemporaryDirectory()as d:
 c=pathlib.Path(d)/'test.cpp';c.write_text(source);exe=c.with_suffix('')
 subprocess.run(['clang++','-std=c++17','-I'+str(ROOT/'src/nodes'),'-I'+str(ROOT.parent.parent/'libs/json/include'),str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: production control listeners snap edits/connections, recall and vector transposes; no duplicate sends; Start obeys active range.')
