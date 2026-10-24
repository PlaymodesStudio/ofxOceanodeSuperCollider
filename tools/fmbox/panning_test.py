#!/usr/bin/env python3
"""Check fmBox's production speaker placement equations without an audio device."""
import pathlib,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[2]
s=(ROOT/'src/nodes/boxSynths/fmBox/scFMBox.cpp').read_text()
a=s.index('std::vector<float> expandPan(');b=s.index('\n}',a)+2;expand=s[a:b]
a=s.index('std::vector<float> scFMBox::panPositions() const');b=s.index('\n}',a)+2;positions=s[a:b]
cpp=r'''
#include <vector>
#include <string>
#include <memory>
#include <algorithm>
#include <cmath>
#include <cassert>
#include <iostream>
template<class T>struct Param{T value{};T get()const{return value;}void set(T v){value=v;}};
EXPAND
struct scFMBox{
 Param<int>voices,channels,spreadMode,spreadLayout;
 Param<float>spreadCenter;Param<std::vector<float>>panP;
 struct Control{std::string sc;Param<float>p;};std::vector<std::unique_ptr<Control>>sound;
 std::vector<float>panPositions()const;
};
POSITIONS
int main(){
 scFMBox box;auto width=std::make_unique<scFMBox::Control>();width->sc="stereo";box.sound.push_back(std::move(width));
 box.voices.set(4);box.channels.set(2);
 auto check=[&](std::vector<float>expected){auto p=box.panPositions();assert(p.size()==expected.size());for(int i=0;i<int(p.size());++i)assert(std::abs(p[i]-expected[i])<1e-6);};
 check({.5,.5,.5,.5}); // centered default
 box.sound[0]->p.set(1);check({0,1.f/3,2.f/3,1});
 box.spreadCenter.set(.4);check({.2,1.f/3+.2,2.f/3+.2,1.2}); // spreader clamps/wraps positions
 box.spreadCenter.set(0);box.spreadMode.set(1);check({0,1,0,1});
 box.channels.set(4);check({0,.25,.5,.75}); // ring channel addressing
 box.spreadMode.set(0);check({0,.25,.5,.75}); // no duplicate speaker at ring endpoints
 box.spreadLayout.set(1);check({0,1.f/3,2.f/3,1});
 box.spreadMode.set(2);box.panP.set({.2,.9});check({.2,.9,.2,.9});
 box.panP.set({});check({.5,.5,.5,.5});
 box.voices.set(1);box.spreadMode.set(0);box.channels.set(2);check({.5});
 box.channels.set(1);box.spreadMode.set(1);check({.5});
 std::cout<<"PASS: centered stereo, spread/center, discrete channels, ring/line, manual vectors, one voice and mono output\n";
}
'''.replace('EXPAND',expand).replace('POSITIONS',positions)
with tempfile.TemporaryDirectory(prefix='fmbox-pan-')as folder:
    p=pathlib.Path(folder);(p/'pan.cpp').write_text(cpp)
    subprocess.run(['clang++','-std=c++17','-fsanitize=address,undefined',str(p/'pan.cpp'),'-o',str(p/'pan')],check=True)
    subprocess.run([str(p/'pan')],check=True)
