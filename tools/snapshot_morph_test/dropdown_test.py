#!/usr/bin/env python3
"""Exercise sound dropdowns through real box snapshot hosts (ASan/UBSan).

The fixture alone accesses private implementation for assertions. Production
translation units compile with normal C++ access checks.
"""
from pathlib import Path
import os,sys,tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT.parent/'ofxOceanode/tests'))
import macro_mixer_regression as native
native.PRELUDE+=native.block((native.ROOT/'src/Nodes/ofxOceanodeNodeModel.cpp').read_text(),'void ofxOceanodeNodeModel::removeSeparator(')+'\n'
for header in ['wavetableBox/scWavetableBox.h','grainBoxPoly/scGrainBoxPoly.h','sampleBox/scSampleBox.h','polyComb/scPolyComb.h','physicalBox/scPhysicalBox.h','steamPipeBox/scSteamPipeBox.h']:
 native.PRELUDE+='#include "'+str(ROOT/'src/nodes/boxSynths'/header)+'"\n'
native.CASES=r'''

int main(int argc,char**argv){
 std::filesystem::current_path(argv[2]);ofDisableDataPath();ofSeedRandom();auto window=std::make_shared<ofAppNoWindow>();ofGetMainLoop()->setCurrentWindow(window);std::vector<serverManager*> servers;
 auto near=[](float a,float b){if(std::abs(a-b)>1e-4)std::cerr<<a<<" != "<<b<<"\n";assert(std::abs(a-b)<1e-4);};
 {scWavetableBox b(servers);b.setup();auto a=b.captureSound(),z=a;
  for(int stage=0;stage<2;++stage)for(int x=0;x<b.NumWarps;++x)for(int y=0;y<b.NumWarps;++y){
   a["Warp_1"]={stage==0?x:0};z["Warp_1"]={stage==0?y:0};a["Warp_2"]={stage==1?x:0};z["Warp_2"]={stage==1?y:0};a["Warp_1_Amt"]={.7f};z["Warp_1_Amt"]={.7f};a["Warp_2_Amt"]={.7f};z["Warp_2_Amt"]={.7f};
   for(float t:{.001f,.499f,.501f,.999f}){b.snaps.applyExternalMorph(a,z,t,t>=.5);for(float p:{.031f,.17f,.42f,.77f,.97f})near(b.morphedWarpPhase(p,0,.7f,.7f),(1-t)*b.warpPhase(p,x,.7f)+t*b.warpPhase(p,y,.7f));}
  }
 }
 {scGrainBoxPoly b(servers);b.setup();auto& ph=*b.playheads[0];auto a=b.captureSnapshot(),z=a;
  const auto shape=ph.envShape.getEscapedName(),placement=ph.placement.getEscapedName();
  for(int x=0;x<7;++x)for(int y=0;y<7;++y){a[shape]=std::to_string(x);z[shape]=std::to_string(y);
   b.snaps.applyExternal(a);b.computeEnvData(ph);auto from=ph.envData;b.snaps.applyExternal(z);b.computeEnvData(ph);auto to=ph.envData;
   for(float t:{.001f,.499f,.501f,.999f}){b.snaps.applyExternalMorph(a,z,t,t>=.5);b.computeEnvData(ph);for(size_t i=0;i<from.size();++i)near(ph.envData[i],(1-t)*from[i]+t*to[i]);}
  }
  const auto scale=ph.scaleType.getEscapedName(),root=ph.scaleRoot.getEscapedName();
  for(int x=0;x<12;++x)for(int y=0;y<12;++y){a[scale]=std::to_string(x);z[scale]=std::to_string(y);a[root]="2";z[root]="11";auto from=b.scaleMorphData(ph,a),to=b.scaleMorphData(ph,z);
   for(float t:{.499f,.501f}){b.snaps.applyExternalMorph(a,z,t,t>=.5);auto values=b.snaps.morph.resolvedValue(ph.scaleType);assert(values.size()==25);for(int i=0;i<25;++i)near(values[i],(1-t)*from[i]+t*to[i]);}
  }
  for(int x=0;x<3;++x)for(int y=0;y<3;++y){a[placement]=std::to_string(x);z[placement]=std::to_string(y);b.snaps.applyExternal(a);auto from=b.panSend(ph,4);b.snaps.applyExternal(z);auto to=b.panSend(ph,4);
   for(float t:{.001f,.499f,.501f,.999f}){b.snaps.applyExternalMorph(a,z,t,t>=.5);auto pan=b.panSend(ph,4);for(size_t i=0;i<pan.size();++i)near(pan[i],(1-t)*from[i]+t*to[i]);}
  }
 }
 {scSampleBox b(servers);b.setup();auto a=b.captureSnapshot(),z=a;auto& pad=*b.pads[0];a["pads"][0]["controls"]["Stretch_Mode"]="0";z["pads"][0]["controls"]["Stretch_Mode"]="1";auto base=b.engineDef(pad);base.resize(base.size()-2);
  // both readers (_M) only while the morph is between the modes; R / T at the ends
  for(float t:{0.f,.001f,.499f,.501f,.999f,1.f}){b.snaps.applyExternalMorph(a,z,t,t>=.5);near(b.snaps.morph.weights(pad.stretchMode)[1],t);assert(b.engineDef(pad)==base+(t<=0.f?"_R":t>=1.f?"_T":"_M"));}
 }

 {scPolyComb b(servers);b.setup();auto a=b.captureSnapshot(),z=a;
  a["controls"][b.filterOn.getEscapedName()]=true;z["controls"][b.filterOn.getEscapedName()]=true;
  a["controls"][b.filterModel.getEscapedName()]=0;z["controls"][b.filterModel.getEscapedName()]=1;
  a["controls"][b.combMode.getEscapedName()]=0;z["controls"][b.combMode.getEscapedName()]=1;
  a["controls"][b.ringPitchSource.getEscapedName()]=0;z["controls"][b.ringPitchSource.getEscapedName()]=2;
  for(float t:{0.f,.001f,.499f,.5f,.501f,.999f,1.f}){b.snaps.applyExternalMorph(a,z,t,t>=.5);near(b.snaps.morph.activation(b.filterOn),std::abs(1-2*t));near(b.snaps.morph.activation(b.combMode),t);auto w=b.snaps.morph.weights(b.ringPitchSource);near(w[0],1-t);near(w[2],t);}
  b.snaps.applyExternalMorph(a,z,.2f,false);auto held=b.snaps.captureForExternalMorph();b.snaps.applyExternal(held);near(b.snaps.morph.activation(b.filterOn),.6f);
  ofJson preset;b.presetSave(preset);auto converted=b.snapshotOf(preset);b.snaps.applyExternal(converted);near(b.snaps.morph.activation(b.filterOn),.6f);
 }

 // The same evaluator serves every spatial dropdown; test actual host
 // registration and position calculations on both sides of the UI switch.
 auto spatial=[&](auto& b,auto& placement,auto& layout,auto positions,int modes,const char* container){
  auto state=[&](int mode,int speaker){ofJson j=ofJson::object();auto& c=*container?j[container]:j;c[placement.getEscapedName()]=mode;c[layout.getEscapedName()]=speaker;return j;};
  for(int x=0;x<modes;++x)for(int y=0;y<modes;++y)for(int l=0;l<2;++l){
   auto a=state(x,l),z=state(y,l);
   b.snaps.morph.begin(a,a,1);b.snaps.morph.end();auto from=positions();
   b.snaps.morph.begin(z,z,1);b.snaps.morph.end();auto to=positions();
   for(float t:{.001f,.499f,.501f,.999f}){b.snaps.morph.begin(a,z,t);b.snaps.morph.end();auto p=positions();assert(p.size()==from.size());for(size_t i=0;i<p.size();++i)near(p[i],(1-t)*from[i]+t*to[i]);}
  }
  for(int x=0;x<modes;++x){auto a=state(x,0),z=state(x,1);b.snaps.morph.begin(a,a,1);b.snaps.morph.end();auto from=positions();b.snaps.morph.begin(z,z,1);b.snaps.morph.end();auto to=positions();
   for(float t:{.499f,.501f}){b.snaps.morph.begin(a,z,t);b.snaps.morph.end();auto p=positions();for(size_t i=0;i<p.size();++i)near(p[i],(1-t)*from[i]+t*to[i]);near(b.snaps.morph.weights(layout)[1],t);}}
 };
 {scWavetableBox b(servers);b.setup();b.voices=4;b.outChannels=4;spatial(b,b.placement,b.speakerLayout,[&](){return b.voicePositions();},3,"");}
 {scPolyComb b(servers);b.setup();b.voices=4;b.outChannels=4;spatial(b,b.spreadMode,b.spreadLayout,[&](){return b.panPositions();},3,"controls");}
 {scPhysicalBox b(servers);b.setup();b.voices=4;b.channels=4;spatial(b,b.placement,b.speakerLayout,[&](){return b.panPositions();},3,"");
  auto& family=b.soundParam("bfamily");for(int x=0;x<6;++x)for(int y=0;y<6;++y){ofJson a={{family.getEscapedName(),x}},z={{family.getEscapedName(),y}};for(float t:{.499f,.501f}){b.snaps.morph.begin(a,z,t);b.snaps.morph.end();auto w=b.snaps.morph.weights(family);for(int i=0;i<6;++i)near(w[i],(i==x?1-t:0)+(i==y?t:0));}}}
 {scSteamPipeBox b(servers);b.setup();b.voices=4;b.channels=4;spatial(b,b.placement,b.speakerLayout,[&](){return b.panPositions();},4,"");}

 // Verify the optimization contracts alongside the dropdown behavior: a
 // repeated state is silent on OSC, but explicit offline resends stay complete.
 {
  auto& capture=*new ofxSCServer("127.0.0.1",9,0);capture.beginNRTCapture(true);
  auto ack=[&](){auto nodes=capture.nodeFeedbackFunctions;for(const auto& e:nodes)if(e.first->nodeID>0 && !e.first->isCreated() && !e.first->hasEnded()){
   ofxOscMessage go;go.setAddress("/n_go");go.addIntArg(e.first->nodeID);e.first->feedbackListener(go);
  }};
  {
   scWavetableBox b(servers);b.setup();auto& m=b.modulation;
   m.create(&capture,1,2,0);ack();m.sendChanged();
   auto count=capture.nrtEvents.size();m.sendChanged();assert(capture.nrtEvents.size()==count);
   m.resend();assert(capture.nrtEvents.size()>count);
   m.sendChanged();count=capture.nrtEvents.size();m.sendChanged();assert(capture.nrtEvents.size()==count);
   m.free(&capture);
  }
  {
   scGrainBoxPoly b(servers);b.setup();auto& ph=*b.playheads[0];
   auto a=b.captureSnapshot(),z=a;a.erase(ph.scaleRoot.getEscapedName());z.erase(ph.scaleRoot.getEscapedName());
   a[ph.scaleType.getEscapedName()]="2";z[ph.scaleType.getEscapedName()]="3";
   for(int root:{2,9}){ph.scaleRoot.setWithoutEventNotifications(root);b.snaps.morph.begin(a,z,.25f);b.snaps.morph.end();auto x=b.scaleMorphData(ph,a),y=b.scaleMorphData(ph,z);auto v=b.snaps.morph.resolvedValue(ph.scaleType);for(int i=0;i<25;++i)near(v[i],.75f*x[i]+.25f*y[i]);}
   auto* synth=new ofxSCSynth("graphiceq1",&capture);synth->createAndRun(1,1,true);ack();
   b.fx[&capture].synths[0]=synth;b.fx[&capture].plan={0};
   b.sendFxParamsAll();auto count=capture.nrtEvents.size();b.sendFxParamsAll();assert(capture.nrtEvents.size()==count);
   b.resendParametersForNRT();assert(capture.nrtEvents.size()>count);
   b.freeFx(&capture);
  }
  capture.endNRTCapture();
 }
 std::cout<<"PASS: optimized shared source/route/FX senders skip unchanged state; explicit offline resends and live scale dependencies remain correct\n";
 std::cout<<"PASS: all 81 Wavetable warp pairs in both stages, all 49 Grain envelope pairs, Grain scales, Sample playback, PolyComb pitch/filter state, all Physical families and all four spatial hosts preserve continuous evaluated state around .5\n";
}
'''
SC=ROOT.parent/'ofxSuperCollider';OSC=ROOT.parent/'ofxOsc'
sources=[str(native.ROOT/'src/Managers/ofxOceanodeScheduling.cpp')]
sources+=[str(ROOT/'src'/p)for p in ['scNode.cpp','serverManager.cpp','ui/scTimelineMenu.cpp','nodes/boxSynths/shared/scModulation.cpp','nodes/boxSynths/wavetableBox/scWavetableBox.cpp','nodes/boxSynths/grainBoxPoly/scGrainBoxPoly.cpp','nodes/boxSynths/sampleBox/scSampleBox.cpp','nodes/boxSynths/slicerBox/scSlicerBox.cpp','nodes/boxSynths/polyComb/scPolyComb.cpp','nodes/boxSynths/physicalBox/scPhysicalBox.cpp','nodes/boxSynths/steamPipeBox/scSteamPipeBox.cpp']]
sources+=[str(p)for p in sorted((SC/'src').glob('*.cpp'))]+[str(p)for p in sorted((OSC/'src').glob('*.cpp'))]
sources+=[str(p)for p in sorted((OSC/'libs/oscpack/src').rglob('*.cpp'))if 'win32'not in str(p)]
includes=[ROOT/'src',*sorted({p.parent for p in(ROOT/'src').rglob('*.h')}),SC/'src',OSC/'src',OSC/'libs/oscpack/src',*sorted({p.parent for p in(OSC/'libs/oscpack/src').rglob('*.h')})]
with tempfile.TemporaryDirectory(prefix='dropdown-morph-build-')as directory:
 wrapper=Path(directory)/'cxx'
 wrapper.write_text('#!/bin/sh\nfor arg in "$@"; do\n case "$arg" in */regression.cpp) exec clang++ -fno-access-control "$@";; esac\ndone\nexec clang++ "$@"\n')
 wrapper.chmod(0o755);os.environ['CXX']=str(wrapper)
 native.main(extra_sources=sources,extra_includes=includes,std='c++20')
