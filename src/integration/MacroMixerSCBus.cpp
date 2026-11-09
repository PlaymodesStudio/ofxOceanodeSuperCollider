#include "MacroMixerSCBus.h"
#include "MacroMixerServerAllocator.h"
#include "MacroMixerMixing.h"
#include "../scNode.h"
#include "../serverManager.h"
#include "ofxSCSynth.h"
#include "ofxSCServer.h"
#include "../ofxOceanodeSuperColliderConfig.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <stdexcept>
#include <filesystem>

namespace {
using Value=MacroMixerMixing::Value;
using Placement=MacroMixerMixing::Placement;
// The mixer's audio bridge: one synth per server it is pulled on, mixing input
// A (gainA) and input B (gainB). With a placement (Audio Servers: Alternate)
// each input has a server affinity: input A takes part only in its server's
// graph walk and input B only in its own, as SC Mixer Pro scopes its tracks.
// The bridge synth then runs on both servers, each with one input silent, and
// the audio device sums them: the server playing the origin never builds the
// destination's graph and no audio is routed between scsynth processes.
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
    std::vector<serverManager*> managers;
    std::map<serverManager*,uint64_t> routingTickets;
    // Input i takes part in server k's graph only when affinity[i] is k (or
    // both are nullptr: every server pulling the bridge, as with one server).
    std::array<ofxSCServer*,2> affinity{{nullptr,nullptr}};
    std::shared_ptr<Placement> placementA,placementB;
    // Placed but not on a server yet: the first server whose walk reaches the
    // bridge takes the graph, so two SC Outputs never both run it.
    bool claimPending=false;
    bool islandDecided=false;
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
    bool sameSources() const { return MacroMixerMixing::object<nodePort>(sourceA)==MacroMixerMixing::object<nodePort>(sourceB); }
    bool managed(ofxSCServer* server) const {
        for(auto* manager:managers) if(manager && manager->getServer()==server) return true;
        return false;
    }
    // Whether input `input` takes part in `server`'s graph. An identity (both
    // inputs one source: a graph at rest, or morphing in place) plays it on
    // either side's server.
    bool exposes(size_t input,ofxSCServer* server) const {
        if(!server || input>1 || (!affinity[0] && !affinity[1])) return true;
        if(!affinity[input] || affinity[input]==server) return true;
        return sameSources() && affinity[1-input]==server;
    }
    bool scoped(ofxSCServer* server) const { return server && (!exposes(0,server) || !exposes(1,server)); }
    // The affinity the current placements ask for. A graph not placed yet has none.
    std::array<ofxSCServer*,2> wanted() const {
        if(!placementA || !placementA->instance) return {{nullptr,nullptr}};
        auto* a=static_cast<ofxSCServer*>(placementA->instance);
        const auto* b=placementB ? placementB.get() : placementA.get();
        if(b==placementA.get()) {
            auto* incoming=static_cast<ofxSCServer*>(placementA->incoming);
            return {{a,incoming ? incoming : a}};
        }
        auto* other=static_cast<ofxSCServer*>(b->instance);
        return {{a,other ? other : a}};
    }
    // Called from the first walk that reaches the bridge after a graph was
    // placed without a server: that walk's server takes it (both sides).
    void claim(ofxSCServer* server) {
        claimPending=false;
        if(!placementA || !managed(server)) return;
        if(!placementA->instance) placementA->instance=server;
        if(placementB && placementB!=placementA && !placementB->instance) placementB->instance=server;
        affinity=wanted();
    }
    std::set<void*> reach() const {
        std::set<void*> result;
        for(const auto& state:states) result.insert(state.first);
        return result;
    }
    ofxSCServer* serverAt(int index) const {
        return index>=0 && index<int(managers.size()) && managers[index] ? managers[index]->getServer() : nullptr;
    }
    // The server the incoming side starts on (see MacroMixerServerAllocator).
    ofxSCServer* allocate(ofxSCServer* origin,Placement& placement,const std::set<void*>& reach) {
        placement.note.clear();
        if(!placement.spread) return origin;
        ofxSCServer* chosen=origin;
        if(placement.pin>=0) {
            // Its saved nodes name a server (an SC FFT HD reading the graph's bus there).
            auto* pinned=serverAt(placement.pin);
            if(pinned && reach.count(pinned)) chosen=pinned;
            else placement.note="its nodes name server "+std::to_string(placement.pin)+", which plays no SC Output of the mixer";
        } else {
            MacroMixerServerAllocator::Request request; request.origin=origin;
            for(auto* manager:managers) if(manager && manager->getServer()) {
                auto* server=manager->getServer();
                request.candidates.push_back({server,reach.count(server)!=0,manager->hasSoundNodes(),
                    manager->areRequiredSynthdefsLoading(),server->getAvgCPU()});
            }
            if(auto* other=MacroMixerServerAllocator::choose(request)) chosen=other;
        }
        if(chosen!=origin) for(auto* manager:managers)
            if(manager && manager->getServer()==chosen && manager->areRequiredSynthdefsLoading())
                placement.note="loading SynthDefs on server "+serverName(chosen);
        return chosen;
    }
    // Reads the placements stamped on the sources and chooses the incoming
    // side's server once: for a second graph (a crossfade) on its own
    // placement, for an internal blend in place on the graph's `incoming`.
    void place(std::shared_ptr<Placement> a,std::shared_ptr<Placement> b,bool same) {
        if(!a) b=nullptr; else if(!b) b=a;
        placementA=std::move(a); placementB=std::move(b);
        claimPending=false;
        if(!placementA) return;
        const bool internal=placementA==placementB && !same;
        // Only the router-level bridge knows which servers play the mixer;
        // an internal blend is reached through it.
        if(!internal) { placementA->reach=reach(); if(placementB!=placementA) placementB->reach=placementA->reach; }
        auto* origin=static_cast<ofxSCServer*>(placementA->instance);
        // An internal blend decides once, on its first call (the morph's
        // Start). Placing the island later would move it after a server had
        // already built it; with no placed origin yet it stays single-server.
        if(internal && !islandDecided) {
            islandDecided=true;
            if(origin && !placementA->incoming) placementA->incoming=allocate(origin,*placementA,placementA->reach);
        }
        if(!origin) { claimPending=true; return; }
        if(!internal && placementB!=placementA && !placementB->instance) placementB->instance=allocate(origin,*placementB,placementA->reach);
    }
    void requestRouting() {
        for(auto* manager:managers) if(manager && (manager->hasGraphNodes() || states.count(manager->getServer())))
            routingTickets[manager]=manager->requestMixerRouting();
    }
public:
    explicit AudioBridge(const std::vector<serverManager*>& managers):scNode("MacroMixer Audio"),managers(managers) {
        addInput("A"); addInput("B"); addOutput("Out");
        for(auto* manager:managers) if(manager) graphListeners.push(manager->graphComputed.newListener([this,manager] { completed(manager->getServer()); }));
    }
    ~AudioBridge() override { for(auto& state:states) if(state.second.synth) state.second.synth->free(); }
    std::string serverName(const void* server) const {
        for(size_t i=0;i<managers.size();++i) if(managers[i] && managers[i]->getServer()==server) return std::to_string(i);
        return {};
    }
    void check() const {
        for(const auto& item:states) if(item.second.failed) throw std::runtime_error("SC audio-bus allocation failed; origin retained. Free server resources and retry.");
    }
    bool ready() const {
        for(const auto& item:routingTickets) if(!item.first->mixerRoutingReady(item.second)) return false;
        for(const auto& item:states) if(!item.second.ready) return false;
        return true;
    }
    void retire(const std::shared_ptr<void>& lease) {
        for(auto* manager:managers) if(manager && (manager->hasGraphNodes() || states.count(manager->getServer()))) manager->requestMixerRouting(lease);
    }
    bool isMacroMixerBridge() const override { return true; }
    void graphWillRebuild(ofxSCServer* server) override { states[server].ready=false; }
    // Unscoped (no affinity, or both inputs on this server): the ordinary walk,
    // exactly as before. Scoped: SC Mixer Pro's walk over this server's inputs.
    bool appendOrderedNodes(vector<scNode*>& nodesList,map<scNode*,std::pair<int,vector<int>>>& visitedNodeChilds,vector<scNode*> parents) override {
        auto* server=getGraphTraversalServer();
        if(server && claimPending) claim(server);
        if(!scoped(server)) return scNode::appendOrderedNodes(nodesList,visitedNodeChilds,parents);
        parents.push_back(this);
        for(size_t i=0;i<availableInputs.size();++i) {
            if(!exposes(i,server)) continue;
            scNode* source=availableInputs[i]->getNodeRef();
            if(source==nullptr || std::find(parents.begin(),parents.end(),source)!=parents.end()) continue;
            source->appendOrderedNodes(nodesList,visitedNodeChilds,parents);
        }
        if(std::find(nodesList.begin(),nodesList.end(),this)==nodesList.end()) { nodesList.push_back(this); return true; }
        return false;
    }
    void getConnections(std::map<nodePort,vector<scNode*>>& connections) override {
        auto* server=getGraphTraversalServer();
        if(!scoped(server)) { scNode::getConnections(connections); return; }
        for(size_t i=0;i<availableInputs.size();++i)
            if(exposes(i,server) && availableInputs[i]->getNodeRef()!=nullptr) connections[*availableInputs[i]].push_back(this);
    }
    // Input A on server a, input B on server b (nullptr for both: any server).
    // Every server involved rebuilds and acknowledges, as for a source change.
    void setAffinity(ofxSCServer* a,ofxSCServer* b) {
        const std::array<ofxSCServer*,2> next{{a,b}};
        if(next==affinity) return;
        affinity=next;
        for(auto& state:states) state.second.ready=false;
        requestRouting();
    }
    void updateSources(const Value& av,const Value& bv,float progress,const ofJson& options,const std::string& policy) {
        const auto& a=MacroMixerMixing::object<nodePort>(av); const auto& b=MacroMixerMixing::object<nodePort>(bv);
        if(a.getNodeRef()==this || b.getNodeRef()==this) throw std::runtime_error("A ScBus router cannot feed its own mixer output back into itself");
        const int count=options.at("channels").get<int>();
        if(count!=channels && !states.empty()) throw std::runtime_error("Clear Playback before changing ScBus channel count");
        channels=count; smoothing=options.at("smoothing_ms").get<float>()*.001f;
        const auto& oldA=MacroMixerMixing::object<nodePort>(sourceA); const auto& oldB=MacroMixerMixing::object<nodePort>(sourceB);
        const bool changed=!(a==oldA) || !(b==oldB);
        place(std::get<MacroMixerMixing::Object>(av).placement,std::get<MacroMixerMixing::Object>(bv).placement,a==b);
        const auto target=wanted();
        const bool reaffined=target!=affinity;
        const bool changeGains=changed || reaffined || ready();
        if(changed) {
            Retirement r{sourceA,sourceB,{}};
            for(auto& state:states) { r.pending.insert(state.first); state.second.ready=false; state.second.failed=false; }
            if(!r.pending.empty()) retired.push_back(std::move(r));
            sourceA=av; sourceB=bv;
            suppressInputRefresh=true; inputs[0]=a; inputs[1]=b; suppressInputRefresh=false;
        } else { sourceA=av; sourceB=bv; }
        // A new source and its server move travel in one rebuild per server,
        // before any server walks again (rebuilds are coalesced to the update).
        if(reaffined) { affinity=target; for(auto& state:states) state.second.ready=false; }
        if(changed || reaffined) requestRouting();
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
    void free(ofxSCServer* server) override {
        auto found=states.find(server); if(found!=states.end()) { if(found->second.synth) found->second.synth->free(); states.erase(found); }
        // The server playing the graph at rest stopped pulling the mixer (its
        // SC Output moved or was disconnected): the next server whose walk
        // reaches the bridge takes the graph.
        if(placementA && placementA->instance==server && (!placementB || placementB==placementA) && sameSources()) {
            placementA->instance=nullptr; placementA->incoming=nullptr;
            affinity={{nullptr,nullptr}}; claimPending=true;
            for(auto* manager:managers) if(manager && states.count(manager->getServer())) manager->requestRecomputeGraph();
        }
    }
    void setOutputBus(ofxSCServer* server,int,int bus) override { auto& s=states[server]; s.out=bus; if(s.synth) s.synth->set("out",bus); }
    int getOutputBusIndex(ofxSCServer* server,int) override { auto found=states.find(server); return found==states.end() ? -1 : found->second.out; }
    int getLastSynthID(ofxSCServer* server) override { auto found=states.find(server); return found==states.end() || !found->second.synth ? -1 : found->second.synth->nodeID; }
    void resetInputBusses(ofxSCServer* server,int bus) override { auto& s=states[server]; s.a=s.b=bus; if(s.synth) { s.synth->set("inA",bus); s.synth->set("inB",bus); } }
    // An input outside this server's graph keeps the silent bus.
    void setInputBus(ofxSCServer* server,scNode* node,int bus) override {
        auto& s=states[server];
        for(size_t i=0;i<inputs.size();++i) if(exposes(i,server) && inputs[i].get().getNodeRef()==node && inputs[i].get().getBusIndex(server)==bus) {
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
// Every saved node of a source definition, nested Macros included, as (path, entry).
void forEachSavedNode(const ofJson& definition,const std::function<void(const std::string&,const ofJson&)>& visit,const std::string& prefix={},int depth=0) {
    if(depth>64 || !definition.is_object() || !definition.contains("nodes") || !definition.at("nodes").is_object()) return;
    const auto& nodes=definition.at("nodes");
    for(auto it=nodes.begin();it!=nodes.end();++it) {
        if(!it.value().is_object() || !it.value().contains("model") || !it.value().at("model").is_string()) continue;
        visit(prefix+it.key(),it.value());
        if(it.value().contains("graph")) forEachSavedNode(it.value().at("graph"),visit,prefix+it.key()+" / ",depth+1);
    }
}
// A source that shares a server-bound resource with the rest of the patch
// cannot alternate: an ScBus Portal would pull its other end onto a second
// server, and an SC Mixer Pro with strips on several servers names them
// itself (both: single server). A node saved with a Server (SC FFT HD,
// Buffer Allocator, A2k), or a Mixer Pro whose strips all name one server
// without Follow Graph Server, plays only there: the source is pinned to it.
// A saved setting as a number or bool: parameters save as strings ("1"), custom blocks as JSON values.
bool savedInt(const ofJson& value,int& result) {
    if(value.is_number_integer()) { result=value.get<int>(); return true; }
    if(value.is_boolean()) { result=value.get<bool>() ? 1 : 0; return true; }
    if(value.is_string()) {
        const auto text=value.get<std::string>();
        if(text=="true" || text=="false") { result=text=="true"; return true; }
        try { size_t used=0; result=std::stoi(text,&used); return used==text.size(); } catch(...) { return false; }
    }
    return false;
}
// Where a saved SC Mixer Pro plays: its strips' common server (home, -1 when
// they name several), and whether Follow Graph Server moves them with the graph.
struct MixerProPlacement { int home=0; bool follows=false; };
MixerProPlacement mixerProPlacement(const ofJson& settings) {
    MixerProPlacement result;
    int on=0;
    result.follows=settings.contains("Follow_Graph_Server") && savedInt(settings.at("Follow_Graph_Server"),on) && on;
    if(!settings.contains("mixerPro") || !settings.at("mixerPro").is_object()) return result; // Default strips: server 0.
    const auto& state=settings.at("mixerPro");
    int home=-1;
    for(const char* key:{"tracks","buses","returns"}) {
        if(!state.contains(key) || !state.at(key).is_array()) continue;
        for(const auto& strip:state.at(key)) {
            int server=0;
            if(strip.is_object() && strip.contains("server") && !savedInt(strip.at("server"),server)) { result.home=-1; return result; }
            if(home<0) home=server; else if(server!=home) { result.home=-1; return result; }
        }
    }
    result.home=std::max(0,home);
    return result;
}
MacroMixerMixing::PlacementPin savedServerPin(const ofJson& definition) {
    MacroMixerMixing::PlacementPin pin;
    std::string single;
    forEachSavedNode(definition,[&](const std::string& path,const ofJson& entry) {
        const auto model=entry.at("model").get<std::string>();
        if(!single.empty()) return;
        const ofJson none=ofJson::object();
        const auto& saved=entry.contains("settings") && entry.at("settings").is_object() ? entry.at("settings") : none;
        if(model=="Portal ScBus") { single=path+" (an ScBus Portal) shares audio with the rest of the patch"; return; }
        // A node that plays only on a given server pins the source there. Two
        // nodes naming different servers keep it single.
        const auto pinTo=[&](int index,const std::string& why) {
            if(pin.index<0) { pin.index=index; pin.reason=why; }
            else if(pin.index!=index) single=why+" while "+pin.reason;
        };
        if(model=="SC Mixer Pro") {
            // Following, its strips play wherever the graph runs. Otherwise they
            // play only on the server they name, so the source must go there.
            const auto mixer=mixerProPlacement(saved);
            if(mixer.home<0) single=path+" (SC Mixer Pro) spreads its strips over several servers";
            else if(!mixer.follows) pinTo(mixer.home,path+" (SC Mixer Pro) plays its strips on server "+std::to_string(mixer.home)
                +"; switch on its Follow Graph Server to let it alternate");
            return;
        }
        // SC Output is kept out of every source (its Server places the host's graph).
        if(model.rfind("SC ",0)!=0 || model=="SC Output") return;
        int index=-1;
        if(!saved.contains("Server") || !savedInt(saved.at("Server"),index)) return;
        pinTo(index,path+" uses server "+std::to_string(index));
    });
    if(!single.empty()) { pin.index=-1; pin.single=true; pin.reason=single; }
    return pin;
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
        folder=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()/"data/Supercollider/Synthdefs/Defaults/macromixer";
    for(auto* manager:servers) if(manager) manager->requireSynthdefFolder(folder.string());
    MacroMixerMixing::PlacementHooks hooks;
    // SC nodes and the ScBus Router/Portal carry audio; control nodes do not.
    hooks.carries=[](const std::string& model) { return model.rfind("SC ",0)==0 || model=="Router ScBus" || model=="Portal ScBus"; };
    hooks.pin=savedServerPin;
    // A free server may never have loaded what the destination needs: load the
    // SynthDefs of every SC SynthDef node (saved "SC <name>*") on every server
    // when the source is imported, not only on the server pulling it. Boxes
    // that choose their SynthDef folder from their settings (SampleBox,
    // DrumKitBox) still require it when they are first built.
    hooks.preload=[servers](const ofJson& definition) {
        std::set<std::string> synthdefs;
        forEachSavedNode(definition,[&](const std::string&,const ofJson& entry) {
            auto model=entry.at("model").get<std::string>();
            if(model.rfind("SC ",0)!=0 || std::count(model.begin(),model.end(),'*')!=1) return;
            model.erase(0,3); model.erase(std::remove(model.begin(),model.end(),'*'),model.end());
            synthdefs.insert(model);
        });
        for(auto* manager:servers) if(manager) for(const auto& synthdef:synthdefs) {
            const auto path=manager->synthdefFolderOf(synthdef);
            if(!path.empty()) manager->requireSynthdefFolder(path);
        }
    };
    hooks.name=[servers](const void* server) -> std::string {
        for(size_t i=0;i<servers.size();++i) if(servers[i] && servers[i]->getServer()==server) return std::to_string(i);
        return {};
    };
    hooks.instance=[servers](int index) -> void* {
        return index>=0 && index<int(servers.size()) && servers[index] ? servers[index]->getServer() : nullptr;
    };
    registry.registerPlacement("ScBus",std::move(hooks));
    for(const auto& spec:std::vector<std::pair<std::string,std::string>>{{"linear","Linear"},{"equal_power","Equal Power"},{"switch","Switch"},{"fade_out","FadeOut"}}) {
        MacroMixerMixing::Policy p{spec.first,spec.second,"ScBus",[](const Value&,const Value&,float,const ofJson&)->Value {
            throw std::runtime_error("ScBus mixing requires a private update-stage processor");
        },[switching=spec.first=="switch"](const ofJson& o) {return audioOptions(o,switching);}};
        p.createProcessor=[servers,id=spec.first,folder] {
            if(!std::filesystem::exists(folder/"macroMixer.scsyndef"))
                throw std::runtime_error("Missing MacroMixer audio SynthDefs: copy the addon's data/Supercollider folder into the application data folder and restart");
            auto bridge=std::make_shared<AudioBridge>(servers);
            // The sources carry their graphs' placements (Audio Servers:
            // Alternate): the bridge chooses the incoming side's server from
            // them, writes it back and scopes its inputs to their servers.
            return [bridge,id](const Value& a,const Value& b,float t,const ofJson& options) {
                bridge->updateSources(a,b,t,options,id);
                return MacroMixerMixing::object("ScBus",nodePort(0,bridge.get(),bridge));
            };
        };
        p.ready=[](const Value& v) {return static_cast<AudioBridge*>(MacroMixerMixing::object<nodePort>(v).getNodeRef())->ready();};
        p.retire=[](const Value& v,std::shared_ptr<void> lease) {
            if(auto* bridge=dynamic_cast<AudioBridge*>(MacroMixerMixing::object<nodePort>(v).getNodeRef())) bridge->retire(lease);
        };
        registry.registerPolicy(std::move(p),spec.first=="linear");
    }
}
