#!/usr/bin/env python3
"""V1 bit identity and V2 DSP feature regressions against captured/source-derived references."""
import json,pathlib,importlib.util,subprocess,ctypes,hashlib
root=pathlib.Path(__file__).resolve().parents[2];sp=importlib.util.spec_from_file_location('gen',root/'tools/steampipebox/generate_ugen.py');g=importlib.util.module_from_spec(sp);sp.loader.exec_module(g)
code,params=g.translate(root/'synthdefs/Defaults/SteamPipeBox/steamPipeBox.eel2','PipeKernel')
pre='''#include <cmath>
#include <algorithm>
#include <vector>
#include "PrimaryCompat.h"
inline double clip(double x,double a,double b){return std::clamp(x,a,b);}
inline double min(double a,double b){return std::min(a,b);}
inline double max(double a,double b){return std::max(a,b);}
inline double mod(double a,double b){return std::fmod(a,b);}
using std::floor;using std::abs;using std::pow;using std::exp;using std::sin;using std::atan;
'''
main='\nextern "C" void render(const float* p,float* out){PipeKernel k;std::vector<double>b(98304);k.buf=b.data();k.init();'
for i,p in enumerate(params):main+=f'k.{p}=p[{i}];'
main+='''for(int i=0;i<32000;++i){k._gate=i<12000 ? (i<2000?.3:1) : i<16000?0:1;k._velocity=i<16000?.8:.4;k._pitch=i<20000?60:67;k.sample();out[i]=k.out0;}}'''
probe=r'''
extern "C" void probe(float* out){
 PipeKernel k;std::vector<double>b(98304);k.buf=b.data();k.init();
 for(int i=0;i<8;++i)b[100-i]=i*i;
 out[0]=k.readH(0,49152,100,2.25);out[1]=k.readL(0,49152,100,2.25);
 for(int i=1;i<8;++i)b[49152-i]=i*i;
 out[2]=k.readH(0,49152,0,2.25);
 k._pitch=60;k._hp0=60;k._lp0=60;k._amp_original=1;k.sample();out[3]=k.main_delay;
 k._steampipe2=1;k.sample();out[4]=k.main_delay;
 for(int mode=0;mode<2;++mode){
  PipeKernel a;std::vector<double>ab(98304);a.buf=ab.data();a.init();
  a._amp_original=1;a._legato=mode;a._velocity_amount=1;a._attack=40;
  a._gate=.3;a._velocity=.3;
  for(int i=0;i<200;++i)a.sample();
  a._gate=1;a._velocity=.9;a.sample();out[5+mode]=a.held_velocity;
  a._gate=0;a.sample();a._gate=1;a.sample();out[7+mode]=a.held_velocity;
 }
 k._gate=1;k._velocity=1;k._attack=25;k._noise_cut=60;k._noise_env=24;k.sample();
 out[9]=k.env;out[10]=k.noise_pitch;
}
'''
pathlib.Path('/tmp/steam-v2-features.cpp').write_text(pre+code+main+probe)
subprocess.run(['clang++','-std=c++17','-O2','-ffp-contract=off','-shared','-I'+str(root/'ugens/OceanodeSteamPipe'),'/tmp/steam-v2-features.cpp','-o','/tmp/steam-v2-features.dylib'],check=True)
lib=ctypes.CDLL('/tmp/steam-v2-features.dylib');lib.render.argtypes=[ctypes.POINTER(ctypes.c_float),ctypes.POINTER(ctypes.c_float)]
factory=json.loads((root/'synthdefs/Defaults/SteamPipeBox/factory.json').read_text())[:172];controls=json.loads((root/'synthdefs/Defaults/SteamPipeBox/controls.json').read_text());results=[]
for x in factory:
 values={'_'+c['sc']:x['values'][c['name']] for c in controls};values.update(_amp_original=1,_amp_curve0=-4,_amp_curve1=-4,_amp_curve2=-4)
 p=(ctypes.c_float*len(params))(*(values.get(k,0) for k in params));out=(ctypes.c_float*32000)();lib.render(p,out)
 results.append({'name':x['name'],'sha256':hashlib.sha256(bytes(out)).hexdigest()})
expected=json.loads((root/'tools/steampipebox/v1-voice-reference.json').read_text())['cases']
assert results==expected,[(a['name'],a['sha256'],b['sha256']) for a,b in zip(results,expected) if a!=b][:5]
print('PASS: all 172 original-mode voice outputs are bit-identical')

lib.probe.argtypes=[ctypes.POINTER(ctypes.c_float)]
out=(ctypes.c_float*11)();lib.probe(out)
assert abs(out[0]-5.0625)<1e-6 and abs(out[1]-5.25)<1e-6
assert out[0]==out[2], 'Ring-buffer wrap changed cubic interpolation'
assert abs(out[4]/out[3]-(1+0.3330000042915344*.125-.5*.125))<1e-6
assert abs(out[5]-.9)<1e-6 and abs(out[6]-.3)<1e-6, 'Legato retrigger / velocity latch'
assert abs(out[7]-.9)<1e-6 and abs(out[8]-.9)<1e-6, 'Fresh gates must retrigger'
assert abs(out[10]-(60+24*out[9]))<1e-5, 'Exciter envelope cutoff depth'
print('PASS: four-point interpolation including buffer wrap, audited HP/LP compensation, legato/fresh gates and noise-filter envelope amount')
