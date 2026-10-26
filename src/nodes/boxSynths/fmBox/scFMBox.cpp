#include "../shared/scDroneMode.h"
#include "../../../audio/scFxRouting.h"
#include "scFMBox.h"
#include "../../../ui/scTypedValue.h"
#include "../../../ui/scEnvelopeCurve.h"
#include "../../../audio/scSynthControlBatch.h"
#include "../../../ofxOceanodeSuperColliderConfig.h"
#include "imgui_internal.h" // vertical DragFloat interaction for FM junctions
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace {
namespace pal {
const ImU32 canvas     = IM_COL32( 11,  12,  14, 255);
const ImU32 border     = IM_COL32( 46,  50,  58, 255);
const ImU32 grid       = IM_COL32(255, 255, 255,  14);
const ImU32 gridStrong = IM_COL32(255, 255, 255,  34);
const ImU32 header     = IM_COL32( 30,  32,  38, 255);
const ImU32 headerText = IM_COL32(205, 205, 215, 255);
const ImU32 textDim    = IM_COL32(150, 150, 160, 255);
const ImU32 accent     = IM_COL32(150, 165, 255, 255);   // periwinkle
}
float zoom() { return ofxOceanodeShared::getZoomLevel(); }
float gapW() { return 12.0f * zoom(); }
float colW(float w, int n) { return std::max(1.0f, (w - gapW() * (n - 1)) / n); }
void nextCol() { ImGui::EndGroup(); ImGui::SameLine(0, gapW()); ImGui::BeginGroup(); }
void canvas(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    dl->AddRectFilled(a,b,pal::canvas,3*zoom());
    dl->AddRect(a,b,pal::border,3*zoom());
}
std::vector<float> expandPan(const std::vector<float>& values,int n) {
    std::vector<float> result(n,0.5f);
    for(int i=0;i<n;++i)if(!values.empty())result[i]=values[i%values.size()];
    return result;
}
bool liveBus(ofxSCServer* s, ofxSCBus* b, bool audio) {
    if(!s || !b || b->index < 0) return false;
    auto& buses = audio ? s->audioBusses : s->controlBusses;
    return b->index < int(buses.size()) && buses[b->index] == b;
}
void dropBus(ofxSCServer* s, ofxSCBus*& b, bool audio) {
    if(liveBus(s, b, audio)) b->free();
    delete b; b = nullptr;
}
float first(const std::vector<float>& v, int i, float fallback) {
    return v.empty() ? fallback : v[i % v.size()];
}
ofJson blend(const ofJson& a, const ofJson& b, float t, bool sw, bool discrete) {
    if(!discrete && a.is_number() && b.is_number()) return ofLerp(a.get<float>(), b.get<float>(), t);
    if(!discrete && a.is_array() && b.is_array() && !a.empty() && !b.empty()) {
        ofJson out = ofJson::array();
        for(size_t i = 0; i < std::max(a.size(), b.size()); ++i) out.push_back(blend(a[i % a.size()], b[i % b.size()], t, sw, false));
        return out;
    }
    return sw ? b : a;
}
}

scFMBox::scFMBox(std::vector<serverManager*> s) : scNode("fmBox"), servers(std::move(s)) {
    description = "DX7-Supercollider synthesis and .afx preset banks. Show opens the dockable fmBox editor: operator envelopes, "
                  "6 x 6 FM matrix, snapshots and morphing, per-voice modulation and effects. "
                  "Right-click controls to publish, modulate or add to the timeline.";
}
scFMBox::~scFMBox() {
    scheduled.disable();
    listeners.unsubscribeAll();
    for(auto& [s, st] : states) release(s, st);
}

template<class T> void scFMBox::reg(ofParameter<T>& p, std::function<void()> send) {
    Control c;
    c.key = p.getEscapedName(); c.param = &p;
    c.discrete = std::is_same_v<T, bool> || std::is_same_v<T, int>;
    c.add = [this, &p]() { return addParameter(p); };
    c.save = [&p]() { return ofJson(p.get()); };
    c.load = [&p](const ofJson& j) { try { p.set(j.get<T>()); } catch(const std::exception&) {} };
    controls.push_back(std::move(c));
    listeners.push(p.newListener([this, &p, send](T&) { snaps.morph.edited(p); if(!restoring) send(); }));
}
void scFMBox::setup() {
    timeline.setup(this, [this](const std::string& k){ publish(k); }, [this](const std::string& k){ unpublish(k); }, [this](){ return publishedKeys; });
    addParameter(show.set("Show", false));
    addParameter(voices.set("Voices", 5, 1, 16));
    addParameter(channels.set("Channels", 2, 1, 16));
    // Gate (levels: a gate rising to 0.2 plays at 0.2), Pitch: Oceanode values or audio cables
    gateIn.attach(addParameter(gate.set("Gate", {0.0f}, {0.0f}, {1.0f})), availableInputs, [this](){ refreshOutputs(); });
    pitchIn.attach(addParameter(pitch.set("Pitch", {60.0f}, {0.0f}, {127.0f})), availableInputs, [this](){ refreshOutputs(); });
    addParameter(master.set("Master", 0.6f, 0.0f, 1.0f));
    resetLFOs.set("Reset");
    addOutput("Out");

    drone.set("Drone", false);
    reg(drone,[this](){ for(auto& [s, st] : states) sendNotes(s, st, NoteRouting); });

    scheduled.add(gateIn.handle, gate); scheduled.add(pitchIn.handle, pitch);
    scheduled.enable(this, [this](){ for(auto& [s, st] : states) if(st.created) return true; return false; });
    for(auto* p : {&gate, &pitch}) {
        listeners.push(p->newListener([this,p](std::vector<float>& v) {
            // the timeline's frame set of a value it already sent timetagged: nothing to do
            if(scScheduling::isBackendSendSuppressed()) return;
            for(auto& [s, st] : states) sendNotes(s, st, p == &gate ? NoteGate : NotePitch);
        }));
    }
    listeners.push(voices.newListener([this](int&) { structureDirty = true; }));
    listeners.push(channels.newListener([this](int&) { structureDirty = true; }));
    listeners.push(master.newListener([this](float&) { if(!restoring) sendAll(); }));
    listeners.push(resetLFOs.newListener([this]() { mod.resetLfos(); }));

    auto add = [this](const std::string& name, const std::string& sc, const std::string& group,
                      float value, float lo, float hi, bool discrete=false) {
        auto c = std::make_unique<SoundControl>();
        c->p.set(name,value,lo,hi);c->sc=sc;c->group=group;
        reg(c->p,[this,sc](){if(sc=="algorithm")setAlgorithm(int(std::round(soundParam("algorithm").get())));sendAll();});
        controls.back().discrete=discrete;
        sound.push_back(std::move(c));
    };
    for(const auto& f:scDX7::globalFields) {
        const std::string sc=f.sc;
        const bool discrete=sc=="algorithm"||sc=="lfo_wave"||sc=="osc_sync"||sc=="lfo_sync";
        add(f.name,f.sc,"Global",f.initial,0,f.max,discrete);
    }
    add("FM Amount","mod_scale","Global",1,0,10);
    add("Matrix Model","matrix_model","Global",0,0,1,true);
    dropdowns.set(soundParam("matrix_model").getEscapedName(),{"DX7","Raw FM"});
    add("Envelope Time","env_time","Global",1,0.01f,20);
    add("Envelope Model","env_model","Global",0,0,1,true);
    add("Envelope Curve","env_curve","Global",-4,-8,8);
    dropdowns.set(soundParam("env_model").getEscapedName(),{"DX7","Seconds"});
    add("Width","stereo","Global",0,0,1);
    dropdowns.set(soundParam("algorithm").getEscapedName(),[](){std::vector<std::string> n;for(int k=1;k<=32;++k)n.push_back("Algorithm "+ofToString(k));return n;}());
    dropdowns.set(soundParam("lfo_wave").getEscapedName(),{"Triangle","Saw down","Saw up","Square","Sine","Sample & hold"});
    for(int op=0;op<6;++op) {
        const std::string name="Op "+ofToString(op+1)+" ",sc="op"+ofToString(op)+"_";
        for(const auto& f:scDX7::operatorFields) {
            const std::string field=f.sc;
            add(name+f.name,sc+f.sc,"Operators",f.initial,0,f.max,field=="mode"||field=="left_curve"||field=="right_curve");
        }
        for(int k=0;k<4;++k)
            add(name+"Seconds "+ofToString(k+1),sc+"seconds"+ofToString(k),"Operators",0.1f,0.00001f,20);
        dropdowns.set(soundParam(sc+"mode").getEscapedName(),{"Ratio","Fixed Hz"});
        for(const char* side:{"left_curve","right_curve"}) dropdowns.set(soundParam(sc+side).getEscapedName(),{"-Linear","-Exponential","+Exponential","+Linear"});
        add(name+"Mix","mix"+ofToString(op),"Matrix",scDX7::algorithms[0][36+op],0,1);
        for(int src=0;src<6;++src)
            add("FM "+ofToString(src+1)+" > "+ofToString(op+1),"fm"+ofToString(op)+"_"+ofToString(src),"Matrix",scDX7::algorithms[0][op*6+src],0,4);
    }
    afxPreset.set("AFX Preset",1,1,65536);
    reg(afxPreset,[this](){recallAFX(afxPreset.get()-1);});
    // ── Effects (MixerPro's inserts) ─────────────────────────────────────────
    spreadMode.set("Placement",0,0,2);reg(spreadMode,[this](){sendAll();});
    spreadCenter.set("Spread Center",0,-1,1);reg(spreadCenter,[this](){sendAll();});
    spreadLayout.set("Speaker Layout",0,0,1);reg(spreadLayout,[this](){sendAll();});
    panP.set("Pan",{0.5f},{0.0f},{1.0f});reg(panP,[this](){sendAll();});
    dropdowns.set(spreadMode.getEscapedName(),{"Spread","Channels","Manual"});
    dropdowns.set(spreadLayout.getEscapedName(),{"Ring","Line"});
    auto toStages = [this]() { sendAll(); };
    eqOn.set("EQ On", false);                 reg(eqOn, toStages);
    {
        const char* names[5] = {"Low", "Lo Mid", "Mid", "Hi Mid", "High"};
        const float freqs[5] = {80, 250, 1000, 4000, 12000};
        for(int b = 0; b < 5; b++){
            const bool shelf = scEQEditor::isShelf(b);
            eqGain[b].set(std::string("EQ ") + names[b], 0.0f, -24.0f, 24.0f);
            eqFreq[b].set(std::string("EQ ") + names[b] + " Freq", freqs[b], 20.0f, 20000.0f);
            eqShape[b].set(std::string("EQ ") + names[b] + (shelf ? " Slope" : " Q"), 1.0f, 0.1f, shelf ? 4.0f : 20.0f);
            for(auto* p : {&eqGain[b], &eqFreq[b], &eqShape[b]}) reg(*p, [this]() { sendStageAll(StEq); });
        }
    }
    echoOn.set("Echo On", false);             reg(echoOn, toStages);
    echoDelay.set("Echo Delay", 0.375f, 0.005f, 4.0f);
    echoBeats.set("Echo Sync", true);
    echoBeatVal.set("Echo Beats", 0.75f, 0.0625f, 4.0f);
    echoFeed.set("Echo Feed", 0.45f, 0.0f, 0.98f);
    echoCutoff.set("Echo Cutoff", 84.0f, 12.0f, 130.0f);
    echoResonance.set("Echo Resonance", 0.2f, 0.0f, 0.9f);
    echoFilter.set("Echo Filter", 0, 0, 3);
    echoPingPong.set("Echo Ping-pong", 0.5f, 0.0f, 1.0f);
    echoMix.set("Echo Mix", 0.3f, 0.0f, 1.0f);
    for(auto* p : {&echoDelay, &echoBeatVal, &echoFeed, &echoCutoff, &echoResonance, &echoPingPong, &echoMix})
        reg(*p, [this]() { sendStageAll(StEcho); });
    reg(echoBeats, [this]() { sendStageAll(StEcho); });
    reg(echoFilter, [this]() { sendStageAll(StEcho); });
    dropdowns.set(echoFilter.getEscapedName(), {"LowPass", "HighPass", "BandPass", "PeakEQ"});
    rvbOn.set("Reverb On", false);            reg(rvbOn, toStages);
    rvbMix.set("Reverb Mix", 0.3f, 0.0f, 1.0f);
    rvbDecay.set("Reverb Decay", 3.0f, 0.05f, 30.0f);
    rvbSize.set("Reverb Size", 30.0f, 0.0f, 60.0f);
    rvbPredelay.set("Reverb Predelay", 20.0f, 0.0f, 500.0f);
    rvbLate.set("Reverb Late", 0.8f, 0.0f, 1.0f);
    rvbSpread.set("Reverb Spread", 0.15f, -1.0f, 1.0f);
    rvbLowpass.set("Reverb Lowpass", 10000.0f, 20.0f, 20000.0f);
    rvbHiDamp.set("Reverb Hi Damp", 6.0f, 0.0f, 60.0f);
    rvbLoDamp.set("Reverb Lo Damp", 0.0f, 0.0f, 60.0f);
    rvbModRate.set("Reverb Mod Rate", 0.2f, 0.001f, 20.0f);
    rvbSpin.set("Reverb Spin", 1.0f, 0.0f, 20.0f);
    rvbWander.set("Reverb Wander", 0.25f, 0.0f, 20.0f);
    for(auto* p : {&rvbMix, &rvbDecay, &rvbSize, &rvbPredelay, &rvbLate, &rvbSpread, &rvbLowpass,
                   &rvbHiDamp, &rvbLoDamp, &rvbModRate, &rvbSpin, &rvbWander})
        reg(*p, [this]() { sendStageAll(StRvb); });

    satOn.set("Saturation On", false);                      reg(satOn, toStages);
    satDrive.set("Saturation Drive", 6.0f, 0.0f, 36.0f);    reg(satDrive, [this](){ sendStageAll(StSat); });
    satBias.set("Saturation Bias", 0.0f, 0.0f, 1.0f);       reg(satBias, [this](){ sendStageAll(StSat); });
    satTone.set("Saturation Tone", 20000.0f, 500.0f, 20000.0f); reg(satTone, [this](){ sendStageAll(StSat); });
    satOutput.set("Saturation Output", 0.0f, -24.0f, 12.0f); reg(satOutput, [this](){ sendStageAll(StSat); });
    satMix.set("Saturation Mix", 1.0f, 0.0f, 1.0f);         reg(satMix, [this](){ sendStageAll(StSat); });
    snaps.setupParameters();
    for(auto* p : {&snaps.snapshotP, &snaps.morphAP, &snaps.morphBP, &snaps.morphCurveP, &snaps.morphSwitchP}) reg(*p, [](){});
    for(auto* p : {&snaps.morphP, &snaps.morphTimeP}) reg(*p, [](){});
    for(auto* p : {&snaps.morphBeatsP, &snaps.morphGlideP}) reg(*p, [](){});
    scModulation::Host h;
    h.node = this;
    h.voiceParamsChanged = [this](){ if(!restoring) sendAll(); };
    h.regF = [this](ofParameter<float>& p, std::function<void()> f){ reg(p, f); };
    h.regI = [this](ofParameter<int>& p, std::function<void()> f){ reg(p, f); };
    h.regB = [this](ofParameter<bool>& p, std::function<void()> f){ reg(p, f); };
    h.regV = [this](ofParameter<std::vector<float>>& p, std::function<void()> f){ reg(p, f); };
    h.control = [this](const std::string& k) -> ofAbstractParameter* { for(auto& c : controls) if(c.key == k && !scSnapshots::isOwnKey(k) && k!=afxPreset.getEscapedName()) return c.param; return nullptr; };
    h.publishMenu = [this](const std::string& k){ menu(k); };
    h.publishedUnderline = [this](const std::string& k){ mark(k); };
    h.unpublish = [this](const std::string& k){ unpublish(k); };
    h.publishedKeys = [this]() -> const std::vector<std::string>& { return publishedKeys; };
    h.voices = [this](){ return voices.get(); };
    std::vector<scModulation::Target> targets={{"Pitch",24,"st"},{"FM Amount",4,""},{"Feedback",7,""},{"Amp",1,""},{"Pan",1,""}};
    for(const auto& kind: {std::string("Coarse"),std::string("Detune"),std::string("Output")})
        for(int i=0;i<6;++i) targets.push_back({"Op "+ofToString(i+1)+" "+kind,kind=="Coarse"?16.0f:kind=="Detune"?7.0f:99.0f,""});
    mod.setup(h,targets,true,true,true);
    mod.lfoCount=mod.envCount=mod.extCount=0;
    mod.targetOfKey[pitch.getEscapedName()]=Pitch;
    mod.targetOfKey[panP.getEscapedName()]=Pan;
    mod.targetOfKey[soundParam("mod_scale").getEscapedName()]=Scale;
    mod.targetOfKey[soundParam("feedback").getEscapedName()]=Feedback;
    for(int i=0;i<6;++i) for(int k=0;k<3;++k) {
        const char* sc[]={"coarse","detune","output"};
        const int start[]={Ratio,Detune,Level};
        mod.targetOfKey[soundParam("op"+ofToString(i)+"_"+sc[k]).getEscapedName()]=start[k]+i;
    }
    mod.changed=[this](){if(!restoring) sendAll();};
    for(const auto& c:sound) if(c->sc!="stereo") mod.addVoiceParam(c->p);
    initial = capture();
    const auto bundled=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path().parent_path()/"data/Supercollider/Presets/FMBox/DX7.afx";
    const auto installed=ofToDataPath("Supercollider/Presets/FMBox/DX7.afx",true);
    if(ofFile::doesFileExist(installed)) loadAFX(installed,false);
    else if(std::filesystem::exists(bundled)) loadAFX(bundled.string(),false);
    scSnapshots::Host sh;
    sh.folder = "FMBoxDX7"; sh.title = "fmBox";
    sh.capture = [this](){ return capture(); };
    sh.apply = [this](const ofJson& a, const ofJson& b, float t, bool sw){ apply(a,b,t,sw); };
    sh.factory = [this](){ return factory(); };
    sh.bpm = [this](){ return bpm; };
    sh.paramMenu = [this](const std::string& k){ mark(k); menu(k); };
    sh.morphBlocked = [this](const std::string& k) {
        auto it = published.find(k); return it != published.end() && it->second->hasInConnection();
    };
    for(auto* p : {&satOn,&eqOn,&echoOn,&rvbOn}) snaps.morph.bypass(*p);
    ampMorph.setupVca(snaps.morph,[this](const ofJson& j){return mod.envRoutes(j,"fmMod",Amp);});
    sh.morphChanged=[this,toStages](){sendAll();toStages();};
    snaps.morph.mode(echoFilter, 4);
    for(const auto& c : sound){
        if(c->sc=="algorithm")snaps.morph.mode(c->p,32);
        if(c->sc.rfind("op",0)==0 && c->sc.substr(4)=="coarse"){
            const auto key=c->p.getEscapedName();const float fallback=c->p.get();
            snaps.morph.resolved(c->p,[key,fallback](const ofJson& j){
                const auto it=j.find(key);const float coarse=std::clamp(it!=j.end()&&it->is_number()?it->get<float>():fallback,0.f,31.f);
                static const float decades[]={1,10,100,1000};
                return ofJson::array({coarse,decades[int(std::round(coarse))%4]});
            });
        }
        if(c->sc.size()>10 && (c->sc.substr(4)=="left_curve" || c->sc.substr(4)=="right_curve"))snaps.morph.mode(c->p,4);
        if(c->sc=="lfo_wave" || c->sc=="lfo_sync")for(const auto& depth:sound)
            if(depth->sc=="lfo_pmd" || depth->sc=="lfo_amd")snaps.morph.neutralDepth(c->p,depth->p);
        if(c->sc=="env_model" || c->sc=="matrix_model" || c->sc=="osc_sync" ||
           (c->sc.rfind("op",0)==0 && c->sc.size()>5 && c->sc.substr(4)=="mode"))
            snaps.morph.bypass(c->p);
    }
    snaps.setup(sh);
    if(snaps.active<0&&!afxBank.empty())recallAFX(0);
    requireSynthdefs();
}

