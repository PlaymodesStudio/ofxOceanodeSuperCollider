#!/usr/bin/env python3
"""Production Gate listener / reseeding path, recording OSC for two servers."""
import ast,pathlib,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[2]
s=(ROOT/'src/nodes/scPolyComb.cpp').read_text();a=s.index('void scPolyComb::randomizeMapSeedsOnGate(');b=s.index('\n}\n',a)+3
fn=s[a:b]
a=s.index('listeners.push(gate.newListener(');a=s.index('std::vector<float>& v) {',a)+len('std::vector<float>& v) {');b=s.index('\n    }));',a);listener=s[a:b]
adapter=ast.parse((ROOT/'tools/polycomb/note_traffic_test.py').read_text())
stub=next(ast.literal_eval(n.value)for n in adapter.body if isinstance(n,ast.Assign)and any(isinstance(t,ast.Name)and t.id=='stub'for t in n.targets))
code=r'''
#include <algorithm>
#include "scSynthControlBatch.h"
namespace scScheduling{bool suppressed=false;bool isBackendSendSuppressed(){return suppressed;}}
int randomCalls=0;float ofRandom(float){++randomCalls;return 100.f;}
template<class T>struct Param{T v{};T get()const{return v;}void setWithoutEventNotifications(T value){v=value;}};
struct Part{ofxSCSynth*synth=nullptr;};
struct ServerState{ofxSCServer*server;ofxSCSynth*pitchS;Part ins[2];};
struct scPolyComb{
 enum{IComb,IRing};struct MapParams{Param<bool>indexSeedOnGate;Param<int>indexSeed;};
 MapParams oscMap,combMap;std::vector<float>previousMapGate;std::vector<ServerState*> states;
 Param<int>ringPitchSource;bool internal=true,mapped=true,gateTouched=false;int gateSends=0,messagesBeforeGate=0;
 struct Notes{int inEffectNow(){return 0;}}noteInputs;
 int n(){return 3;}bool internalPitch(){return internal;}bool combMapped(){return mapped;}
 template<class F>void forStates(F fn){for(auto*st:states)fn(st->server,*st);}
 std::vector<float>mapPitches(const MapParams&m){return std::vector<float>(n(),float(m.indexSeed.get()));}
 std::vector<float>resonatorPitches(){return mapped?mapPitches(combMap):mapPitches(oscMap);}
 void sendVec(ofxSCSynth*s,const char*key,const std::vector<float>&v){
   for(auto*st:states)if(st->ins[0].synth==s || st->ins[1].synth==s){scSynthControlBatch batch(s,st->server);batch.set(key,v);batch.send();return;}
 }
 void randomizeMapSeedsOnGate(const std::vector<float>&);
 void handleGate(std::vector<float>v){auto toGate=[this](){++gateSends;messagesBeforeGate=states[0]->server->sent.size();};
LISTENER
 }
};
FUNCTION
int main(){
 ofxSCServer a,b;ofxSCSynth pa,pb,ca,cb,ra,rb;
 ServerState sa{&a,&pa,{{&ca},{&ra}}},sb{&b,&pb,{{&cb},{&rb}}};scPolyComb node;node.states={&sa,&sb};node.ringPitchSource.v=2;
 node.oscMap.indexSeedOnGate.v=node.combMap.indexSeedOnGate.v=true;
 node.handleGate({1});assert(randomCalls==2 && a.sent.size()==3 && b.sent.size()==3 && node.messagesBeforeGate==3);
 assert(node.oscMap.indexSeed.get()==100 && node.combMap.indexSeed.get()==100);
 // Held gates and positive-level changes never retrigger; neither do note-offs.
 node.handleGate({.5});node.handleGate({0});assert(randomCalls==2 && a.sent.size()==3);
 // An entire chord rising together chooses one seed per map, not per voice/backend.
 node.handleGate({1,1,1});assert(randomCalls==4 && a.sent.size()==6 && b.sent.size()==6);
 assert(node.oscMap.indexSeed.get()==101 && node.combMap.indexSeed.get()==101);
 // Timeline GUI delivery must not consume the edge or regenerate the scheduled seed.
 scScheduling::suppressed=true;node.handleGate({0});assert(node.previousMapGate==std::vector<float>({1,1,1}) && randomCalls==4);
 scScheduling::suppressed=false;node.handleGate({0});node.handleGate({0,1,0});assert(randomCalls==6);
 node.handleGate({1,1,0});assert(randomCalls==8); // another voice rises while one is held
 node.oscMap.indexSeedOnGate.v=node.combMap.indexSeedOnGate.v=false;
 node.handleGate({0});node.handleGate({1});assert(randomCalls==8);
 // A resonator-map follower still updates the ring when its comb is disabled.
 node.combMap.indexSeedOnGate.v=true;node.internal=node.mapped=false;
 node.handleGate({0});a.sent.clear();b.sent.clear();node.handleGate({1});
 assert(randomCalls==9 && a.sent.empty() && b.sent.empty());
 node.oscMap.indexSeedOnGate.v=true;node.combMap.indexSeedOnGate.v=false;node.internal=true;
 node.handleGate({0});a.sent.clear();b.sent.clear();node.handleGate({1});
 assert(randomCalls==10 && a.sent.size()==2 && b.sent.size()==2); // oscillator plus inherited ring map
 // Turning the option on under a held gate waits for the next rising edge.
 node.oscMap.indexSeedOnGate.v=true;node.handleGate({.7});assert(randomCalls==10);
}
'''.replace('LISTENER',listener).replace('FUNCTION',fn)
with tempfile.TemporaryDirectory()as d:
 out=pathlib.Path(d);(out/'ofxSCSynth.h').write_text(stub);(out/'test.cpp').write_text(code)
 subprocess.run(['clang++','-std=c++17','-I'+d,'-I'+str(ROOT/'src'),str(out/'test.cpp'),'-o',str(out/'test')],check=True)
 subprocess.run([str(out/'test')],check=True)
print('PASS: rising gates reseed once per map across two servers; held gates, note-offs, disabled options and suppressed timeline delivery stay stable; mapped ring follows.')
