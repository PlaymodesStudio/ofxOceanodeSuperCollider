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
}
native.PRELUDE += native.block((native.ROOT/'src/Nodes/ofxOceanodeNodeModel.cpp').read_text(), 'void ofxOceanodeNodeModel::removeSeparator(') + '\n'
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
    if folder in ('wavetableBox','pulsarBox'):
        cases += 'box.publishByKey("Drone");\n'
    else:
        cases += 'box.publish("Drone");\n'
    cases += 'assert(box.getParameterGroup().get("Drone").type()==typeid(ofParameter<bool>).name());\n'
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
cases += 'std::cout << "PASS: all nine Box synths default gated, publish boolean Drone, and recall legacy modes\\n";\n}\n'
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
