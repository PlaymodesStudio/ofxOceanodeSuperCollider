#!/usr/bin/env python3
"""Compare canonical native ADSR and filter math against captured NI engine outputs.
No Reaktor runtime or audio device is needed; fixture hashes cover all 60,000 samples.
"""
import importlib.util,pathlib,subprocess,ctypes,json
root=pathlib.Path(__file__).resolve().parents[2];spec=importlib.util.spec_from_file_location('gen',root/'tools/steampipebox/generate_ugen.py');gen=importlib.util.module_from_spec(spec);spec.loader.exec_module(gen)
code,params=gen.translate(root/'synthdefs/Defaults/SteamPipeBox/steamPipeBox.eel2','PipeKernel')
s='''#include <cmath>
#include <algorithm>
#include <vector>
#include "PrimaryCompat.h"
inline double clip(double x,double a,double b){return std::clamp(x,a,b);}
inline double min(double a,double b){return std::min(a,b);}
inline double max(double a,double b){return std::max(a,b);}
inline double mod(double a,double b){return std::fmod(a,b);}
using std::floor; using std::abs; using std::pow; using std::exp; using std::sin;
'''+code+'''
extern "C" void envcase(float pitch,float* output) {
 PipeKernel k;std::vector<double> b(98304);k.buf=b.data();k.init();k._pitch=pitch;
 k._amp_original=1;k._attack=40;k._decay=34;k._release=40;k._sustain=.3f;k._env_key_scaling=-.5;
 k._velocity_amount=1;k._velocity=.8f;
 for(int i=0;i<20000;++i) { k._gate=(i<4800 || (i>=9600 && i<14400))?1:0;k._velocity=i<9600?.8f:.4f;k.sample();output[i]=k.out1; }
}
extern "C" void filtercase(float pitch,float res,float* output) {output[0]=niFilterF(pitch,res,48000);output[1]=niFilterD(pitch,res,48000);}
'''
pathlib.Path('/tmp/steam-compare.cpp').write_text(s)
subprocess.run(['clang++','-std=c++17','-O2','-ffp-contract=off','-shared','-I'+str(root/'ugens/OceanodeSteamPipe'),'/tmp/steam-compare.cpp','-o','/tmp/steam-compare.dylib'],check=True)
lib=ctypes.CDLL('/tmp/steam-compare.dylib');lib.envcase.argtypes=[ctypes.c_float,ctypes.POINTER(ctypes.c_float)];lib.filtercase.argtypes=[ctypes.c_float,ctypes.c_float,ctypes.POINTER(ctypes.c_float)]
import hashlib,struct
data=json.loads((root/'tools/steampipebox/primary-reference.json').read_text());results=[]
for case in data['envelopes']:
 out=(ctypes.c_float*20000)();lib.envcase(case['pitch'],out)
 digest=hashlib.sha256(bytes(out)).hexdigest()
 assert digest==case['sha256'],(case['pitch'],digest,case['sha256'])
 results.append(dict(pitch=case['pitch'],samples=len(out),matches_original=True))
filters=[]
for c in data['filter_coefficients']:
 out=(ctypes.c_float*2)();lib.filtercase(c['pitch'],c['resonance'],out)
 filters.append(max(abs(out[0]-c['f']),abs(out[1]-c['d'])))
assert max(filters)==0,filters
print(json.dumps(dict(envelopes=results,filter_max_error=max(filters)),indent=2))
