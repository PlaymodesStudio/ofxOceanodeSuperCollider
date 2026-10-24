#!/usr/bin/env python3
"""Exercise the production temporary voice-bank lifecycle without an OF window."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'src/nodes/scPolyCombVoiceMorph.inl').read_text()
polycomb=(ROOT/'src/nodes/scPolyComb.cpp').read_text()
apply_state=polycomb.split('void scPolyComb::applyState(',1)[1]
voice_count=apply_state.split('    if(!(voicesHandle && voicesHandle->hasInConnection())){',1)[1].split('\n\n    // the controls:',1)[0]
voice_count='    if(!(voicesHandle && voicesHandle->hasInConnection())){'+voice_count
def function(text, signature):
    start=text.index(signature)
    opening=text.index('{',start)
    depth=1
    end=opening+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}')
        end+=1
    return text[start:end]
snapshots=(ROOT/'src/nodes/scSnapshots.h').read_text()
snapshot_setup=snapshots.split('        host = std::move(h);',1)[1].split('        manager.bindSlots',1)[0]
snapshot_methods='\n'.join(function(snapshots,s) for s in ('ofJson captureForExternalMorph()', 'void applyExternalMorph('))
macro_methods='\n'.join(function(polycomb,s) for s in ('bool scPolyComb::morphCheck(', 'ofJson scPolyComb::snapshotOf(', 'bool scPolyComb::morphBegin(', 'void scPolyComb::morphApply(', 'void scPolyComb::morphEnd('))
header=r'''
#include "scVoiceCountMorph.h"
#include "scMorphPolicies.h"
#include <array>
#include <cassert>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <utility>
float clockNow=0;
float ofGetElapsedTimef(){return clockNow;}
std::string ofToString(int n){return std::to_string(n);}
struct ofEventArgs{};
struct ofxSCServer{bool isNRTCapturing()const{return false;}bool isNRTCaptureOnly()const{return false;}bool latency=false;bool getBLatency()const{return latency;}void setBLatency(bool v){latency=v;}};
constexpr int RATE_AUDIO=1;
struct ofxSCBus {
    static inline int live=0,next=100;
    int index=next++;
    ofxSCBus(int,int,ofxSCServer*){++live;}
    ~ofxSCBus(){--live;}
    void free(){}
};
bool liveAudioBus(ofxSCServer*,ofxSCBus* b){return b!=nullptr;}
bool liveControlBus(ofxSCServer*,ofxSCBus* b){return b!=nullptr;}
struct ofxSCSynth {
    static inline int live=0,next=2000;
    int nodeID=next++; bool created=true;
    std::map<std::string,ofJson> values;
    ofxSCSynth(std::string,ofxSCServer*){++live;}
    ~ofxSCSynth(){--live;}
    template<class T> void set(std::string k,T v){values[k]=v;}
    bool isCreated()const{return created;}
    void resendStoredArgs(){}
    void free(){}
    void moveBefore(int){}
};
template<class S,class V,class F> void scStartConfiguredSynth(S*,V*,int,int,F f){f();}
struct ofxSCGroup{int nodeID=100;void moveBefore(int){} void free(){}};
template<class T> struct Param {
    T value;
    const T& get()const{return value;}
    void set(T v){value=v;}
    void setWithoutEventNotifications(T v){value=v;}
    std::string getEscapedName()const{return "Voices";}
};
float ofClamp(float v,float lo,float hi){return std::clamp(v,lo,hi);}
// The production policy wrapper and external entry points; only the library/UI
// setup is omitted from this audio-resource lifecycle fixture.
class scSnapshots {public:
    struct Host {
        std::function<ofJson()> capture;
        std::function<void(const ofJson&,const ofJson&,float,bool)> apply;
        std::function<bool(const std::string&)> morphBlocked;
        std::function<void()> morphChanged;
    } host;
    bool running=false;
    scMorphPolicies morph;
    void setup(Host h){host=std::move(h); SNAPSHOT_SETUP }
    SNAPSHOT_METHODS
};
struct Handle{bool connected=false;bool hasInConnection()const{return connected;}};
struct Input {
    std::shared_ptr<int> port;
    std::shared_ptr<Handle> handle;
    std::map<ofxSCServer*,int> buses;
};
struct Notes {int inEffectNow(){return 0;}};
struct Mod{void voicesChanged(){} void rebuild(ofxSCServer*){} bool adoptServerState(Mod&,ofxSCServer*){return true;} bool hasVoiceBank(ofxSCServer*,int)const{return true;}};
class scPolyComb {public:
    static constexpr int MAX_VOICES=32,SlGate=0,SlSpread=1,SlComp=2,NumOsc=1,NumIns=1;
    bool voiceRenderer=false,applyingSnapshot=false,syncingVoiceMorph=false;
    int morphRenderVoices=0,voiceAliasCount=0,morphDomainVoices=0;
    struct Fx {ofxSCSynth* synth=nullptr;};
    struct Part{ofxSCSynth* synth=nullptr;};
    struct ServerState {
        ofxSCServer* server=nullptr;
        bool created=false;
        int builtVoices=4,spreadLanes=16;
        bool modCreated=false;
        ofxSCSynth *gateS=nullptr,*ampEnv=nullptr,*pitchS=nullptr,*amp=nullptr,*delta=nullptr;
        ofxSCBus *gateBus=nullptr,*envBus=nullptr,*fmBus=nullptr,*oscBus=nullptr,*voiceBus=nullptr,*noteBus=nullptr,*visBlock=nullptr;
        Part fm,sub;
        std::array<Part,NumOsc> osc{};
        std::array<Part,NumIns> ins{};
        std::array<ofxSCBus*,NumIns> insBus{};
        ofxSCSynth* voiceMix=nullptr,*spread=nullptr;
        ofxSCBus* voiceMainBus=nullptr,*scratch=nullptr;
        std::array<ofxSCBus*,MAX_VOICES> voicePeerBuses{};
        float voiceMixReady=0;
        std::vector<float> voiceMixInputs,voiceMixWeights;
        std::array<Fx,2> fx{};
        ofxSCGroup* root=nullptr;
        std::array<ofxSCGroup*,3> groups{};
    };
    std::map<ofxSCServer*,ServerState> states;
    std::vector<int> servers;
    std::shared_ptr<Handle> voicesHandle=std::make_shared<Handle>();
    Param<int> voices{4},outChannels{2},spreadLayout{0};
    struct Control{std::string key;std::function<ofJson()>save;std::function<void(const ofJson&)>load;};
    std::vector<Control> controls;
    float externalValue=0;
    Param<std::vector<float>> gate{{1}},pitch{{60}};
    Param<float> master{.8f};
    int userVoices=4;
    std::vector<int> scrambleOrder; std::vector<float> previousMapGate;
    Input gateIn,pitchIn;
    bool gateTouched=false;
    float currentBpm=120,rebuildAt=-1;
    Notes noteInputs;
    Mod modulation;
    scSnapshots snaps;
    ofJson morphFrom,morphTo;
    struct VoicePeer{std::unique_ptr<scPolyComb> node;ofJson lastSound;float unusedSince=-1;};
    std::map<int,VoicePeer> voicePeers;
    std::map<ofxSCServer*,int> rendererBefore;
    std::vector<float> voiceMorphWeights;
    int voiceHandoff=0,rebuilds=0;
    float voiceEndpointSince=-1,voiceHandoffAt=-1;
    explicit scPolyComb(std::vector<int> s={},bool r=false):voiceRenderer(r),servers(s){}
    ~scPolyComb(){
        voicePeers.clear();
        for(auto& [server,st]:states)freeAll(server,st);
    }
    int n()const{return morphRenderVoices>0?morphRenderVoices:voices.get();}
    void setup(){
        controls.push_back({"Ext_1",[this](){return ofJson(externalValue);},[this](const ofJson&v){externalValue=v.get<float>();}});
        if(!voiceRenderer){scSnapshots::Host h;
            h.capture=[this](){return captureSnapshot();};
            h.apply=[this](const ofJson&a,const ofJson&b,float t,bool sw){applyState(a,b,t,sw);};
            snaps.setup(h);
        }
    }
    bool morphCheck(const ofJson&,std::string&)const;
    ofJson snapshotOf(const ofJson&)const;
    bool morphBegin(const ofJson&,std::string&);
    void morphApply(float,bool);
    void morphEnd(bool);
    void updatePitchIndexLimits(){} void updateScramble(){}
    void setBpm(float v){currentBpm=v;}
    bool voicesFromSnapshot=false;
    void applyState(const ofJson& a,const ofJson& b,float t,bool){
        if(!voiceRenderer)prepareVoiceMorph(a,b,t);
        // Production count recall block, inserted below.
        VOICE_COUNT_BLOCK
        syncVoiceMorph();
    }
    ofJson captureSnapshot()const{
        ofJson j={{"voices",voices.get()},{"controls",ofJson::object()}};
        scVoiceCountMorph::capture(j,voiceMorphWeights);return j;
    }
    void buildSynth(ofxSCServer* s){states[s].created=false;}
    void setOutputBus(ofxSCServer*,int,int){}
    void createSynth(ofxSCServer* s){
        auto&st=states[s];st.server=s;st.created=true;st.builtVoices=n();
        st.spread=new ofxSCSynth("spread",s);st.root=new ofxSCGroup;
        for(auto*&g:st.groups)g=new ofxSCGroup;
        st.scratch=new ofxSCBus(RATE_AUDIO,32,s);
    }
    void freeAll(ofxSCServer* s,ServerState& st){
        releaseVoiceMorphServer(s,st);delete st.spread;st.spread=nullptr;delete st.scratch;st.scratch=nullptr;
        for(auto*&g:st.groups){delete g;g=nullptr;}delete st.root;st.root=nullptr;st.created=false;
    }
    void free(ofxSCServer* s){if(states.count(s))freeAll(s,states[s]);}
    void routeChain(ServerState&){}
    void freeSynths(ServerState& st){delete st.spread;st.spread=nullptr;for(auto& f:st.fx)assert(!f.synth);}
    void createAll(ofxSCServer* s,ServerState& st){++rebuilds;st.builtVoices=n();st.spread=new ofxSCSynth("spread",s);}
    void update(ofEventArgs&){}
    void prepareVoiceMorph(const ofJson&,const ofJson&,float);
    void configureVoiceMix(ServerState&);
    void syncVoiceMorph();
    void updateVoiceMorph(ofEventArgs&);
    void releaseVoiceMorphServer(ofxSCServer*,ServerState&);
    void releaseVoiceMorph();
    bool adoptVoiceBank(int);
};
'''
header=header.replace('VOICE_COUNT_BLOCK',voice_count)
header=header.replace('SNAPSHOT_SETUP',snapshot_setup).replace('SNAPSHOT_METHODS',snapshot_methods)
source+='\n'+macro_methods
test=r'''
void near(float a,float b){assert(std::abs(a-b)<1e-5);}
int main(){
    ofxSCServer server;ofEventArgs args;
    {
        scPolyComb node;node.setup();node.externalValue=.7f;node.spreadLayout.set(1);
        node.createSynth(&server);
        auto& st=node.states[&server];
        ofxSCSynth effects("shared reverb",&server);st.fx[0].synth=&effects;
        const int baseSynths=ofxSCSynth::live,baseBuses=ofxSCBus::live;
        ofJson a={{"voices",4}},b={{"voices",12}},c={{"voices",24}};
        // Ordinary playback and same-count morphs allocate nothing.
        for(int i=0;i<100;++i){node.prepareVoiceMorph(a,a,i/100.f);node.updateVoiceMorph(args);}
        assert(node.voicePeers.empty() && node.voiceMorphWeights.empty());
        assert(ofxSCSynth::live==baseSynths && ofxSCBus::live==baseBuses);
        auto apply=[&](const ofJson& from,const ofJson& to,float t){
            node.applyState(from,to,t,t>=.5f);
        };
        apply(a,b,0);
        assert(node.voices.get()==4 && node.voicePeers.empty() && !st.voiceMix);
        assert(ofxSCSynth::live==baseSynths && ofxSCBus::live==baseBuses);
        // First slider movement, including Ease/Exp curves at raw 0.001.
        // No epsilon or discrete switch may postpone the destination bank.
        apply(a,b,1e-9f);
        assert(node.voices.get()==12 && node.voicePeers.count(12) && node.n()==4);
        clockNow=.03f;node.updateVoiceMorph(args);
        assert(st.voiceMixWeights[12]>0);
        apply(a,b,.00101f);
        assert(node.voices.get()==12);
        near(st.voiceMixWeights[12],.00101f);
        apply(a,b,.499f);
        const int earlyDestination=node.voicePeers.at(12).node->states.at(&server).spread->nodeID;
        apply(a,b,.501f);
        assert(node.voices.get()==12 && node.rebuilds==0);
        assert(node.voicePeers.at(12).node->states.at(&server).spread->nodeID==earlyDestination);
        apply(a,b,.49f);
        near(node.voicePeers.at(12).node->externalValue,.7f);
        assert(node.voicePeers.at(12).node->spreadLayout.get()==1);
        assert(st.builtVoices==4 && node.n()==4 && node.voicePeers.count(12));
        clockNow=.1f;node.updateVoiceMorph(args);
        near(st.voiceMixWeights[0],.51f);near(st.voiceMixWeights[12],.49f);
        apply(a,b,.51f);near(st.voiceMixWeights[0],.49f);near(st.voiceMixWeights[12],.51f);
        assert(st.builtVoices==4 && node.rebuilds==0 && st.fx[0].synth==&effects);
        // Store/interrupt a two-count blend; retain both old contributors.
        auto current=node.captureSnapshot();
        apply(current,c,.5f);
        near(st.voiceMixWeights[0],.245f);near(st.voiceMixWeights[12],.255f);near(st.voiceMixWeights[24],.5f);
        assert(node.voicePeers.size()==2 && node.rebuilds==0);
        // Settle at B: adopt the live target bank; no node/envelope restarts.
        const int destination=node.voicePeers.at(12).node->states.at(&server).spread->nodeID;
        apply(a,b,1);clockNow=.2f;node.updateVoiceMorph(args);
        clockNow=.41f;node.updateVoiceMorph(args);
        assert(node.voiceHandoff==2 && node.n()==12 && st.builtVoices==12);
        assert(st.spread->nodeID==destination && node.rebuilds==0);
        assert(st.fx[0].synth==&effects && node.voicePeers.empty());
        clockNow=.52f;node.updateVoiceMorph(args);assert(node.voiceHandoff==2);
        near(st.voiceMixWeights[0],1);near(st.voiceMixWeights[12],0);
        clockNow=.73f;node.updateVoiceMorph(args);
        assert(node.voicePeers.empty() && node.voiceMorphWeights.empty() && !st.voiceMix);
        assert(node.n()==12 && node.voices.get()==12);
        assert(ofxSCSynth::live==baseSynths && ofxSCBus::live==baseBuses);
        // Reversing back to the original count also leaves one normal bank.
        apply(b,a,.5f);clockNow=.8f;node.updateVoiceMorph(args);
        apply(b,a,1);clockNow=.9f;node.updateVoiceMorph(args);
        clockNow=1.11f;node.updateVoiceMorph(args);
        clockNow=1.22f;node.updateVoiceMorph(args);
        clockNow=1.43f;node.updateVoiceMorph(args);
        assert(node.voicePeers.empty() && node.voiceMorphWeights.empty() && node.n()==4);
        assert(ofxSCSynth::live==baseSynths && ofxSCBus::live==baseBuses);
        // A future-timetagged /s_new may not exist on the realtime server yet.
        // Keep the old bank and wait for creation + the coefficient fade.
        ofJson wide={{"voices",32}};
        apply(a,wide,1);st.voiceMix->created=false;st.voiceMixReady=-1;
        clockNow=2;node.updateVoiceMorph(args);clockNow=3;node.updateVoiceMorph(args);
        assert(node.n()==4 && node.voicePeers.count(32) && node.voiceHandoff==0);
        st.voiceMix->created=true;clockNow=3.1f;node.updateVoiceMorph(args);
        clockNow=3.2f;node.updateVoiceMorph(args);clockNow=3.41f;node.updateVoiceMorph(args);
        assert(node.n()==32 && node.voiceHandoff==2);
        clockNow=3.62f;node.updateVoiceMorph(args);
        assert(node.voicePeers.empty() && !st.voiceMix && ofxSCSynth::live==baseSynths && ofxSCBus::live==baseBuses);
        // Legacy counts and corrupted optional metadata fall back safely.
        auto legacy=scVoiceCountMorph::endpoint(ofJson::object(),12,32);assert(scVoiceCountMorph::single(legacy)==12);
        auto bad=scVoiceCountMorph::endpoint({{"voices",4},{"_voiceCountBlend",{1,2}}},12,32);
        assert(scVoiceCountMorph::single(bad)==4);
        // Descending morphs retain the wider source until its last contribution.
        assert(scVoiceCountMorph::activeCount(scVoiceCountMorph::blend(b,a,.00101f,4,32))==12);
        assert(scVoiceCountMorph::activeCount(scVoiceCountMorph::blend(b,a,.999f,4,32))==12);
        assert(scVoiceCountMorph::activeCount(scVoiceCountMorph::blend(b,a,1,4,32))==4);
        node.voicesHandle->connected=true;
        node.voices.set(7);apply(a,b,.00101f);assert(node.voices.get()==7);
        st.fx[0].synth=nullptr;
    }
    assert(ofxSCSynth::live==0 && ofxSCBus::live==0);
    {
        // Actual MacroMixer hooks must produce the snapshot's bank coefficients
        // and lifetime behavior, including interruption from a blended sound.
        scPolyComb snapshot,macro;snapshot.setup();macro.setup();
        snapshot.createSynth(&server);macro.createSynth(&server);
        const int baselineSynths=ofxSCSynth::live,baselineBuses=ofxSCBus::live;
        ofJson a={{"voices",4}},b={{"voices",12}};
        ofJson target={{"polyComb",ofJson::object()},{"Voices",12}};
        std::string reason;
        macro.snaps.running=true;
        assert(macro.morphBegin(target,reason)&&!macro.snaps.running);
        for(float t:{0.f,.00101f,.25f,.49f,.51f,.9f}){
            clockNow+=.1f;
            snapshot.snaps.applyExternalMorph(a,b,t,t>=.5f);
            macro.morphApply(t,t>=.5f);
            clockNow+=.03f;snapshot.updateVoiceMorph(args);macro.updateVoiceMorph(args);
            assert(snapshot.voices.get()==macro.voices.get());
            assert(snapshot.voiceMorphWeights==macro.voiceMorphWeights);
            assert(snapshot.states.at(&server).voiceMixWeights==macro.states.at(&server).voiceMixWeights);
        }
        macro.morphApply(.25f,false);
        const auto before=macro.voiceMorphWeights;
        assert(macro.morphBegin({{"polyComb",ofJson::object()},{"Voices",24}},reason));
        macro.morphApply(.4f,false);
        near(macro.voiceMorphWeights[3],.45f);
        near(macro.voiceMorphWeights[11],.15f);
        near(macro.voiceMorphWeights[23],.4f);
        macro.morphEnd(false);assert(macro.voiceMorphWeights==before);
        assert(macro.morphBegin(target,reason));macro.morphApply(1,true);macro.morphEnd(true);
        snapshot.snaps.applyExternalMorph(a,b,1,true);
        for(int i=0;i<10;++i){clockNow+=.1f;snapshot.updateVoiceMorph(args);macro.updateVoiceMorph(args);}
        assert(snapshot.n()==12&&macro.n()==12);
        assert(snapshot.voicePeers.empty()&&macro.voicePeers.empty());
        assert(!snapshot.states.at(&server).voiceMix&&!macro.states.at(&server).voiceMix);
        assert(ofxSCSynth::live==baselineSynths&&ofxSCBus::live==baselineBuses);
    }
    assert(ofxSCSynth::live==0 && ofxSCBus::live==0);
    std::cout<<"PASS: snapshot/MacroMixer coefficient parity; interrupted/cancelled hooks; zero idle/same-count allocations; continuous midpoint; shared FX retained; temporary banks/mixer/buses released at endpoints\n";
}
'''
with tempfile.TemporaryDirectory(prefix='polycomb-voice-morph-') as directory:
    out=Path(directory)
    (out/'test.cpp').write_text(header+source+test)
    subprocess.run(['clang++','-std=c++20','-O1','-g','-fsanitize=address,undefined',
        '-I'+str(ROOT/'tools/snapshot_morph_test/stubs'),'-I'+str(ROOT/'src/nodes'),
        '-I'+str(ROOT.parents[1]/'libs/json/include'),str(out/'test.cpp'),'-o',str(out/'test')],check=True)
    subprocess.run([str(out/'test')],check=True)
