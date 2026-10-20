#!/usr/bin/env python3
"""Exercise production rebuild/startup code and render it offline, without speakers."""
import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/aliasingbox'))
from render_test import message, bundle, read_wav

source = (ROOT / 'src/nodes/scPolyComb.cpp').read_text()
modulation = (ROOT / 'src/nodes/scModulation.cpp').read_text()

def function(code, signature):
    start = code.index(signature)
    return code[start:code.index('\n}\n', start) + 3]

methods = '\n'.join(function(source, 'void scPolyComb::' + name + '(')
                    for name in ('forStates', 'sendSpread', 'routeChain', 'rebuildOutput', 'rebuildAll'))
methods += function(modulation, 'void scModulation::forStates(')

stub = r'''
#include "scSynthStartup.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>
using Json=nlohmann::json;
Json packets=Json::array();
struct ofxSCServer {
 bool latency=false;
 bool getBLatency(){return latency;}
 void setBLatency(bool b){latency=b;}
 void sendMsg(ofxOscMessage& m){packets.push_back(Json::array({m.payload}));}
};
struct ofxSCBus {int index;void free(){}};
bool liveAudioBus(ofxSCServer*,ofxSCBus* b){return b!=nullptr;}
std::string ofToString(int n){return std::to_string(n);}
std::vector<float> expand(std::vector<float> v,int n,float fallback){v.resize(n,v.empty()?fallback:v.back());return v;}
template<class T>struct Param {T value;T get()const{return value;}};
struct ofxSCSynth {
 std::string definition;int nodeID=0;bool running=false;
 std::map<std::string,Json> pending;
 ofxSCSynth(std::string d):definition(d){}
 bool isCreated(){return false;} // Deliberately withhold /n_go throughout.
 void set(std::string name,int v){pending[name]=v;}
 void set(std::string name,float v){pending[name]=v;}
 void set(std::string name,std::vector<float> v){pending[name]=v;}
 void createAndRun(int position,int group,bool run){
  static int next=2000;nodeID=next++;
  Json create=Json::array({"/s_new",definition,nodeID,position,group});
  for(auto& [name,value]:pending){create.push_back(name);create.push_back(value);}
  pending.clear();
  packets.push_back(Json::array({create,Json::array({"/n_run",nodeID,run?1:0})}));
  running=run;
 }
 void resendStoredArgs(){
  if(pending.empty())return;
  Json group=Json::array();
  for(auto& [name,value]:pending){
   if(value.is_array()){
    Json m=Json::array({"/n_setn",nodeID,name,value.size()});
    for(auto& v:value)m.push_back(v);
    group.push_back(m);
   }else group.push_back(Json::array({"/n_set",nodeID,name,value}));
  }
  pending.clear();packets.push_back(group);
 }
 void run(bool){assert(false && "run() would wait for /n_go; startup must send directly");}
 void free(){packets.push_back(Json::array({Json::array({"/n_free",nodeID})}));}
};
struct Group{int nodeID=1;};
struct Fx{ofxSCSynth* synth=nullptr;ofxSCBus* in=nullptr;bool running=false;float offAt=-1;};
struct ServerState {
 bool created=true;int builtVoices=4,builtChannels=2,spreadLanes=16,outBus=2;
 ofxSCSynth* spread=nullptr;std::array<Fx,4> fx{};Group* groups[1];
 ofxSCBus* voiceBus=nullptr;ofxSCBus* visBlock=nullptr;
};
struct scModulation {
 int voices=4,rebuilds=0;
 std::map<ofxSCServer*,ServerState> states;
 int n()const{return voices;}
 void forStates(const std::function<void(ofxSCServer*,ServerState&)>&);
 void rebuild(ofxSCServer*){++rebuilds;}
};
struct scPolyComb {
 static constexpr int LEGACY_VOICES=16,W=32,SlSpread=0,NumStages=4,VIS_PAN=32;
 int voices=4;Param<int> spreadLayout{0},outChannels{2};scModulation modulation;
 std::map<ofxSCServer*,ServerState> states;
 int n()const{return voices;}
 int mod(const ServerState&,int){return 1000;}
 std::vector<float> panPositions(){return std::vector<float>(n(),.5f);}
 ofxSCSynth* make(ofxSCServer*,ServerState&,std::string name,int){return new ofxSCSynth(name);}
 void updateStages(ofxSCServer*,ServerState& st){routeChain(st);}
 void freeSynths(ServerState& st){if(st.spread){st.spread->free();delete st.spread;st.spread=nullptr;}}
 void createAll(ofxSCServer*,ServerState& st){st.builtVoices=n();rebuildOutput();}
 void forStates(const std::function<void(ofxSCServer*,ServerState&)>&);
 void sendSpread(ServerState&);void routeChain(ServerState&);void rebuildOutput();void rebuildAll();
};
'''
main = r'''
int main(){
 ofxSCServer server,other;Group group;ofxSCBus audio{32},control{1100};
 scPolyComb node;auto& st=node.states[&server];st.groups[0]=&group;st.voiceBus=&audio;st.visBlock=&control;
 // A voice change must not deliver arrays sized for 32 voices to a 4-voice synth.
 int sends=0;node.voices=32;node.forStates([&](auto*,auto&){++sends;});assert(sends==0);
 scModulation mod;mod.voices=32;mod.states[&server].builtVoices=4;mod.states[&other].builtVoices=32;
 mod.forStates([&](auto* s,auto&){assert(s==&other);++sends;});assert(sends==1);
 Json batches=Json::array();
 for(int voices:{1,4,16,17,32,2,1}){
  node.voices=voices;node.rebuildAll();
  assert(st.builtVoices==voices && !server.latency);
  bool started=false,configured=false;int id=st.spread->nodeID;
  for(auto& packet:packets)for(auto& m:packet){
   if(m[0]=="/s_new" && m[2]==id) assert(m.size()==5); // No array-valued /s_new arguments.
   if(m[0]=="/n_set" && m[1]==id && m[2]=="out")configured=m[3]==2;
   if(m[0]=="/n_run" && m[1]==id && m[2]==1){assert(configured);started=true;}
  }
  assert(started);batches.push_back(packets);packets=Json::array();
 }
 assert(node.modulation.rebuilds==7);
 // Restored preset updates during a pending rebuild must not touch the old spreader.
 node.voices=32;node.sendSpread(st);assert(packets.empty());
 // A pending spreader must get the effects input bus without waiting for /n_go.
 node.voices=st.builtVoices;ofxSCBus effectIn{96};ofxSCSynth fx("Unused");fx.nodeID=999;
 st.fx[0].synth=&fx;st.fx[0].running=true;st.fx[0].in=&effectIn;
 node.routeChain(st);bool routed=false;
 for(auto& packet:packets)for(auto& m:packet)
  if(m[0]=="/n_set" && m[1]==st.spread->nodeID && m[2]=="out")routed=m[3]==96;
 assert(routed);st.fx[0]={};node.freeSynths(st);
 std::cout<<batches.dump();
}
'''