void scFMBox::sendNotes(ofxSCServer* server, State& st, unsigned fields) {
    if(!st.gate) return;
    auto now = scheduled.inEffectNow();   // not a scheduled send: the notes as they stand now
    const int n = st.builtVoices;
    // A pitch event must never resend Gate, nor a gate event resend Pitch.
    // Sound automation uses a separate path and cannot overwrite either one.
    // Bundle a polyphonic pitch update into one packet with one timetag.
    bool ready = st.gate->isCreated();
    for(int i=0;i<n;++i) ready = ready && st.voices[i] && st.voices[i]->isCreated();
    if(ready && !(fields & NoteRouting)) {
        ofxOscBundle bundle;
        if(fields & NotePitch) for(int i=0;i<n;++i) {
            ofxOscMessage message;
            message.setAddress("/n_set"); message.addIntArg(st.voices[i]->nodeID);
            message.addStringArg("pitch"); message.addFloatArg(first(pitch.get(),i,60));
            bundle.addMessage(message);
        }
        if(fields & NoteGate) {
            ofxOscMessage message;
            message.setAddress("/n_setn"); message.addIntArg(st.gate->nodeID);
            message.addStringArg("gate"); message.addIntArg(n);
            for(int i=0;i<n;++i) message.addFloatArg(first(gate.get(),i,0));
            bundle.addMessage(message);
        }
        server->sendBundle(bundle);
        return;
    }
    auto vec = [&](const char* name, const std::vector<float>& v, float fallback) {
        std::vector<float> out(n); for(int i=0;i<n;++i) out[i] = first(v,i,fallback);
        st.gate->set(name,out);
    };
    if(fields & NoteGate) vec("gate",gate.get(),0);
    if(fields & NoteRouting) {
        vec("velocity",{1.0f},1); // Gate carries note velocity.
        st.gate->set("out", st.gates->index);
        st.gate->set("vel", mod.sourceBus(server,scModulation::SRC_VEL));
        st.gate->set("rnd", mod.sourceBus(server,scModulation::SRC_RND));
        st.droneSent=droning() ? 1 : 0;
        st.gate->set("drone",float(st.droneSent));
        st.gate->set("vel_sel",0.0f);
        const int gateBus = gateIn.bus(server);
        st.gate->set("gate_sel",gateBus >= 0 ? 1.0f : 0.0f);
        if(gateBus>=0) st.gate->mapan("gate_ar",gateBus,n);
    }
    const int bus = pitchIn.bus(server);
    for(int i=0;i<n;++i) if(auto* v=st.voices[i]) {
        if(fields & NotePitch) v->set("pitch", first(pitch.get(),i,60));
        if(fields & NoteRouting) {
            v->set("pitch_sel",bus>=0 ? 1.0f : 0.0f);
            if(bus>=0) v->mapan("pitch_ar",bus+i,1);
        }
    }
}
void scFMBox::sendSound(ofxSCServer* server, State& st) {
    if(!st.created) return;
    static const char* names[5] = {"mpitch","mscale","mfeedback","mamp","mpan"};
    const float vcaW = ampMorph.vcaAmount(liveVca());
    const float comp=1.0f/std::sqrt(std::max(1.0f,float(st.builtVoices)/st.builtChannels));
    for(int i=0;i<st.builtVoices;++i) if(auto* v=st.voices[i]) {
        scSynthControlBatch batch(v,server);
        auto set = [&](const std::string& name,float value) {
            auto& sent=st.sent[i];auto it=sent.find(name);
            if(it!=sent.end()&&it->second==value) return;
            sent[name]=value;batch.set(name,value);
        };
        auto setVector=[&](const std::string& name,const std::vector<float>& values){
            bool changed=false;auto& sent=st.sent[i];
            for(size_t k=0;k<values.size();++k){
                const auto key=name+"["+ofToString(k)+"]";
                auto previous=sent.find(key);
                if(previous==sent.end() || previous->second!=values[k]){sent[key]=values[k];changed=true;}
            }
            if(changed)batch.set(name,values);
        };
        std::vector<float> morphControls(93,0);morphControls[0]=mod.voiceValue(soundParam("algorithm"),i);
        for(const auto& c : sound) if(c->sc!="stereo"){
            if(c->sc.rfind("op",0)==0 && c->sc.substr(4)=="coarse"){
                const auto tuning=snaps.morph.resolvedValue(c->p);
                if(tuning.is_array() && tuning.size()==2 && tuning[0].is_number() && tuning[1].is_number()){
                    const size_t offset=81+size_t(c->sc[2]-'0')*2;
                    morphControls[offset]=tuning[0].get<float>();
                    morphControls[offset+1]=tuning[1].get<float>();
                }
            }
            set(c->sc,(c->sc=="env_model" || c->sc=="matrix_model" || c->sc=="osc_sync" ||
             (c->sc.rfind("op",0)==0 && c->sc.size()>5 && c->sc.substr(4)=="mode"))
             ? snaps.morph.activation(c->p) : mod.morphVoiceValue(c->p,i,snaps.morph));
            if(c->sc=="algorithm" || (c->sc.size()>10 && (c->sc.substr(4)=="left_curve" || c->sc.substr(4)=="right_curve"))){
                auto weights=snaps.morph.weights(c->p);
                // Preserve per-voice parameter modulation. During a snapshot
                // blend it offsets the endpoint weights, rather than replacing
                // them with the editor's selected side.
                if(!snaps.morph.hasEffective(c->p)){
                    std::fill(weights.begin(),weights.end(),0);
                    weights[std::clamp(int(std::round(mod.voiceValue(c->p,i))),0,int(weights.size())-1)]=1;
                }else{
                    const float delta=mod.morphVoiceValue(c->p,i,snaps.morph)-snaps.morph.value(c->p);
                    if(delta!=0){
                        auto shifted=weights;std::fill(shifted.begin(),shifted.end(),0);
                        for(size_t k=0;k<weights.size();++k){
                            const float position=std::clamp(float(k)+delta,0.f,float(weights.size()-1));
                            const int left=int(position),right=std::min(left+1,int(weights.size())-1);
                            const float f=position-left;
                            shifted[left]+=weights[k]*(1-f);shifted[right]+=weights[k]*f;
                        }
                        weights=std::move(shifted);
                    }
                }
                const size_t offset=c->sc=="algorithm"?1:33+size_t(c->sc[2]-'0')*8+(c->sc.substr(4)=="right_curve"?4:0);
                std::copy(weights.begin(),weights.end(),morphControls.begin()+offset);
            }
        }
        setVector("algorithm",morphControls);
        set("gatebus",st.gates->index+i);
        set("nativebus",st.nativeBus->index);
        set("velbus",mod.sourceBus(server,scModulation::SRC_VEL)+i);
        set("out",st.audio->index+i);
        set("voice",i); set("voices",st.builtVoices);
        set("master",master.get()); set("bpm",bpm);
        set("normalization",1.0f/(st.builtVoices*comp));
        set("vca", vcaW);
        set("vis",st.vis->index);
        for(int t=0;t<5;++t) if(t!=Pan) set(names[t],mod.targetBus(server,t)+i);
        for(int op=0;op<6;++op) {
            set("mratio"+ofToString(op),mod.targetBus(server,Ratio+op)+i);
            set("mdetune"+ofToString(op),mod.targetBus(server,Detune+op)+i);
            set("mlevel"+ofToString(op),mod.targetBus(server,Level+op)+i);
        }
        batch.send();
    }
    if(st.native) {
        scSynthControlBatch native(st.native,server,&st.nativeSent);
        native.set("gatebus",st.gates->index);native.set("out",st.nativeBus->index);
        for(const auto& c:sound) if(c->sc.rfind("lfo_",0)==0) native.set(c->sc,mod.morphVoiceValue(c->p,0,snaps.morph));
        native.send();
    }
    if(st.spread) {
        scSynthControlBatch spread(st.spread,server,&st.spreadSent);
        spread.set("in",st.audio->index);
        spread.set("voices",st.builtVoices);
        auto pans=panPositions();pans.resize(16,0.5f);
        spread.set("line",spreadLayout.get()==1?1.0f:0.0f);
        spread.set("pan",pans); spread.set("mpan",mod.targetBus(server,Pan));
        // Spreader telemetry has its own space after the voice meters.
        spread.set("vis",st.vis->index+96);
        spread.send();
    }
    updateStages(server,st);
}
// Parameter listeners can run many times during one automation/morph pass.
// Send the final sound once in update(); notes keep their exact timetags.
void scFMBox::sendAll() { soundDirty = true; }
void scFMBox::flushSound() {
    if(!soundDirty) return;
    soundDirty = false;
    for(auto& [s,st] : states) sendSound(s,st);
}
void scFMBox::buildSynth(ofxSCServer* server) {
    if(!server) return;
    auto& st=states[server]; release(server,st); st.root=new ofxSCGroup(server);
}
void scFMBox::createSynth(ofxSCServer* server) {
    auto it=states.find(server); if(it==states.end() || !it->second.root) return;
    auto& st=it->second;
    if(placeBefore>=0) st.root->create(2,placeBefore); else st.root->create(0,1);
    for(auto*& g:st.groups) { g=new ofxSCGroup(server); g->create(1,st.root->nodeID); }
    st.gates=new ofxSCBus(RATE_AUDIO,16,server); st.audio=new ofxSCBus(RATE_AUDIO,16,server);
    st.vis=new ofxSCBus(RATE_CONTROL,112,server);
    st.nativeBus=new ofxSCBus(RATE_AUDIO,2,server);
    if(!liveBus(server,st.gates,true)||!liveBus(server,st.audio,true)||!liveBus(server,st.vis,false)||!liveBus(server,st.nativeBus,true)) { release(server,st); return; }
    st.builtVoices=voices.get(); st.builtChannels=channels.get(); st.created=true;
    mod.create(server,st.groups[SlSrc]->nodeID,st.groups[SlRoute]->nodeID,st.gates->index);
    st.gate=new ofxSCSynth("ModGate"+ofToString(st.builtVoices),server);
    for(int i=0;i<st.builtVoices;++i) st.voices[i]=new ofxSCSynth("FMBoxVoice",server);
    st.native=new ofxSCSynth("FMBoxDX7LFO",server);
    st.spread=new ofxSCSynth("PolyCombSpread"+ofToString(st.builtChannels),server);
    sendNotes(server,st); sendSound(server,st);
    st.gate->createAndRun(1,st.groups[SlGate]->nodeID,true);
    st.native->createAndRun(1,st.groups[SlNative]->nodeID,true);
    for(int i=0;i<st.builtVoices;++i) st.voices[i]->createAndRun(1,st.groups[SlVoice]->nodeID,true);
    st.spread->createAndRun(1,st.groups[SlSpread]->nodeID,true);
    routeChain(st);
    if(!getActive()) st.root->run(false);
}
void scFMBox::release(ofxSCServer* server, State& st) {
    if(st.created && st.root) st.root->free();
    if(st.created) mod.free(server,false);
    for(auto& f:st.fx) { delete f.synth; f.synth=nullptr; dropBus(server,f.in,true); f.running=false; f.offAt=-1; f.sent.clear(); f.outSent=-1; }
    delete st.native;st.native=nullptr;dropBus(server,st.nativeBus,true);
    delete st.gate; st.gate=nullptr; delete st.spread; st.spread=nullptr;
    for(auto*& v:st.voices) { delete v; v=nullptr; }
    for(auto& cache:st.sent) cache.clear();
    st.nativeSent.clear();st.spreadSent.clear();st.spreadOutSent=-1;
    for(auto*& g:st.groups) { delete g; g=nullptr; }
    delete st.root; st.root=nullptr; st.created=false;st.droneSent=-1;
    dropBus(server,st.gates,true); dropBus(server,st.audio,true); dropBus(server,st.vis,false);
}
void scFMBox::free(ofxSCServer* s) { auto it=states.find(s); if(it!=states.end()) { release(s,it->second); it->second.out=-1; } }
// A new voice / speaker count: everything again, but in the old root's place.
// The graph is not recomputed (same topology), so a root at the head of the
// default group would run before the nodes feeding Gate/Pitch by
// cable, read silent buses and stay quiet until a reconnection.
void scFMBox::rebuild() {
    structureDirty=false; mod.voicesChanged();
    for(auto& [s,st]:states) if(st.created) {
        const bool wasLatency=s->getBLatency(); s->setBLatency(true);
        ofxSCGroup* old=st.root; st.root=nullptr;
        release(s,st);                  // the old synths go with their root below
        st.root=new ofxSCGroup(s);
        placeBefore=old->nodeID; createSynth(s); placeBefore=-1;
        old->free(); delete old;
        s->setBLatency(wasLatency);
    }
    refreshOutputs();
}
void scFMBox::moveSynthBefore(ofxSCServer* s,int id) { auto it=states.find(s); if(it!=states.end()&&it->second.created) it->second.root->moveBefore(id); }
int scFMBox::getLastSynthID(ofxSCServer* s) { auto it=states.find(s); return it!=states.end()&&it->second.created ? it->second.root->nodeID : -1; }
void scFMBox::setOutputBus(ofxSCServer* s,int,int bus) { auto& st=states[s]; st.out=bus; if(st.created) routeChain(st); }
int scFMBox::getOutputBusIndex(ofxSCServer* s,int) { auto it=states.find(s); return it==states.end() ? -1 : it->second.out; }
void scFMBox::setInputBus(ofxSCServer* s,scNode* node,int bus) { auto& st=states[s]; if(gateIn.take(s,node,bus)|pitchIn.take(s,node,bus)) sendNotes(s,st); }
void scFMBox::resetInputBusses(ofxSCServer* s,int) { gateIn.reset(s);pitchIn.reset(s);sendNotes(s,states[s]); }
void scFMBox::activate() { for(auto& [s,st]:states) if(st.created) st.root->run(true); }
void scFMBox::deactivate() { for(auto& [s,st]:states) if(st.created) st.root->run(false); }
void scFMBox::setBpm(float v) { bpm=std::max(1.0f,v);mod.setBpm(bpm);sendAll(); }
void scFMBox::resendParametersForNRT() {
    for(auto& [s,st]:states) {
        for(auto& cache:st.sent) cache.clear();
        st.nativeSent.clear();st.spreadSent.clear();st.spreadOutSent=-1;
        for(auto& f:st.fx) { f.sent.clear();f.outSent=-1; }
        sendNotes(s,st);
    }
    mod.resend();sendAll();flushSound();
}
#ifdef OFXOCEANODE_HAS_TIMELINE_INSTRUMENT
bool scFMBox::getTimelineInstrument(ofxOceanodeTimelineInstrument& out) {
    if(!gateIn.handle || !pitchIn.handle) return false;
    out.gate=gateIn.handle.get();out.pitch=pitchIn.handle.get();out.velocity=nullptr;out.voices=voices.get();
    return true;
}
#endif

