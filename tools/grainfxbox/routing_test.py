#!/usr/bin/env python3
"""Render shared Ramp -> Pos trajectories, easing and converted factory effects."""
import argparse,importlib.util,json,pathlib,subprocess,sys
import numpy as np
ROOT=pathlib.Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('grain_render',ROOT/'tools/grainfxbox/render_test.py')
r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r)
ap=argparse.ArgumentParser();ap.add_argument('--output',type=pathlib.Path,default=pathlib.Path('/tmp/grainfxbox-ramp-routing'));work=ap.parse_args().output;work.mkdir(parents=True,exist_ok=True)
source=(ROOT/'src/nodes/boxSynths/grainBoxPoly/scGrainBoxPoly.cpp').read_text()
factory=source[source.index('std::vector<ofJson> scGrainBoxPoly::factorySnapshots'):]
bank=json.loads(factory.split('R"JSON(')[2].split(')JSON"')[0])
(work/'raw.json').write_text(json.dumps(bank))
(work/'ofMain.h').write_text('#include <nlohmann/json.hpp>\nusing ofJson=nlohmann::json;\n')
(work/'migration.cpp').write_text(r'''
#include "scGrainStretchRouting.h"
#include <fstream>
#include <iostream>
#include <cassert>
int main(int argc,char**argv){
 ofJson raw;std::ifstream(argv[1])>>raw;ofJson result=ofJson::array();
 for(const auto& preset:raw){
  ofJson sound={{"name",preset["name"]}};
  if(preset.contains("set"))for(auto it=preset["set"].begin();it!=preset["set"].end();++it){
   auto key=it.key();std::replace(key.begin(),key.end(),' ','_');sound[key]=it.value();
  }
  auto routed=scGrainStretchRouting(sound,23,7);
  if(sound.contains("Motion")){
   assert(!routed.contains("Motion"));
   if(sound["Motion"]==true){
    assert(routed["gbpModLfos"]==1 && routed["LFO1_Ramp"]==true);
    assert(scGrainStretchRouting(routed,23,7)==routed);
    auto stored=sound;
    for(auto it=stored.begin();it!=stored.end();++it)if(it.value().is_number()||it.value().is_boolean())it.value()=it.value().dump();
    assert(scGrainStretchRouting(stored,23,7)["LFO1_Ramp_From"]==routed["LFO1_Ramp_From"]);
    auto edited=sound;edited["gbpModMatrix"]=std::vector<float>(161,0);edited["gbpModMatrix"][1]=0.3;
    auto result=scGrainStretchRouting(edited,23,7);assert(result["gbpModMatrix"][1]==0.3f && result["LFO2_Ramp"]==true);
   }
  }
  result.push_back(routed);
 }
 std::cout<<result.dump();
}
''')
subprocess.run(['clang++','-std=c++17','-I'+str(work),'-I'+str(ROOT/'src/nodes/boxSynths/grainBoxPoly'),
                '-I'+str(ROOT.parent.parent/'libs/json/include'),str(work/'migration.cpp'),'-o',str(work/'migration')],check=True)
bank={p['name']:p for p in json.loads(subprocess.check_output([str(work/'migration'),str(work/'raw.json')]))}
print('PASS: production preset migration, idempotent recall and preserved custom routes',flush=True)
defs=r.compile_defs(work)
for name in ['modulation.scd','modulationRandom.scd','modulationRamp.scd']:
 s=(ROOT/'synthdefs/Defaults/Modulation'/name).read_text().replace('(1..32).do','(1..2).do')
 (work/name).write_text(s)
probe=r'''SynthDef(\GBTestRouteProbe,{|spansec=1|Out.ar(0,K2A.ar([In.kr(800)*spansec,In.kr(700)]))}).writeDefFile("WORK");'''.replace('WORK',str(work))
(work/'shared.scd').write_text('"'+str(work/'modulation.scd')+'".load;\n"'+str(work/'modulationRandom.scd')+'".load;\n"'+str(work/'modulationRamp.scd')+'".load;\n'+probe+'\n"COMPILED".postln;0.exit;\n')
x=subprocess.run([str(r.SCLANG),'-l',str(work/'config.yaml'),str(work/'shared.scd')],capture_output=True,text=True,timeout=120,
                 env={'HOME':str(work/'home'),'PATH':'/usr/bin:/bin'},stdin=subprocess.DEVNULL)
