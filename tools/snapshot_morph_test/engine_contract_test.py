#!/usr/bin/env python3
"""Run real Box setup and snapshot entry points headlessly with ASan/UBSan.

Requires neighboring ofxOceanode's native test build helper and OF libraries.
No server, audio device or application presets are used.
"""
from pathlib import Path
import argparse,os,sys,tempfile
ROOT=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--capture-dir',type=Path)
args=parser.parse_args()
if args.capture_dir:
 args.capture_dir.mkdir(parents=True,exist_ok=True)
 os.environ['BOX_MORPH_SCORE']=str(args.capture_dir/'additive-cap.osc')
 os.environ['BOX_MORPH_DEFS']=str(args.capture_dir/'CompiledSynthdefs')
sys.path.insert(0,str(ROOT.parent/'ofxOceanode/tests'))
import macro_mixer_regression as native
native.PRELUDE+=native.block((native.ROOT/'src/Nodes/ofxOceanodeNodeModel.cpp').read_text(),'void ofxOceanodeNodeModel::removeSeparator(')+'\n'
native.PRELUDE+='\n'+''.join('#include "'+str(ROOT/'src/nodes/boxSynths'/p)+'"\n' for p in ['additiveBox/scAdditiveBox.h','analogBox/scAnalogBox.h','grainBoxPoly/scGrainBoxPoly.h','pulsarBox/scPulsarBox.h','wavetableBox/scWavetableBox.h'])
native.CASES=r'''
int main(int argc,char**argv){
 assert(argc==3);std::filesystem::current_path(argv[2]);ofDisableDataPath();ofSeedRandom();
 auto window=std::make_shared<ofAppNoWindow>();ofGetMainLoop()->setCurrentWindow(window);
 std::vector<serverManager*> servers;
 auto near=[](float a,float b){if(std::abs(a-b)>=1e-4)std::cerr<<"mismatch "<<a<<" expected "<<b<<"\n";assert(std::abs(a-b)<1e-4);};
 {
 scAdditiveBox box(servers);box.setup();auto a=box.captureSound(),b=a;
 b["Voices"]=8;b[box.waveA.getEscapedName()]={4};b[box.unison.getEscapedName()]=4;
 for(float t:{0.f,.001f,.499f,.5f,.501f,1.f}){
  box.snaps.applyExternalMorph(a,b,t,t>=.5);near(box.voiceMorphGain(0),1);if(t>0)near(box.voiceMorphGain(7),t);else assert(box.numVoicesClamped()==1);
  auto data=box.computeVoice(0,0);for(auto x:data.view)assert(std::isfinite(x));
  if(t>0 && t<1){auto w=box.snaps.morph.weights(box.waveA);near(w[0],1-t);near(w[4],t);}
 }
 }
 {
 scAdditiveBox box(servers);box.setup();box.partials=9;box.pitch=std::vector<float>{0};box.tilt=std::vector<float>{0};
 box.filterMode=std::vector<int>{0};box.filter2Mode=std::vector<int>{0};
 auto a=box.captureSound(),b=a;b[box.partials.getEscapedName()]=15;
 for(float t:{0.f,.001f,.499f,.501f,1.f}){
  box.snaps.applyExternalMorph(a,b,t,t>=.5);assert(!box.partialBankMode && box.partialCap()==16);
  auto data=box.computeVoice(0,0);near(data.arrays.at("pa")[14],.25f/std::sqrt(15.f)*t);
  near(data.arrays.at("pa")[0],.25f*((1-t)/3+t/std::sqrt(15.f)));
 }
 }
 {
 ofxSCServer server("127.0.0.1",9,0);server.beginNRTCapture(true);
 {
 scAdditiveBox box(servers);box.setup();box.partials=8;box.numOutputs=1;
 box.pitch=std::vector<float>{0};box.gate=std::vector<float>{1};box.tilt=std::vector<float>{0};
 box.waveA=std::vector<int>{0};box.waveB=std::vector<int>{0};box.filterMode=std::vector<int>{0};box.filter2Mode=std::vector<int>{0};
 box.volume=.15f;auto a=box.captureSound(),b=a;b[box.partials.getEscapedName()]=256;
 if(const char* definitions=std::getenv("BOX_MORPH_DEFS")){
  ofxOscMessage defs;defs.setAddress("/d_loadDir");defs.addStringArg(definitions);server.sendMsg(defs);
 }
 ofxOscMessage root;root.setAddress("/g_new");root.addIntArg(1);root.addIntArg(0);root.addIntArg(0);server.sendMsg(root);
 auto acknowledge=[&](){
  auto nodes=server.nodeFeedbackFunctions;
  for(auto& e:nodes)if(e.first->nodeID>0 && !e.first->isCreated() && !e.first->hasEnded()){
   ofxOscMessage go;go.setAddress("/n_go");go.addIntArg(e.first->nodeID);e.first->feedbackListener(go);
  }
 };
 server.setNRTTime(.1);box.buildSynth(&server);box.setOutputBus(&server,0,0);box.createSynth(&server);acknowledge();
 auto& state=box.states.at(&server);assert(state.voices.size()==1 && state.noteWriters.size()==1);
 const int original=state.voices[0].cores[0][0]->nodeID,note=state.noteWriters[0]->nodeID;
 const auto count=server.getNRTEventCount();box.syncNoteWriters(&server);assert(server.getNRTEventCount()==count);
 int step=0;
 for(float t:{0.f,.001f,.499f,.501f,1.f}){
  server.setNRTTime(.2+.2*step++);box.snaps.applyExternalMorph(a,b,t,t>=.5);acknowledge();
  assert(state.voices[0].cores[0][0]->nodeID==original && state.noteWriters[0]->nodeID==note);
  if(t>0){assert(state.partialBanks.count(256));assert(state.partialBanks.at(256).size()==1);}
 }
 server.setNRTTime(1.4);box.finishPartialMorph();acknowledge();assert(!box.partialBankMode && box.builtCap==256 && state.partialBanks.empty());
 assert(state.noteWriters[0]->nodeID==note && state.voices[0].cores[0][0]->nodeID!=original);
 step=0;
 for(float t:{.001f,.499f,.501f,1.f}){server.setNRTTime(1.6+.2*step++);box.snaps.applyExternalMorph(b,a,t,t>=.5);acknowledge();}
 server.setNRTTime(2.6);box.finishPartialMorph();acknowledge();assert(box.builtCap==8 && state.partialBanks.empty() && state.noteWriters[0]->nodeID==note);
 // One cap's spectrum is conditional on its configuration. Its output gain
 // applies the weight once, rather than introducing new partials quadratically.
 box.snaps.applyExternalMorph(a,b,.001f,false);box.renderPartialCap=256;box.renderPartialGain=.001f;
 auto high=box.computeVoice(0,0);near(high.arrays.at("pa")[200],.25f/16);near(high.scalars.at("amp"),.0005f);
 box.renderPartialCap=8;box.renderPartialGain=.999f;auto low=box.computeVoice(0,0);near(low.arrays.at("pa")[0],.25f/std::sqrt(8.f));
 box.renderPartialCap=0;box.renderPartialGain=1;
 server.setNRTTime(3.0);box.free(&server);
 }
 server.endNRTCapture(3.1);
 if(const char* score=std::getenv("BOX_MORPH_SCORE"))assert(server.writeNRTScore(score,3.1));
 }
 {
 scAnalogBox box(servers);box.setup();auto a=box.captureSnapshot(),b=a;
 a["controls"][box.oscCount.getEscapedName()]=1;b["controls"][box.oscCount.getEscapedName()]=6;
 for(float t:{0.f,.001f,.499f,.501f,1.f}){
  box.snaps.applyExternalMorph(a,b,t,t>=.5);near(box.oscillatorPresence(0),1);near(box.oscillatorPresence(5),t);
  if(t>0)assert(box.oscs()==6);
 }
 }
 {
 scGrainBoxPoly box(servers);box.setup();auto a=box.captureSnapshot(),b=a;auto& ph=*box.playheads[0];
 a[ph.numPlayheads.getEscapedName()]="1";b[ph.numPlayheads.getEscapedName()]="8";
 b[ph.filterType.getEscapedName()]="5";
 for(float t:{0.f,.001f,.499f,.501f,1.f}){
  box.snaps.applyExternalMorph(a,b,t,t>=.5);
  near(scVoiceCountMorph::presence(ph.voiceMorphWeights,7),t);if(t>0)assert(box.phVoices(ph)==8);
 }
 auto held=box.captureSnapshot();box.snaps.applyExternalMorph(b,a,.4,false);held=box.captureSnapshot();
 box.snaps.applyExternalMorph(held,a,0,false);near(scVoiceCountMorph::presence(ph.voiceMorphWeights,7),.6);
 }
 {
 scPulsarBox box(servers);box.setup();auto a=box.captureSound(),b=a;
 b["Voices"]=8;b[box.numLayers.getEscapedName()]=4;b[box.pulsaret[0].getEscapedName()]="Saw";
 for(float t:{0.f,.001f,.499f,.501f,1.f}){
  box.snaps.applyExternalMorph(a,b,t,t>=.5);float sum=0;for(const auto&e:box.bankWeights)sum+=e.second;near(sum,1);
  box.refreshTables();for(float x:box.tables[0][0])assert(std::isfinite(x));
 }
 box.snaps.applyExternalMorph(a,b,.4f,false);auto held=box.captureSound();
 box.loadSound(held);assert(box.bankWeights.size()==2);
 }
 {
 scWavetableBox box(servers);box.setup();auto a=box.captureSound(),b=a;b[box.warp1.getEscapedName()]={8};
 for(float t:{0.f,.001f,.499f,.501f,1.f}){
  box.snaps.applyExternalMorph(a,b,t,t>=.5);auto w=box.snaps.morph.weights(box.warp1);near(w[0],1-t);near(w[8],t);
 }
 }
 std::cout<<"PASS: real Additive/Analog/Grain/Pulsar/Wavetable setup, snapshot entry points, early counts, evaluated modes and interrupted state (ASan/UBSan)\n";
}
'''
SC=ROOT.parent/'ofxSuperCollider';OSC=ROOT.parent/'ofxOsc'
sources=[str(native.ROOT/'src/Managers/ofxOceanodeScheduling.cpp')]
sources += [str(ROOT/'src'/p) for p in ['scNode.cpp','serverManager.cpp','ui/scTimelineMenu.cpp','nodes/boxSynths/shared/scModulation.cpp','nodes/boxSynths/additiveBox/scAdditiveBox.cpp','nodes/boxSynths/analogBox/scAnalogBox.cpp','nodes/boxSynths/grainBoxPoly/scGrainBoxPoly.cpp','nodes/boxSynths/wavetableBox/scWavetableBox.cpp']]
sources += [str(p) for p in sorted((SC/'src').glob('*.cpp'))]+[str(p) for p in sorted((OSC/'src').glob('*.cpp'))]
sources += [str(p) for p in sorted((OSC/'libs/oscpack/src').rglob('*.cpp')) if 'win32' not in str(p)]
includes=[ROOT/'src',*sorted({p.parent for p in (ROOT/'src').rglob('*.h')}),SC/'src',OSC/'src',OSC/'libs/oscpack/src',*sorted({p.parent for p in (OSC/'libs/oscpack/src').rglob('*.h')})]
with tempfile.TemporaryDirectory(prefix='box-contract-build-') as directory:
 wrapper=Path(directory)/'cxx';wrapper.write_text('#!/bin/sh\nexec clang++ -fno-access-control "$@"\n');wrapper.chmod(0o755)
 os.environ['CXX']=str(wrapper)
 native.main(extra_sources=sources,extra_includes=includes,std='c++20')
