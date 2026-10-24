#!/usr/bin/env python3
"""Exercise real Box gate defaults, boolean publication and legacy preset recall."""
from pathlib import Path
import os, sys, tempfile
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT.parent / 'ofxOceanode/tests'))
import macro_mixer_regression as native
hosts = {
    'aliasingBox': 'scAliasingBox', 'fmBox': 'scFMBox',
    'physicalBox': 'scPhysicalBox', 'steamPipeBox': 'scSteamPipeBox',
    'analogBox': 'scAnalogBox', 'polyComb': 'scPolyComb',
    'wavetableBox': 'scWavetableBox', 'grainBoxPoly': 'scGrainBoxPoly',
    'pulsarBox': 'scPulsarBox',
    'additiveBox': 'scAdditiveBox',
}
if os.environ.get('OCEANODE_TEST_BOX'):
    hosts = {key: value for key, value in hosts.items() if key == os.environ['OCEANODE_TEST_BOX']}
native.PRELUDE += native.block((native.ROOT/'src/Nodes/ofxOceanodeNodeModel.cpp').read_text(), 'void ofxOceanodeNodeModel::removeSeparator(') + '\n'
native.PRELUDE += f'#include "{ROOT}/src/nodes/boxSynths/shared/scDroneMode.h"\n'
native.PRELUDE += ''.join(f'#include "{ROOT}/src/nodes/boxSynths/{folder}/{cls}.h"\n' for folder, cls in hosts.items())
cases = r'''
int main(int argc, char** argv) {
 assert(argc == 3); std::filesystem::current_path(argv[2]); ofDisableDataPath(); ofSeedRandom();
 auto window = std::make_shared<ofAppNoWindow>(); ofGetMainLoop()->setCurrentWindow(window);
 std::vector<serverManager*> servers;
 // The migration is idempotent, accepts framework strings, and preserves new values.
 for(int mode : {0, 1, 2, 3}) {
  ofJson j={{"Gate_Mode", std::to_string(mode)}, {"published", {"Gate_Mode"}},
            {"layer", {{"Gate_Mode_2", mode}}}};
  scDroneMode::migrate(j, true);
  assert(j["Drone"] == (mode == 2)); assert(j["Pulse"] == (mode == 3));
  assert(j["layer"]["Drone_2"] == (mode == 2)); assert(j["published"][0] == "Drone");
  auto copy=j; scDroneMode::migrate(j,true); assert(copy==j);
 }
 ofJson modern={{"Drone",false},{"Env_Mode",2}};scDroneMode::migrate(modern);assert(modern["Drone"]==false);
'''
for folder, cls in hosts.items():
    if folder == 'grainBoxPoly':
        cases += r'''
 { scGrainBoxPoly box(servers); box.setup(); box.setNumPlayheads(2);
   for(auto& ph : box.playheads) {
    assert(!ph->drone.get() && box.ampGated(*ph));
    ph->gate=std::vector<float>{1};assert(box.ampGated(*ph));
    ph->drone=true; assert(!box.ampGated(*ph));
   }
   auto saved=box.captureSnapshot();
   for(auto& ph : box.playheads){saved.erase(ph->drone.getEscapedName());saved["Gate_Mode"+(ph->index?"_2":std::string())]="2";}
   box.applySnapshot(saved);for(auto& ph : box.playheads) assert(ph->drone.get());
   for(auto& ph : box.playheads){saved["Gate_Mode"+(ph->index?"_2":std::string())]="0";}
   box.applySnapshot(saved);for(auto& ph : box.playheads) assert(!ph->drone.get());
 }
'''
        continue
    cases += f'\n {{ {cls} box(servers); box.setup(); assert(!box.drone.get());\n'
    if folder in ('aliasingBox', 'fmBox', 'physicalBox', 'steamPipeBox', 'analogBox', 'polyComb'):
        cases += 'assert(!box.droning());box.gate=std::vector<float>{1};assert(!box.droning());box.drone=true;assert(box.droning());\n'
        cases += 'box.gate=std::vector<float>{0};assert(box.droning());\n'
    else:
        cases += 'box.drone=true;\n'
    if folder in ('physicalBox','steamPipeBox'):
        cases += 'assert(!box.pulsing());box.pulseEnabled=true;assert(box.pulsing() && !box.droning());box.pulseEnabled=false;\n'
    # Check the real parameter can be published as bool.
    if folder in ('wavetableBox','pulsarBox','additiveBox'):
        cases += 'box.publishByKey("Drone");\n'
    else:
        cases += 'box.publish("Drone");\n'
    cases += 'assert(box.getParameterGroup().get("Drone").type()==typeid(ofParameter<bool>).name());\n'
    if folder == 'additiveBox':
        cases += 'auto chord=ofLoadJson("' + str(ROOT/'presets/nodeSnapshots/AdditiveBox/Presets/Astres 01 Chord.json') + '");\n'
        cases += r'''
   assert(box.gate.get()==std::vector<float>{0});
   assert(box.mod.envValues(0).s==1.0f);
   auto saved=box.captureSound();assert(saved["Drone"]==true);
   box.drone=false;box.loadSound(saved);assert(box.drone.get());
   saved.erase("Drone");box.loadSound(saved);assert(!box.drone.get());
   box.drone=true;ofJson old=ofJson::object();box.presetRecallAfterSettingParameters(old);assert(!box.drone.get());
   box.snaps.slots[1]=chord;box.snaps.used[1]=true;box.snaps.recall(1);
   auto restored=box.captureSound();std::cout<<"Actual chord envelope: "<<restored["Env1_Sustain"]<<std::endl;
   assert(restored["Env1_Sustain"]==1.0f);
   box.findPublishAction("Env1_Sustain")->load(0.0f);
   box.drone=true;
   assert(box.mod.skipRoute(scModulation::SRC_ENV,3));
   assert(!box.mod.skipRoute(scModulation::SRC_ENV,0));
   assert(!box.mod.skipRoute(0,3));
   assert(box.computeVoice(0,0).scalars.at("vca")==0);
   assert(box.captureSound()["Env1_Sustain"]==0);
   box.drone=false;
   assert(!box.mod.skipRoute(scModulation::SRC_ENV,3));
   assert(box.computeVoice(0,0).scalars.at("vca")==1);
   if(const char* score=std::getenv("BOX_SUSTAIN_SCORE")){
    ofxSCServer server("127.0.0.1",9,0);server.beginNRTCapture(true);
    server.setNRTTimeProvider([&](){return server.nrtTime;});server.setNRTTimeProviderEnabled(true);
    {
     scAdditiveBox render(servers);render.setup();render.numOutputs=1;
     ofxOscMessage defs;defs.setAddress("/d_loadDir");defs.addStringArg(std::getenv("BOX_SUSTAIN_DEFS"));server.sendMsg(defs);
     ofxOscMessage root;root.setAddress("/g_new");root.addIntArg(1);root.addIntArg(0);root.addIntArg(0);server.sendMsg(root);
     auto ack=[&](){auto nodes=server.nodeFeedbackFunctions;for(auto& e:nodes)if(e.first->nodeID>0 && !e.first->isCreated() && !e.first->hasEnded()){
      ofxOscMessage go;go.setAddress("/n_go");go.addIntArg(e.first->nodeID);e.first->feedbackListener(go);
     }};
     server.setNRTTime(.1);render.buildSynth(&server);render.setOutputBus(&server,0,0);render.createSynth(&server);ack();
     server.setNRTTime(.2);render.snaps.slots[1]=chord;render.snaps.used[1]=true;render.snaps.recall(1);ack();render.flushDirty();ack();
     server.setNRTTime(.3);render.states.at(&server).curvesPending=false;render.uploadCurves(&server,true);render.flushDirty();
     ofxSCSynth probe("SustainProbe",&server);probe.set("env",render.mod.targetBus(&server,3));probe.set("gatebus",render.states.at(&server).gateBus->index);probe.createAndRun(1,1,true);
     server.setNRTTime(.4);render.gate=std::vector<float>{1};ack();render.flushDirty();
     server.setNRTTime(3);render.gate=std::vector<float>{0};render.flushDirty();
     server.setNRTTime(7);render.findPublishAction("Env1_Sustain")->load(0.0f);render.drone=true;render.flushDirty();ack();
     server.setNRTTime(10);render.drone=false;render.flushDirty();
     server.setNRTTime(14);render.free(&server);
    }
    server.endNRTCapture(14.1);assert(server.writeNRTScore(score,14.1));
   }
   if(const char* bankFile=std::getenv("BOX_MORPH_BENCH_BANK")){
    auto bank=ofLoadJson(bankFile);const auto a=bank["slots"][1],b=bank["slots"][5];
    ofxSCServer server("127.0.0.1",9,0);server.beginNRTCapture(true);
    scAdditiveBox render(servers);render.setup();render.numOutputs=1;render.loadSound(a);
    server.setNRTTimeProvider([&](){return server.nrtTime;});server.setNRTTimeProviderEnabled(true);
    if(const char* definitions=std::getenv("BOX_MORPH_BENCH_DEFS")){
     ofxOscMessage defs;defs.setAddress("/d_loadDir");defs.addStringArg(definitions);server.sendMsg(defs);
     ofxOscMessage root;root.setAddress("/g_new");root.addIntArg(1);root.addIntArg(0);root.addIntArg(0);server.sendMsg(root);
    }
    auto ack=[&](){auto nodes=server.nodeFeedbackFunctions;for(auto& e:nodes)if(e.first->nodeID>0 && !e.first->isCreated() && !e.first->hasEnded()){
     ofxOscMessage go;go.setAddress("/n_go");go.addIntArg(e.first->nodeID);e.first->feedbackListener(go);
    }};
    server.setNRTTime(.1);render.buildSynth(&server);render.setOutputBus(&server,0,0);render.createSynth(&server);ack();render.flushDirty();
    server.setNRTTime(.2);render.states.at(&server).curvesPending=false;render.uploadCurves(&server,true);render.flushDirty();
    std::vector<double> times;int computations=0;size_t events=0;
    for(int i=0;i<10;++i){
     float t=.05f+.09f*i;auto before=render.dataGen;auto count=server.getNRTEventCount();
     server.setNRTTime(.4+25*t);
     auto start=std::chrono::steady_clock::now();
     render.snaps.applyExternalMorph(a,b,t,t>=.5f);ack();render.flushDirty();
     times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
     computations+=render.dataGen-before;events+=server.getNRTEventCount()-count;
    }
    std::sort(times.begin(),times.end());
    std::cout<<"MORPH BENCH median_ms="<<times[times.size()/2]<<" max_ms="<<times.back()
             <<" computations_per_frame="<<computations/10.0<<" messages_per_frame="<<events/10.0<<std::endl;
    if(const char* score=std::getenv("BOX_MORPH_BENCH_SCORE")){
     server.setNRTTime(25.4);render.snaps.applyExternalMorph(a,b,1,true);ack();render.flushDirty();
     server.setNRTTime(26);render.finishVoiceMorph();render.finishPartialMorph();render.flushDirty();ack();
     server.setNRTTime(27);render.drone=false;render.flushDirty();
     server.endNRTCapture(31.1);assert(server.writeNRTScore(score,31.1));
    }
   }
 }
'''
        continue
    # Older presets: explicit Drone retained; Auto and Gated load gated.
    for mode in ((2,0,1,3) if folder in ('physicalBox','steamPipeBox') else (2,0,1)):
        modekey='Env_Mode' if folder in ('analogBox','polyComb','wavetableBox') else 'Gate_Mode'
        if folder in ('wavetableBox','pulsarBox'):
            json=f'ofJson j={{{{"{modekey}",{mode}}}}};'
        else:
            statekey = "steampipeBox" if folder == "steamPipeBox" else folder
            json=f'ofJson j={{{{"{statekey}",{{{{"{modekey}",{mode}}}}}}}}};'
        cases += '{'+json+'box.loadBeforeConnections(j);ofDeserialize(j,box.drone);box.presetRecallAfterSettingParameters(j);assert(box.drone.get()=='+('true' if mode==2 else 'false')+');'+('assert(box.pulsing()=='+('true' if mode==3 else 'false')+');' if folder in ('physicalBox','steamPipeBox') else '')+'}\n'
    cases += '}\n'
