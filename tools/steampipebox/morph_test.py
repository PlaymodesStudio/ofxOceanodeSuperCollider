#!/usr/bin/env python3
"""Check fractional allpass/noise-filter activation in the production native kernel."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('generate_ugen', ROOT/'tools/steampipebox/generate_ugen.py')
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)
code, _ = gen.translate(ROOT/'synthdefs/Defaults/SteamPipeBox/steamPipeBox.eel2', 'PipeKernel')
cpp = r'''
#include <cmath>
#include <algorithm>
#include <vector>
#include <cassert>
#include <iostream>
#include "PrimaryCompat.h"
#include "FeedbackMorph.h"
inline double clip(double x,double a,double b){return std::clamp(x,a,b);}
inline double min(double a,double b){return std::min(a,b);}
inline double max(double a,double b){return std::max(a,b);}
inline double mod(double a,double b){return std::fmod(a,b);}
using std::floor;using std::abs;using std::pow;using std::exp;using std::sin;using std::atan;
'''+code+r'''
int main(){
 double maxError=0,allpassDifference=0,noiseDifference=0;
 for(double t:{0.,.001,.1,.499,.5,.501,.9,1.}){
  PipeKernel k;std::vector<double>b(98304);k.buf=b.data();k.init();
  k._pitch=60;k._gate=1;k._velocity=1;k._amp_original=1;
  k._attack=25;k._decay=30;k._sustain=.7;k._release=30;
  k._dc_noise=1;k._noise_cut=90;k._noise_resonance=.3;
  k._allpass_tune=12;k._diffusion=.6;k._push=1.2;k._polarity=1;
  k._offset=.8;k._feedback_rt=-30;k._hp0=20;k._lp0=110;
  k._allpass_on=t;k._noise_one_pole=t;
  for(int i=0;i<16000;++i){
   k.sample();
   assert(std::isfinite(k.out0));
   const double ap=k.pipe_delayed*(1-t)+k.ap_output*t;
   const double noise=k.noise_lp2*(1-t)+k.noise_lp*t;
   maxError=std::max(maxError,std::abs(k.resonator_signal-ap));
   maxError=std::max(maxError,std::abs(k.filtered_noise-noise));
   allpassDifference=std::max(allpassDifference,std::abs(k.ap_output-k.pipe_delayed));
   noiseDifference=std::max(noiseDifference,std::abs(k.noise_lp-k.noise_lp2));
  }
 }
 // Outside-loop blending must equal two independent feedback states,
 // rather than interpolating the delay tap inside a shared feedback loop.
 for(double t:{.001,.1,.499,.501,.9,.999}){
  PipeKernel a,b,dry,wet;
  std::vector<double> ab(98304),bb(98304),db(98304),wb(98304);
  a.buf=ab.data();b.buf=bb.data();dry.buf=db.data();wet.buf=wb.data();
  a.init();a._pitch=60;a._gate=1;a._velocity=1;a._amp_original=1;
  a._attack=25;a._decay=30;a._sustain=.7;a._noise_cut=90;a._dc_noise=1;
  a._push=1.2;a._offset=.8;a._polarity=1;a._feedback_rt=-30;
  a._allpass_tune=12;a._diffusion=.6;a._hp0=20;a._lp0=110;
  auto copy=[&](PipeKernel& k){auto* buffer=k.buf;k=a;k.buf=buffer;};
  copy(dry);copy(wet);dry._allpass_on=0;wet._allpass_on=1;
  bool active=false;int mode=0;
  for(int i=0;i<16000;++i){
   dry.sample();wet.sample();a._allpass_on=t;
   sampleAllpassMorph(a,b,active,mode,98304);
   assert(active);
   assert(std::abs(a.out0-((1-t)*dry.out0+t*wet.out0))<1e-12);
  }
  // Reaching either endpoint keeps its already-running state, then stops
  // processing the second loop. Reverse paths retain the matching state too.
  a._allpass_on=1;wet.sample();sampleAllpassMorph(a,b,active,mode,98304);
  assert(!active && mode==1 && a.out0==wet.out0);
  const auto parked=b.main_w;
  a._allpass_on=1;sampleAllpassMorph(a,b,active,mode,98304);
  assert(!active && b.main_w==parked);
  a._allpass_on=.4;sampleAllpassMorph(a,b,active,mode,98304);assert(active && mode==1);
  a._allpass_on=0;sampleAllpassMorph(a,b,active,mode,98304);assert(!active && mode==0);
 }
 assert(maxError<1e-12);
 assert(allpassDifference>1e-4 && noiseDifference>1e-4);
 std::cout<<"PASS: independent allpass feedback states and noise-filter blends start at .001, remain fractional across .5, and retain exact endpoints; max error "<<maxError<<"\n";
}
'''
with tempfile.TemporaryDirectory(prefix='steam-morph-') as work:
    work = Path(work)
    (work/'test.cpp').write_text(cpp)
    subprocess.run(['clang++','-std=c++17','-O2','-ffp-contract=off',
                    '-I'+str(ROOT/'ugens/OceanodeSteamPipe'),str(work/'test.cpp'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True)
