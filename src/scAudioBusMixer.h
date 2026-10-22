#pragma once
#include "scNode.h"
#include "serverManager.h"
#include "ofxSCSynth.h"
#include "ofxOceanodeSuperColliderConfig.h"
#include <cmath>
#include <set>
#include <stdexcept>

// Audio-routing infrastructure. No dependency on macros, snapshots or policies.
std::string scAudioBusMixerSynthdefFolder();
void registerSCAudioInputs(const std::vector<serverManager*>& servers);

class scAudioBusMixer : public scNode {
    struct State {
        std::unique_ptr<ofxSCSynth> synth;
        int a=0,b=0,out=0;
        bool ready=false,failed=false;
    };
    struct Retirement { nodePort a,b; std::set<ofxSCServer*> pending; };
    std::map<ofxSCServer*,State> states;
    std::vector<Retirement> retired;
    nodePort sourceA,sourceB;
    int channels=2;
    bool inferChannels=false;
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
    explicit scAudioBusMixer(const std::vector<serverManager*>& managers, bool infer=false):scNode("Audio Bus Mixer"),inferChannels(infer) {
        addInput("A"); addInput("B"); addOutput("Out");
        for(auto* manager:managers) if(manager) graphListeners.push(manager->graphComputed.newListener([this,manager] { completed(manager->getServer()); }));
    }
    ~scAudioBusMixer() override { for(auto& state:states) if(state.second.synth) state.second.synth->free(); }
    void check() const {
        for(const auto& item:states) if(item.second.failed) throw std::runtime_error("SC audio-bus allocation failed; origin retained. Free server resources and retry.");
    }
    bool ready() const { for(const auto& item:states) if(!item.second.ready) return false; return true; }
    bool requiresGraphSync() const override { return true; }
    void graphWillRebuild(ofxSCServer* server) override { states[server].ready=false; }
    void setSources(const nodePort& a,const nodePort& b,int count,float ga,float gb,float smooth=0) {
        if(a.getNodeRef()==this || b.getNodeRef()==this) throw std::runtime_error("An audio bus mixer cannot feed itself");
        if(count<1 || count>MAX_NODE_CHANNELS || !std::isfinite(ga) || !std::isfinite(gb) || !std::isfinite(smooth) || smooth<0)
            throw std::runtime_error("Invalid audio bus mixer settings");
        if(count!=channels && !states.empty()) throw std::runtime_error("Cannot change a running audio bus mixer's channel count");
        channels=count; smoothing=smooth;
        const auto& oldA=sourceA; const auto& oldB=sourceB;
        const bool changed=!(a==oldA) || !(b==oldB);
        const bool changeGains=changed || ready();
        if(changed) {
            Retirement r{sourceA,sourceB,{}};
            for(auto& state:states) { r.pending.insert(state.first); state.second.ready=false; state.second.failed=false; }
            if(!r.pending.empty()) retired.push_back(std::move(r));
            sourceA=a; sourceB=b;
            suppressInputRefresh=true; inputs[0]=a; inputs[1]=b; suppressInputRefresh=false;
        } else { sourceA=a; sourceB=b; }
        // Keep the prepared start gains while the server is still executing
        // the scheduled rebuild; host frames cannot advance that fade yet.
        if(!changeGains) return;
        gainA=ga; gainB=gb;
        if(!a.getNodeRef()) gainA=0; if(!b.getNodeRef()) gainB=0;
        for(auto& state:states) if(state.second.ready) send(state.second);
    }
    int getOutputNumChannels(int) override {
        if(!inferChannels) return -1;
        const auto& a=sourceA;
        const auto& b=sourceB;
        auto count=[this](const nodePort& p) {
            if(!p.getNodeRef()) return 0;
            const int n=p.getNodeRef()->getOutputNumChannels(p.getIndex());
            return n<0 ? channels : std::min(n,channels);
        };
        return std::max(count(a),count(b));
    }
    std::string requiredSynthdefFolder() const override { return scAudioBusMixerSynthdefFolder(); }
    void buildSynth(ofxSCServer* server) override {
        auto& state=states[server];
        state.synth=std::make_unique<ofxSCSynth>("oceanodeAudioBusMixer"+std::to_string(channels),server);
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
