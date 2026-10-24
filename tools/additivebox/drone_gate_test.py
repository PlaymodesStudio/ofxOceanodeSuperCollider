#!/usr/bin/env python3
"""Run production note-writer routing against a recording SC synth adapter."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'src/nodes/boxSynths/additiveBox/scAdditiveBox.cpp').read_text()
start = source.index('void scAdditiveBox::syncNoteWriters(')
end = source.index('\nfloat scAdditiveBox::sampleRate()', start)
cpp = r'''
#include <algorithm>
#include <cassert>
#include <map>
#include <string>
#include <vector>
#include <iostream>
using ofxSCServer=int;
struct ofxSCSynth {
    int nodeID=10, mapped=-1;bool created=false;
    std::map<std::string,float> values;
    ofxSCSynth(const char*,ofxSCServer*){}
    void free(){}
    void set(const std::string& k,float v){values[k]=v;if(k=="gate")mapped=-1;}
    void mapan(const char*,int bus,int){mapped=bus;}
    void createAndRun(int,int,bool){created=true;}
};
struct scModulation {static constexpr int SRC_RND=13,SRC_VEL=12;};
struct scAdditiveBox {
    struct Bool {bool value=false;bool get()const{return value;}} drone;
    struct Gate {std::vector<float> value{0,.25};const auto& get()const{return value;}} gate;
    struct Input {int index=-1;int bus(ofxSCServer*)const{return index;}} gateIn;
    struct Bus {int index=20;} bus;
    struct Mod {int sourceBus(ofxSCServer*,int s){return 100+s;}} mod;
    ofxSCSynth group{"group",nullptr};
    struct State {
        bool modCreated=true;ofxSCSynth* modGroup=nullptr;ofxSCSynth* modSources=nullptr;
        Bus* gateBus=nullptr;
        std::vector<ofxSCSynth*> noteWriters;
        std::vector<std::map<std::string,float>> noteSent;
        std::vector<int> noteMapped;
    };
    std::map<ofxSCServer*,State> states;
    scAdditiveBox(){auto& s=states[nullptr];s.modGroup=s.modSources=&group;s.gateBus=&bus;}
    ~scAdditiveBox(){for(auto* s:states[nullptr].noteWriters)delete s;}
    int numVoicesClamped()const{return 2;}
    float valueFor(const std::vector<float>& v,int i,float fallback){return v.empty()?fallback:v[i%v.size()];}
    void syncNoteWriters(ofxSCServer*,bool=false);
};
METHOD
int main(){
    scAdditiveBox box;box.syncNoteWriters(nullptr);
    auto& writers=box.states[nullptr].noteWriters;
    assert(writers[0]->values.at("gate")==0 && writers[1]->values.at("gate")==.25f);
    box.gateIn.index=40;box.syncNoteWriters(nullptr);
    assert(writers[0]->mapped==40 && writers[1]->mapped==41);
    box.drone.value=true;box.syncNoteWriters(nullptr);
    for(auto* w:writers)assert(w->mapped==-1 && w->values.at("gate")==1);
    box.gate.value={0,0};box.syncNoteWriters(nullptr);
    for(auto* w:writers)assert(w->values.at("gate")==1);
    box.drone.value=false;box.syncNoteWriters(nullptr);
    assert(writers[0]->mapped==40 && writers[1]->mapped==41);
    box.gateIn.index=-1;box.syncNoteWriters(nullptr);
    for(auto* w:writers)assert(w->mapped==-1 && w->values.at("gate")==0);
    box.drone.value=true;box.syncNoteWriters(nullptr);
    for(auto* w:writers)assert(w->values.at("gate")==1);
    box.drone.value=false;box.syncNoteWriters(nullptr);
    for(auto* w:writers)assert(w->values.at("gate")==0);
    std::cout<<"PASS: Drone holds every gate open and restores cable or scalar gates when disabled\n";
}
'''.replace('METHOD', source[start:end])
with tempfile.TemporaryDirectory(prefix='additive-drone-') as work:
    work = Path(work)
    (work / 'test.cpp').write_text(cpp)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    str(work / 'test.cpp'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
