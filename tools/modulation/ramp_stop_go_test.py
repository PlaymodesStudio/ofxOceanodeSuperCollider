#!/usr/bin/env python3
"""Render the production Ramp completion trigger and exercise its host receiver."""
from pathlib import Path
import argparse
import subprocess,sys,tempfile
import numpy as np
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/aliasingbox'))
from render_test import message,bundle,read_wav
bundled=ROOT.parents[1]/'apps/Playmodes/OceanAudio/bin/data/Supercollider/Scsynth/bin/scsynth'
ap=argparse.ArgumentParser();ap.add_argument('--server',type=Path,default=bundled if bundled.exists() else Path('/Applications/SuperCollider.app/Contents/Resources/scsynth'));server=ap.parse_args().server
plugins=server.parent/'plugins'
work=Path('/tmp/ramp-stop-go');work.mkdir(exist_ok=True)
s=(ROOT/'synthdefs/Defaults/Modulation/modulationRamp.scd').read_text().replace('(1..32).do','(1..2).do')
# Capture the real SendReply trigger in an NRT audio channel (NRT has no
# language client to receive replies). The trigger expression is unchanged.
line=next(line for line in s.splitlines() if 'SendReply.kr(' in line)
trigger=line.split('SendReply.kr(',1)[1].split(",'/modRampDone'",1)[0]
s=s.replace(line,'        Out.kr(900,'+trigger+');')
(work/'ramp.scd').write_text(s)
(work/'actual').mkdir(exist_ok=True)
(work/'actual/ramp.scd').write_text((ROOT/'synthdefs/Defaults/Modulation/modulationRamp.scd').read_text())
(work/'config.yaml').write_text('includePaths:\n  - '+str(ROOT/'sclang/SCClassLibrary')+'\nexcludeDefaultPaths: true\n')
(work/'home').mkdir(exist_ok=True)
probe='SynthDef(\\GoGate,{Out.ar(32,K2A.ar(\\gate.kr(0)))}).writeDefFile("'+str(work)+'");\n'
probe+='SynthDef(\\GoProbe,{Out.ar(0,K2A.ar([In.kr(700),In.kr(900)]))}).writeDefFile("'+str(work)+'");\n'
(work/'run.scd').write_text('try { "'+str(work/'actual/ramp.scd')+'".load;\n"'+str(work/'ramp.scd')+'".load;\n'+probe+'"COMPILED".postln;0.exit;} {|e|e.reportError;1.exit;}')
r=subprocess.run([str(ROOT/'sclang/osx/sclang'),'-l',str(work/'config.yaml'),str(work/'run.scd')],capture_output=True,text=True,timeout=60,env={'HOME':str(work/'home'),'PATH':'/usr/bin:/bin'},stdin=subprocess.DEVNULL)
assert 'COMPILED'in r.stdout and 'ERROR'not in r.stdout,r.stdout+r.stderr
defs=list((work/'CompiledSynthdefs/Modulation').glob('*.scsyndef'))+list(work.glob('Go*.scsyndef'))
def render(name,ending=3,mode=2,enabled=1,freq=1,restart=0,report=1):
 score=bundle(0,[message('/d_recv',p.read_bytes())for p in defs]+[message('/g_new',1,0,0)])
 score+=bundle(.01,[message('/s_new','GoGate',100,1,1),message('/s_new','ModRamp1',101,1,1,'out',700.,'gate',32.,'ending',float(ending),'trigmode',float(mode),'enabled',float(enabled),'freq',float(freq),'restartsec',float(restart),'reportdone',float(report),'pow',.4,'bipow',-.2),message('/s_new','GoProbe',102,1,1)])
 for t,g in [(.5,1),(2,0),(2.5,1)]:score+=bundle(t,[message('/n_set',100,'gate',float(g))])
 score+=bundle(4,[message('/c_set',0,0.)]);osc=work/(name+'.osc');osc.write_bytes(score);wav=work/(name+'.wav')
 r=subprocess.run([str(server),'-N',str(osc),'_',str(wav),'48000','WAV','float','-o','2','-i','0','-c','2048','-a','256','-D','0','-U',str(plugins)],capture_output=True,text=True,timeout=30)
 assert r.returncode==0,r.stdout+r.stderr
 data,ch,sr=read_wav(wav);x=np.array(data).reshape(-1,ch)
 indices=np.flatnonzero(x[:,1]>.1);edges=indices[np.r_[True,np.diff(indices)>1]] if len(indices) else indices
 return edges/sr
assert np.allclose(render('stop-go'),[1.5,3.5],atol=.01)
assert np.allclose(render('beats',freq=2),[1,3],atol=.01)
for name,args in [('hold',{'ending':0}),('loop',{'ending':1}),('continue',{'ending':2}),('disabled',{'enabled':0}),('free',{'mode':0}),('secondary-chunk',{'report':0}),('restart-before-end',{'restart':.75})]:
 assert len(render(name,**args))==0,name
