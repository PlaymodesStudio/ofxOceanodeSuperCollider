#!/usr/bin/env python3
"""Exercise production note-send functions with a recording OSC transport."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
source = (ROOT / 'src/nodes/boxSynths/polyComb/scPolyComb.cpp').read_text()
functions = []
for name in ('sendGate', 'sendPitch', 'sendSub'):
    start = source.index(f'void scPolyComb::{name}(')
    end = source.index('\n}\n', start) + 3
    functions.append(source[start:end])
stub = r'''
#pragma once
#include <string>
#include <vector>
#include <map>
#include <cassert>
struct Arg { char type; std::string text; float value; };
struct ofxOscMessage {
    std::string address; std::vector<Arg> args;
    void setAddress(std::string s){address=s;}
    void addIntArg(int v){args.push_back({'i',"",float(v)});}
    void addFloatArg(float v){args.push_back({'f',"",v});}
    void addStringArg(std::string v){args.push_back({'s',v,0});}
    void addCharArg(char v){args.push_back({v,"",0});}
};
struct ofxSCServer { std::vector<ofxOscMessage> sent; void sendMsg(ofxOscMessage& m){sent.push_back(m);} };
struct ofxSCSynth {
    int nodeID=1, mappings=0; bool created=true;
    std::map<std::string,std::vector<float>> stored;
    bool isCreated(){return created;}
    void set(std::string key,float v){stored[key]={v};}
    void set(std::string key,int v){set(key,float(v));}
    void set(std::string key,const std::vector<float>& v){stored[key]=v;}
    void mapan(std::string,int,int){++mappings;}
};
'''
test = r'''
#include <algorithm>
#include <iostream>
#include "audio/scSynthControlBatch.h"
template<class T> struct Param { T value; T get()const{return value;} };
std::vector<float> expand(const std::vector<float>& v,int n,float fallback=0){
    if(v.empty())return std::vector<float>(n,fallback);
    auto out=v;out.resize(n,v.back());return out;
}
struct Bus{int index=128;};
struct Part{ofxSCSynth* synth=nullptr;};
struct ServerState{
    int builtVoices=3;
    ofxSCServer* server;ofxSCSynth *gateS=nullptr,*pitchS=nullptr;
    Part sub,ins[2];Bus *gateBus=nullptr,*noteBus=nullptr,*voiceBus=nullptr,*visBlock=nullptr;
};
struct scPolyComb {
    static constexpr int SRC_VEL=0,SRC_RND=1,VIS_SUB=65,IComb=0,IRing=1;
    Param<std::vector<float>> gate{{1}},pitch{{60}},transpose{{0}},detune{{0}};
    Param<float> glide{0},vibRate{5},vibRand{0},vibAmt{0};
    Param<float> subTranspose{-12},subFineTune{0},subVibRate{5},subVibAmt{0},subVibRand{0},subLevel{.25},master{.8},velSens{.5};
    Param<float> subA{.01},subD{.5},subS{1},subR{.5},subCA{-4},subCD{-4},subCR{-4};
    Param<bool> subOn{true},expandPitch{false};Param<int> ringPitchSource{1};
    struct Input{int cable=-1,count=-1;int bus(ofxSCServer*){return cable;}int channels()const{return count;}} gateIn,pitchIn;
    struct Notes{int inEffectNow(){return 0;}}noteInputs;
    int oscMap=0,combMap=1,insSends=0;bool internal=false,mapped=false,drone=false;
    int n()const{return 3;}
    bool droning()const{return drone;}
    bool internalPitch()const{return internal;}
    bool combMapped()const{return mapped;}
    int src(const ServerState&,int i){return 200+i*32;}
    int mod(const ServerState&,int i){return 400+i*32;}
    std::vector<float> mapPitches(int m){return m==0?std::vector<float>{60,64,67}:std::vector<float>{48,52,55};}
    std::vector<float> oscillatorPitches(){return internal?mapPitches(oscMap):expand(pitch.get(),n());}
    std::vector<float> resonatorPitches(){return mapped?mapPitches(combMap):oscillatorPitches();}
    std::vector<float> combMapPitches(){return resonatorPitches();}
    std::vector<float> processedPitch(){return pitch.get();}
    void sendIns(ServerState&,int){++insSends;}
    void sendVec(ofxSCSynth* s,std::string key,std::vector<float> v){
        scSynthControlBatch controls(s,&server);controls.set(key,expand(v,n()));controls.send();
    }
    struct Mod {template<class T>std::vector<float> voiceValues(const Param<T>&p){return std::vector<float>(3,float(p.get()));}} modulation;
    ofxSCServer server;
    void sendGate(ServerState&,bool=false);void sendPitch(ServerState&,bool=false);void sendSub(ServerState&,bool=false);
};
std::vector<float> values(const ofxOscMessage& m,const std::string& key){
    assert(m.address=="/n_setn");
    for(size_t i=1;i<m.args.size();){
        assert(m.args[i].type=='s' && m.args[i+1].type=='i');
        int count=int(m.args[i+1].value);
        if(m.args[i].text==key){
            std::vector<float> v;for(int j=0;j<count;++j)v.push_back(m.args[i+2+j].value);return v;
        }
        i+=2+count;
    }return {};
}
'''
main = r'''
int main(){
    scPolyComb node;ofxSCSynth gate,pitch,sub,comb;Bus bus;
    gate.nodeID=10;pitch.nodeID=11;sub.nodeID=12;comb.nodeID=13;
    ServerState st;st.server=&node.server;st.gateS=&gate;st.pitchS=&pitch;st.sub.synth=&sub;st.ins[0].synth=&comb;
    st.gateBus=st.noteBus=st.voiceBus=st.visBlock=&bus;
    // Playback uses one gate packet and one per pitch destination, no mapping or envelope traffic.
    node.sendGate(st,true);node.sendPitch(st,true);
    assert(node.server.sent.size()==3);
    assert(values(node.server.sent[0],"gate")==std::vector<float>({1,1,1}));
    assert(values(node.server.sent[0],"out").empty());
    assert(values(node.server.sent[1],"pitch")==std::vector<float>({60,60,60}));
    assert(values(node.server.sent[1],"atk").empty());
    assert(values(node.server.sent[2],"pitch")==std::vector<float>({60,60,60}));
    assert(values(node.server.sent[2],"transpose").empty());
    assert(gate.mappings+pitch.mappings+sub.mappings==0);
    // Note-off and short vectors still fill every voice, preventing stuck notes/stale pitches.
    node.server.sent.clear();node.gate.value={0};node.pitch.value={62,65};
    node.sendGate(st,true);node.sendPitch(st,true);
    assert(values(node.server.sent[0],"gate")==std::vector<float>({0,0,0}));
    assert(values(node.server.sent[2],"pitch")==std::vector<float>({62,65,65}));
    // Independent sub stays on raw input root; the oscillator/comb maps remain separate.
    node.server.sent.clear();node.internal=node.mapped=true;node.sendPitch(st,true);
    assert(node.server.sent.size()==3 && node.insSends==0);
    assert(values(node.server.sent[0],"pitch")==std::vector<float>({62,65,65}));
    assert(values(node.server.sent[1],"pitch")==std::vector<float>({60,64,67}));
    assert(values(node.server.sent[2],"combpitch")==std::vector<float>({48,52,55}));
    // Resonator-follow ringmod updates its mapped pitches on each played note.
    ofxSCSynth ring;ring.nodeID=14;st.ins[1].synth=&ring;node.ringPitchSource.value=2;
    node.server.sent.clear();node.sendPitch(st,true);
    assert(values(node.server.sent[2],"ringpitch")==std::vector<float>({48,52,55}));
    node.ringPitchSource.value=1;st.ins[1].synth=nullptr;
    // Creation/reconnection still configures every bus and maps live audio cables.
    node.server.sent.clear();node.internal=node.mapped=false;node.gateIn.cable=20;node.pitchIn.cable=30;
    node.sendGate(st);node.sendPitch(st);
    assert(gate.mappings==1 && pitch.mappings==1 && sub.mappings==1);
    assert(values(node.server.sent[0],"out")==std::vector<float>({128}));
    assert(values(node.server.sent[1],"atk")==std::vector<float>({.01f,.01f,.01f}));
    // A note arriving before /n_go must retain the full creation arguments.
    gate.created=pitch.created=sub.created=false;node.server.sent.clear();
    node.sendGate(st,true);node.sendPitch(st,true);
    assert(node.server.sent.empty());
    assert(gate.stored.count("velocity") && pitch.stored.count("vibrate") && sub.stored.count("rel"));
    // Connections/notes arriving during a voice-count fade must not resize
    // arrays on the old definitions, even through the direct send functions.
    gate.created=pitch.created=sub.created=true;st.builtVoices=2;node.server.sent.clear();
    node.sendGate(st);node.sendPitch(st);
    assert(node.server.sent.empty());
    std::cout<<"PASS: 3 packets per gate+pitch update with sub; 4 with mapped comb; note-off, vector expansion, maps, cable reconnect, pending creation.\n";
}
'''
with tempfile.TemporaryDirectory(prefix='polycomb-note-traffic-') as directory:
    out = pathlib.Path(directory)
    (out / 'ofxSCSynth.h').write_text(stub)
    (out / 'test.cpp').write_text(test + '\n'.join(functions) + main)
    subprocess.run(['c++', '-std=c++17', '-I', str(out), '-I', str(ROOT / 'src'), str(out / 'test.cpp'), '-o', str(out / 'test')], check=True)
    subprocess.run([str(out / 'test')], check=True)
