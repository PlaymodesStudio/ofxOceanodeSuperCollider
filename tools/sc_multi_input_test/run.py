#!/usr/bin/env python3
"""Exercise production connections, presets and SC graph routing without an audio device."""
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT.parent / 'ofxOceanode/tests'))
import macro_mixer_sc_regression as sc
mixer = sc.mixer
mixer.PRELUDE += '\n#include "scAudioBusMixer.h"\n'
# Reuse the graph fixture, but do not link or register MacroMixerSCBus.
# This test must work with the standalone routing implementation alone.
mixer.CASES = mixer.CASES[:mixer.CASES.index('    // Policy coefficients/options')].replace('registerMacroMixerSCBus(managers);registerMacroMixerSCBus(managers);', 'registerSCAudioInputs(managers);registerSCAudioInputs(managers);')+r'''
    auto sums=[&](ofxSCServer& server) {
        int n=0;
        for(const auto& entry:server.synths) if(entry.second.name=="oceanodeAudioBusMixer128") {
            ++n;
            near(entry.second.controls.at("gainA"),1);
            near(entry.second.controls.at("gainB"),1);
            near(entry.second.controls.at("smoothing"),0);
        }
        return n;
    };
    for(int i=1;i<=3;++i) parent.createNode(registry->create("SC Mixer Test Tone"),i);
    auto& effect=static_cast<TestEffect&>(parent.createNode(registry->create("SC Mixer Test Effect"),1).getNodeModel());
    auto& output=parent.createNode(registry->create("SC Output"),1).getNodeModel();
    auto& inlet=static_cast<ofxOceanodeAbstractParameter&>(effect.getParameterGroup().get("In"));
    auto& numeric=static_cast<ofxOceanodeAbstractParameter&>(effect.getParameterGroup().get("Freq"));
    assert(inlet.allowsMultipleInputs() && !numeric.allowsMultipleInputs());
    assert(parent.createConnectionFromInfo("SC_Mixer_Test_Effect_1","Out","SC_Output_1","In"));
    auto connect=[&](int i) {
        return parent.createConnectionFromInfo("SC_Mixer_Test_Tone_"+std::to_string(i),"Out","SC_Mixer_Test_Effect_1","In");
    };
    auto* first=connect(1);assert(first);recompute(m1,s1);
    assert(inlet.getInConnections().size()==1 && sums(s1)==0);
    assert(effect.getParameter<nodePort>("In").get().getNodeRef()->nodeName()=="SC Mixer Test Tone");
    auto* second=connect(2);assert(second);recompute(m1,s1);
    assert(inlet.getInConnections().size()==2 && sums(s1)==1);
    assert(connect(2)==second && inlet.getInConnections().size()==2);
    auto* third=connect(3);assert(third);recompute(m1,s1);
    assert(inlet.getInConnections().size()==3 && sums(s1)==2);
    first->setActive(false);recompute(m1,s1);assert(sums(s1)==1);
    first->setActive(true);recompute(m1,s1);assert(sums(s1)==2);
    third->deleteSelf();recompute(m1,s1);assert(inlet.getInConnections().size()==2 && sums(s1)==1);
    // Every cable survives the existing preset format; no private sum nodes
    // are serialized into modules.json.
    parent.savePreset(scratch+"/multi");
    assert(ofLoadJson(scratch+"/multi/modules.json").size()==3);
    parent.clearContainer();recompute(m1,s1);recompute(m2,s2);
    assert(TestTone::live==0 && ofxSCSynth::live==0);
    assert(parent.loadPreset(scratch+"/multi"));parent.update();recompute(m1,s1);
    auto& restored=parent.getDynamicNodes().at("SC Mixer Test Effect").at(1)->getNodeModel();
    auto& restoredIn=static_cast<ofxOceanodeAbstractParameter&>(restored.getParameterGroup().get("In"));
    assert(restoredIn.getInConnections().size()==2 && sums(s1)==1);
    auto& restoredOut=parent.getDynamicNodes().at("SC Output").at(1)->getNodeModel();
    restoredOut.getParameter<int>("Server")=1;recompute(m1,s1);recompute(m2,s2);
    assert(sums(s2)==1 && sums(s1)==0);
    restoredOut.getParameter<int>("Server")=0;recompute(m2,s2);recompute(m1,s1);
    assert(sums(s1)==1 && sums(s2)==0);
    // Deleting a source removes its cable and restores the direct route.
    parent.getDynamicNodes().at("SC Mixer Test Tone").at(2)->deleteSelf();
    parent.update();recompute(m1,s1);
    assert(restoredIn.getInConnections().size()==1 && sums(s1)==0);
    restoredIn.removeAllConnections();recompute(m1,s1);
    assert(!restoredIn.hasInConnection() && !restored.getParameter<nodePort>("In").get().getNodeRef());
    parent.clearContainer();recompute(m1,s1);recompute(m2,s2);
    assert(liveGui==0 && TestTone::live==0 && ofxSCSynth::live==0);
    fs::current_path(cwd);
    std::cout<<"ScBus multi-input passed: direct route, unity sums, three sources, duplicate protection, activation, disconnection, preset reload, source deletion, server switching and teardown; ordinary numeric ports remain single-input.\n";
}
'''
mixer.main(extra_includes=[mixer.ROOT/'tests/support/macro_mixer_sc',ROOT/'src',ROOT.parent/'ofxOsc/src',ROOT.parent/'ofxOsc/libs/oscpack/src'],
    extra_sources=[str(ROOT.parent/'ofxOsc/src/ofxOscMessage.cpp')]+[str(ROOT/'src'/name) for name in ('scNode.cpp','scOutput.cpp','scAudioBusMixer.cpp')])
