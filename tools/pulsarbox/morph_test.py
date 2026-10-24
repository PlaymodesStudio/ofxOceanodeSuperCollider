#!/usr/bin/env python3
"""Production Pulsar graph-bank lifetime and stored/interrupted configuration weights."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'src/nodes/boxSynths/pulsarBox/scPulsarBox.h').read_text()
def block(signature):
 start=source.index(signature);i=source.index('{',start)+1;depth=1
 while depth:
  depth+=(source[i]=='{')-(source[i]=='}');i+=1
 return source[start:i]
methods='\n'.join(block(x) for x in ['static void clearBanks(', 'void applyStructure()'])
cpp=r'''
#include "nodes/boxSynths/shared/scConfigurationMorph.h"
#include <cassert>
#include <iostream>
float now=0;float ofGetElapsedTimef(){return now;}
std::string ofToString(int x){return std::to_string(x);}
struct Param{int n;int get()const{return n;}};
using ofxSCServer=int;
int nextId=0,alive=0,freed=0;
struct ofxSCSynth{
 int nodeID=++nextId;bool created=false;float gain=1;
 ofxSCSynth(const std::string&,int*){++alive;}
 ~ofxSCSynth(){--alive;}
 bool isCreated()const{return created;}
 void free(){++freed;created=false;}
 void moveBefore(int){}
 void createAndRun(int action,int id,bool){assert(action!=4);if(action==2 || action==3)assert(id!=nodeID);created=true;}
};
struct Box{
 struct Bank{ofxSCSynth*synth=nullptr;float weight=1;};
 struct ServerState{std::map<std::pair<int,int>,Bank>banks;ofxSCSynth*synth=nullptr,*gateS=nullptr,*modGroup=nullptr;bool modCreated=true;int builtOutputs=2,builtVoices=1,builtLayers=1;};
 Param numVoices{1};int layerCount=1,bankCapacity=0;float bankRetireAt=-1;
 scConfigurationMorph::Weights bankWeights;
 std::map<int*,ServerState>states;
 struct Mod{void voicesChanged(){}void rebuild(int*){}}mod;
 int voices()const{return std::max(numVoices.get(),bankCapacity);}int outputs()const{return 2;}int layers()const{return layerCount;}bool getActive()const{return true;}
 void sendAll(int*s,bool){for(auto&e:states[s].banks)e.second.synth->gain=e.second.weight;}
 void createGate(int*,ServerState&){}void sendModArgs(int*,ServerState&){}
 METHODS
 Box(){auto&st=states[nullptr];auto&b=st.banks[{1,1}];b.synth=new ofxSCSynth("",nullptr);b.synth->created=true;st.synth=b.synth;}
 ~Box(){for(auto&[s,st]:states)clearBanks(st);}
};
void near(float a,float b){assert(std::abs(a-b)<1e-6);}
int main(){
 const std::vector<scConfigurationMorph::Field>fields={{"Voices",1,8},{"Formants",1,4}};
 const ofJson a={{"Voices",1},{"Formants",1}},b={{"Voices",8},{"Formants",4}};
 {
 Box box;int original=box.states[nullptr].synth->nodeID;
 for(float t:{0.f,.001f,.499f,.501f,.9f,1.f}){
  box.bankWeights=scConfigurationMorph::blend(a,b,t,"banks",fields);
  box.bankRetireAt=now+.35;box.numVoices.n=t<.5?1:8;box.layerCount=t<.5?1:4;
  box.applyStructure();auto&banks=box.states[nullptr].banks;
  assert(banks.at({1,1}).synth->nodeID==original);near(banks.at({1,1}).synth->gain,1-t);
  if(t>0){assert(banks.size()==2);near(banks.at({8,4}).synth->gain,t);}
 }
 assert(freed==0 && alive==2);
 box.bankWeights=scConfigurationMorph::blend(b,a,.4,"banks",fields);box.applyStructure();
 ofJson held;scConfigurationMorph::capture(held,"banks",box.bankWeights);
 auto resumed=scConfigurationMorph::blend(held,a,0,"banks",fields);assert(resumed==box.bankWeights);
 box.bankWeights=scConfigurationMorph::blend(held,a,1,"banks",fields);box.numVoices.n=1;box.layerCount=1;
 box.applyStructure();assert(alive==2);
 box.bankRetireAt=-1;box.applyStructure();assert(alive==1 && freed==1 && box.voices()==1);
 assert(box.states[nullptr].synth->nodeID==original);
 }
 assert(alive==0 && freed==2);
 ofJson corrupt={{"banks",ofJson::array({{{"configuration",{99,1}},{"weight",1}}})}};
 assert(scConfigurationMorph::endpoint(corrupt,"banks",fields)==scConfigurationMorph::endpoint(a,"banks",fields));
 std::cout<<"PASS: first-nonzero configuration banks, stable IDs, midpoint, reverse/store/resume, delayed retirement and idle cleanup\n";
}
'''.replace('METHODS',methods)
with tempfile.TemporaryDirectory(prefix='pulsar-morph-') as d:
 p=Path(d);(p/'test.cpp').write_text(cpp)
 subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src'),'-I'+str(ROOT/'tools/snapshot_morph_test/stubs'),'-I'+str(ROOT.parents[1]/'libs/json/include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
