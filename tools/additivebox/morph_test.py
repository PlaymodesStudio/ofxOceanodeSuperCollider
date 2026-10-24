#!/usr/bin/env python3
"""Production AdditiveBox morph/count/lifecycle code with deterministic server adapters.

No audio device: verifies early voice presence, stable common node IDs, deferred
retirement, stored/interrupted transitions, routes and incoming-control precedence.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'src/nodes/boxSynths/additiveBox/scAdditiveBox.cpp').read_text()


def block(text, signature):
    start=text.index(signature)
    opening=text.index('{',start)
    end=opening+1
    depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}')
        end+=1
    return text[start:end]

snap=(ROOT/'src/nodes/boxSynths/shared/scSnapshots.h').read_text()
snapshot_helpers='\n'.join(block(snap,sig) for sig in (
    'static ofJson blendValue(', 'static bool logKey(', 'template<typename Actions, typename Skip>'))
methods='\n'.join(block(source,sig) for sig in (
    'int scAdditiveBox::numVoicesClamped()', 'float scAdditiveBox::voiceMorphGain(',
    'void scAdditiveBox::finishVoiceMorph(', 'ofJson scAdditiveBox::captureSound(',
    'void scAdditiveBox::applySound(', 'void scAdditiveBox::loadSound(',
    'void scAdditiveBox::syncVoices(', 'int scAdditiveBox::capForCount(', 'int scAdditiveBox::partialCap(',
    'float scAdditiveBox::partialPresence(', 'float scAdditiveBox::partialCountValue(',
    'std::map<int,float> scAdditiveBox::partialCapWeights(', 'void scAdditiveBox::preparePartialMorph(',
    'void scAdditiveBox::forPartialBanks(', 'void scAdditiveBox::finishPartialMorph('))
cpp=r'''
#include "nodes/boxSynths/shared/scVoiceCountMorph.h"
#include "nodes/boxSynths/shared/scModulationMorph.h"
#include "nodes/boxSynths/shared/scMorphPolicies.h"
#include <cassert>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <iostream>
float now=0; float ofGetElapsedTimef(){return now;}
float ofLerp(float a,float b,float t){return (1-t)*a+t*b;}
struct scSnapshots{HELPERS};
using ofxSCServer=int;
struct Param{
 int value=1;std::string name="Voices";std::function<void()> changed;
 std::string getEscapedName()const{return name;}
 int get()const{return value;}
 Param&operator=(int v){if(v!=value){value=v;if(changed)changed();}return *this;}
};
struct Action{
 std::string key;std::function<void(ofJson&)>save;std::function<void(const ofJson&)>load;std::function<void()>reset;
};
struct Voice{int id=0;float gain=1;};
struct State{bool created=true,modCreated=true;std::vector<Voice>voices;std::map<int,std::vector<Voice>>partialBanks;};
struct Mod{
 static constexpr int MAX_LFOS=8,MAX_ENVS=4,MAX_EXT=8;
 int lfoCount=0,envCount=0,extCount=0,rebuilds=0;
 std::vector<float>matrix{0,0,0};
 void refresh(){}void voicesChanged(){}void rebuild(ofxSCServer*){++rebuilds;}void afterLoad(){}
 void load(const ofJson&j,const std::string&p){matrix=j.value(p+"Matrix",std::vector<float>(3,0));}
 void loadMorph(const ofJson&a,const ofJson&b,float t,bool sw,const std::string&p,const scModulationMorph::Blocked& blocked={}){
  load(scModulationMorph::state(a,b,t,sw,p,blocked),p);
 }
 void patchBases(ofJson&)const{}void save(ofJson&j,const std::string&p)const{j[p+"Matrix"]=matrix;}
};
using scModulation=Mod;
struct scAdditiveBox{
 static constexpr int MAX_VOICES=16,MAX_PARTIALS=256;
 static constexpr std::array<int,6>CAPS={8,16,32,64,128,256};
 Param numVoices,partials;Mod mod;struct{scMorphPolicies morph;}snaps;
 std::vector<float>voiceMorphWeights,partialMorphWeights;
 float partialMorphRetireAt=-1,renderPartialGain=1;bool partialBankMode=false;int builtCap=32,renderPartialCap=0;
 int voiceMorphCapacity=0;float voiceMorphRetireAt=-1;bool applyingVoiceMorph=false,loadingBankPreset=false;
 const ofJson*modSource=nullptr;
 std::map<ofxSCServer*,State>states;
 std::vector<Action>publishActions;
 std::set<std::string>driven;
 int nextId=100,created=0,freed=0,unexpectedStructure=0;bool dirty=false;
 float level=.5;std::vector<float>gains;
 bool soundDriven(const std::string&k)const{return driven.count(k);}
 bool notInPresets(const std::string&)const{return false;}
 void migrateOldModulation(const ofJson&){}void migrateOldEnvelope(const ofJson&){}
 void markDirty(){dirty=true;}
 int numVoicesClamped()const;float voiceMorphGain(int)const;void finishVoiceMorph();
 ofJson captureSound()const;void applySound(const ofJson&,const ofJson&,float,bool);void loadSound(const ofJson&);
 void syncVoices(ofxSCServer*);
 static int capForCount(int);int partialCap()const;float partialPresence(int)const;
 float partialCountValue(const std::function<float(int)>&)const;std::map<int,float>partialCapWeights()const;
 void preparePartialMorph(const ofJson&,const ofJson&,float);void forPartialBanks(ofxSCServer*,const std::function<void()>&);
 void finishPartialMorph();void syncNoteWriters(ofxSCServer*){}
 void freeVoice(ofxSCServer*,Voice&v,bool){assert(v.id);++freed;v.id=0;}
 void ensureBuses(ofxSCServer*){}void uploadCurves(ofxSCServer*,bool){}
 void computeAll(){gains.resize(numVoicesClamped());for(int v=0;v<numVoicesClamped();++v)gains[v]=voiceMorphGain(v)*renderPartialGain;}
 void syncVoice(ofxSCServer*s,int v,bool){auto&voice=states[s].voices[v];if(!voice.id){voice.id=++nextId;++created;}voice.gain=voiceMorphGain(v)*renderPartialGain;assert(voice.id);}
 void applyStructure(){for(auto&[s,st]:states)syncVoices(s);markDirty();}
 scAdditiveBox(){
  states[nullptr]=State{};partials.value=32;partials.name="Partials";
  numVoices.changed=[this](){if(!loadingBankPreset){++unexpectedStructure;applyStructure();}};
  publishActions.push_back({"Levels",[this](ofJson&j){j["Levels"]=level;},[this](const ofJson&j){level=j.get<float>();},[this](){level=.5;}});
  publishActions.push_back({"Partials",[this](ofJson&j){j["Partials"]=partials.get();},[this](const ofJson&j){partials=j.get<int>();},[this](){partials=32;}});
  applyStructure();
 }
};
METHODS
void near(float a,float b){assert(std::abs(a-b)<1e-5);}
int main(){
 scAdditiveBox box;
 ofJson a={{"Voices",1},{"Levels",.2f},{"additiveBoxModMatrix",{0.f,1.f,0.f}}};
 ofJson b={{"Voices",8},{"Levels",.8f},{"additiveBoxModMatrix",{1.f,0.f,.5f}}};
 const int original=box.states[nullptr].voices[0].id;
 box.applySound(a,b,0,false);assert(box.numVoicesClamped()==1);
 for(float t:{.001f,.1f,.499f,.501f,.9f,1.f}){
  box.applySound(a,b,t,t>=.5);
  assert(box.numVoicesClamped()==8);
  assert(box.states[nullptr].voices[0].id==original);
  assert(box.freed==0 && box.unexpectedStructure==0);
  near(box.voiceMorphGain(0),1);near(box.voiceMorphGain(7),t);
  near(box.mod.matrix[0],t);near(box.mod.matrix[1],1-t);
 }
 assert(box.created==8 && box.mod.rebuilds==1);
 box.applySound(b,a,.4,false);near(box.voiceMorphGain(7),.6);
 auto stored=box.captureSound();assert(stored.contains(scVoiceCountMorph::key));
 box.applySound(stored,a,0,false);near(box.voiceMorphGain(7),.6);
 box.applySound(stored,a,.5,true);near(box.voiceMorphGain(7),.3);
 box.applySound(stored,a,1,true);
 assert(box.numVoices.get()==1 && box.numVoicesClamped()==8 && box.freed==0);
 near(box.voiceMorphGain(7),0);
 now=.1;box.finishVoiceMorph();
 assert(box.numVoicesClamped()==1 && box.states[nullptr].voices.size()==1);
 assert(box.freed==7 && box.states[nullptr].voices[0].id==original);
 assert(box.voiceMorphWeights.empty());
 // The graph's incoming controls retain precedence at intermediate and final recall.
 box.driven={"Voices","Levels"};box.level=.37;
 box.applySound(a,b,.4,false);assert(box.numVoices.get()==1);near(box.level,.37);
 box.applySound(a,b,1,true);assert(box.numVoices.get()==1);near(box.level,.37);
 assert(box.unexpectedStructure==0);
 {
  scAdditiveBox p;p.partials=8;p.builtCap=8;
  ofJson low={{"Voices",1},{"Partials",8}},high={{"Voices",1},{"Partials",256}};
  const int original=p.states[nullptr].voices[0].id;
  for(float t:{0.f,.001f,.499f,.501f,1.f}){
   p.applySound(low,high,t,t>=.5);auto&st=p.states[nullptr];
   assert(st.voices[0].id==original);
   near(st.voices[0].gain,1-t);
   if(t>0){assert(st.partialBanks.size()==1);near(st.partialBanks.at(256)[0].gain,t);}
  }
  const int destination=p.states[nullptr].partialBanks.at(256)[0].id;
  p.finishPartialMorph();assert(p.builtCap==256 && !p.partialBankMode);
  assert(p.states[nullptr].partialBanks.empty() && p.states[nullptr].voices[0].id==destination);
  assert(p.freed==1);
  p.applySound(high,low,.4f,false);auto stored=p.captureSound();assert(stored.contains("_partialCountBlend"));
  auto before=p.partialMorphWeights;p.applySound(stored,low,0,false);assert(p.partialMorphWeights==before);
  bool threw=false;try{p.forPartialBanks(nullptr,[&](){if(p.renderPartialCap==8)throw 1;});}catch(...){threw=true;}
  assert(threw && p.renderPartialCap==0 && p.states[nullptr].voices[0].id==destination);
  p.applySound(stored,low,1,true);p.finishPartialMorph();assert(p.builtCap==8 && !p.partialBankMode && p.states[nullptr].partialBanks.empty());
 }
 std::cout<<"PASS: AdditiveBox voice presence at .001, stable common IDs across .5, deferred freeing, stored/interrupted blends, routing interpolation, connected controls and retained partial-cap banks\n";
}
'''.replace('HELPERS',snapshot_helpers).replace('METHODS',methods)
with tempfile.TemporaryDirectory(prefix='additive-morph-') as work:
    work=Path(work);(work/'test.cpp').write_text(cpp)
    subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror',
                    '-I'+str(ROOT/'tools/snapshot_morph_test/stubs'),'-I'+str(ROOT/'src'),
                    '-I'+str(ROOT.parents[1]/'libs/json/include'),str(work/'test.cpp'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True)
