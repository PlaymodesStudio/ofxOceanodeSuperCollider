#!/usr/bin/env python3
"""Production comb controls: shared oscillator map + coarse/fine offsets."""
import pathlib,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[2]
s=(ROOT/'src/nodes/scPolyComb.cpp').read_text();a=s.index('        case IComb:',s.index('void scPolyComb::sendIns'));b=s.index('        case IRing:',a);branch=s[a:b]
code=r'''
#include <vector>
#include <map>
#include <string>
#include <cassert>
#include <cmath>
template<class T>struct P{T value{};T get()const{return value;}};
struct Synth{std::map<std::string,std::vector<float>>v;void set(std::string key,float value){v[key]={value};}};
struct ServerState{};
struct scPolyComb{
 enum{IComb};bool own=false,internal=true;P<int>combMode;
 struct Map{P<float>transpose,offset;}combMap;
 P<float>combSatLevel,combTone,combGlide,combVibRate,combVibRand,combVibAmt;P<std::vector<float>>combPitch,combTranspose,combDetune,combFeed,combMix;
 Synth storage;bool combMapped(){return own;}bool internalPitch(){return internal;}
 std::vector<float>resonatorPitches(){return {48.137f,52.137f,55.137f};}
 int mod(ServerState&,int){return 0;}void sendVec(Synth*s,std::string key,const std::vector<float>&v){s->v[key]=v;}
 void send(){auto*s=&storage;ServerState st;switch(IComb){
BRANCH
 }}
};
int main(){
 scPolyComb n;n.combMode.value=1;n.combPitch.value={72};n.combMap.transpose.value=7;n.combMap.offset.value=.137f;
 n.combGlide.value=.75f;n.combVibRate.value=6;n.combVibRand.value=.25f;n.combVibAmt.value=.5f;
 n.send();assert(n.storage.v["combmode"][0]==0);
 assert(n.storage.v["combglide"][0]==.75f); // the resonators' own glide
 assert(n.storage.v["combvibrate"][0]==6 && n.storage.v["combvibrand"][0]==.25f && n.storage.v["combvibamt"][0]==.5f);
 assert(std::abs(n.storage.v["combpitch"][0]-7.137f)<1e-5); // shared source bus + offsets
 n.own=true;n.send();assert(n.storage.v["combmode"][0]==1 && n.storage.v["combpitch"]==n.resonatorPitches());
 n.own=false;n.internal=false;n.send();assert(n.storage.v["combmode"][0]==1);
 assert(std::abs(n.storage.v["combpitch"][0]-79.137f)<1e-5); // legacy external fixed note still works
 n.combMode.value=0;n.combPitch.value={-12};n.send();assert(n.storage.v["combmode"][0]==0);
 assert(std::abs(n.storage.v["combpitch"][0]+4.863f)<1e-5);
}
'''.replace('BRANCH',branch)
with tempfile.TemporaryDirectory()as d:
 c=pathlib.Path(d)/'test.cpp';c.write_text(code);exe=c.with_suffix('')
 subprocess.run(['clang++','-std=c++17',str(c),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
print('PASS: shared resonators follow oscillator bus with coarse/fine offsets; own-map tuning and legacy external pitch modes remain wired correctly.')
