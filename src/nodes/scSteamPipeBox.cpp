#include "scFxRouting.h"
#include "scSteamPipeBox.h"
#include "scSteamPipeBoxData.h"
#include "scTypedValue.h"
#include "scEnvelopeCurve.h"

#include "scSynthControlBatch.h"
#include "ofxOceanodeSuperColliderConfig.h"
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace {
namespace pal {
const ImU32 canvas     = IM_COL32( 11,  12,  14, 255);
const ImU32 border     = IM_COL32( 46,  50,  58, 255);
const ImU32 header     = IM_COL32( 30,  32,  38, 255);
const ImU32 headerText = IM_COL32(205, 205, 215, 255);
const ImU32 textDim    = IM_COL32(150, 150, 160, 255);
const ImU32 accent     = IM_COL32(150, 165, 255, 255);   // periwinkle
}
float zoom() { return ofxOceanodeShared::getZoomLevel(); }
float gapW() { return 12.0f * zoom(); }
float colW(float w, int n) { return std::max(1.0f, (w - gapW() * (n - 1)) / n); }
void nextCol() { ImGui::EndGroup(); ImGui::SameLine(0, gapW()); ImGui::BeginGroup(); }
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

scSteamPipeBox::scSteamPipeBox(std::vector<serverManager*> s) : scNode("steampipeBox"), servers(std::move(s)) {
    description = "SteamPipe physical modelling: steam exciter, tuned pipe feedback, diffuser, "
                  "MW filters and MicroSpace reverb. Includes the recovered Reaktor snapshots, "
                  "dynamic envelopes, LFOs, routing and box effects. Gate Mode Pulse plays the voices by itself, "
                  "from a clock to a Geiger counter. Right-click controls to publish or automate.";
}
scSteamPipeBox::~scSteamPipeBox() {
    scheduled.disable();
    listeners.unsubscribeAll();
    for(auto& [s, st] : states) release(s, st);
}

template<class T> void scSteamPipeBox::reg(ofParameter<T>& p, std::function<void()> send) {
    Control c;
    c.key = p.getEscapedName(); c.param = &p;
    c.discrete = std::is_same_v<T, bool> || std::is_same_v<T, int>;
    c.add = [this, &p]() { return addParameter(p); };
    c.save = [&p]() { return ofJson(p.get()); };
    c.load = [&p](const ofJson& j) { try { p.set(j.get<T>()); } catch(const std::exception&) {} };
    controls.push_back(std::move(c));
    listeners.push(p.newListener([this, send](T&) { if(!restoring) send(); }));
}
void scSteamPipeBox::setup() {
    timeline.setup(this, [this](const std::string& k){ publish(k); }, [this](const std::string& k){ unpublish(k); }, [this](){ return publishedKeys; });
    addParameter(show.set("Show", false));
    addParameter(voices.set("Voices", 6, 1, 16));
    addParameter(channels.set("Channels", 2, 1, 16));
    // Gate (levels: a gate rising to 0.2 plays at 0.2), Pitch, Velocity: Oceanode values or audio cables
    gateIn.attach(addParameter(gate.set("Gate", {0.0f}, {0.0f}, {1.0f})), availableInputs, [this](){ refreshOutputs(); });
    pitchIn.attach(addParameter(pitch.set("Pitch", {60.0f}, {0.0f}, {127.0f})), availableInputs, [this](){ refreshOutputs(); });
    velIn.attach(addParameter(velocity.set("Velocity", {1.0f}, {0.0f}, {1.0f})), availableInputs, [this](){ refreshOutputs(); });
    addParameter(master.set("Master", 1.0f, 0.0f, 1.0f));
    addParameter(resetLFOs.set("Reset"));
    addOutput("Out");
    // Auto: a drone until Gate is connected, timelined or gets a note
    // Pulse: the automatic gates
    gateMode.set("Gate Mode",0,0,3);
    reg(gateMode,[this](){sendAll();});
    dropdowns.set(gateMode.getEscapedName(),{"Auto","Gated","Drone","Pulse"});
    {
        scPulseGate::Host ph;
        ph.regF=[this](ofParameter<float>& p,std::function<void()> f){reg(p,f);};
        ph.regI=[this](ofParameter<int>& p,std::function<void()> f){reg(p,f);};
        ph.regB=[this](ofParameter<bool>& p,std::function<void()> f){reg(p,f);};
        ph.sendAll=[this](){sendAll();};
        ph.bpm=[this](){return bpm;};
        ph.voices=[this](){return voices.get();};
        ph.section=[this](const char* t,float w,const char*){sectionHeader(t,w);};
        ph.slider=[this](ofParameter<float>& p,const char* l,float w,const char* f,bool log){slider(p,l,w,f,log);};
        ph.toggle=[this](ofParameter<bool>& p,const char* l){toggle(p,l);};
        ph.mark=[this](const std::string& k){mark(k);};
        ph.menu=[this](const std::string& k){menu(k);};
        ph.labelW=&labelW;
        pulse.setup(ph);
    }
    scheduled.add(gateIn.handle, gate); scheduled.add(pitchIn.handle, pitch); scheduled.add(velIn.handle, velocity);
    scheduled.enable(this, [this](){ for(auto& [s, st] : states) if(st.created) return true; return false; });
    for(auto* p : {&gate, &pitch, &velocity}) {
        listeners.push(p->newListener([this,p](std::vector<float>& v) {
            // the timeline's frame set of a value it already sent timetagged: nothing to do
            if(scScheduling::isBackendSendSuppressed()) return;
            if(p==&gate) for(float value:v) if(value>0) gateTouched=true;
            for(auto& [s, st] : states) sendNotes(s, st);
        }));
    }
    listeners.push(voices.newListener([this](int&) { structureDirty = true; }));
    listeners.push(channels.newListener([this](int&) { structureDirty = true; }));
    listeners.push(master.newListener([this](float&) { if(!restoring) sendAll(); }));
    listeners.push(resetLFOs.newListener([this]() { mod.resetLfos(); pulse.restart=true; }));

    const auto firstSnapshot=ofJson::parse(steamPipeBoxData::factory)[0]["values"];
    for(const auto& item : ofJson::parse(steamPipeBoxData::controls)) {
        // Imported preset data retains these keys; the removed post-mix
        // delay/R66 stage no longer registers or publishes their controls.
        if(item["group"] == "R66" || item["group"] == "Delay" || item["sc"] == "gain") continue;
        auto c = std::make_unique<SoundControl>();
        c->p.set(item["name"].get<std::string>(), firstSnapshot[item["name"].get<std::string>()].get<float>(), item["min"].get<float>(), item["max"].get<float>());
        c->sc = item["sc"].get<std::string>(); c->group = item["group"].get<std::string>();
        c->help=item.value("description",std::string());
        c->discrete = item["mode"] == "switch";
        reg(c->p, [this](){ sendAll(); });
        controls.back().discrete = c->discrete;

        sound.push_back(std::move(c));
    }
    originalEnvelope.set("Original ADSR",true); reg(originalEnvelope,[this](){sendAll();});
    steamPipe2.set("SteamPipe 2",false); reg(steamPipe2,[this](){sendAll();});
    legato.set("Legato",false); reg(legato,[this](){sendAll();});
    noiseEnvelopeAmount.set("Noise Filter Envelope",0,0,120); reg(noiseEnvelopeAmount,[this](){sendAll();});
    // Preserve the recovered snapshots' placement unless another mode is chosen.
    placement.set("Placement",0,0,3); reg(placement,[this](){sendAll();});
    speakerLayout.set("Speaker Layout",1,0,1); reg(speakerLayout,[this](){sendAll();});
    spreadWidth.set("Spread",1,0,1); reg(spreadWidth,[this](){sendAll();});
    spreadCenter.set("Spread Center",0,-1,1); reg(spreadCenter,[this](){sendAll();});
    panP.set("Pan",{0.5f},{0.0f},{1.0f}); reg(panP,[this](){sendAll();});
    dropdowns.set(placement.getEscapedName(),{"Original","Spread","Channels","Manual"});
    dropdowns.set(speakerLayout.getEscapedName(),{"Ring","Line"});
    // ── Effects (MixerPro's inserts) ─────────────────────────────────────────
    auto toStages = [this]() { for(auto& [sv, st] : states) if(st.created) updateStages(sv, st); };
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
    for(int e=0;e<1;++e) for(int k=0;k<3;++k) {
        const char* names[]={"Attack Curve","Decay Curve","Release Curve"};
        envCurves[e][k].set(std::string(e ? "Filter " : "Amp ")+names[k],-4,-12,12);
        reg(envCurves[e][k],[this](){sendAll();});
    }
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
    h.control = [this](const std::string& k) -> ofAbstractParameter* { for(auto& c : controls) if(c.key == k && !scSnapshots::isOwnKey(k)) return c.param; return nullptr; };
    h.publishMenu = [this](const std::string& k){ menu(k); };
    h.publishedUnderline = [this](const std::string& k){ mark(k); };
    h.unpublish = [this](const std::string& k){ unpublish(k); };
    h.publishedKeys = [this]() -> const std::vector<std::string>& { return publishedKeys; };
    h.voices = [this](){ return voices.get(); };
    mod.unifiedLfoModels = true; // Same shared LFO models and editor as PolyComb.
    mod.setup(h, {{"Pitch",24,"st"}, {"Tune",48,"st"}, {"Allpass Tune",48,"st"},
        {"Noise Cutoff",48,"st"}, {"Noise Resonance",1,""}, {"Diffusion",1,""},
        {"Push",2,""}, {"Offset",1,""}, {"Feedback RT",60,"st"}, {"HP 0",48,"st"},
        {"LP 0",48,"st"}, {"Mod Wheel",1,""}, {"Amp",1,""}, {"Pan",1,""}}, true, true, true);
    for(int i=1;i<=scModulation::MAX_LFOS;++i)
        dropdowns.set("LFO"+ofToString(i)+"_Model", {"Off", "Classic", "Noise", "Random"});
    mod.lfoCount = mod.envCount = mod.extCount = 0;
    const std::map<std::string,int> targets={{"tune",Tune},{"allpass_tune",AllpassTune},
        {"noise_cut",NoiseCut},{"noise_resonance",NoiseRes},{"diffusion",Diffusion},{"push",Push},
        {"offset",Offset},{"feedback_rt",FeedbackRT},{"hp0",HP},{"lp0",LP},{"mod_wheel",MW}};
    for(const auto& c:sound) {auto it=targets.find(c->sc);if(it!=targets.end()) mod.targetOfKey[c->p.getEscapedName()]=it->second;}
    mod.targetOfKey[panP.getEscapedName()]=Pan;
    mod.changed = [this](){ if(!restoring) sendAll(); };
    for(const auto& c : sound)
        if(c->group!="Output" && c->group!="R66" && c->group!="Delay") mod.addVoiceParam(c->p);
    for(auto& curves : envCurves) for(auto& p : curves) mod.addVoiceParam(p);
    mod.addVoiceParam(noiseEnvelopeAmount);
    initial = capture();
    scSnapshots::Host sh;
    sh.folder = "SteamPipeBox"; sh.title = "SteamPipeBox";
    sh.capture = [this](){ return capture(); };
    sh.apply = [this](const ofJson& a, const ofJson& b, float t, bool sw){ apply(a,b,t,sw); };
    sh.factory = [this](){ return factory(); };
    sh.bpm = [this](){ return bpm; };
    sh.paramMenu = [this](const std::string& k){ mark(k); menu(k); };
    // Seed missing bundled banks without overwriting a user's edited copies.
    const auto bundled=factory();
    const auto bankDir=std::filesystem::path(ofToDataPath("nodeSnapshots/SteamPipeBox/banks",true));
    std::filesystem::create_directories(bankDir);
    for(const auto& bank:ofJson::parse(steamPipeBoxData::banks)) {
        const auto path=bankDir/bank["file"].get<std::string>();
        if(std::filesystem::exists(path)) continue;
        ofJson slots=ofJson::array();
        for(const auto& index:bank["indices"])
            slots.push_back(index.is_null() ? ofJson(nullptr) : bundled.at(index.get<size_t>()));
        ofSavePrettyJson(path.string(),ofJson{{"snapshotBank",1},{"active",0},{"slots",slots}});
    }
    sh.defaultBank="Reaktor SteamPipe 01 - Instrument - Bank 1.json";
    snaps.setup(sh);
    // The old importer mixed ensemble and instrument collections in file order.
    // Keep a complete, recoverable copy of the working library and old banks
    // before switching an old bundled collection to the instrument's native bank.
    const std::vector<std::string> legacyBanks={"Reaktor SteamPipe 001-128.json","Reaktor SteamPipe 129-172.json"};
    std::string storedBank;
    try { storedBank=ofLoadJson((bankDir.parent_path()/"library.json").string()).value("bank",std::string()); } catch(...) {}
    if(std::find(legacyBanks.begin(),legacyBanks.end(),storedBank)!=legacyBanks.end()) {
        try {
            const auto backup=bankDir.parent_path()/("Legacy bank layout - "+ofGetTimestampString("%Y%m%d-%H%M%S-%i"));
            std::filesystem::create_directories(backup/"banks");
            for(const auto& entry:std::filesystem::directory_iterator(bankDir.parent_path()))
                if(entry.is_regular_file() && (entry.path().filename()=="library.json" || entry.path().filename().string().rfind("snapshot_",0)==0))
                    std::filesystem::copy_file(entry.path(),backup/entry.path().filename());
            for(const auto& name:legacyBanks) if(std::filesystem::exists(bankDir/name))
                std::filesystem::copy_file(bankDir/name,backup/"banks"/name);
            if(snaps.loadBank((bankDir/sh.defaultBank).string()))
                for(const auto& name:legacyBanks) std::filesystem::remove(bankDir/name);
        } catch(const std::exception& e) { ofLogWarning("steampipeBox")<<"Bank layout migration: "<<e.what(); }
    }
    if(snaps.active<0 && snaps.used[0] && snaps.name(0)=="Flute") { snaps.active=0; snaps.snapshotP.setWithoutEventNotifications(1); }
    requireSynthdefs();
}

void scSteamPipeBox::sendNotes(ofxSCServer* server, State& st) {
    if(!st.gate) return;
    auto now = scheduled.inEffectNow();   // not a scheduled send: the notes as they stand now
    const int n = st.builtVoices;
    auto vec = [&](const char* name, const std::vector<float>& v, float fallback) {
        std::vector<float> out(n); for(int i=0;i<n;++i) out[i] = first(v,i,fallback);
        st.gate->set(name,out);
    };
    // Pulse: the gate synth takes the generator's gates, Gate (Follow Gate) holds its voices
    const int pbus=pulse.bus(server);
    vec("gate",pbus>=0 ? std::vector<float>{0.0f} : gate.get(),0); vec("velocity",velocity.get(),1);
    st.gate->set("out", st.gates->index);
    st.gate->set("velout",st.velocities->index);
    st.gate->set("vel", mod.sourceBus(server,scModulation::SRC_VEL));
    st.gate->set("rnd", mod.sourceBus(server,scModulation::SRC_RND));
    st.droneSent=droning() ? 1 : 0;
    st.gate->set("drone",float(st.droneSent));
    for(auto [p,name] : {std::make_pair(&gateIn,"gate"),std::make_pair(&velIn,"vel")}) {
        const int bus = p==&gateIn && pbus>=0 ? pbus : p->bus(server);
        st.gate->set(std::string(name)+"_sel",bus >= 0 ? 1.0f : 0.0f);
        if(bus>=0) st.gate->mapan(std::string(name)+"_ar",bus,n);
    }
    pulse.enable(server,gate.get(),n,gateIn.bus(server));
    const int bus = pitchIn.bus(server);
    for(int i=0;i<n;++i) if(auto* v=st.voices[i]) {
        v->set("pitch", first(pitch.get(),i,60));
        v->set("pitch_sel",bus>=0 ? 1.0f : 0.0f);
        if(bus>=0) v->mapan("pitch_ar",bus+i,1);
    }
}
bool scSteamPipeBox::soundOn(const std::string& sc) { return soundParam(sc).get()>0.5f; }
void scSteamPipeBox::sendSound(ofxSCServer* server, State& st) {
    if(!st.created) return;
    static const char* names[NUM_TARGETS] = {"mpitch","mtune","maptune","mnoise","mnoiseres","mdiffusion","mpush","moffset","mfeedback","mhp","mlp","mmw","mamp","mpan"};
    bool vca = false;
    for(int e=0;e<mod.envCount;++e) if(std::abs(mod.matrix[(scModulation::SRC_ENV+e)*NUM_TARGETS+Amp])>0.0001f) vca=true;
    for(int i=0;i<st.builtVoices;++i) if(auto* v=st.voices[i]) {
        scSynthControlBatch updates(v,server);
        auto set = [&](const std::string& name,float value) {
            auto& sent=st.sent[i];auto it=sent.find(name);
            if(it!=sent.end()&&it->second==value) return;
            sent[name]=value;updates.set(name,value);
        };
        for(const auto& c : sound) if(c->sc!="stereo") set(c->sc,mod.voiceValue(c->p,i));
        for(int e=0;e<1;++e) for(int k=0;k<3;++k) set(std::string(e?"filter_curve":"amp_curve")+ofToString(k),mod.voiceValue(envCurves[e][k],i));
        set("gatebus",st.gates->index+i);
        set("velabus",st.velocities->index+i);
        set("out",st.audio->index+i);
        set("voice",i); set("voices",st.builtVoices);
        set("master",master.get()); set("bpm",bpm);
        set("normalization",1.0f);
        set("amp_original",originalEnvelope.get()?1.f:0.f);
        set("steampipe2",steamPipe2.get()?1.f:0.f);
        set("legato",legato.get()?1.f:0.f);
        set("noise_env",mod.voiceValue(noiseEnvelopeAmount,i));
        set("vca", vca ? 1.0f : 0.0f);
        set("vis",st.vis->index);
        for(int t=0;t<NUM_TARGETS-1;++t) set(names[t],mod.targetBus(server,t)+i);
        updates.send();
    }
    if(st.spread) {
        st.spread->set("in",st.audio->index);
        st.spread->set("voices",st.builtVoices);
        auto pans=panPositions();
        pans.resize(16,0.5f);
        st.spread->set("pan",pans); st.spread->set("mpan",mod.targetBus(server,Pan));
        st.spread->set("line",speakerLayout.get()==1?1.f:0.f);
        st.spread->set("vispan",st.vis->index+VIS_PAN);
        // Spreader telemetry has its own space after the voice meters.
        st.spread->set("vis",st.vis->index+96);
    }
    updateStages(server,st);
}
void scSteamPipeBox::sendAll() { for(auto& [s,st] : states) { sendNotes(s,st); sendSound(s,st); } pulse.sendAll(); }
void scSteamPipeBox::buildSynth(ofxSCServer* server) {
    if(!server) return;
    auto& st=states[server]; release(server,st); st.root=new ofxSCGroup(server);
}
void scSteamPipeBox::createSynth(ofxSCServer* server) {
    auto it=states.find(server); if(it==states.end() || !it->second.root) return;
    auto& st=it->second;
    if(placeBefore>=0) st.root->create(2,placeBefore); else st.root->create(0,1);
    for(auto*& g:st.groups) { g=new ofxSCGroup(server); g->create(1,st.root->nodeID); }
    st.gates=new ofxSCBus(RATE_AUDIO,16,server); st.velocities=new ofxSCBus(RATE_AUDIO,16,server); st.audio=new ofxSCBus(RATE_AUDIO,16,server);
    st.vis=new ofxSCBus(RATE_CONTROL,VIS_SIZE,server);
    if(!liveBus(server,st.gates,true)||!liveBus(server,st.velocities,true)||!liveBus(server,st.audio,true)||!liveBus(server,st.vis,false)) { release(server,st); return; }
    st.builtVoices=voices.get(); st.builtChannels=channels.get(); st.created=true;
    mod.create(server,st.groups[SlSrc]->nodeID,st.groups[SlRoute]->nodeID,st.gates->index);
    if(pulsing()) pulse.start(server,st.groups[SlGate]->nodeID,st.vis->index+VIS_PULSE,st.builtVoices);
    st.gate=new ofxSCSynth("SteamPipeBoxGate"+ofToString(st.builtVoices),server);
    for(int i=0;i<st.builtVoices;++i) st.voices[i]=new ofxSCSynth("SteamPipeBoxVoice",server);
    st.spread=new ofxSCSynth("SteamPipeBoxSpread"+ofToString(st.builtChannels),server);
    sendNotes(server,st); sendSound(server,st);
    st.gate->createAndRun(1,st.groups[SlGate]->nodeID,true);
    for(int i=0;i<st.builtVoices;++i) st.voices[i]->createAndRun(1,st.groups[SlVoice]->nodeID,true);
    st.spread->createAndRun(1,st.groups[SlSpread]->nodeID,true);
    routeChain(st);
    if(!getActive()) st.root->run(false);
}
void scSteamPipeBox::release(ofxSCServer* server, State& st) {
    if(st.created && st.root) st.root->free();
    if(st.created) mod.free(server,false);
    for(auto& f:st.fx) { delete f.synth; f.synth=nullptr; dropBus(server,f.in,true); f.running=false; f.offAt=-1; }
    delete st.gate; st.gate=nullptr; delete st.spread; st.spread=nullptr;
    pulse.release(server);
    for(auto*& v:st.voices) { delete v; v=nullptr; }
    for(auto& cache:st.sent) cache.clear();
    for(auto*& g:st.groups) { delete g; g=nullptr; }
    delete st.root; st.root=nullptr; st.created=false;st.droneSent=-1;
    dropBus(server,st.gates,true); dropBus(server,st.velocities,true); dropBus(server,st.audio,true); dropBus(server,st.vis,false);
}
void scSteamPipeBox::free(ofxSCServer* s) { auto it=states.find(s); if(it!=states.end()) { release(s,it->second); it->second.out=-1; } }
// A new voice / speaker count: everything again, but in the old root's place.
// The graph is not recomputed (same topology), so a root at the head of the
// default group would run before the nodes feeding Gate/Pitch/Velocity by
// cable, read silent buses and stay quiet until a reconnection.
void scSteamPipeBox::rebuild() {
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
void scSteamPipeBox::moveSynthBefore(ofxSCServer* s,int id) { auto it=states.find(s); if(it!=states.end()&&it->second.created) it->second.root->moveBefore(id); }
int scSteamPipeBox::getLastSynthID(ofxSCServer* s) { auto it=states.find(s); return it!=states.end()&&it->second.created ? it->second.root->nodeID : -1; }
void scSteamPipeBox::setOutputBus(ofxSCServer* s,int,int bus) { auto& st=states[s]; st.out=bus; if(st.created) routeChain(st); }
int scSteamPipeBox::getOutputBusIndex(ofxSCServer* s,int) { auto it=states.find(s); return it==states.end() ? -1 : it->second.out; }
void scSteamPipeBox::setInputBus(ofxSCServer* s,scNode* node,int bus) { auto& st=states[s]; if(gateIn.take(s,node,bus)|pitchIn.take(s,node,bus)|velIn.take(s,node,bus)) sendNotes(s,st); }
void scSteamPipeBox::resetInputBusses(ofxSCServer* s,int) { gateIn.reset(s);pitchIn.reset(s);velIn.reset(s);sendNotes(s,states[s]); }
void scSteamPipeBox::activate() { for(auto& [s,st]:states) if(st.created) st.root->run(true); }
void scSteamPipeBox::deactivate() { for(auto& [s,st]:states) if(st.created) st.root->run(false); }
void scSteamPipeBox::setBpm(float v) { bpm=std::max(1.0f,v);mod.setBpm(bpm);sendAll(); }
void scSteamPipeBox::resendParametersForNRT() { for(auto& [s,st]:states) for(auto& cache:st.sent) cache.clear(); mod.resend();sendAll(); }
#ifdef OFXOCEANODE_HAS_TIMELINE_INSTRUMENT
bool scSteamPipeBox::getTimelineInstrument(ofxOceanodeTimelineInstrument& out) {
    if(!gateIn.handle || !pitchIn.handle || !velIn.handle) return false;
    out.gate=gateIn.handle.get();out.pitch=pitchIn.handle.get();out.velocity=velIn.handle.get();out.voices=voices.get();
    return true;
}
#endif

bool scSteamPipeBox::connected(const std::string& k) const {
    auto it=published.find(k);return it!=published.end() && (it->second->hasInConnection()||it->second->hasOutConnections());
}
void scSteamPipeBox::publish(const std::string& k) {
    if(published.count(k)||getParameterGroup().contains(k)) return;
    auto it=std::find_if(controls.begin(),controls.end(),[&](const Control& c){return c.key==k;});if(it==controls.end()) return;
    if(!separator){addSeparator("Published",ofColor(200));separator=true;}
    published[k]=it->add();dropdowns.apply(k,published[k]);publishedKeys.push_back(k);parameterGroupChanged.notify(this);
}
void scSteamPipeBox::unpublish(const std::string& k) {
    auto it=published.find(k);if(it==published.end()||connected(k)) return;
    removeParameter(it->second->getName());published.erase(it);
    publishedKeys.erase(std::remove(publishedKeys.begin(),publishedKeys.end(),k),publishedKeys.end());
    if(published.empty()&&separator){removeSeparator("Published");separator=false;}
    parameterGroupChanged.notify(this);
}
void scSteamPipeBox::mark(const std::string& k) {
    timeline.mark(k);
    if(published.count(k)&&!timeline.isHidden(k)) {
        auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x,b.y),b,IM_COL32(106,212,196,255),2);
    }
}
void scSteamPipeBox::menu(const std::string& k) {
    if(!ImGui::BeginPopupContextItem(("##steam"+k).c_str())) return;
    auto it=std::find_if(controls.begin(),controls.end(),[&](const Control& c){return c.key==k;});
    if(it!=controls.end()) scTyped::jsonField(k,it->save,it->load);
    if(timeline.isHidden(k)){if(ImGui::MenuItem("Publish to Node GUI")) timeline.reveal(k);}
    else if(published.count(k)){
        if(connected(k)) ImGui::TextDisabled("Disconnect it to unpublish");
        else if(ImGui::MenuItem("Unpublish from Node")) timeline.unpublish(k);
    }else if(ImGui::MenuItem("Publish to Node GUI")) publish(k);
    timeline.menuItems(k);mod.modulateMenu(k);ImGui::EndPopup();
}
ofJson scSteamPipeBox::capture() const {
    ofJson j=ofJson::object();
    for(const auto& c:controls) if(!scSnapshots::isOwnKey(c.key)) j[c.key]=c.save();
    mod.patchBases(j);mod.save(j,"steamPipeMod");return j;
}
void scSteamPipeBox::apply(const ofJson& a,const ofJson& b,float t,bool sw) {
    restoring=true;
    for(const auto& c:controls) if(!scSnapshots::isOwnKey(c.key)&&!(published.count(c.key)&&published.at(c.key)->hasInConnection())) {
        // Older SteamPipe snapshots stored Noise / Random Type separately.
        // Infer the new selector when absent, so recalling them keeps their LFO mode.
        auto value = [&](const ofJson& state) -> ofJson {
            if(state.contains(c.key)) return state[c.key];
            for(int i=1;i<=scModulation::MAX_LFOS;++i) {
                const auto prefix="LFO"+ofToString(i)+"_";
                if(c.key==prefix+"Model") {
                    if(state.value(prefix+"Random_Type",0)>0) return 3;
                    return state.value(prefix+"Noise",false) ? 2 : 1;
                }
            }
            return initial[c.key];
        };
        c.load(blend(value(a),value(b),t,sw,c.discrete));
    }
    const auto& src=sw?b:a;
    if(modSource!=&src || t>=1) { modSource=&src; mod.load(src,"steamPipeMod");mod.afterLoad(); }
    auto count = [&](const char* key,int max) {
        const int av=a.value(key,0),bv=b.value(key,0);
        return std::clamp(t<=0 ? av : t>=1 ? bv : std::max(av,bv),0,max);
    };
    const int nl=count("steamPipeModLfos",scModulation::MAX_LFOS),ne=count("steamPipeModEnvs",scModulation::MAX_ENVS),nx=count("steamPipeModExts",scModulation::MAX_EXT);
    bool counts=nl!=mod.lfoCount||ne!=mod.envCount||nx!=mod.extCount;
    mod.lfoCount=nl;mod.envCount=ne;mod.extCount=nx;
    auto ma=a.value("steamPipeModMatrix",std::vector<float>(scModulation::NUM_SOURCES*NUM_TARGETS,0));
    auto mb=b.value("steamPipeModMatrix",std::vector<float>(scModulation::NUM_SOURCES*NUM_TARGETS,0));
    ma.resize(mod.matrix.size(),0);mb.resize(mod.matrix.size(),0);
    bool dirty=false;
    for(size_t i=0;i<mod.matrix.size();++i) {float value=ofLerp(ma[i],mb[i],t);if(value!=mod.matrix[i]){mod.matrix[i]=value;dirty=true;}}
    if(counts) mod.refresh();else if(dirty) mod.sendMatrix();
    restoring=false;sendAll();
}
std::vector<ofJson> scSteamPipeBox::factory() const {
    std::vector<ofJson> out;
    for(const auto& preset:ofJson::parse(steamPipeBoxData::factory)) {
        ofJson j=initial;j["name"]=preset["name"];
        for(const auto& c:sound) if(preset["values"].contains(c->p.getName())) j[c->p.getEscapedName()]=preset["values"][c->p.getName()];
        if(preset.contains("extras")) for(auto it=preset["extras"].begin();it!=preset["extras"].end();++it) j[it.key()]=it.value();
        out.push_back(j);
    }
    return out;
}
void scSteamPipeBox::presetSave(ofJson& j) {
    mod.saveLfoOutputs(j);
    j["steampipeBox"]=capture();
    // Snapshot controls belong in a patch, but never inside a sound snapshot.
    for(const auto& c:controls) if(scSnapshots::isOwnKey(c.key)) j["steampipeBox"][c.key]=c.save();
    j["steampipeBoxPublished"]=publishedKeys;timeline.save(j,"steampipeBoxTimeline");

}
void scSteamPipeBox::loadBeforeConnections(ofJson& j) {
    mod.loadLfoOutputs(j);
    std::vector<std::string> wanted;
    if(j.contains("steampipeBoxPublished")&&j["steampipeBoxPublished"].is_array()) for(const auto& k:j["steampipeBoxPublished"]) if(k.is_string()) wanted.push_back(k.get<std::string>());
    const auto old=publishedKeys;for(const auto& k:old) if(std::find(wanted.begin(),wanted.end(),k)==wanted.end()) unpublish(k);
    for(const auto& k:wanted) publish(k);timeline.load(j,"steampipeBoxTimeline");
}
void scSteamPipeBox::presetRecallAfterSettingParameters(ofJson& j) {
    if(!j.contains("steampipeBox")) return;
    auto state=j["steampipeBox"];
    // a loaded patch starts from Auto's own rule: droning unless Gate is
    // connected or timelined (a value it gets from here on switches it to gated)
    gateTouched=false;
    apply(state,state,1,true);
    restoring=true;for(const auto& c:controls) if(scSnapshots::isOwnKey(c.key)&&state.contains(c.key)) c.load(state[c.key]);restoring=false;
}
void scSteamPipeBox::requireSynthdefs() {
    for(const auto& entry:std::vector<std::pair<std::string,std::string>>{
        {"SteamPipeBoxVoice.scsyndef","SteamPipeBox"},{"ModLfo1.scsyndef","Modulation"},{"SteamPipeBoxSpread2.scsyndef","SteamPipeBox"},{"PulseGate.scsyndef","Modulation"},{"mixerProEQ2.scsyndef","MixerPro"}}) {
        std::filesystem::path folder;
        try {
            const std::filesystem::path data=ofToDataPath(SYNTHDEF_DIRECTORY,true);
            if(std::filesystem::exists(data)) for(const auto& f:std::filesystem::recursive_directory_iterator(data,std::filesystem::directory_options::skip_permission_denied))
                if(f.is_regular_file()&&f.path().filename()==entry.first){folder=f.path().parent_path();break;}
            if(folder.empty()) folder=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()/"synthdefs/Defaults"/entry.second/"CompiledSynthdefs"/(entry.second=="MixerPro" ? "mixerpro" : entry.second);
            if(!std::filesystem::exists(folder/entry.first)){ofLogError("steampipeBox")<<"Compile synthdefs/Defaults/"<<entry.second<<" first";continue;}
            for(auto* sm:servers) if(sm) sm->requireSynthdefFolder(folder.string());
        } catch(const std::exception& e){ofLogError("steampipeBox")<<e.what();}
    }
}
void scSteamPipeBox::update(ofEventArgs&) {
    timeline.update();snaps.update();
    {   // the snapshot names, to a published Snapshot / Morph A / B dropdown
        const auto names=snaps.slotNames();
        for(const auto& k:snaps.slotKeys()) dropdowns.set(k,names,published.count(k)?published[k]:nullptr);
    }
    if(structureDirty) rebuild();
    for(auto& [server,st]:states) if(st.created && st.droneSent!=(droning()?1:0)) sendNotes(server,st);
    // Pulse on / off / again (a new Seed, Reset); SteamPipeBoxGate holds no level, so it stays
    for(auto& [server,st]:states) if(st.created && st.groups[SlGate] && liveBus(server,st.vis,false)) {
        if(pulsing() && (!pulse.running(server) || pulse.restart)) {
            if(pulse.start(server,st.groups[SlGate]->nodeID,st.vis->index+VIS_PULSE,st.builtVoices)) sendNotes(server,st);
        } else if(!pulsing() && pulse.running(server)) { pulse.stop(server); sendNotes(server,st); }
    }
    pulse.restart=false;
    mod.showing=show.get();mod.update();
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
        if(now>=nextRead){nextRead=now+1.0f/(pulsing()?60:25);s->requestControlValues(st.vis->index,VIS_SIZE);}
        break;
    }
    if(pulsing()) pulse.read(live,VIS_PULSE);
}
void scSteamPipeBox::drawWindow() {
    const float z=ofxOceanodeShared::getZoomLevel();
    ImGui::SetNextWindowSize(ImVec2(1150*z,850*z),ImGuiCond_FirstUseEver);
    bool open=show.get();
    if(!ImGui::Begin(getWindowTitle().c_str(),&open,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();if(!open) show=false;return;
    }
    if(!open){show=false;ImGui::End();return;}
    keyboardPiano.beginContent(pitch, gate, voices.get());
    const float w=ImGui::GetContentRegionAvail().x;
    drawHeader();
    snaps.drawBar(w);
    if(pulsing()) pulse.drawPanel(w);
    ImGui::Spacing();
    if(ImGui::BeginTabBar("##steamTabs")) {
        auto tab=[&](const char* name,std::function<void(float)> body) {
            if(!ImGui::BeginTabItem(name)) return;
            ImGui::BeginChild((std::string("##steamTab")+name).c_str(),ImVec2(0,0),false);
            ImGui::Spacing();
            const float tw=ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ScrollbarSize;
            labelW=70;
            body(tw);
            ImGui::EndChild();
            ImGui::EndTabItem();
        };
        tab("Sound",[this](float tw){drawSoundTab(tw);});
        tab("Envelopes",[this](float tw){
            mod.drawEnvelopeCards(tw,{[this](float cw){drawNativeEnvelope(0,cw);}});
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
void scSteamPipeBox::draw(ofEventArgs&) {
    if(show.get()) drawWindow();
    keyboardPiano.finishFrame();
}

void scSteamPipeBox::sectionHeader(const char* title, float w, ofParameter<bool>* on, ImU32 accent) {
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

void scSteamPipeBox::slider(ofParameter<float>& p, const char* label, float w, const char* fmt, bool log) {
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

void scSteamPipeBox::toggle(ofParameter<bool>& p, const char* label) {
    bool v = p.get();
    if(ImGui::Checkbox(label, &v)) p.set(v);
    mark(p.getEscapedName());
    menu(p.getEscapedName());
}


void scSteamPipeBox::updateStages(ofxSCServer* server, State& st) {
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
            f.offAt = ofGetElapsedTimef() + stageTail(s);
        }
    }
    routeChain(st);
}

// the spreader -> the running effects -> out
void scSteamPipeBox::routeChain(State& st) {
    int next = std::max(0,st.out);
    for(int s = NumStages - 1; s >= 0; s--){
        auto& f = st.fx[s];
        if(!f.synth || !f.running || !f.in) continue;
        f.synth->set("out", next);
        next = f.in->index;
    }
    if(st.spread) st.spread->set("out", next);
}

bool scSteamPipeBox::stageOn(int s) const {
    return s == StSat ? satOn.get() : s == StEq ? eqOn.get() : s == StEcho ? echoOn.get() : rvbOn.get();
}

std::string scSteamPipeBox::stageDef(int s) const {
    static const char* names[NumStages] = {"mixerProSat", "mixerProEQ", "mixerProEcho", "boxMicroSpace"};
    return names[s] + ofToString(channels.get());
}

float scSteamPipeBox::echoDelaySec() const {
    if(!echoBeats.get()) return ofClamp(echoDelay.get(), 0.005f, 4.0f);
    return ofClamp(echoBeatVal.get() * 60.0f / std::max(1.0f, bpm), 0.005f, 4.0f);
}

float scSteamPipeBox::stageTail(int s) const {
    if(s == StEq || s == StSat) return 0.02f;
    if(s == StEcho){
        const float feed = ofClamp(echoFeed.get(), 0.0f, 0.98f);
        const float repeats = feed > 0.001f ? std::log(0.001f) / std::log(feed) : 0.0f;
        return std::min(30.0f, echoDelaySec() * (repeats + 1.0f) + 0.2f);
    }
    return std::min(30.0f, rvbDecay.get() + rvbPredelay.get() * 0.001f + 0.3f);
}

void scSteamPipeBox::sendStage(int s, ofxSCSynth* x, ofxSCServer* server) {
    if(!x) return;
    scSynthControlBatch controls(x, server);
    const float on = stageOn(s) ? 1.0f : 0.0f;
    if(s == StSat){
        controls.set("drive",satDrive.get());controls.set("bias",satBias.get());controls.set("tone",satTone.get());
        controls.set("autogain",1.0f);controls.set("output",satOutput.get());controls.set("mix",satMix.get());controls.set("on",on);
    }else if(s == StEq){
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

void scSteamPipeBox::sendStageAll(int stage) {
    for(auto& [server,st]:states) if(st.created && st.fx[stage].synth) sendStage(stage,st.fx[stage].synth,server);
}

void scSteamPipeBox::drawFxTab(float w) {
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
            const auto key = p.getEscapedName();
            const bool pub = published.count(key) != 0;
            if(ImGui::MenuItem((std::string(pub ? "Unpublish " : "Publish ") + label).c_str(), nullptr, false, !connected(key))){
                if(pub) unpublish(key); else publish(key);
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
    ImGui::TextColored(ImColor(pal::textDim), "Pipe voices -> Saturation -> EQ -> Echo -> Reverb.");
}

void scSteamPipeBox::drawNativeEnvelope(int e,float w) {
    const float z=zoom();
    const ImU32 col=IM_COL32(150,165,255,235);
    const std::string prefix="";
    auto param=[&](const std::string& name)->ofParameter<float>& {
        return (*std::find_if(sound.begin(),sound.end(),[&](const auto& c){return c->sc==name;}))->p;
    };
    auto& a=param(prefix+"attack");auto& d=param(prefix+"decay");
    auto& su=param(prefix+"sustain");auto& r=param(prefix+"release");
    auto seconds=[](float v){return .001f*std::pow(10.0f,v/20.0f);};
    auto setSeconds=[](ofParameter<float>& p,float v){p=ofClamp(20*std::log10(std::max(.0001f,v)/.001f),p.getMin(),p.getMax());};
    ImGui::PushID("steamEnvelope");ImGui::BeginGroup();
    sectionHeader("Steam Envelope",w,nullptr,col);
    toggle(originalEnvelope,"Original ADSR");
    toggle(legato,"Legato");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Retrigger only after Gate closes. Positive gate-level changes keep the current envelope and velocity.");
    labelW=58;
    const bool native=originalEnvelope.get();
    const float tailScale=native?5.f:1.f;
    scEnvCurve::ADSR env{seconds(a.get()),seconds(d.get())*tailScale,su.get(),seconds(r.get())*tailScale,{native?0.f:envCurves[e][0].get(),native?-5.f:envCurves[e][1].get(),native?-5.f:envCurves[e][2].get()}};
    const ImVec2 origin=ImGui::GetCursorScreenPos();
    if(scEnvCurve::editADSR("##env",ImVec2(w-18*z,110*z),env,col,pal::canvas,false,seconds(r.getMax()),.0001f)) {
        setSeconds(a,env.a);setSeconds(d,env.d/tailScale);su=env.s;setSeconds(r,env.r/tailScale);
        if(!native) for(int k=0;k<3;++k) envCurves[e][k]=env.curve[k];
    }
    auto* dl=ImGui::GetWindowDrawList();
    const ImVec2 ma(origin.x+w-14*z,origin.y),mb(origin.x+w,origin.y+110*z);
    dl->AddRectFilled(ma,mb,pal::canvas,3*z);dl->AddRect(ma,mb,pal::border,3*z);
    for(int v=0;v<voices.get();++v) {
        const int offset=v*6+2;
        const float level=offset<int(live.size()) ? ofClamp(live[offset],0.0f,1.0f) : 0;
        const float x=ma.x+14*z*(.25f+.5f*(voices.get()>1 ? float(v)/(voices.get()-1) : .5f));
        dl->AddCircleFilled(ImVec2(x,mb.y-4*z-level*102*z),2.2f*z,level>.001f ? col : IM_COL32(255,255,255,34));
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x,origin.y+110*z+ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(w,0));
    auto timeSlider=[&](ofParameter<float>& p,const char* label) {
        ImGui::PushID(label);const float x=ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();ImGui::TextUnformatted(label);mark(p.getEscapedName());
        ImGui::SameLine();ImGui::SetCursorPosX(x+labelW*z);ImGui::SetNextItemWidth(std::max(30*z,w-labelW*z));
        float v=seconds(p.get());
        if(ImGui::SliderFloat("##v",&v,seconds(p.getMin()),seconds(p.getMax()),"%.3f s",ImGuiSliderFlags_Logarithmic)) setSeconds(p,v);
        menu(p.getEscapedName());ImGui::PopID();
    };
    timeSlider(a,"Attack");timeSlider(d,"Decay");slider(su,"Sustain",w,"%.2f");timeSlider(r,"Release");
    if(!e) slider(param("velocity_amount"),"Vel",w,"%.2f");
    if(!native) {
    const float x=ImGui::GetCursorPosX();ImGui::AlignTextToFramePadding();ImGui::TextUnformatted("Curves");
    ImGui::SameLine();ImGui::SetCursorPosX(x+labelW*z);
    const float cw=std::max(1.0f,(w-labelW*z-2*ImGui::GetStyle().ItemSpacing.x)/3);
    const char* formats[]={"A %.1f","D %.1f","R %.1f"};
    for(int k=0;k<3;++k) {
        if(k) ImGui::SameLine();ImGui::PushID(k);ImGui::SetNextItemWidth(cw);
        auto& p=envCurves[e][k];float v=p.get();if(ImGui::SliderFloat("##c",&v,-12,12,formats[k])) p=v;
        mark(p.getEscapedName());menu(p.getEscapedName());ImGui::PopID();
    }
    }
    if(native) ImGui::TextColored(ImColor(pal::textDim),"Decay / release: time constants");
    slider(param("env_key_scaling"),"Keytrack",w,"%.2f");
    ImGui::TextColored(ImColor(pal::textDim),"-> Steam pressure / noise exciter");
    labelW=70;ImGui::EndGroup();ImGui::PopID();
}

ofParameter<float>& scSteamPipeBox::soundParam(const std::string& name) {
    return (*std::find_if(sound.begin(),sound.end(),[&](const auto& c){return c->sc==name;}))->p;
}

void scSteamPipeBox::drawSoundTab(float w) {
    const char* groups[]={"Steam","Delay tune","Allpass","Feedback","MW filter","Output","Performance"};
    const float cw=colW(w,4);
    for(int i=0;i<7;++i) {
        if(i==0 || i==4) {ImGui::BeginGroup();ImGui::BeginGroup();} else nextCol();
        sectionHeader(groups[i],cw);labelW=115;
        if(i==0) slider(noiseEnvelopeAmount,"Env -> cutoff",cw,"%.1f st");
        if(i==1) {
            toggle(steamPipe2,"SteamPipe 2");
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Four-point pipe / diffusion delay interpolation and HP/LP phase tuning compensation. Off preserves the original resonator.");
        }
        for(auto& c:sound) if(c->group==groups[i] && c->sc!="stereo" && c->sc!="pan_random") {
            std::string label=c->p.getName();
            for(const auto& prefix:{std::string("Allpass "),std::string("Noise "),std::string("Feedback ")})
                if(label.rfind(prefix,0)==0) {label.erase(0,prefix.size());break;}
            drawCompactControl(*c,cw,label);
            if(ImGui::IsItemHovered() && !c->help.empty()) ImGui::SetTooltip("%s",c->help.c_str());
        }
        if(i==3 || i==6) {ImGui::EndGroup();ImGui::EndGroup();ImGui::Spacing();}
    }
    labelW=70;
}
// Placement modes match the other box synths; Original keeps imported snapshots.
std::vector<float> scSteamPipeBox::panPositions() const {
    const int nv=voices.get(), c=channels.get();
    const bool ring=c>2 && speakerLayout.get()==0;
    std::vector<float> out(nv,0.5f);
    float width=1,randomAmount=1;
    for(const auto& control:sound){
        if(control->sc=="stereo") width=control->p.get();
        if(control->sc=="pan_random") randomAmount=control->p.get();
    }
    for(int i=0;i<nv;++i){
        if(placement.get()==0){
            const float random=std::sin((i+1)*127.1f)*43758.5453f;
            const float pan=random-std::floor(random);
            out[i]=ofClamp(.5f+(pan-.5f)*randomAmount*width,0.f,1.f);
        }else if(placement.get()==3) out[i]=first(panP.get(),i,0.5f);
        else if(placement.get()==2){
            const int ch=i%std::max(1,c);
            out[i]=c<=1?0.5f:ring?float(ch)/c:float(ch)/(c-1);
        }else if(ring) out[i]=float(i)/nv*spreadWidth.get()+spreadCenter.get()*0.5f;
        else {
            const float t=nv>1?float(i)/(nv-1):0.5f;
            out[i]=0.5f+(t-0.5f)*spreadWidth.get()+spreadCenter.get()*0.5f;
        }
    }
    return out;
}

void scSteamPipeBox::sliderVec(ofParameter<std::vector<float>>& p, const char* label, float w, const char* fmt) {
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
    const auto& values = p.get();
    if(values.size() > 1) {
        const ImVec2 a = ImGui::GetCursorScreenPos(), size(sw, ImGui::GetFrameHeight());
        ImGui::InvisibleButton("##bars", size);
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, ImVec2(a.x + size.x, a.y + size.y), pal::canvas, 2 * z);
        const float bw = size.x / values.size();
        for(size_t i = 0; i < values.size(); ++i) {
            const float t = ofClamp(values[i], 0.0f, 1.0f);
            dl->AddRectFilled(ImVec2(a.x + i * bw + 1, a.y + size.y * (1 - t)), ImVec2(a.x + (i + 1) * bw - 1, a.y + size.y), pal::accent);
        }
        if(ImGui::IsItemActive()) p.set({ofClamp((ImGui::GetMousePos().x - a.x) / size.x, 0.0f, 1.0f)});
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%d values, one per voice\nDrag: one value for every voice", int(values.size()));
    } else {
        ImGui::SetNextItemWidth(sw);
        float v = first(values, 0, 0.5f);
        if(ImGui::SliderFloat("##v", &v, 0, 1, fmt)) p.set({v});
    }
    menu(key);
    ImGui::PopID();
}
void scSteamPipeBox::drawPanTab(float w) {
    const float z = zoom();
    const int nv = voices.get(), c = channels.get();
    const bool ring = c > 2 && speakerLayout.get() == 0;
    const float cw = colW(w, 3);
    auto voiceCol = [](int v, int alpha){
        ImColor col = ImColor::HSV(std::fmod(v * 0.61803f, 1.0f), 0.5f, 0.98f);
        col.Value.w = alpha / 255.0f;
        return (ImU32)col;
    };
    ImGui::BeginGroup();
    sectionHeader("Placement", cw);
    combo(placement, "Mode", cw);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Original: recovered snapshot placement\nSpread: evenly across the speakers\nChannels: voice 1 on speaker 1, voice 2 on speaker 2...\n"
                                                 "Manual: each voice where you put it (drag it, or the Pan vector)");
    if(placement.get() == 0){
        slider(soundParam("stereo"),"Width",cw,"%.2f");
        slider(soundParam("pan_random"),"Random",cw,"%.2f");
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("The recovered ensemble's deterministic random voice placement.");
    }
    if(placement.get() == 1){
        slider(spreadWidth, "Spread", cw, "%.2f");
        slider(spreadCenter, "Center", cw, "%+.2f");
    }
    if(placement.get() == 3) sliderVec(panP, "Pan", cw, "%.2f");
    if(c > 2) combo(speakerLayout, "Speakers", cw);
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
        dl->AddRectFilled(a,b,pal::canvas,3*z);
        dl->AddRect(a,b,pal::border,3*z);
        const auto base = panPositions();
        auto live = [&](int v){ const int i = VIS_PAN + v; return i < (int)this->live.size() ? this->live[i] : base[v]; };
        const ImVec2 m = ImGui::GetMousePos();
        int hoverVoice = -1;
        // where position p (0..1) is drawn, for voice v
        std::function<ImVec2(float, int)> place;
        float pad = 30.0f * z;
        if(ring){
            const ImVec2 ctr((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            const float R = h * 0.5f - pad;
            dl->AddCircle(ctr, R, IM_COL32(255,255,255,34), 64, 1.0f * z);
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
                dl->AddCircle(ctr, r, IM_COL32(255,255,255,20), 64);
            }
        }else{
            const float x0 = a.x + pad, x1 = b.x - pad;
            const int sp = std::max(1, c);
            for(int k = 0; k < sp; k++){
                const float x = c <= 1 ? (x0 + x1) * 0.5f : x0 + (x1 - x0) * k / (c - 1);
                dl->AddLine(ImVec2(x, a.y + 22 * z), ImVec2(x, b.y - 6 * z), IM_COL32(255,255,255,20));
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
            if(placement.get() != 3){ panP.set(panPositions()); placement = 3; }
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
            std::vector<float> v(nv);
            for(int i=0;i<nv;++i) v[i]=first(panP.get(),i,0.5f);
            v[panDragVoice] = p;
            panP.set(v);
        }
        if(ImGui::IsItemDeactivated()) panDragVoice = -1;
        if(hovered && hoverVoice >= 0 && panDragVoice < 0)
            ImGui::SetTooltip("Voice %d at %.2f (now %.2f)\nDrag to place it (Shift: onto a speaker)", hoverVoice + 1, base[hoverVoice], live(hoverVoice));
    }
    ImGui::EndGroup();
}

void scSteamPipeBox::drawCompactControl(SoundControl& c,float w,const std::string& label) {
    if(!c.discrete) {slider(c.p,label.c_str(),w,"%.3f");return;}
    ImGui::PushID(c.sc.c_str());
    const float x=ImGui::GetCursorPosX();ImGui::AlignTextToFramePadding();ImGui::TextUnformatted(label.c_str());
    mark(c.p.getEscapedName());ImGui::SameLine();ImGui::SetCursorPosX(x+labelW*zoom());
    ImGui::SetNextItemWidth(std::max(30*zoom(),w-labelW*zoom()));
    int k=std::lround(c.p.get());
    if(c.sc=="polarity") {
        const char* labels[]={"Positive","Negative"};k=c.p.get()<0?1:0;
        if(ImGui::Combo("##value",&k,labels,2)) c.p=k?-1:1;
    } else if(dropdowns.has(c.p.getEscapedName())) {
        const auto items=dropdowns.names(c.p.getEscapedName());
        k=std::clamp(k,0,(int)items.size()-1);
        if(ImGui::Combo("##value",&k,items.data(),(int)items.size())) c.p=float(k);
    }else {bool on=c.p.get()>.5f;if(ImGui::Checkbox("##value",&on)) c.p=on?1:0;}
    menu(c.p.getEscapedName());ImGui::PopID();
}
void scSteamPipeBox::sliderInt(ofParameter<int>& p, const char* label, float w) {
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

void scSteamPipeBox::combo(ofParameter<int>& p, const char* label, float w) {
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


bool scSteamPipeBox::droning() const {
    if(pulsing()) return false;
    if(gateMode.get()==2) return true;
    if(gateMode.get()==1) return false;
    return !(gateTouched || gateIn.connected() || (gateIn.handle && gateIn.handle->isTimelined()));
}
void scSteamPipeBox::drawHeader() {
    const float z=zoom();
    // The same label widths and gutters as PolyComb/AnalogBox. Wrap groups
    // on narrow windows rather than clipping channels or gate mode.
    const float right=ImGui::GetCursorPosX()+ImGui::GetContentRegionAvail().x;
    auto next=[&](float width,float gap=12.0f) {
        if(ImGui::GetItemRectMax().x-ImGui::GetWindowPos().x+gap*z+width<=right) ImGui::SameLine(0,gap*z);
    };
    labelW=54;
    ImGui::BeginGroup();sliderInt(voices,"Voices",170*z);ImGui::EndGroup();
    next(170*z);
    ImGui::BeginGroup();sliderInt(channels,"Channels",170*z);ImGui::EndGroup();
    next(170*z);
    ImGui::BeginGroup();slider(master,"Master",170*z,"%.2f");ImGui::EndGroup();
    next(190*z);labelW=64;
    ImGui::BeginGroup();combo(gateMode,"Gate Mode",190*z);ImGui::EndGroup();
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Auto: drone until Gate is connected, timelined or receives a note.\nGated: follow Gate. Drone: every voice always plays.\nPulse: the voices play by themselves (the Pulse panel).");
    next(ImGui::CalcTextSize("pulsing").x,6);
    ImGui::AlignTextToFramePadding();
    if(pulsing()) ImGui::TextColored(ImVec4(.42f,.83f,.77f,1),"pulsing");
    else ImGui::TextColored(droning()?ImVec4(.96f,.73f,.45f,1):ImVec4(.59f,.65f,1,1),droning()?"droning":"gated");
    next(ImGui::CalcTextSize("Reset LFOs").x+ImGui::GetStyle().FramePadding.x*2);
    if(ImGui::Button(pulsing()?"Reset":"Reset LFOs")) resetLFOs.trigger();
    if(ImGui::IsItemHovered()) ImGui::SetTooltip(pulsing()?"Reset the LFOs and restart the pulse pattern":"Reset the added modulation LFOs together");
    labelW=70;
}
