#!/usr/bin/env python3
"""AnalogBox oscillator def swap (ABOsc <-> ABOscMorph) and cached control batches.

Real scAnalogBox + OF parameters, captured OSC (no server, no audio device).
Runs on the native gate-mode fixture (tools/tests/box_gate_mode_test.py),
restricted to AnalogBox.
  - oscillators start as ABOsc (one MorphOsc); osc 1 never morphs
  - a morph frame with nothing changed sends no oscillator / filter controls
  - Sync at a fractional activation swaps oscillator 2 to ABOscMorph (the old
    synth faded by presence, freed later), and back to ABOsc once it settles
"""
from pathlib import Path
import os,runpy,sys
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root.parent/'ofxOceanode/tests'))
import macro_mixer_regression as native
original=native.main
extra=r'''
 {
    // (leaked: oscpack's receive thread races the socket's teardown under ASan)
    auto& capture=*new ofxSCServer("127.0.0.1",9,0);capture.beginNRTCapture(true);
    auto ack=[&](){auto nodes=capture.nodeFeedbackFunctions;for(const auto&e:nodes)
        if(e.first->nodeID>0 && !e.first->isCreated() && !e.first->hasEnded()){
            ofxOscMessage go;go.setAddress("/n_go");go.addIntArg(e.first->nodeID);e.first->feedbackListener(go);
        }};
    auto each=[&](size_t from,auto f){
        for(size_t i=from;i<capture.nrtEvents.size();++i){const auto&b=capture.nrtEvents[i].bundle;
            for(int m=0;m<b.getMessageCount();++m)f(b.getMessageAt(m));}
    };
    auto controls=[&](size_t from,const std::set<int>& ids){
        size_t n=0;each(from,[&](const ofxOscMessage&m){
            if((m.getAddress()=="/n_set"||m.getAddress()=="/n_setn") && m.getNumArgs() && ids.count(m.getArgAsInt(0)))++n;});
        return n;
    };
    auto created=[&](size_t from,const std::string& prefix){
        std::vector<std::string> out;each(from,[&](const ofxOscMessage&m){
            if(m.getAddress()=="/s_new" && m.getArgAsString(0).rfind(prefix,0)==0)out.push_back(m.getArgAsString(0));});
        return out;
    };
    auto freed=[&](size_t from,int id){
        bool f=false;each(from,[&](const ofxOscMessage&m){if(m.getAddress()=="/n_free" && m.getArgAsInt(0)==id)f=true;});
        return f;
    };
    ofEventArgs args;
    scAnalogBox box(servers);box.setup();box.oscCount=2;
    box.buildSynth(&capture);box.setOutputBus(&capture,0,0);box.createSynth(&capture);ack();
    auto& st=box.states.at(&capture);
    assert(st.osc[0] && st.osc[1] && !st.oscMorph[0] && !st.oscMorph[1]);
    assert(created(0,"ABOscMorph").empty() && created(0,"ABOsc").size()==2);
    // cached batches: an unchanged morph frame sends nothing to the oscillators / filter
    auto a=box.snaps.captureForExternalMorph(),b=a;
    box.snaps.applyExternalMorph(a,a,.5f,false);ack();
    std::set<int> ids{st.osc[0]->nodeID,st.osc[1]->nodeID,st.filter->nodeID};
    auto from=capture.nrtEvents.size();
    box.snaps.applyExternalMorph(a,a,.6f,false);
    assert(controls(from,ids)==0);
    std::set<int> sources;
    for(auto* s : box.modulation.states.at(&capture).lfo) if(s) sources.insert(s->nodeID);
    for(auto* s : box.modulation.states.at(&capture).env) if(s) sources.insert(s->nodeID);
    assert(!sources.empty() && controls(from,sources)==0);
    // a change goes out alone
    b["controls"][box.osc[1].skew.getEscapedName()]=.5f;
    from=capture.nrtEvents.size();box.snaps.applyExternalMorph(a,b,.5f,false);
    assert(controls(from,ids)>0);
    // Sync morph: oscillator 2 swapped to ABOscMorph at the first fractional value
    b=a;b["controls"][box.osc[1].sync.getEscapedName()]=true;
    const int oldId=st.osc[1]->nodeID;
    from=capture.nrtEvents.size();box.snaps.applyExternalMorph(a,b,.3f,false);ack();
    assert(st.oscMorph[1] && !st.oscMorph[0] && st.osc[1]->nodeID!=oldId && st.retiring.size()==1);
    assert(created(from,"ABOscMorph").size()==1);
    from=capture.nrtEvents.size();box.snaps.applyExternalMorph(a,b,.7f,false);ack();
    assert(created(from,"ABOsc").empty());                                  // no swap while it morphs
    // settled at 1: still ABOscMorph until the lag lands, then back to ABOsc
    box.snaps.applyExternalMorph(a,b,1.f,true);box.update(args);ack();
    assert(st.oscMorph[1]);
    const int morphId=st.osc[1]->nodeID;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    from=capture.nrtEvents.size();box.update(args);ack();
    assert(!st.oscMorph[1] && st.osc[1]->nodeID!=morphId);
    auto back=created(from,"ABOsc");assert(back.size()==1 && back[0].rfind("ABOscMorph",0)!=0);
    assert(freed(from,oldId));                                              // the first one, faded out
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    from=capture.nrtEvents.size();box.update(args);
    assert(freed(from,morphId) && st.retiring.empty());
    // a full resend (preset recall / NRT) sends everything again
    from=capture.nrtEvents.size();box.resendParametersForNRT();
    assert(controls(from,{st.osc[0]->nodeID,st.osc[1]->nodeID,st.filter->nodeID})>=3);
    box.free(&capture);assert(st.retiring.empty());
    capture.endNRTCapture();
    std::cout<<"PASS: AnalogBox runs ABOsc, swaps to ABOscMorph only during a fractional Sync and back once settled; cached batches send only changes\n";
 }
'''
def main(*args,**kwargs):
    native.CASES=native.CASES.replace('std::cout << "PASS: Box synths',extra+'\nstd::cout << "PASS: Box synths',1)
    return original(*args,**kwargs)
native.main=main
native.PRELUDE+='#include <thread>\n#include <chrono>\n#include <set>\n'
os.environ['OCEANODE_TEST_BOX']='analogBox'
runpy.run_path(str(root/'tools/tests/box_gate_mode_test.py'),run_name='__main__')
