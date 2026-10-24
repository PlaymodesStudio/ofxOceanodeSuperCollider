#!/usr/bin/env python3
"""Verify live modulation state transfers with production code and ownership checks."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
code=(ROOT/'src/nodes/scModulation.cpp').read_text()
def method(signature):
    a=code.index(signature);return code[a:code.index('\n}\n',a)+3]
methods=method('void scModulation::free(')+method('bool scModulation::hasVoiceBank(')+method('bool scModulation::adoptServerState(')
pre=r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <vector>
struct ofxSCServer{};
struct ofxSCSynth{static inline int live=0,frees=0;int id;explicit ofxSCSynth(int i):id(i){++live;}~ofxSCSynth(){--live;}void free(){++frees;}};
struct ofxSCBus{static inline int live=0;ofxSCBus(){++live;}~ofxSCBus(){--live;}void free(){}};
bool liveControlBus(ofxSCServer*,ofxSCBus*){return true;}
class scModulation{public:
    static constexpr int MAX_LFOS=2,MAX_ENVS=1;
    struct ServerState{
        ofxSCBus*srcBlock=nullptr,*modBlock=nullptr;
        std::array<ofxSCSynth*,2> lfo{};
        std::array<ofxSCSynth*,1> env{};
        std::map<int,ofxSCSynth*>routes;
        bool created=true;int builtVoices=0;
    };
    struct ParamTarget{std::string key;int*p=nullptr;std::vector<float>voiceWritten;};
    std::map<ofxSCServer*,ServerState>states;
    std::vector<float>live;
    std::array<std::vector<float>,2>lfoPhases;
    std::vector<ParamTarget>params;
    int voices=4;
    int n()const{return voices;}
    void free(ofxSCServer*,bool sendFree=true);
    bool hasVoiceBank(ofxSCServer*,int)const;
    bool adoptServerState(scModulation&,ofxSCServer*);
    ~scModulation(){while(!states.empty())free(states.begin()->first);}
    void create(ofxSCServer*s,int count,int id){
        auto&st=states[s];st.builtVoices=count;
        st.srcBlock=new ofxSCBus;st.modBlock=new ofxSCBus;
        for(int i=0;i<2;++i)st.lfo[i]=new ofxSCSynth(id+i);
        st.env[0]=new ofxSCSynth(id+2);st.routes[0]=new ofxSCSynth(id+3);
    }
};
'''
main=r'''
int main(){
    ofxSCServer server,other;
    {
        scModulation main,peer;int own=1,foreign=2;
        main.create(&server,4,100);peer.create(&server,12,200);peer.create(&other,12,300);
        main.params.push_back({"pitch",&own,{1,2,3,4}});
        peer.params.push_back({"pitch",&foreign,std::vector<float>(12,7)});
        peer.live={.2f,.8f};peer.lfoPhases[0]={.1f,.9f};
        assert(!main.adoptServerState(peer,&server));assert(ofxSCSynth::frees==0);
        main.voices=12;
        auto* env=peer.states.at(&server).env[0];auto* lfo=peer.states.at(&server).lfo[0];
        assert(main.adoptServerState(peer,&server));
        assert(main.states.at(&server).env[0]==env && main.states.at(&server).lfo[0]==lfo);
        assert(main.states.at(&server).builtVoices==12 && peer.states.count(&server)==0);
        assert(peer.states.count(&other)==1 && ofxSCSynth::frees==4);
        assert(main.params[0].p==&own && main.params[0].voiceWritten.size()==12);
        assert(main.live==peer.live && main.lfoPhases==peer.lfoPhases);
        assert(!main.adoptServerState(main,&server));
    }
    assert(ofxSCSynth::live==0 && ofxSCBus::live==0 && ofxSCSynth::frees==12);
    std::cout<<"PASS: destination LFO/envelope/route objects and phase data survive adoption; frontend bindings stay local; old state freed exactly once\n";
}
'''
with tempfile.TemporaryDirectory(prefix='voice-morph-mod-')as directory:
    d=Path(directory);(d/'test.cpp').write_text(pre+methods+main)
    subprocess.run(['clang++','-std=c++17','-g','-fsanitize=address,undefined',str(d/'test.cpp'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True)