with tempfile.TemporaryDirectory(prefix='polycomb-voice-rebuild-') as directory:
    out = pathlib.Path(directory)
    (out / 'ofxOscMessage.h').write_text(r'''
#pragma once
#include <nlohmann/json.hpp>
struct ofxOscMessage {
 nlohmann::json payload=nlohmann::json::array();
 void setAddress(std::string address){payload=nlohmann::json::array({address});}
 void addIntArg(int value){payload.push_back(value);}
};
''')
    (out / 'test.cpp').write_text(stub + methods + main)
    subprocess.run(['clang++', '-std=c++17', '-I'+str(out), '-I'+str(ROOT/'src'),
                    '-I'+str(ROOT.parent.parent/'libs/json/include'),
                    str(out/'test.cpp'), '-o', str(out/'test')], check=True)
    batches = json.loads(subprocess.check_output([str(out/'test')], text=True))
    print('PASS: production startup/routing, pending /n_go, 1–32 voice rebuilds, width guards and FX routing.', flush=True)
    # Probe writes a quiet sine on all voice lanes. Hardware channels 0/1 must
    # remain silent throughout every delayed startup; intended audio is on 2/3.
    config = out/'config.yaml'
    config.write_text(f'includePaths:\n    - {ROOT}/sclang/SCClassLibrary\nexcludePaths: []\npostInlineWarnings: false\nexcludeDefaultPaths: true\n')
    probe = out/'probe.scd'
    probe.write_text(f'Archive.archiveDir="{out}";\nSynthDef(\\VoiceRebuildProbe,{{Out.ar(32,SinOsc.ar(440,0,0.01)!32)}}).writeDefFile("{out}");\n0.exit;\n')
    r = subprocess.run([str(ROOT/'sclang/osx/sclang'), '-l', str(config), str(probe)], capture_output=True, text=True, timeout=30)
    assert r.returncode == 0 and (out/'VoiceRebuildProbe.scsyndef').exists(), r.stdout+r.stderr
    folder = ROOT/'synthdefs/Defaults/PolyComb/CompiledSynthdefs/PolyComb'
    definitions = [out/'VoiceRebuildProbe.scsyndef', folder/'PolyCombSpread2.scsyndef', folder/'PCSpreadWide2.scsyndef']
    score = bundle(0, [message('/d_recv', p.read_bytes()) for p in definitions] +
                   [message('/g_new', 1, 0, 0), message('/s_new', 'VoiceRebuildProbe', 100, 0, 1)])
    for i, packets in enumerate(batches):
        for j, packet in enumerate(packets):
            score += bundle(.1+i*.3+j*.003, [message(m[0], *m[1:]) for m in packet])
    score += bundle(2.3, [message('/g_freeAll', 1)]) + bundle(2.4, [message('/c_set', 0, 0.)])
    osc=out/'test.osc';osc.write_bytes(score);wav=out/'test.wav'
    r=subprocess.run(['/Applications/SuperCollider.app/Contents/Resources/scsynth', '-N',str(osc),'_',str(wav),'48000','WAV','float',
                      '-o','4','-i','0','-a','256','-c','2048','-D','0'],capture_output=True,text=True,timeout=30)
    assert r.returncode==0 and 'FAILURE' not in r.stdout+r.stderr, r.stdout+r.stderr
    import math
    samples,channels,rate=read_wav(wav)
    assert all(math.isfinite(x) for x in samples)
    assert max(abs(x) for i,x in enumerate(samples) if i%channels<2)==0, 'Startup leaked onto hardware output 0/1'
    peak=max(abs(x) for i,x in enumerate(samples) if i%channels>=2)
    assert .01<peak<.12, peak
    print(f'PASS: offline rendered voice transitions keep outputs 0/1 silent and intended audio finite (peak {peak:.4f}).')