assert 'COMPILED' in x.stdout and 'ERROR' not in x.stdout,x.stdout[-6000:]
defs+=list((work/'CompiledSynthdefs/Modulation').glob('*.scsyndef'))+[work/'GBTestRouteProbe.scsyndef']
# Measure each shared route against the equation recovered from the ensemble.
for ref in json.loads((ROOT/'tools/grainfxbox/grain_stretch_reference.json').read_text())['snapshots']:
 name='GrainStretch %02d - %s'%(ref['slot'],ref['name']);p=bank[name]
 assert 'Motion' not in p and p['LFO1_Ramp'] and p['gbpModLfos']==1
 window=ref['beats_back']/2
 audio=r.render(work,defs,'route-%02d'%ref['slot'],'GBTestRouteProbe',[],['dry',0.0],
                [(1.5,1),(10.0,0),(10.2,1)],11.0,buffer_seconds=12,lfo=p)
 errors=[]
 for t in np.arange(1.6,9.9,0.071):
  elapsed=(t-1.5)%window;expected=(elapsed*ref['speed_a'])%window
  if min(elapsed,window-elapsed,expected,window-expected)<0.04 and ref['speed_a']:continue
  errors.append(abs(audio[int(t*r.SR),0]-expected))
 assert max(errors)<0.03,(name,max(errors))
 assert abs(audio[int(10.1*r.SR),0])<1e-6
 print('PASS: '+name+' uses shared Ramp/route, matches reference, stops and retriggers',flush=True)
 # Real grains with the same route; validate output and unchanged independent pitch.
 ctl=['autotrig',1.0,'autotrigbeatdiv',1/p['Interval'],'trigdur',1.0,'overlap',1.0,
      'graintail',p['Grain_Tail']/1000,'envbuf',2.0,'pitch',p['Pitch'],'position',0.0,'amp',1.0,'levels',1.0]
 env={'EnvAttack':p['EnvAttack'],'EnvRelease':p['EnvRelease']}
 audio=r.render(work,defs,'audio-%02d'%ref['slot'],'GrainBoxPolyVC_2',ctl,
                ['dry',1.0,'gated',1.0,'gatebus',r.GATE],[(9.0,1),(11.0,0)],11.5,buffer_seconds=12,lfo=p,preset=env)
 assert np.all(np.isfinite(audio)) and r.level(audio[:,0],9.8,10.8)>0.01
 assert abs(r.level(audio[:,0],8.0,8.8)-0.25/np.sqrt(2))<0.005
 assert abs(r.level(audio[:,0],11.2,11.4)-0.25/np.sqrt(2))<0.005
 print('PASS: actual routed grains; dry before Go and after Stop',flush=True)
# Reusable one-shot ramps must hold their end instead of wrapping to zero.
p=bank['Stretch Slowdown']
audio=r.render(work,defs,'one-shot','GBTestRouteProbe',[],['dry',0.0],[(1.5,1),(5.0,0)],5.5,lfo=p)
assert abs(audio[int(2.5*r.SR),0]-0.25)<0.01 # quadratic: 1 s elapsed, 2 s ramp, 1 s span
assert abs(audio[int(4.5*r.SR),0]-2.0)<0.01
assert abs(audio[int(5.2*r.SR),0])<1e-6
print('PASS: shared integrated slowdown, continuing end rate and gate release',flush=True)
# Compare actual Ramp DSP with the production preview/table math, including
# easing, signed endpoints, integrated rates, wrapping and continued end slope.
(work/'reference.cpp').write_text(r'''
#include "scRamp.h"
#include <iostream>
#include <cstdlib>
int main(int argc,char**argv){for(int i=0;i<15;++i){double t=.07+i*.091-(128.0/48000);std::cout.precision(12);
std::cout<<scRamp::value(t,atof(argv[1]),atof(argv[2]),atof(argv[3]),atof(argv[4]),atoi(argv[5]),atoi(argv[6]),atoi(argv[7]),atof(argv[8]),atof(argv[9]))<<"\n";}}
''')
subprocess.run(['clang++','-std=c++17','-I'+str(ROOT/'src/nodes/boxSynths/shared'),str(work/'reference.cpp'),'-o',str(work/'reference')],check=True)
for ci,(start,end,curve,offset,rate,ending,bounds,pow,bipow) in enumerate([
 (0,1,0,0,False,0,0,.65,0),(1,-1,2,.1,False,0,0,0,-.55),
 (0,1,-2,0,True,2,0,-.4,.5),(-1,2,0,0,True,1,2,.3,-.2),
 (0,1,0,0,False,0,1,1,-1)]):
 p=dict(bank['Stretch Slowdown']);p.update(LFO1_Ramp_From=start,LFO1_Ramp_To=end,LFO1_Ramp_Curve=curve,
   LFO1_Ramp_Offset=offset,LFO1_Ramp_Integrate=rate,LFO1_Ramp_End=ending,LFO1_Ramp_Bounds=bounds,
   LFO1_Ramp_Duration=1,LFO1_Sync=False,LFO1_Pow=pow,LFO1_BiPow=bipow,Position_Mod_Span=1000,Position_Mod_Units=1)
 p['gbpModMatrix']=[1]+[0]*160
 audio=r.render(work,defs,'ease-%d'%ci,'GBTestRouteProbe',[],['dry',0.0],[(1.5,1),(3,0)],3.2,lfo=p)
 args=[start,end,curve,offset,int(rate),ending,bounds,pow,bipow]
 expected=np.array([float(x) for x in subprocess.check_output([str(work/'reference')]+list(map(str,args))).split()])
 actual=np.array([audio[int((1.5+.07+i*.091)*r.SR),0] for i in range(15)])
 assert np.max(abs(actual-expected))<.025,(ci,actual,expected)
 assert np.all(np.isfinite(audio))
