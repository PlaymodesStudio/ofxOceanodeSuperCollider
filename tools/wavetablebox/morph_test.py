#!/usr/bin/env python3
"""Exercise production WavetableBox shared-voice morphs with ASan/UBSan.

Uses the neighboring Oceanode native harness; captures OSC without an audio
server. Covers the 12/1 voice counts selected by OceanAudio's wttests preset.
"""
from pathlib import Path
import os, sys, tempfile
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT.parent / 'ofxOceanode/tests'))
import macro_mixer_regression as native
native.PRELUDE += native.block((native.ROOT/'src/Nodes/ofxOceanodeNodeModel.cpp').read_text(), 'void ofxOceanodeNodeModel::removeSeparator(') + '\n'
native.PRELUDE += '#include "' + str(ROOT/'src/nodes/boxSynths/wavetableBox/scWavetableBox.h') + '"\n'
native.CASES = r'''

int main(int argc,char**argv){
 std::filesystem::current_path(argv[2]);ofDisableDataPath();ofSeedRandom();
 auto window=std::make_shared<ofAppNoWindow>();ofGetMainLoop()->setCurrentWindow(window);
 std::vector<serverManager*> servers;
 ofxSCServer server("127.0.0.1",9,0);server.beginNRTCapture(true);
 scWavetableBox box(servers);box.setup();box.drone=true;box.startDecode();box.applyTable(box.decodeTask.get());
 auto a=box.captureSound(),b=a;a["Voices"]=12;b["Voices"]=1;
 // Solar -> SteppedBass also changes quality and both warp dropdowns.
 a[box.oversample.getEscapedName()]=3;b[box.oversample.getEscapedName()]=0;
 b[box.warp1.getEscapedName()]={2};b[box.warp2.getEscapedName()]={6};
 b[box.warp1Amt.getEscapedName()]={.4f};b[box.warp2Amt.getEscapedName()]={.5f};
 // Drone (A) -> gated with the envelope on Amp (B); Echo on (A) -> off (B)
 b[box.drone.getEscapedName()]=false;a[box.fxOn[box.FxEcho].getEscapedName()]=true;b[box.fxOn[box.FxEcho].getEscapedName()]=false;
 a[box.echoFilter.getEscapedName()]=0;b[box.echoFilter.getEscapedName()]=2;
 box.snaps.applyExternal(a);box.buildSynth(&server);box.setOutputBus(&server,0,0);box.createSynth(&server);
 // Counts may change while asynchronous table reads are still pending.
 box.voices=13;box.createSynth(&server);assert(box.modStates.at(&server).gateSynth->getName()=="ModGate13");
 box.voices=12;box.createSynth(&server);assert(box.modStates.at(&server).gateSynth->getName()=="ModGate12");
 auto ack=[&](){auto nodes=server.nodeFeedbackFunctions;for(auto&e:nodes)if(e.first->nodeID>0 && !e.first->isCreated() && !e.first->hasEnded()){
  ofxOscMessage go;go.setAddress("/n_go");go.addIntArg(e.first->nodeID);e.first->feedbackListener(go);
 }};box.swapBuffers(&server);ack();
 auto near=[](float x,float y){if(std::abs(x-y)>1e-5)std::cerr<<x<<" != "<<y<<"\n";assert(std::abs(x-y)<1e-5);};
 assert(box.extraVoices.at(&server).size()==11);
 const int first=box.synths.at(&server)->nodeID,last=box.extraVoices.at(&server).back()->nodeID;
 for(float t:{0.f,.001f,.499f,.50001f,.501f,.9f,1.f}){
  box.snaps.applyExternalMorph(a,b,t,t>=.5f);ack();
  near(box.voiceMorphGain(0),1);near(box.voiceMorphGain(11),1-t);
  assert(box.oversample.get()==3 && box.builtOversample==3);
  const auto w1=box.snaps.morph.weights(box.warp1),w2=box.snaps.morph.weights(box.warp2);
  near(w1[0],1-t);near(w1[2],t);near(w2[0],1-t);near(w2[6],t);
  const float sourceComp=1/std::sqrt(std::max(1.f,12.f/box.outChannels.get()));
  const float spreadComp=1/std::sqrt(std::max(1.f,float(box.MAX_CHANNELS)/box.outChannels.get()));
  near(box.voiceOutputGain(0)*spreadComp,(1-t)*sourceComp+t);
  near(box.voiceOutputGain(11)*spreadComp,(1-t)*sourceComp);
  assert(box.synths.at(&server)->nodeID==first && box.extraVoices.at(&server).back()->nodeID==last);
  assert(box.renderingVoices()==12);
  // Amp paths crossfade: drone 1 -> 0, VCA (B's envelope) 0 -> 1, gates held only at a whole drone
  const auto& sent=box.voiceSent.at(&server).at(0);
  near(sent.at("drone")[0],1-t);near(sent.at("vca")[0],t);
  near(box.modStates.at(&server).gateSent.at("drone")[0],t<=0?1:0);
  // The echo blends its filter weights and keeps running (its tail) at t = 1
  auto& st=box.modStates.at(&server);
  assert(st.effects[box.FxEcho]);
  near(st.effectSent[box.FxEcho].at("on")[0],1-t);near(st.effectSent[box.FxEcho].at("filterweights")[2],t);
  assert(st.gateSynth->getName()=="ModGate12");
 }
 {  // a morph frame where nothing moved sends nothing to the voices
  auto& st=box.modStates.at(&server);const auto before=box.voiceSent.at(&server).at(0);
  box.snaps.applyExternalMorph(a,b,1.f,true);ack();assert(box.voiceSent.at(&server).at(0)==before);
  assert(st.effectOffAt[box.FxEcho]>0);
 }
 box.finishVoiceMorph();ack();assert(box.extraVoices.at(&server).empty());assert(box.synths.at(&server)->nodeID==first);
 // The modulation keeps its width (no LFO reset) until Voices is edited
 assert(box.modStates.at(&server).gateSynth->getName()=="ModGate12");
 const float settledGain=box.voiceOutputGain(0);
 box.finishVoiceMorph();near(box.voiceOutputGain(0),settledGain);
 box.outChannels=4;near(box.voiceSent.at(&server).at(0).at("levels")[0],box.levels.get()[0]*box.voiceOutputGain(0));
 box.outChannels=2;
 for(float t:{.001f,.499f,.501f,.75f}){
  box.snaps.applyExternalMorph(b,a,t,t>=.5f);ack();near(box.voiceMorphGain(11),t);
  assert(box.synths.at(&server)->nodeID==first && box.extraVoices.at(&server).size()==11);
 }
 auto held=box.snaps.captureForExternalMorph();box.snaps.applyExternalMorph(held,b,0,false);near(box.voiceMorphGain(11),.75f);
 box.snaps.applyExternalMorph(held,b,.5,true);near(box.voiceMorphGain(11),.375f);
 // Per-voice controls select their own vector elements and bus offsets.
 box.pitch=std::vector<float>{40,50,60};box.renderingVoice=2;assert(box.expand(box.pitch.get(),0)==std::vector<float>{60});box.renderingVoice=-1;
 // Explicit recall still applies the target quality; A/B morphing does not.
 box.snaps.applyExternal(b);ack();assert(box.oversample.get()==0 && box.builtOversample==0);
 {  // the switched-off echo is freed once its tail has rung out
  auto& st=box.modStates.at(&server);
  box.snaps.applyExternalMorph(a,b,1.f,true);ack();st.effectOffAt[box.FxEcho]=1e-6f;box.retireEffects();
  assert(!st.effects[box.FxEcho] && st.chain.empty());
 }
 box.voices=3;assert(box.modStates.at(&server).gateSynth->getName()=="ModGate3");
 box.free(&server);assert(box.extraVoices.empty() && box.synths.empty());
 std::cout<<"PASS: WavetableBox 12 -> 1 -> 12 shares common oscillator IDs, linear early presence, no midpoint replacement, delayed retirement and stored/interrupted morphs\n";
}

'''
SC=ROOT.parent/'ofxSuperCollider'; OSC=ROOT.parent/'ofxOsc'
sources=[str(native.ROOT/'src/Managers/ofxOceanodeScheduling.cpp')]
sources += [str(ROOT/'src'/p) for p in ['scNode.cpp','serverManager.cpp','ui/scTimelineMenu.cpp','nodes/boxSynths/shared/scModulation.cpp','nodes/boxSynths/wavetableBox/scWavetableBox.cpp']]
sources += [str(p) for p in sorted((SC/'src').glob('*.cpp'))] + [str(p) for p in sorted((OSC/'src').glob('*.cpp'))]
sources += [str(p) for p in sorted((OSC/'libs/oscpack/src').rglob('*.cpp')) if 'win32' not in str(p)]
includes=[ROOT/'src',*sorted({p.parent for p in (ROOT/'src').rglob('*.h')}),SC/'src',OSC/'src',OSC/'libs/oscpack/src',*sorted({p.parent for p in (OSC/'libs/oscpack/src').rglob('*.h')})]
if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='wavetable-morph-build-') as directory:
        wrapper=Path(directory)/'cxx'
        wrapper.write_text('#!/bin/sh\nexec clang++ -fno-access-control "$@"\n')
        wrapper.chmod(0o755); os.environ['CXX']=str(wrapper)
        native.main(extra_sources=sources, extra_includes=includes, std='c++20')