bool scFMBox::connected(const std::string& k) const {
    auto it=published.find(k);return it!=published.end() && (it->second->hasInConnection()||it->second->hasOutConnections());
}
void scFMBox::publish(const std::string& k) {
    if(published.count(k)||getParameterGroup().contains(k)) return;
    auto it=std::find_if(controls.begin(),controls.end(),[&](const Control& c){return c.key==k;});if(it==controls.end()) return;
    if(!separator){addSeparator("Published",ofColor(200));separator=true;}
    published[k]=it->add();dropdowns.apply(k,published[k]);publishedKeys.push_back(k);parameterGroupChanged.notify(this);
}
void scFMBox::unpublish(const std::string& k) {
    auto it=published.find(k);if(it==published.end()||connected(k)) return;
    removeParameter(it->second->getName());published.erase(it);
    publishedKeys.erase(std::remove(publishedKeys.begin(),publishedKeys.end(),k),publishedKeys.end());
    if(published.empty()&&separator){removeSeparator("Published");separator=false;}
    parameterGroupChanged.notify(this);
}
void scFMBox::mark(const std::string& k) {
    timeline.mark(k);
    if(published.count(k)&&!timeline.isHidden(k)) {
        auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x,b.y),b,IM_COL32(106,212,196,255),2);
    }
}
void scFMBox::menu(const std::string& k) {
    if(!ImGui::BeginPopupContextItem(("##fm"+k).c_str())) return;
    controlMenuItems(k);ImGui::EndPopup();
}
void scFMBox::controlMenuItems(const std::string& k) {
    auto it=std::find_if(controls.begin(),controls.end(),[&](const Control& c){return c.key==k;});
    if(it!=controls.end()) scTyped::jsonField(k,it->save,it->load);
    if(timeline.isHidden(k)){if(ImGui::MenuItem("Publish to Node GUI")) timeline.reveal(k);}
    else if(published.count(k)){
        if(connected(k)) ImGui::TextDisabled("Disconnect it to unpublish");
        else if(ImGui::MenuItem("Unpublish from Node")) timeline.unpublish(k);
    }else if(ImGui::MenuItem("Publish to Node GUI")) publish(k);
    timeline.menuItems(k);mod.modulateMenu(k);
}
ofJson scFMBox::capture() const {
    ofJson j=ofJson::object();
    for(const auto& c:controls) if(!scSnapshots::isOwnKey(c.key)&&c.key!=afxPreset.getEscapedName()) j[c.key]=c.save();
    mod.patchBases(j);mod.save(j,"fmMod");return j;
}
void scFMBox::apply(const ofJson& aSaved, const ofJson& bSaved,float t,bool sw) {
    const auto a = scDroneMode::migrated(aSaved), b = scDroneMode::migrated(bSaved);
    restoring=true;
    auto x=a,y=b;snaps.morph.pinSelectors(x,y,t);
    for(const auto& c:controls) if(!scSnapshots::isOwnKey(c.key)&&c.key!=afxPreset.getEscapedName()&&!(published.count(c.key)&&published.at(c.key)->hasInConnection())) {
        const auto& av=x.contains(c.key)?x[c.key]:initial[c.key];
        const auto& bv=y.contains(c.key)?y[c.key]:initial[c.key];
        c.load(blend(av,bv,t,sw,c.discrete));
    }
    mod.loadMorph(a,b,t,sw,"fmMod",[this](const std::string& key){
        auto it=published.find(key);return it!=published.end() && it->second->hasInConnection();
    });
    mod.afterLoad();
    mod.refresh();
    restoring=false;ampMorph.markApplied(liveVca());sendAll();
}
void scFMBox::setAlgorithm(int algorithm) {
    algorithm=std::clamp(algorithm,0,31);
    auto set=[this](const std::string& sc,float value) {
        auto& p=soundParam(sc);const auto it=published.find(p.getEscapedName());
        if(it!=published.end()&&it->second->hasInConnection())return;
        p.setWithoutEventNotifications(value);
    };
    for(int dst=0;dst<6;++dst) {
        for(int src=0;src<6;++src)set("fm"+ofToString(dst)+"_"+ofToString(src),scDX7::algorithms[algorithm][dst*6+src]);
        set("mix"+ofToString(dst),scDX7::algorithms[algorithm][36+dst]);
    }
}

