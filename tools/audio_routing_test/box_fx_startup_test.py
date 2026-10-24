#!/usr/bin/env python3
"""Run the actual Box insert/re-routing methods with delayed or absent /n_go."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MOCK = r'''
#include "audio/scFxRouting.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <map>
#include <string>
#include <iostream>
#include <vector>
constexpr int RATE_AUDIO=1;
float ofGetElapsedTimef() { return 1; }
struct ofxSCServer { bool nrt=false; bool isNRTCapturing() { return nrt; } };
struct ofxSCBus {
    static inline int next=1000;
    int index=next++;
    ofxSCBus(int,int,ofxSCServer*) {}
    void free() {}
};
struct ofxSCSynth {
    int nodeID=1;
    bool created=false;
    std::map<std::string,int> controls;
    ofxSCSynth(std::string,ofxSCServer*) {}
    bool isCreated() { return created; }
    void set(std::string key,int value) { controls[key]=value; }
    void createAndRun(int,int,bool) {} // queued /s_new; no /n_go yet
    void run(bool) {}
};
struct Manager {
    ofxSCServer* server=nullptr; bool loading=false;
    ofxSCServer* getServer() {return server;}
    bool areRequiredSynthdefsLoading() {return loading;}
};
class Box { public:
    std::vector<Manager*> servers;
    enum { StEq, StSat, StEcho, StRvb, NumStages };
    struct Group { int nodeID=1; } group;
    struct Fx { ofxSCSynth* synth=nullptr; ofxSCBus* in=nullptr; bool running=false; float offAt=-1; };
    struct State {
        bool created=true;
        int out=400,outBus=400;
        std::array<Fx,NumStages> fx{};
        std::array<Group*,16> groups{};
        ofxSCSynth* spread=nullptr;
        ofxSCSynth* player=nullptr;
    };
    enum {SlFx=4};
    struct Param { int get() { return 2; } } channels,outChannels;
    bool on[NumStages]={false,false,false,false};
    float stageOn(int s) {return on[s];}
    std::string stageDef(int) {return "boxMicroSpace2";}
    float stageTail(int) {return 3;}
    bool getActive() {return true;}
    bool liveBus(ofxSCServer*,ofxSCBus* b,bool) {return b;}
    bool liveAudioBus(ofxSCServer*,ofxSCBus* b) {return b;}
    bool audioBusIsLive(ofxSCServer*,ofxSCBus* b) {return b;}
    void sendStage(int,ofxSCSynth*,ofxSCServer*) {}
    void sendStage(int,ofxSCSynth*) {}
    void updateStages(ofxSCServer*,State&);
    void routeChain(State&);
};
'''
TEST = r'''
int main() {
    ofxSCServer server;
    Box box; Box::State st;
    ofxSCSynth spread("source",&server), player("source",&server);
    st.spread=&spread;st.player=&player;st.groups.fill(&box.group);
    auto source=[&]() {return st.SOURCE->controls.at("out");};
    box.routeChain(st);assert(source()==400);
    Manager manager;manager.server=&server;manager.loading=true;box.servers={&manager};
    box.on[Box::StRvb]=true;box.updateStages(&server,st);
    assert(!st.fx[Box::StRvb].synth && source()==400);
    assert(!scFxAwaitingRoute(st.fx,&server,[&](int s){return box.stageOn(s); },scFxDefinitionsReady(box.servers,&server)));
    manager.loading=false;
    assert(scFxAwaitingRoute(st.fx,&server,[&](int s){return box.stageOn(s); },scFxDefinitionsReady(box.servers,&server)));
    box.updateStages(&server,st);
    auto& reverb=st.fx[Box::StRvb];
    // Rejected /s_new and a long-delayed /n_go both keep feeding MixerPro.
    for(int frame=0;frame<100;++frame) {
        assert(!scFxAwaitingRoute(st.fx,&server,[&](int s){return box.stageOn(s); }));
        assert(source()==400 && !reverb.running);
    }
    // A normal notification attaches the insert without graph reevaluation.
    reverb.synth->created=true;
    assert(scFxAwaitingRoute(st.fx,&server,[&](int s){return box.stageOn(s); }));
    box.updateStages(&server,st);
    assert(reverb.running && source()==reverb.in->index);
    assert(reverb.synth->controls["out"]==400);
    assert(!scFxAwaitingRoute(st.fx,&server,[&](int s){return box.stageOn(s); }));
    // Adding an unconfirmed effect must preserve an already working chain.
    box.on[Box::StEcho]=true;box.updateStages(&server,st);
    assert(!st.fx[Box::StEcho].running && source()==reverb.in->index);
    // Disable while pending; a late /n_go cannot insert a disabled effect.
    box.on[Box::StEcho]=false;st.fx[Box::StEcho].synth->created=true;
    assert(!scFxAwaitingRoute(st.fx,&server,[&](int s){return box.stageOn(s); }));
    // Tail completion restores the external mixer input and resume works.
    box.on[Box::StRvb]=false;box.updateStages(&server,st);
    assert(reverb.offAt>0);reverb.running=false;box.routeChain(st);assert(source()==400);
    box.on[Box::StRvb]=true;box.updateStages(&server,st);assert(source()==reverb.in->index);
    // NRT has no /n_go but uses ordered creation commands.
    server.nrt=true;box.on[Box::StSat]=true;box.updateStages(&server,st);
    assert(st.fx[Box::StSat].running);
    std::cout<<"PASS: NAME preserves the mixer input while an insert is pending/failed; ack, late disable, tail, resume and NRT routing work\n";
    for(auto& f:st.fx) {delete f.synth;delete f.in;}
}
'''

with tempfile.TemporaryDirectory(prefix='box-fx-startup-') as tmp:
    for name in ('SteamPipe', 'FM', 'Aliasing', 'Physical', 'Analog', 'Slicer'):
        directory = 'fmBox' if name == 'FM' else name[0].lower() + name[1:] + 'Box'
        text = (ROOT / f'src/nodes/boxSynths/{directory}/sc{name}Box.cpp').read_text()
        methods = []
        for method in ('updateStages', 'routeChain'):
            start = text.index(f'void sc{name}Box::{method}(')
            # Methods end at the first unindented closing brace.
            end = text.index('\n}', start) + 2
            methods.append(re.sub(r'void sc\w+Box::'+method+r'\([^\n]+',
                                  f'void Box::{method}(' + ('ofxSCServer* server, State& st) {' if method == 'updateStages' else 'State& st) {'),
                                  text[start:end], count=1))
        cpp = Path(tmp) / 'test.cpp'
        test = TEST.replace('SOURCE', 'player' if name == 'Slicer' else 'spread').replace('NAME', name+'Box')
        cpp.write_text(MOCK + '\n'.join(methods) + test)
        exe = Path(tmp) / 'test'
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-I'+str(ROOT / 'src'), str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

# EchoBox inserts only on its wet branch; dry/wet selection must also wait.
text = (ROOT / 'src/nodes/boxSynths/echoBox/scEchoBox.cpp').read_text()
methods = []
for method in ('routeWet', 'updateReverb'):
    start = text.index(f'void scEchoBox::{method}(')
    end = text.index('\n}', start) + 2
    methods.append(text[start:end].replace('scEchoBox::', 'Echo::').replace('ServerState&', 'State&'))
mock = MOCK[:MOCK.index('class Box { public:')] + r'''
std::string ofToString(int n) {return std::to_string(n);}
class Echo { public:
    std::vector<Manager*> servers;
    struct State {
        ofxSCSynth *mix=nullptr,*rvb=nullptr;
        ofxSCBus *wetA=nullptr,*wetB=nullptr;
        bool rvbRunning=false;float rvbOffAt=-1;
    };
    struct Toggle {
        bool value=true;
        bool get() const {return value;}
        operator bool() const {return value;}
        Toggle& operator=(bool v) {value=v;return *this;}
    } rvbOn;
    struct { struct { float activation(bool v) {return v;} } morph; } snaps;
    int n() {return 2;}
    bool getActive() {return true;}
    void sendRvb(ofxSCSynth*) {}
    float reverbTail() {return 3;}
    void routeWet(State&);
    void updateReverb(ofxSCServer*,State&);
};
'''
test = r'''
int main() {
    ofxSCServer server; Echo box; Echo::State st;
    ofxSCSynth mix("mixer",&server);ofxSCBus wetA(RATE_AUDIO,2,&server),wetB(RATE_AUDIO,2,&server);
    st.mix=&mix;st.wetA=&wetA;st.wetB=&wetB;
    Manager manager;manager.server=&server;manager.loading=true;box.servers={&manager};
    box.updateReverb(&server,st);assert(!st.rvb && mix.controls["wet"]==wetA.index);
    manager.loading=false;box.updateReverb(&server,st);
    assert(st.rvb && !st.rvbRunning && mix.controls["wet"]==wetA.index);
    st.rvb->created=true;box.updateReverb(&server,st);
    assert(st.rvbRunning && mix.controls["wet"]==wetB.index);
    box.rvbOn=false;box.updateReverb(&server,st);assert(st.rvbOffAt>0);
    delete st.rvb;
    std::cout<<"PASS: EchoBox retains its direct wet route until reverb acknowledgement\n";
}
'''
with tempfile.TemporaryDirectory(prefix='echo-fx-startup-') as tmp:
    cpp=Path(tmp)/'test.cpp';cpp.write_text(mock+'\n'.join(methods)+test)
    exe=Path(tmp)/'test'
    subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src'),str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
