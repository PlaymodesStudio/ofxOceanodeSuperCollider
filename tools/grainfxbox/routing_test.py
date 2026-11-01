#!/usr/bin/env python3
"""Render the shared LFO -> Pos path, with standalone tape motion disabled."""
import importlib.util,json,pathlib,subprocess,sys
import numpy as np
ROOT=pathlib.Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('grain_render',ROOT/'tools/grainfxbox/render_test.py')
r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r)
work=pathlib.Path('/tmp/grainfxbox-shared-routing');work.mkdir(exist_ok=True)
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
  if(sound.value("Motion_Mode",0)>0){
   assert(routed["Motion"]==false && routed["gbpModLfos"]==1);
   assert(scGrainStretchRouting(routed,23,7)==routed); // repeat recall is stable
   auto stored=sound;
   for(auto it=stored.begin();it!=stored.end();++it) if(it.value().is_number() || it.value().is_boolean()) it.value()=it.value().dump();
   assert(scGrainStretchRouting(stored,23,7)["LFO1_Trigger_Mode"]==routed["LFO1_Trigger_Mode"]);
   auto edited=sound;edited["gbpModMatrix"]=std::vector<float>(161,0);edited["gbpModMatrix"][1]=0.3;
   assert(scGrainStretchRouting(edited,23,7)==edited); // retain custom routing
   auto published=sound;published["publishedEditorParameters"]={"Motion"};
   assert(scGrainStretchRouting(published,23,7)==published); // preserve published motion controls
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
for name in ['modulation.scd','modulationRandom.scd']:
 s=(ROOT/'synthdefs/Defaults/Modulation'/name).read_text().replace('(1..32).do','(1..2).do')
 (work/name).write_text(s)
probe=r'''SynthDef(\GBTestRouteProbe,{|spansec=1|Out.ar(0,K2A.ar([In.kr(800)*spansec,In.kr(700)]))}).writeDefFile("WORK");'''.replace('WORK',str(work))
(work/'shared.scd').write_text('"'+str(work/'modulation.scd')+'".load;\n"'+str(work/'modulationRandom.scd')+'".load;\n'+probe+'\n"COMPILED".postln;0.exit;\n')
x=subprocess.run([str(r.SCLANG),'-l',str(work/'config.yaml'),str(work/'shared.scd')],capture_output=True,text=True,timeout=120,
                 env={'HOME':str(work/'home'),'PATH':'/usr/bin:/bin'},stdin=subprocess.DEVNULL)
assert 'COMPILED' in x.stdout and 'ERROR' not in x.stdout,x.stdout[-6000:]
defs+=list((work/'CompiledSynthdefs/Modulation').glob('*.scsyndef'))+[work/'GBTestRouteProbe.scsyndef']
# Measure each shared route against the equation recovered from the ensemble.
for ref in json.loads((ROOT/'tools/grainfxbox/grain_stretch_reference.json').read_text())['snapshots']:
 name='GrainStretch %02d - %s'%(ref['slot'],ref['name']);p=bank[name]
 assert not p['Motion'] and p['gbpModLfos']==1
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
 print('PASS: '+name+' uses shared LFO/route, matches reference, stops and retriggers',flush=True)
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
assert abs(audio[int(4.5*r.SR),0]-1.0)<0.01
assert abs(audio[int(5.2*r.SR),0])<1e-6
print('PASS: shared quadratic one shot, terminal hold and gate release',flush=True)
print('ALL PASS',flush=True)