ofJson scFMBox::dx7Snapshot(const scDX7::Patch& patch,int index) const {
    ofJson j=initial;j["name"]=scDX7::presetLabel(index,afxNames);
    auto set=[&](const std::string& sc,float value){for(const auto& c:sound)if(c->sc==sc){j[c->p.getEscapedName()]=value;break;}};
    for(int k=0;k<19;++k)set(scDX7::globalFields[k].sc,patch[k]);
    for(int op=0;op<6;++op) {
        for(int k=0;k<21;++k)set("op"+ofToString(op)+"_"+scDX7::operatorFields[k].sc,patch[19+op*21+k]);
        for(int src=0;src<6;++src)set("fm"+ofToString(op)+"_"+ofToString(src),scDX7::algorithms[patch[0]][op*6+src]);
        set("mix"+ofToString(op),scDX7::algorithms[patch[0]][36+op]);
    }
    return j;
}
bool scFMBox::loadAFX(const std::string& path,bool recallFirst) {
    try {
        auto patches=scDX7::load(path);
        auto catalogue=std::filesystem::path(path);catalogue.replace_extension(".names.tsv");
        if(!std::filesystem::exists(catalogue))catalogue=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path().parent_path()/"data/Supercollider/Presets/FMBox/DX7.names.tsv";
        auto names=scDX7::loadNames(catalogue.string(),patches);
        auto banks=scDX7::bankGroups(patches.size(),names);
        afxBank=std::move(patches);afxNames=std::move(names);afxPath=ofFilePath::getAbsolutePath(path);
        afxBanks=std::move(banks);afxBankSelection=0;
        afxPreset.setMax(int(afxBank.size()));afxPreset.setWithoutEventNotifications(1);
        afxMessage="Loaded "+ofToString(afxBank.size())+" presets";
        if(recallFirst)recallAFX(0);
        return true;
    }catch(const std::exception& e){afxMessage=e.what();return false;}
}
void scFMBox::recallAFX(int index) {
    if(index<0||index>=int(afxBank.size()))return;
    auto state=capture();snaps.morph.capture(state);const auto imported=dx7Snapshot(afxBank[index],index);
    // A native DX7 preset has no effects or auxiliary modulation settings.
    // Keep the Box's processing/routing while replacing its DX7 sound.
    for(const auto& c:sound)if(c->sc!="stereo")state[c->p.getEscapedName()]=imported[c->p.getEscapedName()];
    if(state.contains("fmModParams"))for(auto& entry:state["fmModParams"]) {
        if(!entry.is_object())continue;const auto key=entry.value("key",std::string());
        for(const auto& c:sound)if(c->sc!="stereo"&&c->p.getEscapedName()==key)entry["base"]=std::vector<float>{imported[key].get<float>()};
    }
    snaps.applyExternal(state);
    afxPreset.setWithoutEventNotifications(index+1);
    afxBankSelection=scDX7::bankIndex(index,afxBanks);
    afxMessage=scDX7::presetLabel(index,afxNames,true)+" (algorithm "+ofToString(afxBank[index][0]+1)+")";
}
std::vector<ofJson> scFMBox::factory() const {
    std::vector<ofJson> out;
    // The collection begins with third-party banks. Use Yamaha ROM1A for
    // the default snapshots, retaining its original order and voice names.
    for(int k=0;k<int(afxBank.size())&&k<int(afxNames.size());++k) {
        if(afxNames[k].bank!="ROM1A")continue;
        auto j=dx7Snapshot(afxBank[k],k);
        if(!afxNames[k].name.empty())j["name"]=afxNames[k].name;
        out.push_back(std::move(j));
    }
    if(out.empty()){auto j=initial;j["name"]="DX7 Init";out.push_back(j);}
    return out;
}
void scFMBox::drawPresets(float w) {
    if(ImGui::Button("Open .afx bank...")) {const auto r=ofSystemLoadDialog("Load a DX7-Supercollider .afx preset bank");if(r.bSuccess)loadAFX(r.filePath);}
    if(!afxBank.empty()) {
        ImGui::SameLine();ImGui::TextDisabled("%s / %d presets",ofFilePath::getFileName(afxPath).c_str(),int(afxBank.size()));
        afxBankSelection=std::clamp(afxBankSelection,0,int(afxBanks.size())-1);
        ImGui::SetNextItemWidth(std::min(w-50*zoom(),330*zoom()));
        if(ImGui::BeginCombo("Bank",afxBanks[afxBankSelection].name.c_str())) {
            for(int b=0;b<int(afxBanks.size());++b) {
                const bool selected=b==afxBankSelection;
                if(ImGui::Selectable(afxBanks[b].name.c_str(),selected)){afxBankSelection=b;afxSearch[0]='\0';}
                if(selected)ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        const auto& bank=afxBanks[afxBankSelection];
        auto current=std::find(bank.presets.begin(),bank.presets.end(),afxPreset.get()-1);
        ImGui::BeginDisabled(current==bank.presets.begin());
        if(ImGui::Button("Previous"))afxPreset=current==bank.presets.end()?bank.presets.back()+1:*(current-1)+1;
        ImGui::EndDisabled();ImGui::SameLine();
        ImGui::BeginDisabled(current!=bank.presets.end()&&current+1==bank.presets.end());
        if(ImGui::Button("Next"))afxPreset=current==bank.presets.end()?bank.presets.front()+1:*(current+1)+1;
        ImGui::EndDisabled();ImGui::SameLine();ImGui::TextDisabled("%d presets",int(bank.presets.size()));
        if(std::any_of(afxNames.begin(),afxNames.end(),[](const scDX7::PresetName& n){return !n.bank.empty();}))
            ImGui::TextWrapped("Names follow the loaded bank's catalogue. Unresolved entries are marked Unverified; blank names appear as Unnamed.");
        ImGui::SetNextItemWidth(std::min(w,530*zoom()));ImGui::InputTextWithHint("##afxSearch","Search this bank's names, numbers or algorithms",afxSearch,sizeof(afxSearch));
        const auto query=scDX7::searchText(afxSearch);
        std::vector<int> matches;
        for(int k:bank.presets) {
            const std::string label=scDX7::presetLabel(k,afxNames)+" / algorithm "+ofToString(afxBank[k][0]+1);
            if(query.empty()||scDX7::searchText(label).find(query)!=std::string::npos)matches.push_back(k);
        }
        ImGui::BeginChild("##afxBrowser",ImVec2(w,std::max(160*zoom(),ImGui::GetContentRegionAvail().y-35*zoom())),true);
        const int columns=std::clamp(int(ImGui::GetContentRegionAvail().x/(180*zoom())),1,4);
        const int rows=(int(matches.size())+columns-1)/columns;
        if(ImGui::BeginTable("##afxColumns",columns,ImGuiTableFlags_SizingStretchSame|ImGuiTableFlags_BordersInnerV)) {
            ImGuiListClipper clipper;clipper.Begin(rows);
            while(clipper.Step())for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row) {
                ImGui::TableNextRow();
                for(int col=0;col<columns;++col) {
                    ImGui::TableSetColumnIndex(col);
                    // Read down each column, then continue at the next column.
                    const int i=col*rows+row;if(i>=int(matches.size()))continue;
                    const int k=matches[i];
                    const auto& name=afxNames[k];
                    const std::string label=name.bank.empty()?"Preset "+ofToString(k+1):(name.name.empty()?"Unnamed":name.name);
                    ImGui::PushID(k);
                    if(ImGui::Selectable(label.c_str(),afxPreset.get()==k+1))afxPreset=k+1;
                    if(ImGui::IsItemHovered())ImGui::SetTooltip("%s / algorithm %d",scDX7::presetLabel(k,afxNames,true).c_str(),afxBank[k][0]+1);
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        if(matches.empty())ImGui::TextDisabled("No matching presets in this bank.");
        ImGui::EndChild();
    }
    ImGui::TextWrapped("%s",afxMessage.c_str());
}
void scFMBox::presetSave(ofJson& j) {
    mod.saveLfoOutputs(j);
    j["fmBox"]=capture();
    j["fmBoxAFX"]={{"path",afxPath},{"preset",afxPreset.get()},{"bank",afxBankSelection}};
    // Snapshot controls belong in a patch, but never inside a sound snapshot.
    for(const auto& c:controls) if(scSnapshots::isOwnKey(c.key)) j["fmBox"][c.key]=c.save();
    j["fmBoxPublished"]=publishedKeys;timeline.save(j,"fmBoxTimeline");
}
void scFMBox::loadBeforeConnections(ofJson& j) {
    scDroneMode::migrate(j);
    mod.loadLfoOutputs(j);
    if(j.contains("fmBoxAFX")) {
        const auto& a=j["fmBoxAFX"];const std::string path=a.value("path",std::string());
        if(!path.empty()&&path!=afxPath)loadAFX(path,false);
        afxPreset.setWithoutEventNotifications(std::clamp(a.value("preset",1),1,std::max(1,int(afxBank.size()))));
        afxBankSelection=std::clamp(a.value("bank",scDX7::bankIndex(afxPreset.get()-1,afxBanks)),0,std::max(0,int(afxBanks.size())-1));
    }
    std::vector<std::string> wanted;
    if(j.contains("fmBoxPublished")&&j["fmBoxPublished"].is_array()) for(const auto& k:j["fmBoxPublished"]) if(k.is_string()) wanted.push_back(k.get<std::string>());
    const auto old=publishedKeys;for(const auto& k:old) if(std::find(wanted.begin(),wanted.end(),k)==wanted.end()) unpublish(k);
    for(const auto& k:wanted) publish(k);timeline.load(j,"fmBoxTimeline");
}
void scFMBox::presetRecallAfterSettingParameters(ofJson& j) {
    scDroneMode::migrate(j);
    if(!j.contains(drone.getEscapedName())) drone = false;
    if(!j.contains("fmBox")) return;
    auto state=j["fmBox"];
    apply(state,state,1,true);
    restoring=true;for(const auto& c:controls) if(scSnapshots::isOwnKey(c.key)&&state.contains(c.key)) c.load(state[c.key]);restoring=false;
}
void scFMBox::requireSynthdefs() {
    for(const auto& entry:std::vector<std::pair<std::string,std::string>>{
        {"FMBoxVoice.scsyndef","FMBox"},{"FMBoxDX7LFO.scsyndef","FMBox"},{"ModGate1.scsyndef","Modulation"},{"PolyCombSpread2.scsyndef","PolyComb"},{"mixerProEQ2.scsyndef","MixerPro"}}) {
        std::filesystem::path folder;
        try {
            const std::filesystem::path data=ofToDataPath(SYNTHDEF_DIRECTORY,true);
            if(std::filesystem::exists(data)) for(const auto& f:std::filesystem::recursive_directory_iterator(data,std::filesystem::directory_options::skip_permission_denied))
                if(f.is_regular_file()&&f.path().filename()==entry.first){folder=f.path().parent_path();break;}
            if(folder.empty()) folder=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path().parent_path()/"synthdefs/Defaults"/entry.second/"CompiledSynthdefs"/(entry.second=="MixerPro" ? "mixerpro" : entry.second);
            if(!std::filesystem::exists(folder/entry.first)){ofLogError("fmBox")<<"Compile synthdefs/Defaults/"<<entry.second<<" first";continue;}
            for(auto* sm:servers) if(sm) sm->requireSynthdefFolder(folder.string());
        } catch(const std::exception& e){ofLogError("fmBox")<<e.what();}
    }
}
void scFMBox::update(ofEventArgs&) {
    timeline.update();snaps.update();
    {   // the snapshot names, to a published Snapshot / Morph A / B dropdown
        const auto& names=snaps.slotNames();
        for(const auto& k:snaps.slotKeys()) dropdowns.set(k,names,published.count(k)?published[k]:nullptr);
    }
    if(structureDirty) rebuild();
    for(auto& [server,st]:states) if(st.created && st.droneSent!=(droning()?1:0)) sendNotes(server,st);
    mod.showing=show.get();mod.update();
    flushSound();
    const float now=ofGetElapsedTimef();
    for(auto& [server, st] : states) if(st.created &&
        scFxAwaitingRoute(st.fx, server, [this](int stage){ return stageOn(stage) > 0; },
                          scFxDefinitionsReady(servers, server)))
        updateStages(server, st);
    for(auto& [server,st]:states) if(st.created) {
        for(auto& f:st.fx) {
            if(f.offAt<0 || now<f.offAt) continue;
            f.offAt=-1;f.running=false;routeChain(st);if(f.synth) f.synth->run(false);
        }
    }
    if(!show.get()) return;
    for(auto& [s,st]:states) if(st.created&&liveBus(s,st.vis,false)) {
        live=st.vis->readValues;
        if(now>=nextRead){nextRead=now+1.0f/25;s->requestControlValues(st.vis->index,112);}
        break;
    }
}
void scFMBox::drawWindow() {
    const float z=ofxOceanodeShared::getZoomLevel();
    ImGui::SetNextWindowSize(ImVec2(1100*z,780*z),ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(540*z,400*z),ImVec2(FLT_MAX,FLT_MAX));
    bool open=show.get();
    if(!ImGui::Begin(getWindowTitle().c_str(),&open,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();if(!open) show=false;return;
    }
    if(!open){show=false;ImGui::End();return;}
    keyboardPiano.beginContent(pitch, gate, voices.get());
    const float w=ImGui::GetContentRegionAvail().x;
    drawHeader();
    snaps.drawBar(w);
    ImGui::Spacing();
    if(ImGui::BeginTabBar("##fmTabs")) {
        auto tab=[&](const char* name,std::function<void(float)> body) {
            if(!ImGui::BeginTabItem(name)) return;
            ImGui::BeginChild((std::string("##fmTab")+name).c_str(),ImVec2(0,0),false);
            ImGui::Spacing();
            const float tw=ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ScrollbarSize;
            labelW=70;
            if(std::string(name)=="Effects" || std::string(name)=="Envelopes" || std::string(name)=="LFOs" || std::string(name)=="Routing" || std::string(name)=="Panning" || std::string(name)=="FM Matrix") {
                ImGui::BeginChild("##fxScroll",ImVec2(0,0),false,ImGuiWindowFlags_HorizontalScrollbar);
                body(std::max(tw,std::string(name)=="FM Matrix"?1060*z:960*z));ImGui::EndChild();
            }else body(tw);
            ImGui::EndChild();
            ImGui::EndTabItem();
        };
        tab("DX7 Presets",[this](float tw){drawPresets(tw);});
        tab("Operators",[this](float tw){drawSoundTab(tw);});
        tab("DX7 Global",[this](float tw){drawDX7Global(tw);});
        tab("FM Matrix",[this](float tw){drawMatrix(tw);});
        tab("Envelopes",[this](float tw){
            std::vector<std::function<void(float)>> native;
            for(int op=0;op<6;++op) native.push_back([this,op](float cw){drawNativeEnvelope(op,cw);});
            mod.drawEnvelopeCards(tw,native);
        });
        tab("LFOs",[this](float tw){mod.drawLfos(tw);});
        tab("Routing",[this](float tw){mod.drawRouting(tw);});
        tab("Panning",[this](float tw){drawPanTab(tw);});
        tab("Effects",[this](float tw){drawFxTab(tw);});
        ImGui::EndTabBar();
    }
    scKeyboardPiano::endContent();
    ImGui::End();
}
void scFMBox::draw(ofEventArgs&) {
    if(show.get()) drawWindow();
    keyboardPiano.finishFrame();
}

void scFMBox::sectionHeader(const char* title, float w, ofParameter<bool>* on, ImU32 accent) {
    const float z = zoom();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    const float bh = ImGui::GetFrameHeight() + 6.0f * z;
    dl->AddRectFilled(cur, ImVec2(cur.x + w, cur.y + bh), pal::header, 3.0f * z);
    dl->AddLine(ImVec2(cur.x, cur.y + bh), ImVec2(cur.x + w, cur.y + bh), accent ? accent : pal::accent, 1.5f * z);
    ImGui::SetCursorScreenPos(ImVec2(cur.x + 6.0f * z, cur.y + 3.0f * z));
    if(on){
        bool v = on->get();
        ImGui::PushID(title);
        if(ImGui::Checkbox("##on", &v)) *on = v;
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s on / off", title);
        menu(on->getEscapedName());
        ImGui::PopID();
        ImGui::SameLine(0, 4.0f * z);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, pal::headerText);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(cur.x, cur.y + bh + 4.0f * z));
    ImGui::Dummy(ImVec2(w, 0.0f));
}

void scFMBox::slider(ofParameter<float>& p, const char* label, float w, const char* fmt, bool log) {
    const float z = zoom();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    mark(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(30.0f * z, w - labelW * z));
    float v = p.get();
    if(ImGui::SliderFloat("##v", &v, p.getMin(), p.getMax(), fmt, log ? ImGuiSliderFlags_Logarithmic : 0)) p.set(v);
    menu(key);
    ImGui::PopID();
}

void scFMBox::toggle(ofParameter<bool>& p, const char* label) {
    bool v = p.get();
    if(ImGui::Checkbox(label, &v)) p.set(v);
    mark(p.getEscapedName());
    menu(p.getEscapedName());
}


void scFMBox::updateStages(ofxSCServer* server, State& st) {
    if(!st.created) return;
    for(int s = 0; s < NumStages; s++){
        auto& f = st.fx[s];
        if(stageOn(s)){
            if(!f.synth && !scFxDefinitionsReady(servers, server)) continue;
            if(!liveBus(server, f.in,true)){
                delete f.in;
                f.in = new ofxSCBus(RATE_AUDIO, channels.get(), server);
                if(!liveBus(server, f.in,true)){ f.in->free(); delete f.in; f.in = nullptr; continue; }
                if(f.synth) f.synth->set("in", f.in->index);
            }
            if(!f.synth){
                f.synth = new ofxSCSynth(stageDef(s), server);
                f.synth->set("in", f.in->index);
                f.synth->set("out", std::max(0,st.out));
                sendStage(s, f.synth, server);
                f.synth->createAndRun(1, st.groups[SlFx + s]->nodeID, true);
            }else{
                sendStage(s, f.synth, server);
                if(!f.running) f.synth->run(true);
            }
            f.running = scFxCanRoute(f.synth, server);
            f.offAt = -1.0f;
        }else if(f.synth && f.running && f.offAt < 0.0f){
            if(s != StEq) f.synth->set("on", 0.0f);
            f.sent.erase("on"); // re-enabling must restore the cached control
            f.offAt = ofGetElapsedTimef() + stageTail(s);
        }
    }
    routeChain(st);
}

// the spreader -> the running effects -> out
void scFMBox::routeChain(State& st) {
    int next = std::max(0,st.out);
    for(int s = NumStages - 1; s >= 0; s--){
        auto& f = st.fx[s];
        if(!f.synth || !f.running || !f.in) continue;
        if(f.outSent!=next) { f.synth->set("out", next);f.outSent=next; }
        next = f.in->index;
    }
    if(st.spread && st.spreadOutSent!=next) { st.spread->set("out", next);st.spreadOutSent=next; }
}

float scFMBox::stageOn(int s) const {
    return snaps.morph.activation(s == StSat ? satOn : s == StEq ? eqOn : s == StEcho ? echoOn : rvbOn);
}

std::string scFMBox::stageDef(int s) const {
    static const char* names[NumStages] = {"mixerProSat", "mixerProEQ", "mixerProEcho", "boxMicroSpace"};
    return names[s] + ofToString(channels.get());
}

float scFMBox::echoDelaySec() const {
    if(!echoBeats.get()) return ofClamp(echoDelay.get(), 0.005f, 4.0f);
    return ofClamp(echoBeatVal.get() * 60.0f / std::max(1.0f, bpm), 0.005f, 4.0f);
}

float scFMBox::stageTail(int s) const {
    if(s == StEq || s == StSat) return 0.02f;
    if(s == StEcho){
        const float feed = ofClamp(echoFeed.get(), 0.0f, 0.98f);
        const float repeats = feed > 0.001f ? std::log(0.001f) / std::log(feed) : 0.0f;
        return std::min(30.0f, echoDelaySec() * (repeats + 1.0f) + 0.2f);
    }
    return std::min(30.0f, rvbDecay.get() + rvbPredelay.get() * 0.001f + 0.3f);
}

void scFMBox::sendStage(int s, ofxSCSynth* x, ofxSCServer* server) {
    if(!x) return;
    scSynthControlBatch::Cache* cache = nullptr;
    const auto state = states.find(server);
    if(state != states.end() && state->second.fx[s].synth == x) cache = &state->second.fx[s].sent;
    scSynthControlBatch controls(x, server, cache);
    const float on = stageOn(s);
    if(s == StSat){
        controls.set("drive",satDrive.get());controls.set("bias",satBias.get());controls.set("tone",satTone.get());
        controls.set("autogain",1.0f);controls.set("output",satOutput.get());controls.set("mix",satMix.get());controls.set("on",on);
    }else if(s == StEq){
        controls.set("on", on);
        // mixerProEQ: gains dB, freqs Hz, shelves bNslope (= 1/S), peaks bNrq (= 1/Q)
        for(int b = 0; b < 5; b++){
            const std::string k = "b" + ofToString(b + 1);
            controls.set(k + "gain", eqGain[b].get());
            controls.set(k + "freq", eqFreq[b].get());
            if(scEQEditor::isShelf(b)) controls.set(k + "slope", 1.0f / scEQEditor::safeShelfSlope(eqShape[b].get(), eqGain[b].get()));
            else controls.set(k + "rq", 1.0f / std::max(0.01f, eqShape[b].get()));
        }
    }else if(s == StEcho){
        controls.set("delay", echoDelaySec());
        controls.set("feed", ofClamp(echoFeed.get(), 0.0f, 0.98f));
        controls.set("cutoff", ofClamp(echoCutoff.get(), 12.0f, 130.0f));
        controls.set("resonance", ofClamp(echoResonance.get(), 0.0f, 0.9f));
            controls.set("filterweights", snaps.morph.weights(echoFilter));
        controls.set("filtertype", (float)ofClamp(echoFilter.get(), 0, 3));
        controls.set("pingpong", ofClamp(echoPingPong.get(), 0.0f, 1.0f));
        controls.set("mix", ofClamp(echoMix.get(), 0.0f, 1.0f));
        controls.set("on", on);
    }else{
        controls.set("mix", ofClamp(rvbMix.get(), 0.0f, 1.0f));
        controls.set("decay", std::max(0.05f, rvbDecay.get()));
        controls.set("size", ofClamp(rvbSize.get(), 0.0f, 60.0f));
        controls.set("predelay", ofClamp(rvbPredelay.get(), 0.0f, 500.0f));
        controls.set("position", ofClamp(rvbLate.get(), 0.0f, 1.0f));
        controls.set("spread", ofClamp(rvbSpread.get(), -1.0f, 1.0f));
        controls.set("lowpass", ofClamp(rvbLowpass.get(), 20.0f, 20000.0f));
        controls.set("highdamp", ofClamp(rvbHiDamp.get(), 0.0f, 60.0f));
        controls.set("lowdamp", ofClamp(rvbLoDamp.get(), 0.0f, 60.0f));
        controls.set("modfreq", ofClamp(rvbModRate.get(), 0.001f, 20.0f));
        controls.set("spin", ofClamp(rvbSpin.get(), 0.0f, 20.0f));
        controls.set("wander", ofClamp(rvbWander.get(), 0.0f, 20.0f));
        controls.set("on", on);
    }
    controls.send();
}

void scFMBox::sendStageAll(int) { sendAll(); }

void scFMBox::drawFxTab(float w) {
    const float z = zoom();
    const float cw = colW(w, 4);
    ImGui::BeginGroup();
    // EQ: one column
    sectionHeader("EQ", cw, &eqOn);
    for(int b = 0; b < 5; b++) eqEditor.bands[b] = {eqFreq[b].get(), eqGain[b].get(), eqShape[b].get()};
    eqEditor.dbRange = 24.0f; eqEditor.gainLimit = 24.0f;
    eqEditor.qMin = 0.1f; eqEditor.qMax = 20.0f; eqEditor.slopeMin = 0.1f; eqEditor.slopeMax = 4.0f;
    float sr = 48000.0f;
    for(auto* sm : servers) if(sm && sm->getServer()){ sr = (float)serverManager::getSampleRateForServer(sm->getServer()); break; }
    eqEditor.recompute(sr > 0 ? sr : 48000.0f);
    eqEditor.bandMenu = [this](int b){
        ImGui::TextUnformatted(scEQEditor::bandName(b));
        ImGui::Separator();
        auto value = [this](ofParameter<float>& p, const char* label){
            float v = p.get();
            ImGui::SetNextItemWidth(160.0f * zoom());
            if(ImGui::InputFloat(label, &v, 0, 0, "%.3f")) p.set(ofClamp(v, p.getMin(), p.getMax()));
            if(ImGui::BeginMenu((std::string(label)+" control").c_str())) {
                controlMenuItems(p.getEscapedName());ImGui::EndMenu();
            }
        };
        value(eqFreq[b], "Freq (Hz)");
        value(eqGain[b], "Gain (dB)");
        value(eqShape[b], scEQEditor::isShelf(b) ? "Slope" : "Q");
    };
    if(!eqOn.get()) ImGui::BeginDisabled();
    if(eqEditor.draw(cw, 150.0f * z, scEQEditor::defaultColors(), eqOn.get(), nullptr, 0, "##pcEq")){
        for(int b = 0; b < 5; b++){
            eqFreq[b] = eqEditor.bands[b].freqHz;
            eqGain[b] = eqEditor.bands[b].gainDb;
            eqShape[b] = eqEditor.bands[b].shape;
        }
    }
    if(!eqOn.get()) ImGui::EndDisabled();
    ImGui::TextColored(ImColor(pal::textDim), "Drag the bands;\nright-click one for values.");
    ImGui::Spacing();
    sectionHeader("Saturation", cw, &satOn);
    if(!satOn.get()) ImGui::BeginDisabled();
    slider(satDrive,"Drive",cw,"%.1f dB");
    slider(satBias,"Bias",cw,"%.2f");
    slider(satTone,"Tone",cw,"%.0f Hz",true);
    slider(satOutput,"Output",cw,"%+.1f dB");
    slider(satMix,"Mix",cw,"%.2f");
    if(!satOn.get()) ImGui::EndDisabled();
    nextCol();
    sectionHeader("Echo", cw, &echoOn);
    if(!echoOn.get()) ImGui::BeginDisabled();
    toggle(echoBeats, "Sync to tempo");
    if(echoBeats.get()){
        static const std::vector<std::pair<const char*, float>> notes = {
            {"1/32", 0.125f}, {"1/16T", 1.0f / 6.0f}, {"1/16", 0.25f}, {"1/8T", 1.0f / 3.0f}, {"1/16D", 0.375f},
            {"1/8", 0.5f}, {"1/4T", 2.0f / 3.0f}, {"1/8D", 0.75f}, {"1/4", 1.0f}, {"1/2T", 4.0f / 3.0f},
            {"1/4D", 1.5f}, {"1/2", 2.0f}, {"1/2D", 3.0f}, {"1 bar", 4.0f}};
        int k = 0;
        for(int i = 0; i < (int)notes.size(); i++)
            if(std::abs(notes[i].second - echoBeatVal.get()) < std::abs(notes[k].second - echoBeatVal.get())) k = i;
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Delay");
        mark(echoBeatVal.getEscapedName());
        ImGui::SameLine();
        ImGui::SetCursorPosX(x0 + labelW * z);
        ImGui::SetNextItemWidth(cw - labelW * z);
        if(ImGui::SliderInt("##echoNote", &k, 0, (int)notes.size() - 1, notes[k].first)) echoBeatVal = notes[k].second;
        menu(echoBeatVal.getEscapedName());
    }else{
        slider(echoDelay, "Delay", cw, "%.3f s", true);
    }
    slider(echoFeed, "Feedback", cw, "%.2f");
    slider(echoCutoff, "Cutoff", cw, "%.0f (note)");
    slider(echoResonance, "Resonance", cw, "%.2f");
    {
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Filter");
        ImGui::SameLine();
        ImGui::SetCursorPosX(x0 + labelW * z);
        ImGui::SetNextItemWidth(cw - labelW * z);
        const auto types = dropdowns.names(echoFilter.getEscapedName());
        int ft = std::clamp(echoFilter.get(), 0, (int)types.size() - 1);
        if(ImGui::Combo("##echoFilter", &ft, types.data(), (int)types.size())) echoFilter = ft;
        menu(echoFilter.getEscapedName());
    }
    slider(echoPingPong, "Ping-pong", cw, "%.2f");
    slider(echoMix, "Mix", cw, "%.2f");
    if(!echoOn.get()) ImGui::EndDisabled();
    nextCol();
    sectionHeader("Reverb", cw, &rvbOn);
    if(!rvbOn.get()) ImGui::BeginDisabled();
    slider(rvbMix, "Mix", cw, "%.2f");
    slider(rvbDecay, "Decay", cw, "%.2f s", true);
    slider(rvbSize, "Size", cw, "%.1f");
    slider(rvbPredelay, "Predelay", cw, "%.0f ms");
    slider(rvbLate, "Late", cw, "%.2f");
    slider(rvbSpread, "Spread", cw, "%+.2f");
    if(!rvbOn.get()) ImGui::EndDisabled();
    nextCol();
    sectionHeader("Reverb Tone", cw);
    if(!rvbOn.get()) ImGui::BeginDisabled();
    slider(rvbLowpass, "Lowpass", cw, "%.0f Hz", true);
    slider(rvbHiDamp, "Hi Damp", cw, "%.1f dB");
    slider(rvbLoDamp, "Lo Damp", cw, "%.1f dB");
    slider(rvbModRate, "Mod Rate", cw, "%.3f Hz", true);
    slider(rvbSpin, "Spin", cw, "%.2f");
    slider(rvbWander, "Wander", cw, "%.2f");
    if(!rvbOn.get()) ImGui::EndDisabled();
    ImGui::EndGroup();
    ImGui::TextColored(ImColor(pal::textDim), "Voices -> Saturation -> EQ -> Echo -> Reverb (MixerPro's), at speaker count.");
}

ofParameter<float>& scFMBox::soundParam(const std::string& name) {
    return (*std::find_if(sound.begin(),sound.end(),[&](const auto& c){return c->sc==name;}))->p;
}
void scFMBox::drawSoundTab(float w) {
    const int columns=std::clamp(int(w/(250*zoom())),1,3);const float cw=colW(w,columns);
    ImGui::PushTextWrapPos();ImGui::TextDisabled("DX7 operator Output controls both audible carriers and modulation depth. Mix in the FM Matrix selects carriers. Right-click any control to publish, modulate or automate.");ImGui::PopTextWrapPos();
    auto control=[&](const std::string& pre,const char* key,const char* label) {
        auto& p=soundParam(pre+key);
        const auto items=dropdowns.names(p.getEscapedName());
        if(items.empty()){slider(p,label,cw,"%.0f");return;}
        ImGui::PushID(p.getEscapedName().c_str());
        const float x=ImGui::GetCursorPosX();ImGui::AlignTextToFramePadding();ImGui::TextUnformatted(label);mark(p.getEscapedName());
        ImGui::SameLine();ImGui::SetCursorPosX(x+labelW*zoom());ImGui::SetNextItemWidth(cw-labelW*zoom());
        int index=std::clamp(int(std::round(p.get())),0,int(items.size())-1);
        if(ImGui::Combo("##choice",&index,items.data(),int(items.size())))p=float(index);
        menu(p.getEscapedName());ImGui::PopID();
    };
    for(int row=0;row<(6+columns-1)/columns;++row) {
        ImGui::BeginGroup();
        for(int col=0;col<columns&&row*columns+col<6;++col) {
            if(col)nextCol();const int op=row*columns+col;const auto pre="op"+ofToString(op)+"_";
            sectionHeader(("Operator "+ofToString(op+1)).c_str(),cw);labelW=90;
            for(auto field:{std::make_pair("mode","Mode"),{"coarse","Coarse"},{"fine","Fine"},{"detune","Detune"},{"output","Output"},{"velocity","Velocity"},{"amp_sens","Amp Sens"}})control(pre,field.first,field.second);
            ImGui::PushID(op);
            if(ImGui::TreeNode("Keyboard scaling")) {
                // Keep full-width controls despite the tree's text indentation.
                ImGui::Unindent();
                for(auto field:{std::make_pair("rate_scale","Rate Scale"),{"breakpoint","Breakpoint"},{"left_depth","Left Depth"},{"left_curve","Left Curve"},{"right_depth","Right Depth"},{"right_curve","Right Curve"}})control(pre,field.first,field.second);
                ImGui::Indent();ImGui::TreePop();
            }
            ImGui::PopID();
            float peak=0;for(int v=0;v<voices.get();++v)if(v*6+op<int(live.size()))peak=std::max(peak,live[v*6+op]);
            ImGui::ProgressBar(ofClamp(peak,0.0f,1.0f),ImVec2(cw,5*zoom()),"");
        }
        ImGui::EndGroup();ImGui::Spacing();
    }
    labelW=70;
}
void scFMBox::drawDX7Global(float w) {
    const int columns=std::clamp(int(w/(270*zoom())),1,3);const float cw=colW(w,columns);
    auto section=[&](const char* title,int n){if(n&&n%columns)nextCol();else if(n){ImGui::EndGroup();ImGui::BeginGroup();}sectionHeader(title,cw);};
    auto control=[&](const char* sc,const char* label){
        auto& p=soundParam(sc);const auto items=dropdowns.names(p.getEscapedName());
        if(items.empty()){slider(p,label,cw,"%.0f");return;}
        ImGui::PushID(sc);ImGui::TextUnformatted(label);mark(p.getEscapedName());ImGui::SetNextItemWidth(cw);
        int index=std::clamp(int(std::round(p.get())),0,int(items.size())-1);
        if(ImGui::Combo("##choice",&index,items.data(),int(items.size())))p=float(index);
        menu(p.getEscapedName());ImGui::PopID();
    };
    ImGui::BeginGroup();section("DX7 Engine",0);
    control("algorithm","Algorithm (resets routes)");control("feedback","Feedback");control("transpose","Transpose (24 = no shift)");control("osc_sync","Osc Sync");
    control("matrix_model","Matrix model");
    slider(soundParam("mod_scale"),"FM Amount",cw,"%.3f");slider(soundParam("env_time"),"Env Time",cw,"%.3fx",true);slider(soundParam("stereo"),"Width",cw,"%.3f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("0: voices centered (both stereo channels). 1: voices spread across the outputs.\nAt full width, the first keyboard voice plays on channel 1.");
    control("env_model","Envelope model");
    if(soundParam("env_model").get()>.5f)slider(soundParam("env_curve"),"EG Curve",cw,"%.2f");
    section("Native DX7 LFO",1);
    control("lfo_wave","Wave");control("lfo_speed","Speed");control("lfo_delay","Delay");control("lfo_sync","Sync");
    control("lfo_pmd","Pitch Depth");control("lfo_pms","Pitch Sens");control("lfo_amd","Amp Depth");
    section("Pitch Envelope",2);
    for(int k=0;k<4;++k)slider(soundParam("pitch_r"+ofToString(k)),("Time "+ofToString(k+1)).c_str(),cw,"%.0f");
    for(int k=0;k<4;++k)slider(soundParam("pitch_l"+ofToString(k)),("Level "+ofToString(k+1)).c_str(),cw,"%.0f");
    ImGui::EndGroup();
}
void scFMBox::drawMatrix(float w) {
    const float z=zoom();
    w=std::max(w,1060*z);
    const int algorithm=std::clamp(int(std::round(soundParam("algorithm").get())),0,31);
    const bool raw=soundParam("matrix_model").get()>.5f;
    ImGui::Text(raw?"Raw FM / diagonal feedback":"Algorithm %d",algorithm+1);
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled("Send down a column, receive across a row. Drag a junction up/down; double-click to type. Zero removes a route. Amber marks routes scaled by Feedback.");
    ImGui::PopTextWrapPos();
    slider(soundParam("mod_scale"),"FM Amount",std::min(w,310*z),"%.3f");
    if(w>=650*z)ImGui::SameLine(0,20*z);
    slider(soundParam("feedback"),"Feedback",std::min(w,310*z),"%.3f");
    ImGui::Spacing();
    const float gutter=18*z;
    const float cell=std::min(78*z,(w-480*z-gutter)/7),extent=7*cell;
    ImGui::BeginGroup();ImGui::PushID("diagram");
    const ImVec2 origin=ImGui::GetCursorScreenPos();
    auto point=[&](int col,int row){return ImVec2(origin.x+(col+.5f)*cell,origin.y+(row+.36f)*cell);};
    auto* dl=ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x+extent,origin.y+extent);
    canvas(dl,origin,end);
    const ImU32 colors[]={IM_COL32(150,165,255,255),IM_COL32(112,209,185,255),IM_COL32(245,187,115,255),
        IM_COL32(222,148,200,255),IM_COL32(120,193,240,255),IM_COL32(209,207,132,255)};
    const float boxW=cell*.70f,boxH=cell*.40f,junctionW=cell*.62f,junctionH=ImGui::GetTextLineHeight()+6*z;
    for(int k=1;k<7;++k) {
        dl->AddLine(ImVec2(origin.x+k*cell,origin.y),ImVec2(origin.x+k*cell,end.y),pal::grid);
        dl->AddLine(ImVec2(origin.x,origin.y+k*cell),ImVec2(end.x,origin.y+k*cell),pal::grid);
    }
    auto arrow=[&](ImVec2 tip,float direction,ImU32 color) {
        dl->AddTriangleFilled(tip,ImVec2(tip.x-direction*5*z,tip.y-3*z),ImVec2(tip.x-direction*5*z,tip.y+3*z),color);
    };
    // Orthogonal wires connect source -> junction -> destination. The diagonal
    // operator tiles stay in front of the wires; every route has its own cell.
    for(int dst=0;dst<6;++dst)for(int src=0;src<6;++src) {
        const float amount=soundParam("fm"+ofToString(dst)+"_"+ofToString(src)).get();
        if(amount<0.0001f)continue;
        const auto source=point(src,src),target=point(dst,dst),junction=point(src,dst);
        const float thickness=(1+std::min(amount,2.0f)*.45f)*z;
        if(src!=dst) {
            const float direction=dst>src?1.f:-1.f;
            const ImVec2 tip(target.x-direction*(boxW*.5f+2*z),target.y);
            dl->AddLine(source,junction,colors[src],thickness);
            dl->AddLine(junction,tip,colors[src],thickness);arrow(tip,direction,colors[src]);
        }else {
            const float x=source.x+cell*.43f,y=source.y+cell*.43f;
            dl->AddLine(source,ImVec2(source.x,y),colors[src],thickness);
            dl->AddLine(ImVec2(source.x,y),ImVec2(x,y),colors[src],thickness);
            dl->AddLine(ImVec2(x,y),ImVec2(x,source.y),colors[src],thickness);
            const ImVec2 tip(source.x+boxW*.5f+2*z,source.y);
            dl->AddLine(ImVec2(x,source.y),tip,colors[src],thickness);arrow(tip,-1,colors[src]);
        }
    }
    const auto output=point(6,6);
    for(int op=0;op<6;++op)if(soundParam("mix"+ofToString(op)).get()>.0001f) {
        const auto source=point(op,op),junction=point(op,6);
        dl->AddLine(source,junction,colors[op],1.5f*z);
        dl->AddLine(junction,ImVec2(output.x-boxW*.5f-2*z,output.y),colors[op],1.5f*z);
    }
    auto amountCell=[&](ofParameter<float>& p,ImVec2 center,float max,const char* tooltip,bool feedback) {
        const std::string key=p.getEscapedName();ImGui::PushID(key.c_str());
        ImGui::SetCursorScreenPos(ImVec2(center.x-junctionW*.5f,center.y-junctionH*.5f));
        ImGui::SetNextItemWidth(junctionW);
        const float value=p.get();
        ImGui::PushStyleColor(ImGuiCol_FrameBg,value>.0001f?ImVec4(.17f,.19f,.25f,1):ImVec4(.055f,.06f,.075f,1));
        ImGui::PushStyleColor(ImGuiCol_Text,feedback?ImVec4(.96f,.73f,.45f,1):value>.0001f?ImVec4(.88f,.90f,.96f,1):ImVec4(.36f,.39f,.45f,1));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,0);
        float v=value;
        if(ImGui::DragFloat("##amount",&v,max==1?.005f:.01f,0,max,"%.2f",ImGuiSliderFlags_AlwaysClamp|ImGuiSliderFlags_Vertical))p=v;
        ImGui::PopStyleVar();ImGui::PopStyleColor(2);
        if(feedback)dl->AddRect(ImGui::GetItemRectMin(),ImGui::GetItemRectMax(),IM_COL32(245,187,115,200),0,0,1.5f*z);
        if(ImGui::IsItemHovered())ImGui::SetTooltip("%s\n%.3f | Drag vertically, double-click to type; 0 disconnects.\nRight-click: publish, modulation and timeline.%s",tooltip,p.get(),feedback?"\nFeedback-scaled route.":"");
        mark(key);menu(key);ImGui::PopID();
    };
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(4*z,3*z));
    for(int dst=0;dst<6;++dst)for(int src=0;src<6;++src) {
        auto center=point(src,dst);
        if(src==dst)center.y+=cell*.39f;
        const auto label="Operator "+ofToString(src+1)+" -> Operator "+ofToString(dst+1)+(src==dst?" (self-feedback)":"");
        amountCell(soundParam("fm"+ofToString(dst)+"_"+ofToString(src)),center,4,label.c_str(),(raw?dst==src:dst*6+src==scDX7::feedbackCell[algorithm]));
    }
    for(int op=0;op<6;++op) {
        const auto label="Operator "+ofToString(op+1)+" -> audio output";
        amountCell(soundParam("mix"+ofToString(op)),point(op,6),1,label.c_str(),false);
    }
    ImGui::PopStyleVar();
    for(int op=0;op<6;++op) {
        const auto center=point(op,op);
        const ImVec2 a(center.x-boxW*.5f,center.y-boxH*.5f),b(center.x+boxW*.5f,center.y+boxH*.5f);
        dl->AddRectFilled(a,b,pal::header);dl->AddRect(a,b,colors[op],0,0,1.5f*z);
        const std::string number="Op "+ofToString(op+1);const auto size=ImGui::CalcTextSize(number.c_str());
        dl->AddText(ImVec2(center.x-size.x*.5f,center.y-size.y*.5f),colors[op],number.c_str());
    }
    const ImVec2 a(output.x-boxW*.5f,output.y-boxH*.5f),b(output.x+boxW*.5f,output.y+boxH*.5f);
    dl->AddRectFilled(a,b,pal::header);dl->AddRect(a,b,pal::headerText);
    const auto text=ImGui::CalcTextSize("OUT");dl->AddText(ImVec2(output.x-text.x*.5f,output.y-text.y*.5f),pal::headerText,"OUT");
    ImGui::SetCursorScreenPos(ImVec2(origin.x,end.y+ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(extent,0));
    ImGui::PopID();ImGui::EndGroup();
    ImGui::SameLine(0,gutter);
    ImGui::BeginGroup();ImGui::PushID("sliders");
    const float matrixW=w-extent-gutter;
    sectionHeader("Routing Matrix",matrixW);
    ImGui::TextDisabled(raw?"Raw coefficients; OUT sets the direct mix.":"DX7 units (1 = 4pi); OUT sets the direct mix.");
    if(ImGui::BeginTable("##fmSliders",7,ImGuiTableFlags_Borders|ImGuiTableFlags_SizingStretchSame,ImVec2(matrixW,0))) {
        ImGui::TableSetupColumn("To / From",ImGuiTableColumnFlags_WidthFixed,66*z);
        for(int src=0;src<6;++src)ImGui::TableSetupColumn(ofToString(src+1).c_str());
        ImGui::TableHeadersRow();
        for(int dst=0;dst<7;++dst) {
            ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);
            if(dst==6)ImGui::TextUnformatted("OUT");else ImGui::Text("Op %d",dst+1);
            for(int src=0;src<6;++src) {
                ImGui::TableSetColumnIndex(src+1);
                auto& p=soundParam(dst==6?"mix"+ofToString(src):"fm"+ofToString(dst)+"_"+ofToString(src));
                const auto key=p.getEscapedName();ImGui::PushID(key.c_str());
                ImGui::SetNextItemWidth(-1);
                const bool feedback=dst<6&&(raw?dst==src:dst*6+src==scDX7::feedbackCell[algorithm]);
                if(feedback)ImGui::PushStyleColor(ImGuiCol_FrameBg,ImVec4(.22f,.16f,.11f,1));
                float v=p.get();if(ImGui::SliderFloat("##amount",&v,0,dst==6?1.f:4.f,"%.2f",ImGuiSliderFlags_AlwaysClamp))p=v;
                if(feedback)ImGui::PopStyleColor();
                if(ImGui::IsItemHovered()) {
                    if(dst==6)ImGui::SetTooltip("Operator %d -> audio output",src+1);
                    else ImGui::SetTooltip("Operator %d -> Operator %d%s",src+1,dst+1,feedback?" (feedback-scaled route)":"");
                }
                mark(key);menu(key);ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopID();ImGui::EndGroup();
}

void scFMBox::drawNativeEnvelope(int op,float w) {
    ImGui::PushID(op);ImGui::BeginGroup();sectionHeader(("Operator "+ofToString(op+1)).c_str(),w);
    const std::string pre="op"+ofToString(op)+"_";
    const float z=zoom(),h=100*z;const ImVec2 a=ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##opEnvelope",ImVec2(w,h));auto* dl=ImGui::GetWindowDrawList();
    dl->AddRectFilled(a,ImVec2(a.x+w,a.y+h),pal::canvas);
    ImVec2 points[5];points[0]=ImVec2(a.x,a.y+h*(1-soundParam(pre+"l3").get()/99));
    for(int k=0;k<4;++k)points[k+1]=ImVec2(a.x+(k+1)*w/4,a.y+h*(1-soundParam(pre+"l"+ofToString(k)).get()/99));
    for(int k=0;k<4;++k){dl->AddLine(points[k],points[k+1],pal::accent,2*z);dl->AddCircleFilled(points[k+1],4*z,pal::accent);}
    if(ImGui::IsItemHovered()&&ImGui::IsMouseClicked(0)) {
        float best=100*z*z;draggingPoint=-1;for(int k=0;k<4;++k){const auto m=ImGui::GetMousePos();const float dx=m.x-points[k+1].x,dy=m.y-points[k+1].y;
            if(dx*dx+dy*dy<best){best=dx*dx+dy*dy;draggingPoint=k;draggingOp=op;}}
    }
    if(!ImGui::IsMouseDown(0)){draggingPoint=-1;draggingOp=-1;}
    if(draggingOp==op&&draggingPoint>=0&&ImGui::IsItemActive())soundParam(pre+"l"+ofToString(draggingPoint))=ofClamp(99*(1-(ImGui::GetMousePos().y-a.y)/h),0.0f,99.0f);
    if(ImGui::IsItemHovered())ImGui::SetTooltip(soundParam("env_model").get()>.5f?"Times are seconds; Envelope Curve sets every segment. L3 sustains; L4 starts and ends the envelope.":"AFX times: 0 fast, 99 slow. L4 starts and ends the envelope; L3 sustains. Drag levels.\nWidths are schematic; actual segment times depend on rates, levels, note and velocity.");
    labelW=60;
    const bool seconds=soundParam("env_model").get()>.5f;
    for(int k=0;k<4;++k)slider(soundParam(pre+(seconds?"seconds":"r")+ofToString(k)),("Time "+ofToString(k+1)).c_str(),w,seconds?"%.4f s":"%.0f",seconds);
    for(int k=0;k<4;++k)slider(soundParam(pre+"l"+ofToString(k)),("Level "+ofToString(k+1)).c_str(),w,"%.0f");
    labelW=70;ImGui::EndGroup();ImGui::PopID();
}

void scFMBox::sliderInt(ofParameter<int>& p, const char* label, float w) {
    const float z = zoom();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    mark(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(30.0f * z, w - labelW * z));
    int v = p.get();
    if(ImGui::SliderInt("##v", &v, p.getMin(), p.getMax())) p.set(v);
    menu(key);
    ImGui::PopID();
}

void scFMBox::combo(ofParameter<int>& p, const char* label, float w) {
    const float z = zoom();
    const std::string key = p.getEscapedName();
    const auto items = dropdowns.names(key);
    if(items.empty()) return;
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    mark(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(30.0f * z, w - labelW * z));
    int v = std::clamp(p.get(), 0, (int)items.size() - 1);
    if(ImGui::Combo("##v", &v, items.data(), (int)items.size())) p.set(v);
    menu(key);
    ImGui::PopID();
}


bool scFMBox::droning() const {
    return drone.get();
}
void scFMBox::drawHeader() {
    const float z=zoom();
    // The same label widths and gutters as PolyComb/AnalogBox. Wrap groups
    // on narrow windows rather than clipping channels or gate mode.
    const float right=ImGui::GetCursorPosX()+ImGui::GetContentRegionAvail().x;
    auto next=[&](float width,float gap=12.0f) {
        if(ImGui::GetItemRectMax().x-ImGui::GetWindowPos().x+gap*z+width<=right) ImGui::SameLine(0,gap*z);
    };
    labelW=54;
    ImGui::BeginGroup();sliderInt(voices,"Voices",170*z);ImGui::EndGroup();
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Maximum simultaneous notes. Keyboard chords use separate voices.");
    next(170*z);
    ImGui::BeginGroup();sliderInt(channels,"Channels",170*z);ImGui::EndGroup();
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Audio output channels. Use Voices for keyboard polyphony and Width to spread voices.");
    next(170*z);
    ImGui::BeginGroup();slider(master,"Master",170*z,"%.2f");ImGui::EndGroup();
    next(190*z);labelW=64;
    toggle(drone,"Drone");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Drone: every voice plays continuously. Unchecked: follow Gate.");
    next(ImGui::CalcTextSize("droning").x,6);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(droning()?ImVec4(.96f,.73f,.45f,1):ImVec4(.59f,.65f,1,1),droning()?"droning":"gated");
    next(ImGui::CalcTextSize("Reset LFOs").x+ImGui::GetStyle().FramePadding.x*2);
    if(ImGui::Button("Reset LFOs")) resetLFOs.trigger();
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Reset the added modulation LFOs together");
    labelW=70;
}

std::vector<float> scFMBox::panPositions() const {
    const int nv = voices.get(), c = channels.get();
    float width=0;for(const auto& control:sound)if(control->sc=="stereo")width=control->p.get();
    const bool ring = c > 2 && spreadLayout.get() == 0;
    std::vector<float> p(nv, 0.5f);
    if(spreadMode.get() == 2) return expandPan(panP.get(), nv);
    for(int i = 0; i < nv; i++){
        if(spreadMode.get() == 1){                       // each voice on a channel
            const int ch = i % std::max(1, c);
            p[i] = c <= 1 ? 0.5f : ring ? (float)ch / c : (float)ch / (c - 1);
        }else if(ring){                                  // evenly around the ring
            p[i] = (float)i / nv * width + spreadCenter.get() * 0.5f;
        }else{                                           // evenly from left to right
            const float t = nv > 1 ? (float)i / (nv - 1) : 0.5f;
            p[i] = 0.5f + (t - 0.5f) * width + spreadCenter.get() * 0.5f;
        }
    }
    return p;
}

void scFMBox::sliderVec(ofParameter<std::vector<float>>& p, const char* label, float w, const char* fmt, bool log) {
    const float z = zoom();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    mark(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    const float sw = std::max(30.0f * z, w - labelW * z);
    const float lo = first(p.getMin(), 0, 0.0f), hi = first(p.getMax(), 0, 1.0f);
    const auto& values = p.get();
    if(values.size() > 1){
        const ImVec2 a = ImGui::GetCursorScreenPos(), size(sw, ImGui::GetFrameHeight());
        ImGui::InvisibleButton("##bars", size);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        canvas(dl, a, ImVec2(a.x + size.x, a.y + size.y));
        const float bw = size.x / values.size();
        for(size_t i = 0; i < values.size(); i++){
            const float t = ofClamp((values[i] - lo) / std::max(1e-6f, hi - lo), 0.0f, 1.0f);
            dl->AddRectFilled(ImVec2(a.x + i * bw + 1, a.y + size.y * (1 - t)), ImVec2(a.x + (i + 1) * bw - 1, a.y + size.y), pal::accent);
        }
        if(ImGui::IsItemActive()){
            const float t = ofClamp((ImGui::GetMousePos().x - a.x) / size.x, 0.0f, 1.0f);
            p.set({lo + t * (hi - lo)});
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%d values, one per voice (from a connection)\nDrag: one value for every voice", (int)values.size());
    }else{
        ImGui::SetNextItemWidth(sw);
        float v = first(values, 0, lo);
        if(ImGui::SliderFloat("##v", &v, lo, hi, fmt, log ? ImGuiSliderFlags_Logarithmic : 0)) p.set({v});
    }
    menu(key);
    ImGui::PopID();
}

void scFMBox::drawPanTab(float w) {
    const float z = zoom();
    const int nv = voices.get(), c = channels.get();
    const bool ring = c > 2 && spreadLayout.get() == 0;
    const float cw = colW(w, 3);
    auto voiceCol = [](int v, int alpha){
        ImColor col = ImColor::HSV(std::fmod(v * 0.61803f, 1.0f), 0.5f, 0.98f);
        col.Value.w = alpha / 255.0f;
        return (ImU32)col;
    };
    ImGui::BeginGroup();
    sectionHeader("Placement", cw);
    combo(spreadMode, "Mode", cw);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Spread: evenly across the speakers\nChannels: voice 1 on speaker 1, voice 2 on speaker 2...\n"
                                                 "Manual: each voice where you put it (drag it, or the Pan vector)");
    if(spreadMode.get() == 0){
        slider(soundParam("stereo"), "Spread", cw, "%.2f");
        slider(spreadCenter, "Center", cw, "%+.2f");
    }
    if(spreadMode.get() == 2) sliderVec(panP, "Pan", cw, "%.2f");
    if(c > 2) combo(spreadLayout, "Speakers", cw);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
    ImGui::TextColored(ImColor(pal::textDim), "%d voices on %d speaker%s. Drag a voice to place it (Shift: onto a speaker). "
                       "To move them, route an LFO (or anything) to Pan in Routing: the filled dots show where each voice is now.",
                       nv, c, c == 1 ? "" : "s");
    ImGui::PopTextWrapPos();
    if(c == 1) ImGui::TextColored(ImColor(pal::accent), "One channel: nothing to pan.");
    nextCol();
    const float vw = 2 * cw + gapW();
    sectionHeader("Speakers", vw);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float h = ring ? std::min(vw, 340.0f * z) : std::max(120.0f * z, nv * 18.0f * z + 40 * z);
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + vw, a.y + h);
        ImGui::InvisibleButton("##panView", ImVec2(vw, h));
        const bool hovered = ImGui::IsItemHovered();
        canvas(dl, a, b);
        const auto base = panPositions();
        auto live = [&](int v){ const int i = 96 + v; return i < (int)this->live.size() ? this->live[i] : base[v]; };
        const ImVec2 m = ImGui::GetMousePos();
        int hoverVoice = -1;
        // where position p (0..1) is drawn, for voice v
        std::function<ImVec2(float, int)> place;
        float pad = 30.0f * z;
        if(ring){
            const ImVec2 ctr((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            const float R = h * 0.5f - pad;
            dl->AddCircle(ctr, R, pal::gridStrong, 64, 1.0f * z);
            for(int k = 0; k < c; k++){
                const float ang = -HALF_PI + TWO_PI * k / c;
                const ImVec2 sp(ctr.x + std::cos(ang) * (R + 14 * z), ctr.y + std::sin(ang) * (R + 14 * z));
                dl->AddRectFilled(ImVec2(sp.x - 7 * z, sp.y - 7 * z), ImVec2(sp.x + 7 * z, sp.y + 7 * z), pal::header, 2 * z);
                dl->AddRect(ImVec2(sp.x - 7 * z, sp.y - 7 * z), ImVec2(sp.x + 7 * z, sp.y + 7 * z), pal::headerText, 2 * z);
                const std::string t = ofToString(k + 1);
                const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
                dl->AddText(ImVec2(sp.x - ts.x * 0.5f, sp.y - ts.y * 0.5f), pal::headerText, t.c_str());
            }
            place = [=](float p, int v){
                const float r = R * (0.35f + 0.6f * (nv > 1 ? (float)v / (nv - 1) : 1.0f));
                const float ang = -HALF_PI + TWO_PI * p;
                return ImVec2(ctr.x + std::cos(ang) * r, ctr.y + std::sin(ang) * r);
            };
            for(int v = 0; v < nv; v++){
                const float r = R * (0.35f + 0.6f * (nv > 1 ? (float)v / (nv - 1) : 1.0f));
                dl->AddCircle(ctr, r, pal::grid, 64);
            }
        }else{
            const float x0 = a.x + pad, x1 = b.x - pad;
            const int sp = std::max(1, c);
            for(int k = 0; k < sp; k++){
                const float x = c <= 1 ? (x0 + x1) * 0.5f : x0 + (x1 - x0) * k / (c - 1);
                dl->AddLine(ImVec2(x, a.y + 22 * z), ImVec2(x, b.y - 6 * z), pal::grid);
                dl->AddRectFilled(ImVec2(x - 9 * z, a.y + 4 * z), ImVec2(x + 9 * z, a.y + 20 * z), pal::header, 2 * z);
                const std::string t = c == 2 ? (k == 0 ? "L" : "R") : ofToString(k + 1);
                const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
                dl->AddText(ImVec2(x - ts.x * 0.5f, a.y + 12 * z - ts.y * 0.5f), pal::headerText, t.c_str());
            }
            const float rowH = (h - 34 * z) / std::max(1, nv);
            place = [=](float p, int v){ return ImVec2(x0 + (x1 - x0) * ofClamp(p, 0.0f, 1.0f), a.y + 28 * z + (v + 0.5f) * rowH); };
        }
        for(int v = 0; v < nv; v++){
            const ImVec2 pb = place(base[v], v), pl = place(live(v), v);
            dl->AddLine(pb, pl, voiceCol(v, 90), 1.0f * z);
            dl->AddCircle(pb, 7 * z, voiceCol(v, 230), 0, 2 * z);
            dl->AddCircleFilled(pl, 4.5f * z, voiceCol(v, 255));
            const std::string t = ofToString(v + 1);
            dl->AddText(ImVec2(pb.x + 9 * z, pb.y - 7 * z), voiceCol(v, 200), t.c_str());
            if(hovered && std::hypot(m.x - pb.x, m.y - pb.y) < 9 * z) hoverVoice = v;
        }
        // dragging a voice: Manual from here on
        if(ImGui::IsItemActivated() && hoverVoice >= 0){
            panDragVoice = hoverVoice;
            if(spreadMode.get() != 2){ panP.set(panPositions()); spreadMode = 2; }
        }
        if(ImGui::IsItemActive() && panDragVoice >= 0 && panDragVoice < nv){
            float p;
            if(ring){
                const ImVec2 ctr((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
                p = std::fmod(std::atan2(m.y - ctr.y, m.x - ctr.x) + HALF_PI + TWO_PI, TWO_PI) / TWO_PI;
            }else{
                p = ofClamp((m.x - (a.x + pad)) / (vw - 2 * pad), 0.0f, 1.0f);
            }
            if(ImGui::GetIO().KeyShift && c > 1){       // onto the nearest speaker
                const float steps = ring ? (float)c : (float)(c - 1);
                p = std::round(p * steps) / steps;
                if(ring) p = std::fmod(p, 1.0f);
            }
            auto v = expandPan(panP.get(), nv);
            v[panDragVoice] = p;
            panP.set(v);
        }
        if(ImGui::IsItemDeactivated()) panDragVoice = -1;
        if(hovered && hoverVoice >= 0 && panDragVoice < 0)
            ImGui::SetTooltip("Voice %d at %.2f (now %.2f)\nDrag to place it (Shift: onto a speaker)", hoverVoice + 1, base[hoverVoice], live(hoverVoice));
    }
    ImGui::EndGroup();
}

bool scFMBox::liveVca() const {
    for(int e = 0; e < mod.envCount; ++e)
        if(std::abs(mod.matrix[(scModulation::SRC_ENV + e) * NUM_TARGETS + Amp]) > 0.0001f) return true;
    return false;
}
