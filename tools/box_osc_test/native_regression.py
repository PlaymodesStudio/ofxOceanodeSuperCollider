#!/usr/bin/env python3
"""Use real Box nodes/OF parameters and captured OSC to test scheduled-note isolation.

Extends the native gate-mode fixture. No sound device or live SC server is used.
"""
from pathlib import Path
import os,runpy,sys
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root.parent/'ofxOceanode/tests'))
import macro_mixer_regression as native
original=native.main
native.PRELUDE+=f'#include "{root}/src/nodes/boxSynths/sampleBox/scSampleBox.h"\n'
extra=r'''
 {
    ofxSCServer capture("127.0.0.1",9,0);capture.beginNRTCapture(true);
    auto ack=[&](){auto nodes=capture.nodeFeedbackFunctions;for(const auto&e:nodes)
        if(e.first->nodeID>0 && !e.first->isCreated() && !e.first->hasEnded()){
            ofxOscMessage go;go.setAddress("/n_go");go.addIntArg(e.first->nodeID);e.first->feedbackListener(go);
        }};
    auto writes=[&](size_t from,const std::set<int>& ids,const std::string& key){
        size_t count=0;
        for(size_t i=from;i<capture.nrtEvents.size();++i){
            const auto& bundle=capture.nrtEvents[i].bundle;
            for(int m=0;m<bundle.getMessageCount();++m){
                const auto& message=bundle.getMessageAt(m);
                if(message.getAddress()!="/n_set" && message.getAddress()!="/n_setn")continue;
                if(!message.getNumArgs() || !ids.count(message.getArgAsInt(0)))continue;
                for(int a=1;a<message.getNumArgs();++a)
                    if(message.getArgType(a)==OFXOSC_TYPE_STRING && message.getArgAsString(a)==key)++count;
            }
        }
        return count;
    };
    auto event=[](auto handle,const std::string& value,uint64_t due){
        ofxOceanodeScheduledParameterEvent e;e.value=value;e.dueSteadyTimeUs=due;
        assert(ofxOceanodeScheduling::dispatch(handle.get(),e));
    };
    const uint64_t due=ofxOceanodeScheduling::steadyNowUs()+1000000;
    ofEventArgs args;
    {
        scAdditiveBox box(servers);box.setup();box.numOutputs=1;box.partials=32;
        box.buildSynth(&capture);box.setOutputBus(&capture,0,0);box.createSynth(&capture);ack();box.flushDirty();
        auto& st=box.states.at(&capture);assert(!st.noteWriters.empty());
        std::set<int> ids;for(auto*w:st.noteWriters)ids.insert(w->nodeID);
        for(const auto&v:st.voices){if(v.tail)ids.insert(v.tail->nodeID);for(const auto&u:v.cores)for(auto*c:u)ids.insert(c->nodeID);}
        event(box.pitchIn.handle,"62",due-2000);event(box.gateIn.handle,"1",due);
        const auto from=capture.nrtEvents.size();box.tilt=std::vector<float>{-2};box.flushDirty();
        assert(writes(from,ids,"gate")==0);
        event(box.gateIn.handle,"0",due+100000);assert(writes(from,ids,"gate")>0);
    }
    {
        scWavetableBox box(servers);box.setup();box.buildSynth(&capture);box.setOutputBus(&capture,0,0);box.createSynth(&capture);ack();
        const auto g=box.publishedHandles.at(box.gate.getEscapedName()),p=box.publishedHandles.at(box.pitch.getEscapedName());
        const std::set<int> ids{box.modStates.at(&capture).gateSynth->nodeID};
        event(p,"64",due-2000);event(g,"1",due);
        const auto from=capture.nrtEvents.size();box.warp1=std::vector<int>{1};box.update(args);
        assert(writes(from,ids,"gate")==0);
        event(g,"0",due+100000);assert(writes(from,ids,"gate")==1);
        {ofxOceanodeScheduling::ScopedBackendSuppression suppress;box.gate=std::vector<float>{0};box.pitch=std::vector<float>{64};}
        assert(writes(from,ids,"gate")==1); // GUI catch-up sends no duplicate
    }
    {
        scPulsarBox box(servers);box.setup();box.buildSynth(&capture);box.setOutputBus(&capture,0,0);box.createSynth(&capture);ack();
        auto&st=box.states.at(&capture);std::set<int> ids{st.gateS->nodeID};
        for(const auto&e:st.banks)ids.insert(e.second.synth->nodeID);
        event(box.pitchIn.handle,"67",due-2000);event(box.gateIn.handle,"1",due);
        const auto from=capture.nrtEvents.size();box.volume=.63f;box.update(args);
        assert(writes(from,ids,"gate")==0);
        event(box.gateIn.handle,"0",due+100000);assert(writes(from,ids,"gate")>0);
    }
    {
        scAnalogBox box(servers);box.setup();box.buildSynth(&capture);box.setOutputBus(&capture,0,0);box.createSynth(&capture);ack();
        const std::set<int> ids{box.states.at(&capture).gateS->nodeID};
        const auto from=capture.nrtEvents.size();event(box.pitchIn.handle,"69",due-2000);
        assert(writes(from,ids,"gate")==0); // allocator pitch preparation cannot open/close gates
        event(box.gateIn.handle,"1",due);assert(writes(from,ids,"gate")==1);
    }
    {
        scGrainBoxPoly box(servers);box.setup();auto&ph=box.ph0();ph.modOn=true;
        box.ensureModResources(ph,&capture);box.writeGate(ph,&capture);
        const auto&st=ph.modSC.at(&capture);assert(st.vox && st.gateInitialized);
        const int gateBus=st.vox->index+(scGrainBoxPoly::MOD_EXTS+scGrainBoxPoly::MOD_LFOS)*st.voxCap;
        const auto from=capture.nrtEvents.size();box.writeMod(ph,&capture);
        for(size_t i=from;i<capture.nrtEvents.size();++i){const auto&b=capture.nrtEvents[i].bundle;
            for(int j=0;j<b.getMessageCount();++j){const auto&m=b.getMessageAt(j);if(m.getAddress()!="/c_setn")continue;
                for(int a=0;a<m.getNumArgs();){const int bus=m.getArgAsInt(a++),count=m.getArgAsInt(a++);
                    assert(gateBus<bus || gateBus>=bus+count);a+=count;}
            }
        }
        box.freeModResources(ph,&capture);
    }

    {
        scSampleBox box(servers);box.setup();capture.nrtSyncOutstanding=0;
        box.buildSynth(&capture);box.setOutputBus(&capture,0,0);box.createSynth(&capture);
        auto& pad=*box.pads[0];box.nodeSC.at(&capture).padsPending=false;
        if(!pad.sc.count(&capture))box.createPad(&capture,pad);ack();
        const std::set<int> ids{pad.sc.at(&capture).engine->nodeID};
        event(box.pitchIn.handle,"72",due-2000);event(box.triggerHandle,"1",due);
        const auto from=capture.nrtEvents.size();pad.attack=.05f;pad.release=.25f;pad.filterType=1;
        assert(writes(from,ids,"gate")==0 && writes(from,ids,"pitch")==0);
        assert(writes(from,ids,"fire")==0 && writes(from,ids,"stop")==0);
        event(box.triggerHandle,"0",due+100000);assert(writes(from,ids,"gate")==1);
    }
    {
        // Inputs hold the value due at the event's instant, even when a later
        // one was handed over first, and cables never see the early value.
        ofParameter<std::vector<float>> gp("Sched Gate",{0},{0},{1}),pp("Sched Pitch",{48},{0},{127}),sp("Sched Sink",{0},{0},{1});
        auto gh=std::make_shared<ofxOceanodeParameter<std::vector<float>>>();gh->bindParameter(gp);
        auto ph=std::make_shared<ofxOceanodeParameter<std::vector<float>>>();ph->bindParameter(pp);
        ofxOceanodeParameter<std::vector<float>> sinkHandle;sinkHandle.bindParameter(sp);
        ofxOceanodeConnection<std::vector<float>,std::vector<float>> cable(*gh,sinkHandle,true);
        scScheduledInputs sched;sched.add(gh,gp);sched.add(ph,pp);
        std::vector<float> pitchAtGate;
        auto listener=gp.newListener([&](std::vector<float>&){if(sched.isSending())pitchAtGate=pp.get();});
        int owner=0;sched.enable(&owner,[]{return true;});
        event(ph,"62",due-2000);    // this note's pitch
        event(ph,"60",due+50000);   // the next note's, handed over before this gate
        event(gh,"1",due);
        assert(pitchAtGate==std::vector<float>({62}));
        assert(gp.get()==std::vector<float>({0}) && pp.get()==std::vector<float>({48})); // frame values restored
        assert(sp.get()==std::vector<float>({0})); // the cable waits for the frame
        gp=std::vector<float>{1};assert(sp.get()==std::vector<float>({1}));
        sched.disable();
    }
    capture.endNRTCapture();
    std::cout<<"PASS: scheduled inputs read their value at the event's instant and cables ignore early values; real Additive/Pulsar/Wavetable sound automation never rewrites scheduled gates; note-offs and GUI suppression preserved; Analog allocator pitch does not resend gate; Grain modulation ranges exclude initialized gate buses; Sample envelope/filter automation leaves pitch, gate and trigger counters untouched.\n";
 }
'''
def main(*args,**kwargs):
    native.CASES=native.CASES.replace('std::cout << "PASS: Box synths',extra+'\nstd::cout << "PASS: Box synths',1)
    kwargs["extra_sources"]=[*kwargs.get("extra_sources",()),str(root/"src/nodes/boxSynths/sampleBox/scSampleBox.cpp"),str(root/"src/nodes/boxSynths/slicerBox/scSlicerBox.cpp")]
    return original(*args,**kwargs)
native.main=main
os.environ.pop('OCEANODE_TEST_BOX',None)
runpy.run_path(str(root/'tools/tests/box_gate_mode_test.py'),run_name='__main__')
