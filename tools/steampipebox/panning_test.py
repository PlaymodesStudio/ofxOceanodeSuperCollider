#!/usr/bin/env python3
"""Check real placement math and preserve all factory presets' legacy panning."""
import json,pathlib,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[2]
s=(ROOT/'src/nodes/scSteamPipeBox.cpp').read_text();a=s.index('std::vector<float> scSteamPipeBox::panPositions()');b=s.index('\n}',a)+2;method=s[a:b]
old=subprocess.check_output(['git','show','HEAD:src/nodes/scSteamPipeBox.cpp'],cwd=ROOT,text=True)
a=old.index('        float width=1;',old.index('void scSteamPipeBox::sendSound'));b=old.index('        st.spread->set("pan"',a);legacy=old[a:b]
controls=json.loads((ROOT/'synthdefs/Defaults/SteamPipeBox/controls.json').read_text());keys={c['sc']:c['name']for c in controls}
factory=json.loads((ROOT/'synthdefs/Defaults/SteamPipeBox/factory.json').read_text())
cases=','.join('{'+str(float(p['values'][keys['stereo']]))+'f,'+str(float(p['values'][keys['pan_random']]))+'f}'for p in factory)
cpp=r'''
#include <vector>
#include <string>
#include <memory>
#include <cmath>
#include <algorithm>
#include <cassert>
#include <iostream>
template<class T>T ofClamp(T v,T a,T b){return std::clamp(v,a,b);}
template<class T>struct Param{T v;T get()const{return v;}};
float first(const std::vector<float>&v,int i,float fallback){return v.empty()?fallback:v[i%v.size()];}
struct scSteamPipeBox{
 struct Control{std::string sc;Param<float>p;};std::vector<std::shared_ptr<Control>>sound;
 Param<int>voices{6},channels{2},placement{0},speakerLayout{1};
 Param<float>spreadWidth{1},spreadCenter{0};Param<std::vector<float>>panP{{.5f}};
 std::vector<float>panPositions()const;
 float val(std::string key){for(auto&c:sound)if(c->sc==key)return c->p.get();return 0;}
};
METHOD
std::vector<float>legacy(scSteamPipeBox&box){
 struct{int builtVoices;}st{box.voices.get()};auto&sound=box.sound;
 auto soundParam=[&](std::string key){return Param<float>{box.val(key)};};
LEGACY
 return pans;
}
bool close(float a,float b){return std::abs(a-b)<1e-6;}
int main(){
 scSteamPipeBox box;auto width=std::make_shared<scSteamPipeBox::Control>();width->sc="stereo";
 auto random=std::make_shared<scSteamPipeBox::Control>();random->sc="pan_random";box.sound={width,random};
 std::vector<std::pair<float,float>>cases={CASES};
 for(auto [w,r]:cases)for(int voices=1;voices<=16;++voices){
  width->p.v=w;random->p.v=r;box.voices.v=voices;auto before=legacy(box),after=box.panPositions();
  for(int i=0;i<voices;++i)assert(before[i]==after[i]);
 }
 box.placement.v=1;box.voices.v=4;auto p=box.panPositions();assert(close(p[0],0)&&close(p[1],1.f/3)&&close(p[2],2.f/3)&&close(p[3],1));
 box.spreadWidth.v=.5;box.spreadCenter.v=.25;p=box.panPositions();assert(close(p[0],.375)&&close(p[3],.875));
 box.placement.v=2;box.voices.v=6;box.channels.v=4;box.speakerLayout.v=0;p=box.panPositions();
 for(int i=0;i<6;++i)assert(close(p[i],float(i%4)/4));
 box.speakerLayout.v=1;p=box.panPositions();for(int i=0;i<6;++i)assert(close(p[i],float(i%4)/3));
 box.channels.v=1;p=box.panPositions();for(float v:p)assert(v==.5f);
 box.placement.v=3;box.panP.v={.2f,.8f};p=box.panPositions();for(int i=0;i<6;++i)assert(close(p[i],i%2?.8f:.2f));
 std::cout<<"PASS: all "<<cases.size()<<" factory presets retain bit-identical Original placement at 1..16 voices; Spread/Center, Channels, Ring/Line, Mono and Manual placement.\n";
}
'''.replace('METHOD',method).replace('LEGACY',legacy).replace('CASES',cases)
with tempfile.TemporaryDirectory()as d:
 p=pathlib.Path(d);(p/'t.cpp').write_text(cpp)
 subprocess.run(['c++','-std=c++17',str(p/'t.cpp'),'-o',str(p/'t')],check=True)
 subprocess.run([str(p/'t')],check=True)
