#!/usr/bin/env python3
"""Exercise production LFO pin lifecycle, persistence and hidden-window polling."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
source = (ROOT / 'src/nodes/scModulation.cpp').read_text()
names = ['update', '~scModulation', 'hasLfoOutputs', 'publishLfoOutput',
         'updateLfoOutputs', 'saveLfoOutputs', 'loadLfoOutputs', 'liveValue']
methods = []
for name in names:
    start = source.index('scModulation::' + name + '(')
    start = source.rfind('\n', 0, start) + 1
    end = source.index('\n}', start) + 2
    methods.append(source[start:end])

cpp = r'''
#include <nlohmann/json.hpp>
#include <array>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <memory>
#include <functional>
#include <cmath>
#include <algorithm>
#include <cassert>
#include <iostream>
using ofJson = nlohmann::json;
template<class T> T ofClamp(T v,T lo,T hi){return std::clamp(v,lo,hi);}
std::string ofToString(int i){return std::to_string(i);}
float now=0; float ofGetElapsedTimef(){return now;}
enum {ofxOceanodeParameterFlags_DisableSavePreset=1,ofxOceanodeParameterFlags_DisableSaveProject=2};
template<class T> struct ofParameter {
 std::string name; T value;
 void set(std::string s,T v,T,T){name=s;value=v;} void set(T v){value=v;}
 std::string getName() const{return name;}
};
struct ofxOceanodeAbstractParameter {bool connected=false;bool hasOutConnections(){return connected;}};
struct Event{int count=0;void notify(void*){++count;}};
struct Node{
 Event parameterGroupChanged;std::map<std::string,int> pins;
 auto addOutputParameter(ofParameter<std::vector<float>>& p,int flags){
  assert(flags==3);assert(!pins.count(p.name));pins[p.name]=flags;
  return std::make_shared<ofxOceanodeAbstractParameter>();
 }
 void removeParameter(std::string n){pins.erase(n);}
};
struct Bus{int index=100;std::vector<float> readValues;};
struct Server{int requests=0;void requestControlValues(int,int){++requests;}};
struct State{bool created=true;Bus* srcBlock;};
bool liveControlBus(Server*,Bus*b){return b;}
struct scModulation{
 static constexpr int MAX_LFOS=8;
 struct Host{Node*node=nullptr;}host;
 int maxLfos=8,lfoCount=2,voices=3,busWidth=16;bool perVoice=true,showing=false;
 std::string nameSuffix;std::vector<float> live;float nextLiveRead=0;
 std::function<float(int,int)> liveHook;
 std::function<std::vector<float>(int)> partialBars;
 std::map<Server*,State> states;
 std::array<ofParameter<std::vector<float>>,8> lfoOutputs;
 std::array<std::shared_ptr<ofxOceanodeAbstractParameter>,8> lfoOutputPins{};
 ~scModulation();int n()const{return voices;}
 void applyParams(){}bool paramsActive(){return false;}
 int sourceCount(){return 8;}bool sourceUsed(int i){return i<lfoCount;}
 void update();bool hasLfoOutputs()const;bool publishLfoOutput(int,bool);
 void updateLfoOutputs();void saveLfoOutputs(ofJson&)const;void loadLfoOutputs(const ofJson&);
 float liveValue(int,int)const;
};
METHODS
int main(){
 Node node;
 {
 scModulation m;m.host.node=&node;
 assert(!m.hasLfoOutputs());assert(!m.publishLfoOutput(-1,true));assert(!m.publishLfoOutput(8,true));
 Server server;Bus bus;bus.readValues.resize(128);bus.readValues[0]=.2;bus.readValues[1]=.5;bus.readValues[2]=.8;
 m.states[&server]={true,&bus};m.update();assert(server.requests==0);
 assert(m.publishLfoOutput(0,true));assert(!m.publishLfoOutput(0,true));m.update();
 assert(server.requests==2);assert(m.lfoOutputs[0].value==std::vector<float>({.2f,.5f,.8f}));
 assert(!m.showing);now=.1;bus.readValues[0]=.7;m.update();assert(m.lfoOutputs[0].value[0]==.7f);
 m.voices=1;now=.2;m.update();assert(m.lfoOutputs[0].value.size()==1);
 m.lfoCount=0;m.update();assert(m.lfoOutputs[0].value==std::vector<float>{0});
 m.lfoOutputPins[0]->connected=true;assert(!m.publishLfoOutput(0,false));
 ofJson saved;m.saveLfoOutputs(saved);assert(saved["modulationLfoOutputs"][""]==ofJson::array({0}));
 m.loadLfoOutputs(ofJson::object());assert(m.hasLfoOutputs()); // connected pins survive
 m.lfoOutputPins[0]->connected=false;m.loadLfoOutputs(ofJson::object());assert(!m.hasLfoOutputs());
 m.loadLfoOutputs(saved);assert(m.hasLfoOutputs()); // pins before any connection restoration
 m.lfoCount=1;m.liveHook=[](int,int){return .4f;};m.voices=4;now=.3;m.update();
 assert(m.lfoOutputs[0].value==std::vector<float>(4,.4f));
 m.partialBars=[](int){return std::vector<float>{-.5f,.3f,2.f,NAN};};now=.4;m.update();
 assert(m.lfoOutputs[0].value==std::vector<float>({0,.3f,1,0}));
 m.nameSuffix=" 2";ofJson engines;m.saveLfoOutputs(engines);assert(engines["modulationLfoOutputs"][" 2"]==ofJson::array({0}));
 }
 assert(node.pins.empty());
 std::cout<<"PASS: output pin lifecycle, read-only/live-only flags, hidden polling, voice resizing, inactive zeroing, connection protection, patch restore, engine suffixes and partial vectors\n";
}
'''.replace('METHODS', '\n'.join(methods))

with tempfile.TemporaryDirectory() as folder:
    path = pathlib.Path(folder)
    (path / 'test.cpp').write_text(cpp)
    subprocess.run(['c++', '-std=c++17', '-I' + str(ROOT.parent.parent / 'libs/json/include'),
                    str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)

# Every shared editor exposes and restores pins before cable restoration.
for name in ['scSteamPipeBox.cpp', 'scPolyComb.cpp', 'scAdditiveBox.cpp', 'scAnalogBox.cpp',
             'scAliasingBox.cpp', 'scPhysicalBox.cpp', 'scEchoBox.cpp', 'scPulsarBox.h',
             'scSampleBox.cpp', 'scGrainBoxPoly.cpp']:
    text = (ROOT / 'src/nodes' / name).read_text()
    assert 'h.node = this;' in text, name
    assert 'saveLfoOutputs(' in text and 'loadLfoOutputs(' in text, name
print('PASS: all ten shared modulation hosts support output publication and persistence')
