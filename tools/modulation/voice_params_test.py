#!/usr/bin/env python3
"""Exercise production scalar/vector routing, base preservation and voice updates."""
import pathlib, subprocess, tempfile
ROOT=pathlib.Path(__file__).resolve().parents[2]
s=(ROOT/'src/nodes/scModulation.cpp').read_text()
names=['readParam','writeParam','voiceValue','voiceValues','paramSource','applyParams','removeParam','patchBases']
methods=[]
for name in names:
 a=s.index('scModulation::'+name+'(');a=s.rfind('\n',0,a)+1;b=s.index('\n}',a)+2;methods.append(s[a:b])
cpp=r'''
#include <nlohmann/json.hpp>
#include <vector>
#include <array>
#include <set>
#include <string>
#include <functional>
#include <cmath>
#include <algorithm>
#include <cassert>
#include <iostream>
using ofJson=nlohmann::json;
template<class T>T ofClamp(T x,T a,T b){return std::clamp(x,a,b);}
template<class T>struct ofParameter {T v;std::string key;std::function<void()> changed;T get()const{return v;}void set(T x){v=x;if(changed)changed();}std::string getEscapedName()const{return key;}};
struct Abstract {void* ptr;template<class T>ofParameter<T>&cast(){return *static_cast<ofParameter<T>*>(ptr);}};
float decorrelate(float v,float salt){return std::sin(v+salt);}
struct scModulation{
 static constexpr int MAX_LFOS=8,SRC_RND=13,SRC_EXT=14,SRC_VOICE=22,NUM_SOURCES=23,MAX_SOURCES=27;
 struct ParamTarget {std::string key;Abstract*p=nullptr;int kind=0;float lo=0,hi=1;std::vector<float>base,written,voiceWritten;std::array<float,MAX_SOURCES>amount{};bool applied=false;};
 struct Host{std::function<void()>voiceParamsChanged;}host;
 std::vector<ParamTarget>params;std::set<std::string>voiceParamKeys;
 bool perVoice=true,randomShared=true;int voices=4;bool active=true;
 std::array<ofParameter<std::vector<float>>,8>exts;
 std::vector<float>lfo={.1f,.3f,.7f,.9f};
 int n()const{return voices;}int sourceCount()const{return NUM_SOURCES;}bool sourceUsed(int s)const{return s==0&&active;}
 float liveValue(int,int v)const{return lfo[v];}void applyParamBases(){}
 std::vector<float>readParam(const ParamTarget&)const;void writeParam(ParamTarget&,const std::vector<float>&);
 float voiceValue(const ofParameter<float>&,int)const;std::vector<float>voiceValues(const ofParameter<float>&)const;
 float paramSource(int,int)const;void applyParams();void removeParam(size_t);void patchBases(ofJson&)const;
};
METHODS
bool near(float a,float b){return std::abs(a-b)<1e-5;}
int main(){
 scModulation m;ofParameter<float>p{.5f,"DC_Noise"};Abstract a{&p};scModulation::ParamTarget t;t.p=&a;t.key=p.key;t.amount[0]=1;m.params={t};m.voiceParamKeys.insert(p.key);
 int sends=0;m.host.voiceParamsChanged=[&]{++sends;};
 int scalarWrites=0;p.changed=[&]{++scalarWrites;};
 m.applyParams();assert(sends==1);assert(scalarWrites==0 && near(p.get(),.5));for(int i=0;i<4;++i)assert(near(m.voiceValue(p,i),m.lfo[i]));
 auto j=ofJson{{p.key,p.get()}};m.patchBases(j);assert(near(j[p.key],.5));
 m.lfo[2]=.6;m.applyParams();assert(sends==2);assert(near(m.voiceValue(p,2),.6)); // first voice unchanged
 p.changed={};p.set(.6);assert(near(m.voiceValue(p,2),.6)); // external edits never read stale overrides
 m.applyParams();assert(near(m.voiceValue(p,2),.7));
 m.voices=2;m.applyParams();assert(m.voiceValues(p).size()==2);m.voices=4;m.applyParams();assert(near(m.voiceValue(p,3),1));
 m.active=false;m.applyParams();assert(near(p.get(),.6));for(float v:m.voiceValues(p))assert(near(v,.6));
 m.active=true;m.applyParams();m.removeParam(0);for(float v:m.voiceValues(p))assert(near(v,.6));
 // A truly shared scalar intentionally uses the first source, without per-voice overrides.
 p.set(.5);m.params={t};m.voiceParamKeys.clear();m.applyParams();assert(near(p.get(),.1));for(float v:m.voiceValues(p))assert(near(v,.1));
 // A vector with one base value expands over voices as before.
 ofParameter<std::vector<float>>vp{{.5f},"Vector"};Abstract va{&vp};t.p=&va;t.key=vp.key;t.kind=2;m.params={t};m.applyParams();assert(vp.v.size()==4);assert(near(vp.v[2],.6));
 m.perVoice=false;m.applyParams();for(float v:vp.v)assert(near(v,.1));
 std::cout<<"PASS: per-voice scalar routing, secondary-only changes, stable scalar base, saved bases, edits, resize, disable/remove, shared controls and vector routing.\n";
}
'''.replace('METHODS','\n'.join(methods))
with tempfile.TemporaryDirectory()as d:
 p=pathlib.Path(d);(p/'t.cpp').write_text(cpp)
 subprocess.run(['c++','-std=c++17','-I'+str(ROOT.parent.parent/'libs/json/include'),str(p/'t.cpp'),'-o',str(p/'t')],check=True)
 subprocess.run([str(p/'t')],check=True)
