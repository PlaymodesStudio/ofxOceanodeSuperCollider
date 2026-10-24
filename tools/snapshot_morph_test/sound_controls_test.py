#!/usr/bin/env python3
"""Execute production policy registration and DSP-send loops for numeric switches."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def send_loop(source, name):
    start = source.index('for(const auto& c : sound)', source.index('void '+name+'::sendSound'))
    semicolon = source.index(';', start)
    brace = source.find('{', start, semicolon)
    if brace < 0:
        return source[start:semicolon+1]
    depth, end = 1, brace+1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


sources = {}
for folder, name in [('steamPipeBox', 'scSteamPipeBox'), ('aliasingBox', 'scAliasingBox'), ('fmBox', 'scFMBox')]:
    source = (ROOT/'src/nodes/boxSynths'/folder/(name+'.cpp')).read_text()
    anchor = source.index('snaps.morph.bypass(*p)')
    start = source.index('for(const auto& c : sound)', anchor)
    stop = source.index('snaps.setup(sh)', start) if folder == 'fmBox' else source.index('sh.morphChanged', start)
    extra = ''
    if folder == 'aliasingBox':
        a = source.index('int morphModeCount(')
        extra = source[a:source.index('\n}', a)+2]
    sources[folder] = extra + '\nvoid setup(){'+source[start:stop]+'}\nvoid send(){\n'+send_loop(source,name)+'\n}\n'

cpp = r'''
#include "nodes/boxSynths/shared/scMorphPolicies.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>
std::string ofToString(size_t k){return std::to_string(k);}
struct Sound { ofParameter<float> p; std::string sc; Sound(std::string key,float v):p(key,v),sc(key){} };
struct Probe {
 struct {scMorphPolicies morph;} snaps;
 struct {float offset=0;float voiceValue(ofParameter<float>& p,int){return p.get()+offset;}
 float morphVoiceValue(ofParameter<float>& p,int,const scMorphPolicies& m){return m.value(p)+offset;}} mod;
 std::vector<std::unique_ptr<Sound>> sound;
 std::map<std::string,float> output;
 std::vector<float> morphControls=std::vector<float>(93,0);
 int i=0;
 void set(const std::string& name,float value){output[name]=value;}
 void setVector(const std::string& name,const std::vector<float>& values){for(size_t k=0;k<values.size();++k)output[name+"_"+std::to_string(k)]=values[k];}
 void add(std::string key,float value){sound.push_back(std::make_unique<Sound>(key,value));}
 void near(std::string key,float value){assert(output.count(key));assert(std::abs(output.at(key)-value)<1e-5f);}
};
'''
for folder, methods in sources.items():
    cpp += 'struct '+folder+' : Probe {\n'+methods+'\n};\n'
cpp += r'''
int main(){
 steamPipeBox steam;
 for(auto key:{"allpass_on","noise_one_pole","polarity"})steam.add(key,0);
 steam.setup();
 aliasingBox alias;
 for(auto key:{"filter_on","notch_on","dc_block","read_bidirectional","read_quality","write_quality","filter_lfo_wave"})alias.add(key,0);
 alias.setup();
 fmBox fm;
 for(auto key:{"env_model","matrix_model","osc_sync","op0_mode","op5_mode","algorithm","op0_left_curve","op5_right_curve","op0_coarse","op5_coarse"})fm.add(key,0);
 fm.setup();
 for(float t:{0.f,.001f,.2f,.499f,.5f,.501f,.9f,1.f}){
  auto exercise=[&](auto& box){
   ofJson a=ofJson::object(),b=ofJson::object();
   for(auto& c:box.sound){a[c->sc]=c->sc=="polarity"?-1:0;b[c->sc]=c->sc=="write_quality"?2:c->sc=="filter_lfo_wave"?7:1;}
   box.snaps.morph.begin(a,b,t);
   for(auto& c:box.sound){c->p.set((t<.5f?a:b)[c->sc].template get<float>());box.snaps.morph.edited(c->p);}
   box.send();box.snaps.morph.end();
  };
  exercise(steam);exercise(alias);exercise(fm);
  for(int offset:{1,33,77}){assert(std::abs(fm.morphControls[offset]-(1-t))<1e-5);assert(std::abs(fm.morphControls[offset+1]-t)<1e-5);}
  steam.near("allpass_on",t);steam.near("noise_one_pole",t);steam.near("polarity",2*t-1);
  for(auto key:{"filter_on","notch_on","dc_block","read_bidirectional"})alias.near(key,t);
  for(auto key:{"read_quality","write_quality","filter_lfo_wave"}){
   const int n=alias.morphModeCount(key);
   alias.near(std::string(key)+"_w0",1-t);alias.near(std::string(key)+"_w"+std::to_string(n-1),t);
   for(int k=1;k<n-1;++k)alias.near(std::string(key)+"_w"+std::to_string(k),0);
  }
  for(auto key:{"env_model","matrix_model","osc_sync","op0_mode","op5_mode"})fm.near(key,t);
 }
 for(float t:{0.f,.001f,.499f,.501f,1.f}){
  fm.snaps.morph.begin({{"op0_coarse",0},{"op5_coarse",1}},{{"op0_coarse",4},{"op5_coarse",3}},t);
  fm.send();fm.snaps.morph.end();
  assert(std::abs(fm.morphControls[81]-4*t)<1e-5);assert(fm.morphControls[82]==1);
  assert(std::abs(fm.morphControls[92]-(10*(1-t)+1000*t))<.001);
 }
 fm.snaps.morph.clear();for(auto& c:fm.sound)c->p.set(0);fm.mod.offset=1;fm.send();
 for(int offset:{1,33,77})assert(fm.morphControls[offset+1]==1);
 std::cout<<"PASS: production SteamPipe/Aliasing/FM registration and senders resolve fractional audio controls across the midpoint\n";
}
'''
with tempfile.TemporaryDirectory(prefix='box-sound-controls-') as work:
    work = Path(work)
    (work/'test.cpp').write_text(cpp)
    subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(HERE/'stubs'),
                    '-I'+str(ROOT/'src'),'-I'+str(ROOT.parents[1]/'libs/json/include'),
                    str(work/'test.cpp'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True)
