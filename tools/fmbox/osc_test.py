#!/usr/bin/env python3
"""Exercise production FMBox note dispatch and automation coalescing without audio."""
from pathlib import Path
import subprocess
import os
import tempfile

root = Path(__file__).resolve().parents[2]
source_path = Path(os.environ.get('BOX_OSC_SOURCE', root/'src/nodes/boxSynths/fmBox/scFMBox.cpp'))
source = source_path.read_text().replace(os.environ.get('BOX_OSC_CLASS', 'scFMBox'), 'scFMBox')
def function(signature):
    start=source.index(signature)
    body=source.index('{',start)
    depth=0
    for end in range(body,len(source)):
        if source[end]=='{': depth+=1
        elif source[end]=='}':
            depth-=1
            if depth==0:return source[start:end+1]+'\n'
    raise ValueError(signature)
notes=function('void scFMBox::sendNotes(')
sound_queue=function('void scFMBox::sendAll()')+function('void scFMBox::flushSound()')
stage_queue=function('void scFMBox::sendStageAll(')
replay=function('void scFMBox::resendParametersForNRT()')
cpp = r'''
#include <array>
#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <vector>
struct ofxOscMessage {
    std::string address;std::vector<std::string> args;
    void setAddress(std::string a){address=a;}
    void clear(){address.clear();args.clear();}
    void addIntArg(int v){args.push_back(std::to_string(v));}
    void addFloatArg(float v){args.push_back(std::to_string(v));}
    void addStringArg(std::string v){args.push_back(v);}
};
struct ofxOscBundle {
    std::vector<ofxOscMessage> messages;
    void addMessage(const ofxOscMessage& m){messages.push_back(m);}
};
struct ofxSCServer {
    uint64_t tag=0;
    std::vector<std::pair<uint64_t,ofxOscBundle>> packets;
    void sendBundle(ofxOscBundle& b){packets.push_back({tag,b});}
    void sendMsg(ofxOscMessage& m){ofxOscBundle b;b.addMessage(m);sendBundle(b);}
};
struct ofxSCSynth {
    int nodeID;ofxSCServer* server;bool created=true;
    std::map<std::string,std::vector<float>> queued;
    bool isCreated()const{return created;}
    void set(const std::string& key,float value){set(key,std::vector<float>{value});}
    void set(const std::string& key,int value){set(key,float(value));}
    void set(const std::string& key,const std::vector<float>& values){
        if(!created){queued[key]=values;return;}
        ofxOscMessage m;m.setAddress("/n_setn");m.addIntArg(nodeID);m.addStringArg(key);m.addIntArg(values.size());
        for(float v:values)m.addFloatArg(v);server->sendMsg(m);
    }
    void mapan(const std::string& key,int bus,int){set(key,bus);}
};
#include "audio/scSynthControlBatch.h"
#include "audio/scControlBusBatch.h"
template<class T>struct Param {T value;const T&get()const{return value;}};
struct scModulation {enum {SRC_VEL,SRC_RND};int sourceBus(ofxSCServer*,int){return 32;}void resend(){}};
struct scFMBox {
    enum NoteFields {NoteGate=1,NotePitch=2,NoteRouting=4,NoteAll=7};
    struct State {
        ofxSCSynth* gate=nullptr;int builtVoices=5,droneSent=-1;
        std::array<ofxSCSynth*,16> voices{};
        std::array<std::map<std::string,float>,16> sent;
        struct Bus {int index=64;}*gates=nullptr,*velocities=nullptr;
        std::map<std::string,std::vector<float>> nativeSent,spreadSent;
        int spreadOutSent=-1;
        struct Fx {std::map<std::string,std::vector<float>> sent;int outSent=-1;};
        std::array<Fx,4>fx;
    };
    std::map<ofxSCServer*,State> states;
    Param<std::vector<float>> gate{{0}},pitch{{60}};
    struct Input {int busValue=-1;int bus(ofxSCServer*){return busValue;}}gateIn,pitchIn;
    struct Scheduled {struct Scope{~Scope(){}};Scope inEffectNow(){return {};}}scheduled;
    struct Pulse {
        int busValue=-1,enableWrites=0;std::vector<float> held;
        int bus(ofxSCServer*){return busValue;}
        void enable(ofxSCServer*,const std::vector<float>& v,int,int){held=v;++enableWrites;}
        void sendAll(){}
    } pulse;
    scModulation mod;bool soundDirty=false;int soundSends=0;
    bool droning()const{return false;}
    static float first(const std::vector<float>&v,int i,float fallback){return v.empty()?fallback:v[i%v.size()];}
    void sendNotes(ofxSCServer*,State&,unsigned fields=NoteAll);
    void sendSound(ofxSCServer*,State&){++soundSends;}
    void sendAll();void flushSound();void sendStageAll(int);void resendParametersForNRT();
};
NOTES
QUEUE
STAGE
REPLAY
int main(){
    {
        ofxSCServer capture;ofxSCSynth synth{999,&capture};scSynthControlBatch::Cache cache;
        {scSynthControlBatch batch(&synth,&capture,&cache);batch.set("x",1.f);batch.set("array",std::vector<float>{2,3});batch.send();}
        assert(capture.packets.size()==1);
        {scSynthControlBatch batch(&synth,&capture,&cache);batch.set("x",1.f);batch.set("array",std::vector<float>{2,3});batch.send();}
        assert(capture.packets.size()==1);
        {scSynthControlBatch batch(&synth,&capture,&cache);batch.set("array",std::vector<float>{2});batch.send();}
        assert(capture.packets.size()==2);
        cache.clear();
        {scSynthControlBatch batch(&synth,&capture,&cache);batch.set("x",1.f);batch.send();}
        assert(capture.packets.size()==3);
        synth.created=false;cache.clear();
        {scSynthControlBatch batch(&synth,&capture,&cache);batch.set("x",4.f);batch.send();}
        assert(capture.packets.size()==3 && synth.queued["x"]==std::vector<float>{4});
        synth.created=true;capture.packets.clear();
        {scSynthControlBatch batch(&synth,&capture);
         for(int i=0;i<1000;++i)batch.set("control"+std::to_string(i),float(i));batch.send();}
        assert(capture.packets.size()>1 && capture.packets.size()<20);
        size_t total=0;
        for(const auto&packet:capture.packets)for(const auto&m:packet.second.messages){
            assert(m.args[0]=="999");total+=(m.args.size()-1)/3;
        }
        assert(total==1000); // flushed batches never repeat earlier controls
        capture.packets.clear();
        {scControlBusBatch buses(&capture);buses.set(4,{1,2});buses.set(100,{3});buses.send();}
        assert(capture.packets.size()==1);
        const auto& busMessage=capture.packets[0].second.messages[0];
        assert(busMessage.address=="/c_setn" && busMessage.args.size()==7);
        assert(busMessage.args[0]=="4" && busMessage.args[1]=="2" && busMessage.args[4]=="100" && busMessage.args[5]=="1");
        std::cout<<"PASS: bounded synth batches preserve all 1000 controls without duplicates; disjoint control-bus ranges share one message.\n";
        std::cout<<"PASS: cached control batches skip unchanged sends, detect vector resizing, replay after invalidation and retain creation arguments.\n";
    }
    ofxSCServer server;scFMBox box;auto&st=box.states[&server];
    ofxSCSynth gate{100,&server};st.gate=&gate;
    scFMBox::State::Bus bus;st.gates=&bus;st.velocities=&bus;
    std::vector<ofxSCSynth> voices;voices.reserve(5);
    for(int i=0;i<5;++i){voices.push_back({101+i,&server});st.voices[i]=&voices.back();}
    box.sendNotes(&server,st);server.packets.clear();
    // A polyphonic pitch event is one timestamped packet, without Gate or routing.
    server.tag=1000;box.pitch.value={62,65,67,69,72};
    box.sendNotes(&server,st,scFMBox::NotePitch);
    assert(server.packets.size()==1 && server.packets[0].first==1000);
    assert(server.packets[0].second.messages.size()==5);
    for(const auto&m:server.packets[0].second.messages)assert(m.args[1]=="pitch");
    // Automating many parameters between prepared pitch and gate cannot emit a note.
    for(int i=0;i<100;++i){box.sendAll();box.sendStageAll(i%4);}
    assert(server.packets.size()==1 && box.soundSends==0);
    box.flushSound();assert(box.soundSends==1 && server.packets.size()==1);
    box.flushSound();assert(box.soundSends==1);
    server.tag=3000;box.gate.value={1,0.8f,0,0,0};box.sendNotes(&server,st,scFMBox::NoteGate);
    assert(server.packets.size()==2 && server.packets.back().first==3000);
    const auto&m=server.packets.back().second.messages;
    assert(m.size()==1 && m[0].address=="/n_setn" && m[0].args[1]=="gate" && m[0].args[2]=="5");
    for(int i=0;i<100;++i)box.sendAll();box.flushSound();
    assert(box.soundSends==2 && server.packets.size()==2);
    // Note-off is neither deduplicated nor deferred with sound updates.
    box.gate.value={0};box.sendNotes(&server,st,scFMBox::NoteGate);
    assert(server.packets.size()==3 && server.packets.back().second.messages[0].args[3]=="0.000000");
    // Creation still queues the correct controls, without cross-writing note fields.
    gate.created=false;for(auto&v:voices)v.created=false;
    box.sendNotes(&server,st,scFMBox::NotePitch);
    assert(gate.queued.empty());for(const auto&v:voices)assert(v.queued.size()==1 && v.queued.count("pitch"));
    box.sendNotes(&server,st,scFMBox::NoteGate);assert(gate.queued.size()==1 && gate.queued.count("gate"));
    for(auto&v:voices)v.queued.clear();gate.queued.clear();
    box.sendNotes(&server,st,scFMBox::NoteRouting);
    assert(!gate.queued.count("gate"));for(const auto&v:voices)assert(!v.queued.count("pitch"));
    // Offline capture explicitly flushes sound and restores full note/routing state.
    const auto before=box.soundSends;box.resendParametersForNRT();
    assert(box.soundSends==before+1 && gate.queued.count("gate"));
#ifdef BOX_OSC_PULSE
    gate.created=true;for(auto&v:voices)v.created=true;
    server.packets.clear();box.pulse.busValue=128;
    const int enabled=box.pulse.enableWrites;
    box.sendNotes(&server,st,scFMBox::NotePitch);
    assert(box.pulse.enableWrites==enabled);
    for(const auto&p:server.packets)for(const auto&m:p.second.messages)assert(m.args[1]=="pitch");
    server.packets.clear();box.gate.value={1};box.sendNotes(&server,st,scFMBox::NoteGate);
    assert(box.pulse.enableWrites==enabled+1 && box.pulse.held==std::vector<float>{1});
    assert(server.packets.size()==1 && server.packets[0].second.messages[0].args[3]=="0.000000");
    std::cout<<"PASS: Pulse follows Gate without admitting raw gates, and pitch changes leave pulse enable state untouched.\n";
#endif
    std::cout<<"PASS: five voices use one pitch packet and one gate packet; 200 automation callbacks coalesce to one sound send with zero note writes; note-offs, creation, routing and NRT replay preserved.\n";
}
'''.replace('NOTES', notes).replace('QUEUE', sound_queue).replace('STAGE', stage_queue).replace('REPLAY', replay)
with tempfile.TemporaryDirectory(prefix='fmbox-osc-') as folder:
    work=Path(folder);(work/'test.cpp').write_text(cpp)
    (work/'ofxSCSynth.h').write_text('#pragma once\n')
    (work/'ofxSCServer.h').write_text('#pragma once\n')
    subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Wno-missing-field-initializers',
                    '-fsanitize=address,undefined',*(['-DBOX_OSC_PULSE'] if os.environ.get('BOX_OSC_PULSE') else []),'-I'+str(work),'-I'+str(root/'src'),str(work/'test.cpp'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True)
