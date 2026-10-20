#include "MacroMixerSCBus.h"
#include "MacroMixerMixing.h"
#include "scNode.h"
#include "serverManager.h"
#include "ofxSCSynth.h"
#include "ofxOceanodeSuperColliderConfig.h"
#include <cmath>
#include <set>
#include <stdexcept>
#include <filesystem>

namespace {
using Value=MacroMixerMixing::Value;
class AudioBridge : public scNode {
    struct State {
        std::unique_ptr<ofxSCSynth> synth;
        int a=0,b=0,out=0;
        bool ready=false,failed=false;
    };
    struct Retirement { Value a,b; std::set<ofxSCServer*> pending; };
    std::map<ofxSCServer*,State> states;
    std::vector<Retirement> retired;
    Value sourceA=MacroMixerMixing::object("ScBus",nodePort()),sourceB=sourceA;
    int channels=2;
    float gainA=1,gainB=0,smoothing=.02f;
    ofEventListeners graphListeners;
    void send(State& state) {
        if(!state.synth) return;
        state.synth->set("inA",state.a); state.synth->set("inB",state.b); state.synth->set("out",state.out);
        state.synth->set("gainA",gainA); state.synth->set("gainB",gainB); state.synth->set("smoothing",smoothing);
    }
    void completed(ofxSCServer* server) {
        auto state=states.find(server);
        if(state!=states.end() && !state->second.ready) return;
        for(auto& item:retired) item.pending.erase(server);
        retired.erase(std::remove_if(retired.begin(),retired.end(),[](const auto& r){return r.pending.empty();}),retired.end());
    }
public:
    explicit AudioBridge(const std::vector<serverManager*>& managers):scNode("MacroMixer Audio") {
        addInput("A"); addInput("B"); addOutput("Out");
        for(auto* manager:managers) if(manager) graphListeners.push(manager->graphComputed.newListener([this,manager] { completed(manager->getServer()); }));
    }
    ~AudioBridge() override { for(auto& state:states) if(state.second.synth) state.second.synth->free(); }
    void check() const {
        for(const auto& item:states) if(item.second.failed) throw std::runtime_error("SC audio-bus allocation failed; origin retained. Free server resources and retry.");
    }
    bool ready() const { for(const auto& item:states) if(!item.second.ready) return false; return true; }
    bool isMacroMixerBridge() const override { return true; }
    void graphWillRebuild(ofxSCServer* server) override { states[server].ready=false; }
    void updateSources(const Value& av,const Value& bv,float progress,const ofJson& options,const std::string& policy) {
        const auto& a=MacroMixerMixing::object<nodePort>(av); const auto& b=MacroMixerMixing::object<nodePort>(bv);
        if(a.getNodeRef()==this || b.getNodeRef()==this) throw std::runtime_error("A ScBus router cannot feed its own mixer output back into itself");
        const int count=options.at("channels").get<int>();
        if(count!=channels && !states.empty()) throw std::runtime_error("Clear Playback before changing ScBus channel count");
        channels=count; smoothing=options.at("smoothing_ms").get<float>()*.001f;
        const auto& oldA=MacroMixerMixing::object<nodePort>(sourceA); const auto& oldB=MacroMixerMixing::object<nodePort>(sourceB);
        const bool changed=!(a==oldA) || !(b==oldB);
        const bool changeGains=changed || ready();
        if(changed) {
            Retirement r{sourceA,sourceB,{}};
            for(auto& state:states) { r.pending.insert(state.first); state.second.ready=false; state.second.failed=false; }
            if(!r.pending.empty()) retired.push_back(std::move(r));
            sourceA=av; sourceB=bv;
            suppressInputRefresh=true; inputs[0]=a; inputs[1]=b; suppressInputRefresh=false;
        } else { sourceA=av; sourceB=bv; }
        // Keep the prepared start gains while the server is still executing
        // the scheduled rebuild; host frames cannot advance that fade yet.
        if(!changeGains) return;
        if(policy=="equal_power") { gainA=std::cos(progress*float(M_PI_2)); gainB=std::sin(progress*float(M_PI_2)); }
        else if(policy=="fade_out") { gainA=std::max(0.f,1-2*progress); gainB=std::max(0.f,2*progress-1); }
        else if(policy=="switch") { const bool toB=progress==1 || (progress!=0 && progress>=options.at("threshold").get<float>()); gainA=toB?0:1; gainB=toB?1:0; }
        else { gainA=1-progress; gainB=progress; }
        if(progress==0) {gainA=1;gainB=0;} if(progress==1) {gainA=0;gainB=1;}
        if(!a.getNodeRef()) gainA=0; if(!b.getNodeRef()) gainB=0;
        for(auto& state:states) if(state.second.ready) send(state.second);
    }
    void buildSynth(ofxSCServer* server) override {
        auto& state=states[server];
        state.synth=std::make_unique<ofxSCSynth>("oceanodeMacroMixer"+std::to_string(channels),server);
        state.ready=false; state.failed=false;
    }
    void createSynth(ofxSCServer* server) override { auto& state=states.at(server); send(state); state.synth->createAndRun(0,1,true); }
    void graphDidRebuild(ofxSCServer* server,bool success) override {
        auto& state=states[server]; state.ready=success; state.failed=!success;
        if(success) send(state);
    }
    void moveSynthBefore(ofxSCServer* server,int id) override { auto found=states.find(server); if(found!=states.end() && found->second.synth && id>0) found->second.synth->moveBefore(id); }
    void free(ofxSCServer* server) override { auto found=states.find(server); if(found!=states.end()) { if(found->second.synth) found->second.synth->free(); states.erase(found); } }
    void setOutputBus(ofxSCServer* server,int,int bus) override { auto& s=states[server]; s.out=bus; if(s.synth) s.synth->set("out",bus); }
    int getOutputBusIndex(ofxSCServer* server,int) override { auto found=states.find(server); return found==states.end() ? -1 : found->second.out; }
    int getLastSynthID(ofxSCServer* server) override { auto found=states.find(server); return found==states.end() || !found->second.synth ? -1 : found->second.synth->nodeID; }
    void resetInputBusses(ofxSCServer* server,int bus) override { auto& s=states[server]; s.a=s.b=bus; if(s.synth) { s.synth->set("inA",bus); s.synth->set("inB",bus); } }
    void setInputBus(ofxSCServer* server,scNode* node,int bus) override {
        auto& s=states[server];
        for(size_t i=0;i<inputs.size();++i) if(inputs[i].get().getNodeRef()==node && inputs[i].get().getBusIndex(server)==bus) {
            (i==0 ? s.a : s.b)=bus; if(s.synth) s.synth->set(i==0 ? "inA" : "inB",bus);
        }
    }
    void resendParametersForNRT() override { for(auto& state:states) send(state.second); }
    bool isNRTStemPoint() const override { return true; }
};
ofJson audioOptions(const ofJson& input,bool switching) {
    if(!input.is_object()) throw std::runtime_error("ScBus options must be an object");
    ofJson result={{"channels",2},{"smoothing_ms",20.0}}; if(switching) result["threshold"]=.5;
    for(auto it=input.begin();it!=input.end();++it) {
        if(!result.contains(it.key()) || !it.value().is_number()) throw std::runtime_error("Invalid ScBus option: "+it.key());
        result[it.key()]=it.value();
    }
    const double channels=result["channels"].get<double>(),smooth=result["smoothing_ms"].get<double>();
    if(!result["channels"].is_number_integer() || channels<1 || channels>MAX_NODE_CHANNELS) throw std::runtime_error("ScBus channels must be 1..128");
    if(!std::isfinite(smooth) || smooth<0 || smooth>1000) throw std::runtime_error("ScBus smoothing must be 0..1000 ms");
    if(switching) { const double t=result["threshold"].get<double>(); if(!std::isfinite(t) || t<0 || t>1) throw std::runtime_error("Switch threshold must be 0..1"); }
    return result;
}
}
void registerMacroMixerSCBus(const std::vector<serverManager*>& servers) {
    auto& registry=MacroMixerMixing::registry();
    if(!registry.registerValueType<nodePort>("ScBus","Router ScBus",[](const nodePort& p) {
        if(p.getNodeRef() && (p.getIndex()<0 || p.getIndex()>=p.getNodeRef()->getNumOutputs())) throw std::runtime_error("Invalid ScBus output reference");
        if(auto* bridge=dynamic_cast<AudioBridge*>(p.getNodeRef())) bridge->check();
    })) return;
    // Defaults/MacroMixer in a synced data folder, Defaults/macromixer before
    auto folder=std::filesystem::path(ofToDataPath(std::string(SYNTHDEF_DIRECTORY)+"/Defaults/MacroMixer",true));
    if(!std::filesystem::exists(folder/"macroMixer.scsyndef"))
        folder=std::filesystem::path(ofToDataPath(std::string(SYNTHDEF_DIRECTORY)+"/Defaults/macromixer",true));
    if(!std::filesystem::exists(folder/"macroMixer.scsyndef"))
        folder=std::filesystem::path(__FILE__).parent_path().parent_path()/"data/Supercollider/Synthdefs/Defaults/macromixer";
    for(auto* manager:servers) if(manager) manager->requireSynthdefFolder(folder.string());
    for(const auto& spec:std::vector<std::pair<std::string,std::string>>{{"linear","Linear"},{"equal_power","Equal Power"},{"switch","Switch"},{"fade_out","FadeOut"}}) {
        MacroMixerMixing::Policy p{spec.first,spec.second,"ScBus",[](const Value&,const Value&,float,const ofJson&)->Value {
            throw std::runtime_error("ScBus mixing requires a private update-stage processor");
        },[switching=spec.first=="switch"](const ofJson& o) {return audioOptions(o,switching);}};
        p.createProcessor=[servers,id=spec.first,folder] {
            if(!std::filesystem::exists(folder/"macroMixer.scsyndef"))
                throw std::runtime_error("Missing MacroMixer audio SynthDefs: copy the addon's data/Supercollider folder into the application data folder and restart");
            auto bridge=std::make_shared<AudioBridge>(servers);
            return [bridge,id](const Value& a,const Value& b,float t,const ofJson& options) {
                bridge->updateSources(a,b,t,options,id);
                return MacroMixerMixing::object("ScBus",nodePort(0,bridge.get(),bridge));
            };
        };
        p.ready=[](const Value& v) {return static_cast<AudioBridge*>(MacroMixerMixing::object<nodePort>(v).getNodeRef())->ready();};
        registry.registerPolicy(std::move(p),spec.first=="linear");
    }
}