cases += 'std::cout << "PASS: Box synths default gated, publish boolean Drone, recall legacy modes; AdditiveBox stores Drone and defaults old presets to gated\\n";\n}\n'
native.CASES=cases
SC=ROOT.parent/'ofxSuperCollider'; OSC=ROOT.parent/'ofxOsc'
sources=[str(native.ROOT/'src/Managers/ofxOceanodeScheduling.cpp')]
sources += [str(ROOT/'src'/p) for p in ('scNode.cpp','serverManager.cpp','ui/scTimelineMenu.cpp','nodes/boxSynths/shared/scModulation.cpp')]
sources += [str(ROOT/f'src/nodes/boxSynths/{folder}/{cls}.cpp') for folder,cls in hosts.items() if folder!='pulsarBox']
sources += [str(p) for p in sorted((SC/'src').glob('*.cpp'))]+[str(p) for p in sorted((OSC/'src').glob('*.cpp'))]
sources += [str(p) for p in sorted((OSC/'libs/oscpack/src').rglob('*.cpp')) if 'win32' not in str(p)]
includes=[ROOT/'src',*sorted({p.parent for p in (ROOT/'src').rglob('*.h')}),SC/'src',OSC/'src',OSC/'libs/oscpack/src',*sorted({p.parent for p in (OSC/'libs/oscpack/src').rglob('*.h')})]
with tempfile.TemporaryDirectory(prefix='box-gates-') as directory:
    wrapper=Path(directory)/'cxx'; wrapper.write_text('#!/bin/sh\nexec clang++ -fno-access-control "$@"\n'); wrapper.chmod(0o755)
    os.environ['CXX']=str(wrapper)
    native.main(extra_sources=sources,extra_includes=includes,std='c++20')
