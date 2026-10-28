#include "../shared/scDroneMode.h"
#include "../../../audio/scFxRouting.h"
#include "scAliasingBox.h"
#include "../shared/scGlobalPitch.h"
#include "scAliasingBoxData.h"
#include "../../../ui/scTypedValue.h"
#include "../../../ui/scEnvelopeCurve.h"
#include "scAliasingFilterView.h"
#include "../../../audio/scSynthControlBatch.h"
#include "../../../ofxOceanodeSuperColliderConfig.h"
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
int morphModeCount(const std::string& sc) {
    return sc=="write_quality" ? 3 : sc=="read_quality" ? 2 : sc=="filter_lfo_wave" ? 8 : 0;
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

scAliasingBox::scAliasingBox(std::vector<serverManager*> s) : scNode("aliasingBox"), servers(std::move(s)) {
    description = "Aliasing table synthesis after Dietrich Pank: independent read/write oscillators, "
                  "notch, serial HP/LP FM and saturation. Gate takes Oceanode values and audio-rate gates together. "
                  "Built from small synths: only the parts in use run. Show opens the editor (tables, original "
                  "snapshots, envelopes, LFOs, routing, effects); right-click a control there to publish, "
                  "modulate or add it to the timeline.";
}
scAliasingBox::~scAliasingBox() {
    scGlobalPitch::unfollow(this);
    scheduled.disable();
    listeners.unsubscribeAll();
    for(auto& [s, st] : states) release(s, st);
}

template<class T> void scAliasingBox::reg(ofParameter<T>& p, std::function<void()> send) {
    Control c;
    c.key = p.getEscapedName(); c.param = &p;
    c.discrete = std::is_same_v<T, bool> || std::is_same_v<T, int>;
    c.add = [this, &p]() { return addParameter(p); };
    c.save = [&p]() { return ofJson(p.get()); };
    c.load = [&p](const ofJson& j) { try { p.set(j.get<T>()); } catch(const std::exception&) {} };
    controls.push_back(std::move(c));
    listeners.push(p.newListener([this, &p, send](T&) { snaps.morph.edited(p); if(!restoring) send(); }));
}
void scAliasingBox::setup() {
    scGlobalPitch::follow(this, true, [this](){
        for(auto& [s, st] : states) for(int i = 0; i < st.builtVoices; ++i) if(auto* v = st.voices[i]) v->set("gtrbus", scGlobalPitch::bus(this, s));
    });
    timeline.setup(this, [this](const std::string& k){ publish(k); }, [this](const std::string& k){ unpublish(k); }, [this](){ return publishedKeys; });
    addParameter(show.set("Show", false));
    addParameter(voices.set("Voices", 5, 1, 16));
    addParameter(channels.set("Channels", 2, 1, 16));
    // Gate (levels: a gate rising to 0.2 plays at 0.2), Pitch: Oceanode values or audio cables
    gateIn.attach(addParameter(gate.set("Gate", {0.0f}, {0.0f}, {1.0f})), availableInputs, [this](){ refreshOutputs(); });
    pitchIn.attach(addParameter(pitch.set("Pitch", {60.0f}, {0.0f}, {127.0f})), availableInputs, [this](){ refreshOutputs(); });
    addParameter(master.set("Master", 0.6f, 0.0f, 1.0f));
    addParameter(resetLFOs.set("Reset"));
    addOutput("Out");

    drone.set("Drone", false);
    reg(drone,[this](){for(auto& [s,st]:states)sendNotes(s,st,NoteRouting);});

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

    const auto firstSnapshot=ofJson::parse(aliasingBoxData::factory)[0]["values"];
    for(const auto& item : ofJson::parse(aliasingBoxData::controls)) {
        auto c = std::make_unique<SoundControl>();
        c->p.set(item["name"].get<std::string>(), firstSnapshot[item["name"].get<std::string>()].get<float>(), item["min"].get<float>(), item["max"].get<float>());
        c->sc = item["sc"].get<std::string>(); c->group = item["group"].get<std::string>();
        c->discrete = item["mode"] == "switch";
        reg(c->p, [this](){ sendAll(); });
        controls.back().discrete = c->discrete;
        if(c->sc=="table_range") tableRange=&c->p;
        // the named switches: their combo's names (published: a dropdown)
        if(c->sc=="filter_lfo_wave") dropdowns.set(c->p.getEscapedName(),{"Saw up","Saw down","Pulse","Triangle","Sine","Random","Smoothed random","Cubic random"});
        else if(c->sc=="write_quality") dropdowns.set(c->p.getEscapedName(),{"Ramp (raw)","Triangle / symmetry","Read oscillator"});
        else if(c->sc=="read_quality") dropdowns.set(c->p.getEscapedName(),{"Triangle / direction","Ramp (raw)"});
        sound.push_back(std::move(c));
    }
    // ── Effects (MixerPro's inserts) ─────────────────────────────────────────
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
    for(int e=0;e<2;++e) for(int k=0;k<3;++k) {
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
    // amount 1: the control's whole span (Pitch: 48 st); the last field, older saves' range
    mod.setup(h, {{"Pitch",48,"st",false,24}, {"Write Pitch",194,"st",false,48}, {"Read Pitch",128,"st",false,48}, {"Write Symmetry",1,""},
        {"LP Pitch",130,"st",false,48}, {"HP Pitch",140,"st",false,48}, {"LP Resonance",0.98f,"",false,1}, {"HP Resonance",0.98f,"",false,1},
        {"LP FM",3000,"Hz"}, {"HP FM",3000,"Hz"}, {"Drive",30,"dB"}, {"Amp",1,""}, {"Pan",1,""}}, true, true, true);
    // The original ADSRs remain in the voice. Extra sources start empty and
    // can be added/removed independently with the shared + Add controls.
    mod.lfoCount = mod.envCount = mod.extCount = 0;
    for(const auto& c : sound) {
        static const std::map<std::string, int> targets = {{"Write Pitch",WritePitch},{"Read Pitch",ReadPitch},{"Write Symmetry",Symmetry},
            {"LP Pitch",LP},{"HP Pitch",HP},{"LP Resonance",LPRes},{"HP Resonance",HPRes},{"LP FM",LPFM},{"HP FM",HPFM},{"Drive",Drive}};
        auto it = targets.find(c->p.getName()); if(it != targets.end()) mod.targetOfKey[c->p.getEscapedName()] = it->second;
    }
    mod.changed = [this](){ if(!restoring) sendAll(); };
    for(const auto& c : sound) if(c->sc!="stereo") mod.addVoiceParam(c->p);
    for(auto& curves : envCurves) for(auto& p : curves) mod.addVoiceParam(p);
    initial = capture();
    scSnapshots::Host sh;
    sh.folder = "AliasingBox"; sh.title = "AliasingBox";
    sh.capture = [this](){ return capture(); };
    sh.apply = [this](const ofJson& a, const ofJson& b, float t, bool sw){ apply(a,b,t,sw); };
    sh.factory = [this](){ return factory(); };
    sh.bpm = [this](){ return bpm; };
    sh.paramMenu = [this](const std::string& k){ mark(k); menu(k); };
    for(auto* p : {&satOn, &eqOn, &echoOn, &rvbOn}) snaps.morph.bypass(*p);
    ampMorph.setupVca(snaps.morph,[this](const ofJson& j){return mod.envRoutes(j,"aliasingMod",Amp);});
    for(const auto& c : sound) {
        if(c->sc == "filter_on" || c->sc == "notch_on" || c->sc == "dc_block" || c->sc == "read_bidirectional")
            snaps.morph.bypass(c->p);
        if(const int count=morphModeCount(c->sc)) snaps.morph.mode(c->p,count);
    }
    sh.morphChanged = [this, toStages]() { sendAll(); toStages(); };
    sh.morphBlocked = [this](const std::string& k) {
        auto it = published.find(k); return it != published.end() && it->second->hasInConnection();
    };
    snaps.morph.mode(echoFilter, 4);
    snaps.setup(sh);
    if(snaps.active<0 && snaps.used[0] && snaps.name(0)=="quak") { snaps.active=0; snaps.snapshotP.setWithoutEventNotifications(1); }
    requireSynthdefs();
}

void scAliasingBox::sendNotes(ofxSCServer* server, State& st, unsigned fields) {
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
            v->set("gtrbus", scGlobalPitch::bus(this, server));
            if(bus>=0) v->mapan("pitch_ar",bus+i,1);
        }
    }
}
bool scAliasingBox::soundOn(const std::string& sc) { return soundParam(sc).get()>0.5f; }
void scAliasingBox::sendSound(ofxSCServer* server, State& st) {
    if(!st.created) return;
    static const char* names[NUM_TARGETS] = {"mpitch","mwrite","mread","msym","mlp","mhp","mlpres","mhpres","mlpfm","mhpfm","mdrive","mamp","mpan"};
    const float vcaW = ampMorph.vcaAmount(liveVca());
    const float comp=1.0f/std::sqrt(std::max(1.0f,float(st.builtVoices)/st.builtChannels));
    for(int i=0;i<st.builtVoices;++i) if(auto* v=st.voices[i]) {
        scSynthControlBatch updates(v,server);
        auto set = [&](const std::string& name,float value) {
            auto& sent=st.sent[i];auto it=sent.find(name);
            if(it!=sent.end()&&it->second==value) return;
            sent[name]=value;updates.set(name,value);
        };
        for(const auto& c : sound) if(c->sc!="stereo") {
            set(c->sc, (c->sc=="filter_on" || c->sc=="notch_on" || c->sc=="dc_block" || c->sc=="read_bidirectional")
                ? snaps.morph.activation(c->p) : mod.voiceValue(c->p,i));
            if(morphModeCount(c->sc)) {
                const auto weights=snaps.morph.weights(c->p);
                // Scalar weight controls reuse the ordinary send cache: no
                // extra OSC traffic while a settled preset is running.
                for(size_t k=0;k<weights.size();++k) set(c->sc+"_w"+ofToString(k),weights[k]);
            }
        }
        for(int e=0;e<2;++e) for(int k=0;k<3;++k) set(std::string(e?"filter_curve":"amp_curve")+ofToString(k),mod.voiceValue(envCurves[e][k],i));
        set("gatebus",st.gates->index+i);
        set("velbus",mod.sourceBus(server,scModulation::SRC_VEL)+i);
        set("out",st.audio->index+i);
        set("voice",i); set("voices",st.builtVoices);
        set("master",master.get()); set("bpm",bpm);
        set("normalization",1.0f/(st.builtVoices*comp));
        set("vca", vcaW);
        set("vis",st.vis->index);
        for(int t=0;t<NUM_TARGETS-1;++t) set(names[t],mod.targetBus(server,t)+i);
        updates.send();
    }
    if(st.spread) {
        scSynthControlBatch spread(st.spread,server);
        spread.set("in",st.audio->index);
        spread.set("voices",st.builtVoices);
        float width=1;
        for(const auto& c : sound) if(c->sc=="stereo") width=c->p.get();
        std::vector<float> pans(16,0.5f);
        for(int i=0;i<st.builtVoices;++i) pans[i]=ofClamp(0.5f+(st.builtVoices>1 ? (float(i)/(st.builtVoices-1)-0.5f)*width : 0),0.0f,1.0f);
        spread.set("pan",pans); spread.set("mpan",mod.targetBus(server,Pan));
        // Spreader telemetry has its own space after the voice meters.
        spread.set("vis",st.vis->index+scAliasingTableView::PAN);
        spread.send();
    }
    updateStages(server,st);
}
void scAliasingBox::sendAll() { soundDirty = true; }
void scAliasingBox::flushSound() {
    if(!soundDirty) return;
    soundDirty = false;
    for(auto& [s,st] : states) sendSound(s,st);
}
void scAliasingBox::buildSynth(ofxSCServer* server) {
    if(!server) return;
    auto& st=states[server]; release(server,st); st.root=new ofxSCGroup(server);
}
void scAliasingBox::createSynth(ofxSCServer* server) {
    auto it=states.find(server); if(it==states.end() || !it->second.root) return;
    auto& st=it->second;
    if(placeBefore>=0) st.root->create(2,placeBefore); else st.root->create(0,1);
    for(auto*& g:st.groups) { g=new ofxSCGroup(server); g->create(1,st.root->nodeID); }
    st.gates=new ofxSCBus(RATE_AUDIO,16,server); st.audio=new ofxSCBus(RATE_AUDIO,16,server);
    st.vis=new ofxSCBus(RATE_CONTROL,scAliasingTableView::channels(voices.get()),server);
    if(!liveBus(server,st.gates,true)||!liveBus(server,st.audio,true)||!liveBus(server,st.vis,false)) { release(server,st); return; }
    st.builtVoices=voices.get(); st.builtChannels=channels.get(); st.created=true;
    mod.create(server,st.groups[SlSrc]->nodeID,st.groups[SlRoute]->nodeID,st.gates->index);
    st.gate=new ofxSCSynth("ModGate"+ofToString(st.builtVoices),server);
    for(int i=0;i<st.builtVoices;++i) st.voices[i]=new ofxSCSynth("AliasingBoxVoice",server);
    st.spread=new ofxSCSynth("PolyCombSpread"+ofToString(st.builtChannels),server);
    sendNotes(server,st); sendSound(server,st);
    st.gate->createAndRun(1,st.groups[SlGate]->nodeID,true);
    for(int i=0;i<st.builtVoices;++i) st.voices[i]->createAndRun(1,st.groups[SlVoice]->nodeID,true);
    st.spread->createAndRun(1,st.groups[SlSpread]->nodeID,true);
    routeChain(st);
    if(!getActive()) st.root->run(false);
}
void scAliasingBox::release(ofxSCServer* server, State& st) {
    if(st.created && st.root) st.root->free();
    if(st.created) mod.free(server,false);
    for(auto& f:st.fx) { delete f.synth; f.synth=nullptr; dropBus(server,f.in,true); f.running=false; f.offAt=-1; }
    delete st.gate; st.gate=nullptr; delete st.spread; st.spread=nullptr;
    for(auto*& v:st.voices) { delete v; v=nullptr; }
    for(auto& cache:st.sent) cache.clear();
    for(auto*& g:st.groups) { delete g; g=nullptr; }
    delete st.root; st.root=nullptr; st.created=false;st.droneSent=-1;
    dropBus(server,st.gates,true); dropBus(server,st.audio,true); dropBus(server,st.vis,false);
}
void scAliasingBox::free(ofxSCServer* s) { auto it=states.find(s); if(it!=states.end()) { release(s,it->second); it->second.out=-1; } }
// A new voice / speaker count: everything again, but in the old root's place.
// The graph is not recomputed (same topology), so a root at the head of the
// default group would run before the nodes feeding Gate/Pitch by
// cable, read silent buses and stay quiet until a reconnection.
void scAliasingBox::rebuild() {
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
void scAliasingBox::moveSynthBefore(ofxSCServer* s,int id) { auto it=states.find(s); if(it!=states.end()&&it->second.created) it->second.root->moveBefore(id); }
int scAliasingBox::getLastSynthID(ofxSCServer* s) { auto it=states.find(s); return it!=states.end()&&it->second.created ? it->second.root->nodeID : -1; }
void scAliasingBox::setOutputBus(ofxSCServer* s,int,int bus) { auto& st=states[s]; st.out=bus; if(st.created) routeChain(st); }
int scAliasingBox::getOutputBusIndex(ofxSCServer* s,int) { auto it=states.find(s); return it==states.end() ? -1 : it->second.out; }
void scAliasingBox::setInputBus(ofxSCServer* s,scNode* node,int bus) { auto& st=states[s]; if(gateIn.take(s,node,bus)|pitchIn.take(s,node,bus)) sendNotes(s,st); }
void scAliasingBox::resetInputBusses(ofxSCServer* s,int) { gateIn.reset(s);pitchIn.reset(s);sendNotes(s,states[s]); }
void scAliasingBox::activate() { for(auto& [s,st]:states) if(st.created) st.root->run(true); }
void scAliasingBox::deactivate() { for(auto& [s,st]:states) if(st.created) st.root->run(false); }
void scAliasingBox::setBpm(float v) { bpm=std::max(1.0f,v);mod.setBpm(bpm);sendAll(); }
void scAliasingBox::resendParametersForNRT() {
    for(auto& [s,st]:states) { for(auto& cache:st.sent)cache.clear();sendNotes(s,st); }
    mod.resend();sendAll();flushSound();
}
#ifdef OFXOCEANODE_HAS_TIMELINE_INSTRUMENT
bool scAliasingBox::getTimelineInstrument(ofxOceanodeTimelineInstrument& out) {
    if(!gateIn.handle || !pitchIn.handle) return false;
    out.gate=gateIn.handle.get();out.pitch=pitchIn.handle.get();out.velocity=nullptr;out.voices=voices.get();
    return true;
}
#endif

bool scAliasingBox::connected(const std::string& k) const {
    auto it=published.find(k);return it!=published.end() && (it->second->hasInConnection()||it->second->hasOutConnections());
}
void scAliasingBox::publish(const std::string& k) {
    if(published.count(k)||getParameterGroup().contains(k)) return;
    auto it=std::find_if(controls.begin(),controls.end(),[&](const Control& c){return c.key==k;});if(it==controls.end()) return;
    if(!separator){addSeparator("Published",ofColor(200));separator=true;}
    published[k]=it->add();dropdowns.apply(k,published[k]);publishedKeys.push_back(k);parameterGroupChanged.notify(this);
}
void scAliasingBox::unpublish(const std::string& k) {
    auto it=published.find(k);if(it==published.end()||connected(k)) return;
    removeParameter(it->second->getName());published.erase(it);
    publishedKeys.erase(std::remove(publishedKeys.begin(),publishedKeys.end(),k),publishedKeys.end());
    if(published.empty()&&separator){removeSeparator("Published");separator=false;}
    parameterGroupChanged.notify(this);
}
void scAliasingBox::mark(const std::string& k) {
    timeline.mark(k);
    if(published.count(k)&&!timeline.isHidden(k)) {
        auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x,b.y),b,IM_COL32(106,212,196,255),2);
    }
}
void scAliasingBox::menu(const std::string& k) {
    if(!ImGui::BeginPopupContextItem(("##alias"+k).c_str())) return;
    auto it=std::find_if(controls.begin(),controls.end(),[&](const Control& c){return c.key==k;});
    if(it!=controls.end()) scTyped::jsonField(k,it->save,it->load);
    if(timeline.isHidden(k)){if(ImGui::MenuItem("Publish to Node GUI")) timeline.reveal(k);}
    else if(published.count(k)){
        if(connected(k)) ImGui::TextDisabled("Disconnect it to unpublish");
        else if(ImGui::MenuItem("Unpublish from Node")) timeline.unpublish(k);
    }else if(ImGui::MenuItem("Publish to Node GUI")) publish(k);
    timeline.menuItems(k);mod.modulateMenu(k);ImGui::EndPopup();
}
ofJson scAliasingBox::capture() const {
    ofJson j=ofJson::object();
    for(const auto& c:controls) if(!scSnapshots::isOwnKey(c.key)) j[c.key]=c.save();
    mod.patchBases(j);mod.save(j,"aliasingMod");return j;
}
void scAliasingBox::apply(const ofJson& aSaved, const ofJson& bSaved,float t,bool sw) {
    const auto a = scDroneMode::migrated(aSaved), b = scDroneMode::migrated(bSaved);
    restoring=true;
    for(const auto& c:controls) if(!scSnapshots::isOwnKey(c.key)&&!(published.count(c.key)&&published.at(c.key)->hasInConnection())) {
        const auto& av=a.contains(c.key)?a[c.key]:initial[c.key];
        const auto& bv=b.contains(c.key)?b[c.key]:initial[c.key];
        c.load(blend(av,bv,t,sw,c.discrete));
    }
    mod.loadMorph(a,b,t,sw,"aliasingMod",[this](const std::string& key){
        auto it=published.find(key);return it!=published.end() && it->second->hasInConnection();
    });
    mod.afterLoad();
    mod.refresh();
    restoring=false;ampMorph.markApplied(liveVca());sendAll();
}
std::vector<ofJson> scAliasingBox::factory() const {
    std::vector<ofJson> out;
    for(const auto& preset:ofJson::parse(aliasingBoxData::factory)) {
        ofJson j=initial;j["name"]=preset["name"];
        for(const auto& c:sound) if(preset["values"].contains(c->p.getName())) j[c->p.getEscapedName()]=preset["values"][c->p.getName()];
        out.push_back(j);
    }
    return out;
}
void scAliasingBox::presetSave(ofJson& j) {
    mod.saveLfoOutputs(j);
    j["aliasingBox"]=capture();
    // Snapshot controls belong in a patch, but never inside a sound snapshot.
    for(const auto& c:controls) if(scSnapshots::isOwnKey(c.key)) j["aliasingBox"][c.key]=c.save();
    j["aliasingBoxPublished"]=publishedKeys;timeline.save(j,"aliasingBoxTimeline");
    j["aliasingBoxDisplay"]={{"mode",tableMode}};
}
void scAliasingBox::loadBeforeConnections(ofJson& j) {
    scDroneMode::migrate(j);
    mod.loadLfoOutputs(j);
    std::vector<std::string> wanted;
    if(j.contains("aliasingBoxPublished")&&j["aliasingBoxPublished"].is_array()) for(const auto& k:j["aliasingBoxPublished"]) if(k.is_string()) wanted.push_back(k.get<std::string>());
    const auto old=publishedKeys;for(const auto& k:old) if(std::find(wanted.begin(),wanted.end(),k)==wanted.end()) unpublish(k);
    for(const auto& k:wanted) publish(k);timeline.load(j,"aliasingBoxTimeline");
}
void scAliasingBox::presetRecallAfterSettingParameters(ofJson& j) {
    scDroneMode::migrate(j);
    if(!j.contains(drone.getEscapedName())) drone = false;
    if(j.contains("aliasingBoxDisplay")) {
        const auto& d=j["aliasingBoxDisplay"];
        tableMode=std::clamp(d.value("mode",0),0,1);

    }
    if(!j.contains("aliasingBox")) return;
    auto state=j["aliasingBox"];
    // The first patches stored Drone directly on the node.
    if(!state.contains(drone.getEscapedName())) {
        bool droneOn=false;
        if(j.contains("Drone")) {
            const auto& d=j["Drone"];
            droneOn=d.is_boolean() ? d.get<bool>() : d.is_number() ? d.get<float>()>0.5f : d.is_string() && (d.get<std::string>()=="1" || d.get<std::string>()=="true");
        }
        state[drone.getEscapedName()]=droneOn;
    }
    apply(state,state,1,true);
    restoring=true;for(const auto& c:controls) if(scSnapshots::isOwnKey(c.key)&&state.contains(c.key)) c.load(state[c.key]);restoring=false;
}
void scAliasingBox::requireSynthdefs() {
    for(const auto& entry:std::vector<std::pair<std::string,std::string>>{
        {"AliasingBoxVoice.scsyndef","AliasingBox"},{"ModGate1.scsyndef","Modulation"},{"PolyCombSpread2.scsyndef","PolyComb"},{"mixerProEQ2.scsyndef","MixerPro"}}) {
        std::filesystem::path folder;
        try {
            const std::filesystem::path data=ofToDataPath(SYNTHDEF_DIRECTORY,true);
            if(std::filesystem::exists(data)) for(const auto& f:std::filesystem::recursive_directory_iterator(data,std::filesystem::directory_options::skip_permission_denied))
                if(f.is_regular_file()&&f.path().filename()==entry.first){folder=f.path().parent_path();break;}
            if(folder.empty()) folder=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path().parent_path()/"synthdefs/Defaults"/entry.second/"CompiledSynthdefs"/(entry.second=="MixerPro" ? "mixerpro" : entry.second);
            if(!std::filesystem::exists(folder/entry.first)){ofLogError("aliasingBox")<<"Compile synthdefs/Defaults/"<<entry.second<<" first";continue;}
            for(auto* sm:servers) if(sm) sm->requireSynthdefFolder(folder.string());
        } catch(const std::exception& e){ofLogError("aliasingBox")<<e.what();}
    }
}
void scAliasingBox::update(ofEventArgs&) {
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
        if(now>=nextRead){nextRead=now+1.0f/25;s->requestControlValues(st.vis->index,scAliasingTableView::channels(st.builtVoices));}
        break;
    }
}
void scAliasingBox::drawDisplay(float w) {
    const float z=ofxOceanodeShared::getZoomLevel();
    ImGui::TextDisabled("TABLE MEMORY");ImGui::SameLine(0,18*z);
    ImGui::RadioButton("Lines",&tableMode,0);ImGui::SameLine();ImGui::RadioButton("Intensity",&tableMode,1);
    ImGui::SameLine(0,18*z);ImGui::TextDisabled("%d voices / read: pale / write: violet",voices.get());
    auto a=ImGui::GetCursorScreenPos();
    const float height=ofClamp(voices.get()*22.0f,150.0f,260.0f)*z;   // leaves the tabs their room
    scAliasingTableView::draw(ImGui::GetWindowDrawList(),a,ImVec2(w,height),live,voices.get(),tableMode,0,1,z);
    ImGui::InvisibleButton("##tableMemory",ImVec2(w,height));
    if(ImGui::IsItemHovered()) {
        const auto mouse=ImGui::GetMousePos();
        int voice=std::clamp(int((mouse.y-a.y)/height*voices.get()),0,voices.get()-1);
        float pos=ofClamp((mouse.x-a.x-25*z)/std::max(1.0f,w-25*z),0.0f,1.0f);
        int bin=std::clamp(int(pos*scAliasingTableView::BINS),0,scAliasingTableView::BINS-1);
        ImGui::SetTooltip("Voice %d | table %.1f%% | value %.3f\nWrite %.2f Hz | read %.2f Hz",voice+1,pos*(tableRange?tableRange->get():1)*100,
            scAliasingTableView::value(live,scAliasingTableView::TABLE+voice*scAliasingTableView::BINS+bin),
            scAliasingTableView::value(live,voice*6),scAliasingTableView::value(live,voice*6+1));
    }
    a=ImGui::GetCursorScreenPos();const float h=14*z;
    auto* dl=ImGui::GetWindowDrawList();
    dl->AddRectFilled(a,ImVec2(a.x+w,a.y+h),IM_COL32(2,4,8,255));
    const float range=tableRange?tableRange->get():1;
    dl->AddRectFilled(a,ImVec2(a.x+w*range,a.y+h),IM_COL32(210,255,175,255));
    dl->AddRect(a,ImVec2(a.x+w,a.y+h),IM_COL32(75,102,77,255));
    ImGui::InvisibleButton("##tableRange",ImVec2(w,h));
    if(tableRange && ImGui::IsItemActive() && ImGui::IsMouseDown(0)) {
        float pos=ofClamp((ImGui::GetMousePos().x-a.x)/std::max(1.0f,w),0.0f,1.0f);
        tableRange->set(std::round(pos*512)/512);
    }
    if(ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Table Range %.1f%% | changes read/write addressing and the sound.\nDrag to set; double-click for the full table; right-click to publish or automate.",range*100);
        if(tableRange && ImGui::IsMouseDoubleClicked(0)) tableRange->set(1);
    }
    if(tableRange){mark(tableRange->getEscapedName());menu(tableRange->getEscapedName());}
}
void scAliasingBox::drawWindow() {
    const float z=ofxOceanodeShared::getZoomLevel();
    ImGui::SetNextWindowSize(ImVec2(1150*z,850*z),ImGuiCond_FirstUseEver);
    bool open=show.get();
    if(!ImGui::Begin(getWindowId().c_str(),&open,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();if(!open) show=false;return;
    }
    if(!open){show=false;ImGui::End();return;}
    keyboardPiano.beginContent(pitch, gate, voices.get());
    const float w=ImGui::GetContentRegionAvail().x;
    drawHeader();
    snaps.drawBar(w);
    drawDisplay(w);
    ImGui::Spacing();
    if(ImGui::BeginTabBar("##aliasTabs")) {
        auto tab=[&](const char* name,std::function<void(float)> body) {
            if(!ImGui::BeginTabItem(name)) return;
            ImGui::BeginChild((std::string("##aliasTab")+name).c_str(),ImVec2(0,0),false);
            ImGui::Spacing();
            const float tw=ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ScrollbarSize;
            labelW=70;
            body(tw);
            ImGui::EndChild();
            ImGui::EndTabItem();
        };
        tab("Sound",[this](float tw){drawSoundTab(tw);});
        tab("Envelopes",[this](float tw){
            mod.drawEnvelopeCards(tw,{[this](float cw){drawNativeEnvelope(0,cw);},[this](float cw){drawNativeEnvelope(1,cw);}});
        });
        tab("LFOs",[this](float tw){mod.drawLfos(tw);});
        tab("Routing",[this](float tw){mod.drawRouting(tw);});
        tab("Effects",[this](float tw){drawFxTab(tw);});
        ImGui::EndTabBar();
    }
    scKeyboardPiano::endContent();
    ImGui::End();
}
void scAliasingBox::draw(ofEventArgs&) {
    if(show.get()) drawWindow();
    keyboardPiano.finishFrame();
}

void scAliasingBox::sectionHeader(const char* title, float w, ofParameter<bool>* on, ImU32 accent) {
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

void scAliasingBox::slider(ofParameter<float>& p, const char* label, float w, const char* fmt, bool log) {
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

void scAliasingBox::toggle(ofParameter<bool>& p, const char* label) {
    bool v = p.get();
    if(ImGui::Checkbox(label, &v)) p.set(v);
    mark(p.getEscapedName());
    menu(p.getEscapedName());
}


void scAliasingBox::updateStages(ofxSCServer* server, State& st) {
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
void scAliasingBox::routeChain(State& st) {
    int next = std::max(0,st.out);
    for(int s = NumStages - 1; s >= 0; s--){
        auto& f = st.fx[s];
        if(!f.synth || !f.running || !f.in) continue;
        f.synth->set("out", next);
        next = f.in->index;
    }
    if(st.spread) st.spread->set("out", next);
}

float scAliasingBox::stageOn(int s) const {
    return s == StSat ? snaps.morph.activation(satOn) : s == StEq ? snaps.morph.activation(eqOn) : s == StEcho ? snaps.morph.activation(echoOn) : snaps.morph.activation(rvbOn);
}

std::string scAliasingBox::stageDef(int s) const {
    static const char* names[NumStages] = {"mixerProSat", "mixerProEQ", "mixerProEcho", "boxMicroSpace"};
    return names[s] + ofToString(channels.get());
}

float scAliasingBox::echoDelaySec() const {
    if(!echoBeats.get()) return ofClamp(echoDelay.get(), 0.005f, 4.0f);
    return ofClamp(echoBeatVal.get() * 60.0f / std::max(1.0f, bpm), 0.005f, 4.0f);
}

float scAliasingBox::stageTail(int s) const {
    if(s == StEq || s == StSat) return 0.02f;
    if(s == StEcho){
        const float feed = ofClamp(echoFeed.get(), 0.0f, 0.98f);
        const float repeats = feed > 0.001f ? std::log(0.001f) / std::log(feed) : 0.0f;
        return std::min(30.0f, echoDelaySec() * (repeats + 1.0f) + 0.2f);
    }
    return std::min(30.0f, rvbDecay.get() + rvbPredelay.get() * 0.001f + 0.3f);
}

void scAliasingBox::sendStage(int s, ofxSCSynth* x, ofxSCServer* server) {
    if(!x) return;
    scSynthControlBatch controls(x, server);
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

void scAliasingBox::sendStageAll(int) { sendAll(); }

void scAliasingBox::drawFxTab(float w) {
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
    ImGui::TextColored(ImColor(pal::textDim), "Voices -> Saturation -> EQ -> Echo -> Reverb (MixerPro's), at speaker count.");
}

void scAliasingBox::drawNativeEnvelope(int e,float w) {
    const float z=zoom();
    const ImU32 col=e ? IM_COL32(245,187,115,235) : IM_COL32(150,165,255,235);
    const std::string prefix=e ? "filter_" : "amp_";
    auto param=[&](const std::string& name)->ofParameter<float>& {
        return (*std::find_if(sound.begin(),sound.end(),[&](const auto& c){return c->sc==name;}))->p;
    };
    auto& a=param(prefix+"attack");auto& d=param(prefix+"decay");
    auto& su=param(prefix+"sustain");auto& r=param(prefix+"release");
    auto seconds=[](float v){return .001f*std::pow(10.0f,v/20.0f);};
    auto setSeconds=[](ofParameter<float>& p,float v){p=ofClamp(20*std::log10(std::max(.0001f,v)/.001f),p.getMin(),p.getMax());};
    ImGui::PushID(e ? "filterEnvelope" : "ampEnvelope");ImGui::BeginGroup();
    sectionHeader(e ? "Filter Envelope" : "Amp Envelope",w,nullptr,col);
    labelW=58;
    scEnvCurve::ADSR env{seconds(a.get()),seconds(d.get()),su.get(),seconds(r.get()),{envCurves[e][0].get(),envCurves[e][1].get(),envCurves[e][2].get()}};
    const ImVec2 origin=ImGui::GetCursorScreenPos();
    if(scEnvCurve::editADSR("##env",ImVec2(w-18*z,110*z),env,col,pal::canvas,false,seconds(r.getMax()),.0001f)) {
        setSeconds(a,env.a);setSeconds(d,env.d);su=env.s;setSeconds(r,env.r);
        for(int k=0;k<3;++k) envCurves[e][k]=env.curve[k];
    }
    auto* dl=ImGui::GetWindowDrawList();
    const ImVec2 ma(origin.x+w-14*z,origin.y),mb(origin.x+w,origin.y+110*z);
    dl->AddRectFilled(ma,mb,pal::canvas,3*z);dl->AddRect(ma,mb,pal::border,3*z);
    for(int v=0;v<voices.get();++v) {
        const int offset=e ? scAliasingTableView::filterEnv(voices.get())+v : v*6+2;
        const float level=e && !soundOn("filter_on") ? 0.0f : ofClamp(scAliasingTableView::value(live,offset),0.0f,1.0f);
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
    if(!e) slider(param("velocity_sensitivity"),"Vel",w,"%.2f");
    const float x=ImGui::GetCursorPosX();ImGui::AlignTextToFramePadding();ImGui::TextUnformatted("Curves");
    ImGui::SameLine();ImGui::SetCursorPosX(x+labelW*z);
    const float cw=std::max(1.0f,(w-labelW*z-2*ImGui::GetStyle().ItemSpacing.x)/3);
    const char* formats[]={"A %.1f","D %.1f","R %.1f"};
    for(int k=0;k<3;++k) {
        if(k) ImGui::SameLine();ImGui::PushID(k);ImGui::SetNextItemWidth(cw);
        auto& p=envCurves[e][k];float v=p.get();if(ImGui::SliderFloat("##c",&v,-12,12,formats[k])) p=v;
        mark(p.getEscapedName());menu(p.getEscapedName());ImGui::PopID();
    }
    if(e) slider(param("filter_env_amount"),"Amount",w,"%.2f");
    else slider(param("envelope_keytrack"),"Keytrack",w,"%.2f");
    ImGui::TextColored(ImColor(pal::textDim),e ? "-> HP / LP pitch" : "-> Voice amplitude");
    labelW=70;ImGui::EndGroup();ImGui::PopID();
}

ofParameter<float>& scAliasingBox::soundParam(const std::string& name) {
    return (*std::find_if(sound.begin(),sound.end(),[&](const auto& c){return c->sc==name;}))->p;
}

// One filter card: header, cutoff / resonance / FM (gain, key track for the
// notch) and its response plot; drag the plot for cutoff and resonance.
void scAliasingBox::drawFilterCard(int type,float cw,float sr) {
    const float z=zoom();
    const std::string pre=type==0 ? "hp_" : type==1 ? "lp_" : "notch_";
    const ImU32 color=type==0 ? IM_COL32(150,165,255,235) : type==1 ? IM_COL32(245,187,115,235) : IM_COL32(112,209,185,235);
    ImGui::PushID(type);
    sectionHeader(type==0 ? "High Pass" : type==1 ? "Low Pass" : "Notch",cw,nullptr,color);
    auto& p=soundParam(pre+"pitch");auto& r=soundParam(pre+"resonance");
    const bool on=soundOn(type==2 ? "notch_on" : "filter_on");
    if(!on) ImGui::BeginDisabled();
    const float hz=440*std::pow(2.0f,(p.get()-69)/12);
    const std::string fmt=ofToString(p.get(),1)+"  ("+(hz>=1000 ? ofToString(hz/1000,2)+" kHz)" : ofToString(hz,0)+" Hz)");
    slider(p,"Cutoff",cw,fmt.c_str());
    slider(r,"Resonance",cw,"%.2f");
    if(type==2){slider(soundParam("notch_gain"),"Gain",cw,"%+.1f dB");slider(soundParam("notch_keytrack"),"Key Track",cw,"%.2f");}
    else slider(soundParam(pre+"fm"),"FM",cw,"%.0f Hz");
    float note=p.get(),res=r.get(),gain=type==2 ? soundParam("notch_gain").get() : 0;
    if(scAliasingFilterView::draw("##response",cw,110*z,type,note,res,gain,sr,on,color,z)) {
        p=ofClamp(note,p.getMin(),p.getMax());r=res;if(type==2) soundParam("notch_gain")=gain;
    }
    mark(p.getEscapedName());menu(p.getEscapedName());
    if(!on) ImGui::EndDisabled();
    ImGui::PopID();
}

// Oscillators, output, filters and the original filter LFO: five columns.
void scAliasingBox::drawSoundTab(float w) {
    const float cw=colW(w,5);
    float sr=48000;
    for(auto* sm:servers) if(sm && sm->getServer()){sr=serverManager::getSampleRateForServer(sm->getServer());break;}
    if(sr<=0) sr=48000;
    auto group=[&](const std::string& name,const std::string& strip,float lw,const std::string& sc="") {
        labelW=lw;
        for(auto& c:sound) if(c->group==name && c->sc.rfind(sc,0)==0) {
            std::string label=c->p.getName();
            if(label.rfind(strip,0)==0) label.erase(0,strip.size());
            drawCompactControl(*c,cw,label);
        }
        labelW=70;
    };
    auto onToggle=[&](const char* key,const char* label) {
        auto& p=soundParam(key);bool on=p.get()>.5f;
        if(ImGui::Checkbox(label,&on)) p=on?1:0;
        mark(p.getEscapedName());menu(p.getEscapedName());
    };
    ImGui::BeginGroup();
    sectionHeader("Write Oscillator",cw);group("Write","Write ",85);
    nextCol();
    sectionHeader("Read Oscillator",cw);group("Read","Read ",85);
    nextCol();
    sectionHeader("Output",cw);group("Output","",85);
    ImGui::Spacing();
    sectionHeader("Filter Chain",cw);
    onToggle("filter_on","HP / LP on");
    slider(soundParam("drive"),"Drive",cw,"%.1f dB");
    onToggle("dc_block","DC block");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+cw);
    ImGui::TextColored(ImColor(pal::textDim),"Notch -> HP -> saturation -> LP -> saturation. Plots show the base response; "
                       "FM, envelopes, LFOs and saturation change it while playing.");
    ImGui::PopTextWrapPos();
    nextCol();
    drawFilterCard(0,cw,sr);ImGui::Spacing();drawFilterCard(1,cw,sr);
    nextCol();
    drawFilterCard(2,cw,sr);onToggle("notch_on","Notch on");
    ImGui::Spacing();
    sectionHeader("Filter LFO",cw,nullptr,IM_COL32(245,187,115,235));
    group("Filter envelope","Filter LFO ",85,"filter_lfo_");
    ImGui::EndGroup();
}

void scAliasingBox::drawCompactControl(SoundControl& c,float w,const std::string& label) {
    if(!c.discrete) {slider(c.p,label.c_str(),w,"%.3f");return;}
    ImGui::PushID(c.sc.c_str());
    const float x=ImGui::GetCursorPosX();ImGui::AlignTextToFramePadding();ImGui::TextUnformatted(label.c_str());
    mark(c.p.getEscapedName());ImGui::SameLine();ImGui::SetCursorPosX(x+labelW*zoom());
    ImGui::SetNextItemWidth(std::max(30*zoom(),w-labelW*zoom()));
    int k=std::lround(c.p.get());
    if(dropdowns.has(c.p.getEscapedName())) {
        const auto items=dropdowns.names(c.p.getEscapedName());
        k=std::clamp(k,0,(int)items.size()-1);
        if(ImGui::Combo("##value",&k,items.data(),(int)items.size())) c.p=float(k);
    }else {bool on=c.p.get()>.5f;if(ImGui::Checkbox("##value",&on)) c.p=on?1:0;}
    menu(c.p.getEscapedName());ImGui::PopID();
}
void scAliasingBox::sliderInt(ofParameter<int>& p, const char* label, float w) {
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

void scAliasingBox::combo(ofParameter<int>& p, const char* label, float w) {
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


bool scAliasingBox::droning() const {
    return drone.get();
}
void scAliasingBox::drawHeader() {
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

bool scAliasingBox::liveVca() const {
    for(int e = 0; e < mod.envCount; ++e)
        if(std::abs(mod.matrix[(scModulation::SRC_ENV + e) * NUM_TARGETS + Amp]) > 0.0001f) return true;
    return false;
}
