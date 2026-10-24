#!/usr/bin/env python3
"""Exercise AdditiveBox's production replan method against an in-memory SC graph."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'src/nodes/boxSynths/additiveBox/scAdditiveBox.cpp').read_text()
a=source.index('void scAdditiveBox::rebuildFx(ofxSCServer* server)')
b=source.index('\nvoid scAdditiveBox::rebuildFxAll',a)
method=source[a:b]
mock=r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <map>
#include <string>
#include <vector>
#include <iostream>
struct ofxSCServer {};
constexpr int RATE_AUDIO=1,MAX_OUTPUTS=16;
struct ofxSCBus {
    static inline int next=100;
    int index=next++;
    ofxSCBus(int,int,ofxSCServer*) {}
};
struct ofxSCSynth {
    static inline int next=1000,created=0,freed=0;
    static inline std::vector<int> order;
    int nodeID=next++;
    std::map<std::string,int> controls;
    ofxSCSynth(std::string,ofxSCServer*) {}
    void set(std::string key,int value) { controls[key]=value; }
    void moveBefore(int target) {
        order.erase(std::remove(order.begin(),order.end(),nodeID),order.end());
        auto at=std::find(order.begin(),order.end(),target);
        assert(at!=order.end()); order.insert(at,nodeID);
    }
    void createAndRun(int,int target,bool) { ++created; moveBefore(target); }
    void free() { ++freed; order.erase(std::remove(order.begin(),order.end(),nodeID),order.end()); }
};
class scAdditiveBox { public:
    static constexpr int FX_COUNT=3;
    struct State {
        ofxSCSynth* out=nullptr;
        ofxSCBus* mix=nullptr;
        std::array<ofxSCSynth*,3> fx{};
        std::vector<int> fxPlan;
        int fxChannels=0,outIn=0;
        bool created=true;
        std::vector<ofxSCBus*> fxBuses;
    };
    struct { struct { float activation(float p) const {return p;} } morph; } snaps;
    std::map<ofxSCServer*,State> states;
    float fxOn[3]={0,0,0};
    bool fxPending=false,ready=true;
    int channels=2;
    int numOutputsClamped() const {return channels;}
    bool fxStageReady(ofxSCServer*) const {return ready;}
    bool audioBusIsLive(ofxSCServer*,ofxSCBus*) const {return true;}
    std::string fxDefName(int k) const {return std::to_string(k)+"_"+std::to_string(channels);}
    void sendFxParams(int,ofxSCSynth*) {}
    void freeFxSynths(ofxSCServer* server) {
        auto& st=states[server];
        for(auto*& p:st.fx) if(p){p->free();delete p;p=nullptr;}
        st.fxPlan.clear();
    }
    void rebuildFx(ofxSCServer*);
};
'''
test=r'''
int main() {
    ofxSCServer server;
    scAdditiveBox box;
    auto& st=box.states[&server];
    st.out=new ofxSCSynth("out",&server);
    st.mix=new ofxSCBus(RATE_AUDIO,16,&server);
    ofxSCSynth::order.push_back(st.out->nodeID);
    box.fxOn[1]=1; box.rebuildFx(&server);
    const int echo=st.fx[1]->nodeID;
    // A wet tail must survive another stage entering before it or after it.
    box.fxOn[0]=0.001f;box.fxOn[2]=0.001f;box.rebuildFx(&server);
    assert(st.fx[1]->nodeID==echo && ofxSCSynth::freed==0);
    assert((ofxSCSynth::order==std::vector<int>{st.fx[0]->nodeID,echo,st.fx[2]->nodeID,st.out->nodeID}));
    assert(st.fx[0]->controls["in"]==st.mix->index);
    assert(st.fx[1]->controls["in"]==st.fxBuses[0]->index);
    assert(st.fx[2]->controls["in"]==st.fxBuses[1]->index);
    const int created=ofxSCSynth::created;
    for(int frame=0;frame<100;++frame) {
        box.fxOn[0]=(frame+1)/100.0f;box.rebuildFx(&server);
    }
    assert(ofxSCSynth::created==created && ofxSCSynth::freed==0);
    box.fxOn[0]=0;box.rebuildFx(&server);
    assert(st.fx[1]->nodeID==echo && ofxSCSynth::freed==1);
    assert((ofxSCSynth::order==std::vector<int>{echo,st.fx[2]->nodeID,st.out->nodeID}));
    // Channel variant changes must recreate wrappers even with the same plan.
    box.channels=16;box.rebuildFx(&server);
    assert(st.fx[1]->nodeID!=echo && ofxSCSynth::created==created+2);
    // A deferred definition load must keep a retry pending.
    box.ready=false;box.fxOn[0]=1;box.rebuildFx(&server);assert(box.fxPending);
    std::cout<<"PASS: FX replan preserves existing tails, node order and bus routing; channel variants rebuild and unavailable definitions retry\n";
}
'''
with tempfile.TemporaryDirectory(prefix='morph-fx-') as directory:
    cpp=Path(directory)/'test.cpp';cpp.write_text(mock+method+test)
    exe=Path(directory)/'test'
    subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