print('PASS: Pow/BiPow easing, signed ramps, integration, loop/hold/continue and bounds match C++ preview',flush=True)

# Tape-style presets use precisely the same Ramp -> Pos route.
for name in ['Tape Stop','Octave Drop','Spin Up','Rewind','Brake & Reverse','Stutter 1/16','Stutter Slowdown','Repeated Tape Stops']:
 if name not in bank:continue
 p=bank[name]
 interval=p.get('Interval',40)*(0.5 if p.get('IntervalUnit',0)==2 else .001)
 ctl=['autotrig',1.0,'autotrigbeatdiv',1/(2*interval),'trigdur',1.0,'overlap',p.get('Overlap',2),
      'graintail',p.get('Grain_Tail',0)/1000,'envbuf',2.0,'amp',1.0,'levels',1.0,'position',0.0,'glide',0.0]
 audio=r.render(work,defs,'tape-'+name.replace(' ','-').replace('/','-'),'GrainBoxPolyDC_2' if p.get('Grainless',False) else 'GrainBoxPolyVC_2',ctl,
    ['dry',1.0,'gated',1.0,'gatebus',r.GATE],[(9,1),(13,0)],13.5,buffer_seconds=16,lfo=p,
    preset={'EnvAttack':p.get('EnvAttack',.05),'EnvRelease':p.get('EnvRelease',.1)})
 assert np.all(np.isfinite(audio)) and r.level(audio[:,0],9.1,9.4)>.005,name
 assert abs(r.level(audio[:,0],8,8.8)-.25/np.sqrt(2))<.005,name
 assert abs(r.level(audio[:,0],13.2,13.4)-.25/np.sqrt(2))<.005,name
 if name=='Tape Stop':
  duration=p['LFO1_Ramp_Duration']*(.5 if p['LFO1_Sync'] else 1)
  mid=9+duration*.5
  assert abs(r.peak(audio[:,0],mid-.025,mid+.025)-220)<35
  assert r.level(audio[:,0],9+duration+.2,9+duration+.5)<.005
 if name=='Octave Drop':
  assert abs(r.peak(audio[:,0],11,11.3)-220)<5
 print('PASS: routed '+name+' audio, wet gate and dry return',flush=True)
# Frozen stutter must repeat captured history rather than follow new input.
p=bank['Stutter 1/16']
audio=r.render(work,defs,'frozen-sweep','GrainBoxPolyVC_2',
 ['autotrig',1.0,'autotrigbeatdiv',62.5,'trigdur',1.0,'overlap',2.0,'envbuf',2.0,'amp',1.0,'levels',1.0],
 ['dry',1.0,'gated',1.0,'gatebus',r.GATE],[(1.5,1),(2.9,0)],3.2,src='GBTestSweep',buffer_seconds=4,lfo=p,
 preset={'EnvAttack':.05,'EnvRelease':.1})
a,b=r.peak(audio[:,0],1.8,2.0),r.peak(audio[:,0],2.5,2.7)
assert 400<a<550 and abs(a-b)<30,(a,b)
print('PASS: frozen stutter repeats captured sweep history rather than live input',flush=True)
# Classical grain delay still preserves independent stereo channels and pitch.
audio=r.render(work,defs,'classical-delay','GrainBoxPolyVC_2',
 ['autotrig',1.0,'autotrigbeatdiv',20.0,'trigdur',1.0,'overlap',2.0,'envbuf',2.0,'amp',1.0,'levels',1.0,'position',.1],
 ['dry',0.0],[(1,1),(2.9,0)],3.2,preset={'EnvAttack':.05,'EnvRelease':.1})
assert r.tone(audio[:,0],2,2.5,440)>.02 and r.tone(audio[:,1],2,2.5,660)>.02
assert r.tone(audio[:,0],2,2.5,660)<.01 and r.tone(audio[:,1],2,2.5,440)<.01
print('PASS: classical stereo grain delay retains pitch and source placement',flush=True)
print('ALL PASS',flush=True)