print('PASS: audio-clock completion once per Go, retrigger, beat duration; no completion for other endings, disabled/free sources or secondary chunks',flush=True)
# Exercise the exact production receiver, including subscription replacement.
s=(ROOT/'src/nodes/boxSynths/shared/scModulation.cpp').read_text();a=s.index('void scModulation::bindRampCompletion(');b=s.index('\n}',a)+2
grain=(ROOT/'src/nodes/boxSynths/grainBoxPoly/scGrainBoxPoly.cpp').read_text();ga=grain.index('    if(fxMode)m.stopGo=');gb=grain.index('    m.nameSuffix',ga)
cpp=r'''
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <array>
#include <vector>
#include <cassert>
struct ofxOscMessage{std::string address;std::string getAddress()const{return address;}};
struct Event{std::function<void(ofxOscMessage&)> f;struct Token{Event* e;~Token(){e->f={};}};
 std::unique_ptr<Token>newListener(std::function<void(ofxOscMessage&)>f){this->f=f;return std::unique_ptr<Token>(new Token{this});}
 void emit(std::string a){ofxOscMessage m{a};if(f)f(m);}};
struct ofEventListener{std::unique_ptr<Event::Token>t;ofEventListener(std::unique_ptr<Event::Token> t):t(std::move(t)){}};
struct Synth{Event newFeedbackMessage;};
struct scModulation{struct ServerState{std::array<std::unique_ptr<ofEventListener>,8>rampDoneListeners;std::array<std::vector<Synth*>,8>lfo;};
 std::function<void()> stopGo;int lfoCount=1;bool ramp=true,enabled=true;int ending=3,mode=2;
 bool isRamp(int){return ramp;}bool rampEnabled(int){return enabled;}int rampEnding(int){return ending;}int lfoTriggerMode(int){return mode;}
 void bindRampCompletion(ServerState&,int);
};
METHOD
struct Gate{std::vector<float>v{1};std::string getName()const{return "Gate";}std::string getEscapedName()const{return "Gate";}
 std::vector<float>get()const{return v;}void set(std::vector<float>x){v=x;}};
struct Playhead{int index;Gate gate;};
struct GrainHost{bool fxMode=true,connected=false,published=false;Playhead master{0};
 Playhead& ph0(){return master;}bool hasInputConnection(std::string){return connected;}bool isEditorParameterPublished(std::string){return published;}
 void bind(scModulation& m,Playhead* p){GRAIN}
};
int main(){scModulation m;scModulation::ServerState s;Synth synth;s.lfo[0]={&synth};int count=0;bool go=true;
 m.stopGo=[&]{++count;go=false;};m.bindRampCompletion(s,0);synth.newFeedbackMessage.emit("/modRampDone");assert(count==1&&!go);
 synth.newFeedbackMessage.emit("/grainHead");assert(count==1);
 m.ending=1;synth.newFeedbackMessage.emit("/modRampDone");assert(count==1);m.ending=3;
 m.enabled=false;synth.newFeedbackMessage.emit("/modRampDone");assert(count==1);m.enabled=true;
 m.mode=0;synth.newFeedbackMessage.emit("/modRampDone");assert(count==1);m.mode=2;
 m.bindRampCompletion(s,0);synth.newFeedbackMessage.emit("/modRampDone");assert(count==2);
 m.ramp=false;m.bindRampCompletion(s,0);synth.newFeedbackMessage.emit("/modRampDone");assert(count==2);
 GrainHost g;g.bind(m,&g.master);m.stopGo();assert(g.master.gate.v==std::vector<float>{0});
 g.master.gate.v={1};g.connected=true;m.stopGo();assert(g.master.gate.v==std::vector<float>{1});g.connected=false;
 Playhead follower{1};g.bind(m,&follower);m.stopGo();assert(g.master.gate.v==std::vector<float>{0} && follower.gate.v==std::vector<float>{1});
 g.master.gate.v={1};g.published=true;m.stopGo();assert(follower.gate.v==std::vector<float>{0} && g.master.gate.v==std::vector<float>{1});
}
'''.replace('METHOD',s[a:b]).replace('GRAIN',grain[ga:gb])
(work/'receiver.cpp').write_text(cpp)
subprocess.run(['clang++','-std=c++17',str(work/'receiver.cpp'),'-o',str(work/'receiver')],check=True)
subprocess.run([str(work/'receiver')],check=True)
print('PASS: production receiver releases Go and ignores unrelated, stale-mode and disabled-source replies; subscriptions replaced and removed safely; actual GrainFxBox Go owner/input protection')
