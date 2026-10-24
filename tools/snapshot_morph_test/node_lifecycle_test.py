#!/usr/bin/env python3
"""Capture real ofxSuperCollider OSC during pending-node teardown (ASan/UBSan)."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import os
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
OF=ROOT.parents[1]
SC=ROOT.parent/'ofxSuperCollider'
OSC=ROOT.parent/'ofxOsc'
code=r'''
#include "ofMain.h"
#include "ofxSCServer.h"
#include "ofxSCSynth.h"
#include "ofxSCGroup.h"
#include <cassert>
#include <iostream>
int main(){
 ofInit();
 ofxSCServer server("127.0.0.1",57110,0);
 server.beginNRTCapture(true);
 for(int count:{1,4,16}){
  ofxSCGroup group(&server);group.create();
  std::vector<ofxSCSynth*>voices;
  for(int v=0;v<count;++v){
   auto*s=new ofxSCSynth("LifecycleProbe",&server);
   s->set("amp",.5f);s->set("values",std::vector<float>{.1f,.2f,.3f});
   s->create(1,group.nodeID);
   // A morph can replace a pending voice before /n_go. The values queued
   // afterwards must be discarded when its parent group is freed.
   s->set("amp",.25f);s->set("values",std::vector<float>{.7f,.8f});
   s->mapan("gate",64+v,1);voices.push_back(s);
  }
  group.free();const auto freed=server.getNRTEventCount();
  for(auto*s:voices)delete s;
  assert(server.getNRTEventCount()==freed);
 }
 server.endNRTCapture();
 std::cout<<"PASS: real OSC capture emits no queued parameter/mapping writes after group teardown at 1/4/16 voices (ASan/UBSan)\n";
}
'''
includes=[SC/'src',OSC/'src',OSC/'libs/oscpack/src']
includes+=sorted({p.parent for p in (OF/'libs/openFrameworks').rglob('*.h')})
includes+=[p for p in (OF/'libs').glob('*/include') if p.is_dir()]
includes+=[OF/'libs/freetype/include/freetype',OF/'libs/cairo/include/cairo']
includes+=sorted({p.parent for p in (OSC/'libs/oscpack/src').rglob('*.h')})
sources=[*sorted((SC/'src').glob('*.cpp')),*sorted((OSC/'src').glob('*.cpp'))]
sources += [p for p in sorted((OSC/'libs/oscpack/src').rglob('*.cpp')) if 'win32' not in str(p)]
sources += [OF/'libs/openFrameworks/utils/ofUtils.cpp']
flags=['clang++','-std=c++20','-O0','-g','-w','-fsanitize=address,undefined','-fno-omit-frame-pointer','-DOF_NO_FMOD',*['-I'+str(p) for p in includes]]
with tempfile.TemporaryDirectory(prefix='box-node-lifecycle-') as directory:
    work=Path(directory);cpp=work/'test.cpp';cpp.write_text(code)
    sources=[cpp,*sources]
    objects=[work/(str(i)+'.o') for i in range(len(sources))]
    def compile_unit(pair):
        source,obj=pair
        subprocess.run(flags+['-c',str(source),'-o',str(obj)],check=True)
    with ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(compile_unit,zip(sources,objects)))
    libdir=OF/'libs/openFrameworksCompiled/lib/macos'
    libraries=[str(p) for p in sorted(libdir.glob('*.a')) if 'openframeworks' not in p.name.lower()]
    link=['clang++','-fsanitize=address,undefined',*[str(p) for p in objects],str(libdir/'openFrameworks.a'),*libraries,'-Wl,-dead_strip']
    for framework in ['Accelerate','AppKit','ApplicationServices','AudioToolbox','AVFoundation','Cocoa','CoreAudio','CoreFoundation','CoreMedia','CoreServices','CoreVideo','IOKit','OpenGL','QuartzCore','Security','SystemConfiguration']:
        link+=['-framework',framework]
    binary=work/'test'
    subprocess.run(link+['-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
