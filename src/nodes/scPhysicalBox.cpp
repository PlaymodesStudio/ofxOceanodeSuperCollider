#include "scPhysicalBox.h"
#include "scTypedValue.h"
#include "scSynthControlBatch.h"
#include "ofxOceanodeSuperColliderConfig.h"
#include "../../ugens/OceanodePhysical/PhysicalModes.h"
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace {
namespace pal {
const ImU32 canvas     = IM_COL32( 11,  12,  14, 255);
const ImU32 border     = IM_COL32( 46,  50,  58, 255);
const ImU32 grid       = IM_COL32(255, 255, 255,  14);
const ImU32 header     = IM_COL32( 30,  32,  38, 255);
const ImU32 headerText = IM_COL32(205, 205, 215, 255);
const ImU32 textDim    = IM_COL32(150, 150, 160, 255);
const ImU32 accent     = IM_COL32(222, 170, 105, 255);   // brass
const ImU32 exciter    = IM_COL32(245, 140,  90, 255);   // the contact
const ImU32 pickup     = IM_COL32(106, 212, 196, 255);   // the pickups
const ImU32 waveguide  = IM_COL32(150, 165, 255, 255);
const ImU32 body       = IM_COL32(222, 170, 105, 255);
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
ImU32 mixColor(ImU32 a, ImU32 b, float t) {
    t = ofClamp(t, 0, 1);
    auto ch = [&](int s) { return int(((a >> s) & 255) * (1 - t) + ((b >> s) & 255) * t); };
    return IM_COL32(ch(0), ch(8), ch(16), ch(24));
}
// The factory sounds: PhysicalVoice inputs that differ from the defaults
// (the Phase 0 prototype's presets, synthdefs/WIP/PhysicalBox/physicalbox.scd).
struct Factory { const char* name; std::vector<std::pair<const char*, float>> values; };
const std::vector<Factory>& factorySounds() {
    static const std::vector<Factory> f = {
        {"init", {}},
        {"Struck string", {{"excite", 0}, {"listen", 0}, {"adecay", 3}, {"hardness", 0.6f}}},
        {"Stiff string", {{"excite", 0}, {"listen", 0}, {"astiff", 0.35f}, {"adecay", 5}, {"hardness", 0.75f}, {"abright", 0.8f}, {"mass", 0.4f}}},
        {"Sitar", {{"excite", 0}, {"listen", 0}, {"adecay", 4}, {"rattle", 0.6f}, {"rattleth", 0.05f}, {"hardness", 0.8f}, {"pos", 0.1f}}},
        {"Bowed string", {{"strike", 0}, {"bow", 1}, {"excite", 0}, {"listen", 0}, {"adecay", 2}, {"pos", 0.12f}, {"atk", 0.15f}}},
        {"Scratchy bow", {{"strike", 0}, {"bow", 1}, {"excite", 0}, {"listen", 0}, {"adecay", 2}, {"pos", 0.12f}, {"bowpress", 1.1f}, {"rosin", 0.8f}, {"atk", 0.1f}}},
        {"Flute", {{"strike", 0}, {"blow", 1}, {"blowtype", 0}, {"abore", 0.5f}, {"abright", 0.25f}, {"adecay", 0.3f}, {"breath", 0.6f}, {"pos", 0.02f}, {"turb", 0.25f}, {"excite", 0}, {"listen", 0}}},
        {"Clarinet", {{"strike", 0}, {"blow", 1}, {"blowtype", 1}, {"abore", 0.5f}, {"pos", 0.02f}, {"adecay", 0.5f}, {"excite", 0}, {"listen", 0}}},
        {"Wood bar", {{"excite", 1}, {"listen", 1}, {"bshape", 1}, {"material", 0}, {"bdecay", 2}, {"hardness", 0.5f}}},
        {"Glass bar", {{"excite", 1}, {"listen", 1}, {"bshape", 1}, {"material", 0.5f}, {"bdecay", 2}, {"hardness", 0.7f}}},
        {"Bell", {{"excite", 1}, {"listen", 1}, {"bshape", 3}, {"material", 1}, {"bdecay", 3}, {"hardness", 0.7f}}},
        {"Tam-tam bloom", {{"excite", 1}, {"listen", 1}, {"bshape", 4}, {"material", 1}, {"bdecay", 4}, {"cascade", 0.8f}, {"hardness", 0.8f}, {"mass", 0.8f}}},
        {"Tom", {{"excite", 1}, {"listen", 1}, {"bshape", 5}, {"material", 0.4f}, {"tension", 0.8f}, {"bdecay", 1}, {"hardness", 0.4f}}},
        {"Singing glass", {{"strike", 0}, {"bow", 1}, {"excite", 1}, {"listen", 1}, {"bshape", 2}, {"material", 0.5f}, {"bdecay", 3}, {"pos", 0.5f}, {"bowpress", 0.2f}, {"bowspeed", 0.7f}, {"atk", 0.3f}}},
        {"Bowed metal bar", {{"strike", 0}, {"bow", 1}, {"excite", 1}, {"listen", 1}, {"bshape", 1}, {"material", 1}}},
        {"Blown bell drone", {{"strike", 0}, {"blow", 1}, {"blowtype", 0}, {"excite", 1}, {"listen", 1}, {"bshape", 3}, {"material", 1}, {"atk", 0.5f}}},
        {"Scraped plate", {{"strike", 0}, {"scrape", 1}, {"excite", 1}, {"listen", 1}, {"bshape", 4}, {"material", 1}, {"chatter", 0.5f}}},
        {"Harsh scrape", {{"strike", 0}, {"scrape", 1}, {"rough", 0.9f}, {"grit", 0.8f}, {"chatter", 1}, {"cascade", 0.6f}, {"rattle", 0.4f}, {"rattleth", 0.1f}, {"material", 1}, {"bshape", 4}, {"excite", 1}, {"listen", 1}}},
        {"Sympathetic strings", {{"excite", 0}, {"listen", 0.5f}, {"btune", 0.15f}, {"bshape", 0}, {"couple", 0.4f}, {"mutual", 1}, {"adecay", 4}, {"bdecay", 4}}},
        {"Glass drone", {{"strike", 0}, {"bow", 0.6f}, {"blow", 0.4f}, {"excite", 0.6f}, {"listen", 0.6f}, {"bshape", 2}, {"drift", 1}, {"freeze", 0.5f}, {"atk", 1}, {"rel", 3}}},
    };
    return f;
}
const char* shapeNames[physical::NSHAPES] = {"harmonic", "bar", "glass", "bell", "plate", "membrane"};
}

scPhysicalBox::scPhysicalBox(std::vector<serverManager*> s) : scNode("physicalBox"), servers(std::move(s)) {
    description = "Physical modelling: strike, bow, blow, scrape or feed audio into a waveguide (string to tube) "
                  "and a modal body (bar, glass, bell, plate, membrane), coupled, with rattle, tension and "
                  "cascade, heard by two pickups. Every voice is its own instrument, so LFOs and envelopes "
                  "can move each one separately. Gate is a level (the velocity); In feeds the Ext exciter. "
                  "Gate Mode Pulse plays the voices by itself, from a clock to a Geiger counter, together or each its own. "
                  "Show opens the editor; right-click a control there to publish, modulate or add it to the timeline.";
}
scPhysicalBox::~scPhysicalBox() {
    scheduled.disable();
    listeners.unsubscribeAll();
    for(auto& [s, st] : states) release(s, st);
}

template<class T> void scPhysicalBox::reg(ofParameter<T>& p, std::function<void()> send) {
    Control c;
    c.key = p.getEscapedName(); c.param = &p;
    c.discrete = std::is_same_v<T, bool> || std::is_same_v<T, int>;
    c.add = [this, &p]() { return addParameter(p); };
    c.save = [&p]() { return ofJson(p.get()); };
    c.load = [&p](const ofJson& j) { try { p.set(j.get<T>()); } catch(const std::exception&) {} };
    controls.push_back(std::move(c));
    listeners.push(p.newListener([this, send](T&) { if(!restoring) send(); }));
}

void scPhysicalBox::addSound(const std::string& name, const std::string& sc, float def, float lo, float hi,
                             const std::string& group, const char* fmt, bool log, bool discrete) {
    auto c = std::make_unique<SoundControl>();
    c->p.set(name, def, lo, hi);
    c->sc = sc; c->group = group; c->fmt = fmt; c->log = log; c->discrete = discrete;
    c->voiceInput = sc != "width" && sc != "spread";
    reg(c->p, [this]() { sendAll(); });
    controls.back().discrete = discrete;
    soundControls.push_back(std::move(c));
}

void scPhysicalBox::setup() {
    timeline.setup(this, [this](const std::string& k){ publish(k); }, [this](const std::string& k){ unpublish(k); }, [this](){ return publishedKeys; });
    addParameter(show.set("Show", false));
    addParameter(voices.set("Voices", 4, 1, 16));
    addParameter(channels.set("Channels", 2, 1, 16));
    // Gate (levels: a gate rising to 0.2 plays at 0.2) and Pitch: Oceanode values or audio cables
    gateIn.attach(addParameter(gate.set("Gate", {0.0f}, {0.0f}, {1.0f})), availableInputs, [this](){ refreshOutputs(); });
    pitchIn.attach(addParameter(pitch.set("Pitch", {48.0f}, {0.0f}, {127.0f})), availableInputs, [this](){ refreshOutputs(); });
    addParameter(master.set("Master", 0.6f, 0.0f, 1.0f));
    addParameter(resetLFOs.set("Reset"));
    addInput("In");
    addOutput("Out");
    // Auto: a drone until Gate is connected, timelined or gets a note; Pulse: the automatic gates
    gateMode.set("Gate Mode", 0, 0, 3);
    reg(gateMode, [this](){ sendAll(); });
    dropdowns.set(gateMode.getEscapedName(), {"Auto", "Gated", "Drone", "Pulse"});
    {
        auto toPulse = [this]() { for(auto& [s, st] : states) sendPulse(s, st); };
        pulseSync.set("Pulse Sync", false);                     reg(pulseSync, toPulse);
        pulseRate.set("Pulse Rate", 4.0f, 0.05f, 200.0f);       reg(pulseRate, toPulse);
        pulseBeats.set("Pulse Beats", 0.5f, 0.0625f, 16.0f);    reg(pulseBeats, toPulse);
        pulseChaos.set("Pulse Chaos", 0.0f, 0.0f, 1.0f);        reg(pulseChaos, toPulse);
        pulseCluster.set("Pulse Cluster", 0.0f, 0.0f, 1.0f);    reg(pulseCluster, toPulse);
        pulseChance.set("Pulse Chance", 1.0f, 0.0f, 1.0f);      reg(pulseChance, toPulse);
        pulseLink.set("Pulse Link", 1.0f, 0.0f, 1.0f);          reg(pulseLink, toPulse);
        pulsePhasing.set("Pulse Phasing", 0.0f, 0.0f, 1.0f);    reg(pulsePhasing, toPulse);
        pulseStrum.set("Pulse Strum", 0.0f, -80.0f, 80.0f);     reg(pulseStrum, toPulse);
        pulseLength.set("Pulse Length", 0.5f, 0.01f, 2.0f);     reg(pulseLength, toPulse);
        pulseFixed.set("Pulse Fixed Length", false);            reg(pulseFixed, toPulse);
        pulseTime.set("Pulse Length Time", 0.1f, 0.001f, 8.0f); reg(pulseTime, toPulse);
        pulseJitter.set("Pulse Length Jitter", 0.0f, 0.0f, 1.0f); reg(pulseJitter, toPulse);
        pulseVelocity.set("Pulse Velocity", 0.8f, 0.0f, 1.0f);  reg(pulseVelocity, toPulse);
        pulseVelJitter.set("Pulse Velocity Jitter", 0.0f, 0.0f, 1.0f); reg(pulseVelJitter, toPulse);
        pulseFollow.set("Pulse Follow Gate", false);            reg(pulseFollow, [this](){ sendAll(); });
        // a new seed starts the pattern again (0: a fresh one every start)
        pulseSeed.set("Pulse Seed", 0, 0, 9999);                reg(pulseSeed, [this](){ pulseRestart = true; });
    }
    scheduled.add(gateIn.handle, gate); scheduled.add(pitchIn.handle, pitch);
    scheduled.enable(this, [this](){ for(auto& [s, st] : states) if(st.created) return true; return false; });
    for(auto* p : {&gate, &pitch}) {
        listeners.push(p->newListener([this, p](std::vector<float>& v) {
            // the timeline's frame set of a value it already sent timetagged: nothing to do
            if(scScheduling::isBackendSendSuppressed()) return;
            if(p == &gate) for(float value : v) if(value > 0) gateTouched = true;
            for(auto& [s, st] : states) sendNotes(s, st);
        }));
    }
    listeners.push(voices.newListener([this](int&) { structureDirty = true; }));
    listeners.push(channels.newListener([this](int&) { structureDirty = true; }));
    listeners.push(master.newListener([this](float&) { if(!restoring) sendAll(); }));
    listeners.push(resetLFOs.newListener([this]() { mod.resetLfos(); pulseRestart = true; }));

    // ── Sound: the PhysicalVoice inputs (defaults: PhysicalModes.h) ─────────
    auto d = [](const char* sc) {
        static const std::map<std::string, float> defs = {
#define PHYSICAL_DEFAULT(name, def) {#name, float(def)},
            PHYSICAL_PARAMS(PHYSICAL_DEFAULT)
#undef PHYSICAL_DEFAULT
        };
        return defs.at(sc);
    };
    addSound("Strike", "strike", d("strike"), 0, 1, "Strike");
    addSound("Hardness", "hardness", d("hardness"), 0, 1, "Strike");
    addSound("Mass", "mass", d("mass"), 0, 1, "Strike");
    addSound("Bounce", "bounce", d("bounce"), 0, 0.9f, "Strike");
    addSound("Bow", "bow", d("bow"), 0, 1, "Bow");
    addSound("Bow Pressure", "bowpress", d("bowpress"), 0, 1.2f, "Bow");
    addSound("Bow Speed", "bowspeed", d("bowspeed"), 0, 1, "Bow");
    addSound("Rosin", "rosin", d("rosin"), 0, 1, "Bow");
    addSound("Blow", "blow", d("blow"), 0, 1, "Blow");
    addSound("Breath", "breath", d("breath"), 0, 1.2f, "Blow");
    addSound("Jet Reed", "blowtype", d("blowtype"), 0, 1, "Blow");
    addSound("Jet Ratio", "jetratio", d("jetratio"), 0.1f, 0.8f, "Blow");
    addSound("Turbulence", "turb", d("turb"), 0, 1, "Blow");
    addSound("Scrape", "scrape", d("scrape"), 0, 1, "Scrape");
    addSound("Scrape Speed", "scrapespeed", d("scrapespeed"), 0, 1, "Scrape");
    addSound("Roughness", "rough", d("rough"), 0, 1, "Scrape");
    addSound("Grit", "grit", d("grit"), 0, 1, "Scrape");
    addSound("Chatter", "chatter", d("chatter"), 0, 1, "Scrape");
    addSound("Position", "pos", d("pos"), 0, 1, "Contact");
    addSound("Ext In", "ext", d("ext"), 0, 1, "Contact");
    addSound("Attack", "atk", d("atk"), 0.001f, 4, "Contact", "%.3f s", true);
    addSound("Release", "rel", d("rel"), 0.01f, 8, "Contact", "%.2f s", true);
    addSound("Hold", "hold", d("hold"), 0, 1, "Contact", "%.0f", false, true);
    addSound("A Tune", "atune", d("atune"), -24, 24, "A", "%+.2f st");
    addSound("A Decay", "adecay", d("adecay"), 0.02f, 20, "A", "%.2f s", true);
    addSound("A Brightness", "abright", d("abright"), 0, 1, "A");
    addSound("Bore", "abore", d("abore"), 0, 1, "A");
    addSound("Stiffness", "astiff", d("astiff"), 0, 1, "A");
    addSound("B Tune", "btune", d("btune"), -24, 24, "B", "%+.2f st");
    addSound("B Decay", "bdecay", d("bdecay"), 0.02f, 20, "B", "%.2f s", true);
    addSound("Shape", "bshape", d("bshape"), 0, 5, "B");
    addSound("Stretch", "bstretch", d("bstretch"), -1, 1, "B", "%+.2f");
    addSound("Modes", "bmodes", d("bmodes"), 1, 48, "B", "%.0f");
    addSound("B Brightness", "bbright", d("bbright"), 0, 1, "B");
    addSound("Material", "material", d("material"), 0, 1, "Couple");
    addSound("Excite", "excite", d("excite"), 0, 1, "Couple");
    addSound("Listen", "listen", d("listen"), 0, 1, "Couple");
    addSound("Couple", "couple", d("couple"), 0, 1, "Couple");
    addSound("Mutual", "mutual", d("mutual"), 0, 1, "Couple");
    addSound("Pickup 1", "pick1", d("pick1"), 0, 1, "Couple");
    addSound("Pickup 2", "pick2", d("pick2"), 0, 1, "Couple");
    addSound("Rattle", "rattle", d("rattle"), 0, 1, "Break");
    addSound("Rattle Threshold", "rattleth", d("rattleth"), 0.01f, 1, "Break", "%.3f", true);
    addSound("Tension", "tension", d("tension"), 0, 1, "Break");
    addSound("Cascade", "cascade", d("cascade"), 0, 1, "Break");
    addSound("Drift", "drift", d("drift"), 0, 1, "Life");
    addSound("Drift Rate", "driftrate", d("driftrate"), 0, 1, "Life");
    addSound("Freeze", "freeze", d("freeze"), 0, 1, "Life");
    addSound("Level", "level", 0, -40, 12, "Output", "%+.1f dB");
    addSound("Width", "width", 0.5f, 0, 1, "Output");
    addSound("Spread", "spread", 0.6f, 0, 1, "Output");

    // ── Effects: MixerPro's inserts, then MiVerb ─────────────────────────────
    auto toStages = [this]() { for(auto& [sv, st] : states) if(st.created) updateStages(sv, st); };
    satOn.set("Saturation On", false);                      reg(satOn, toStages);
    satDrive.set("Saturation Drive", 6.0f, 0.0f, 36.0f);    reg(satDrive, [this](){ sendStageAll(StSat); });
    satBias.set("Saturation Bias", 0.0f, 0.0f, 1.0f);       reg(satBias, [this](){ sendStageAll(StSat); });
    satTone.set("Saturation Tone", 20000.0f, 500.0f, 20000.0f); reg(satTone, [this](){ sendStageAll(StSat); });
    satOutput.set("Saturation Output", 0.0f, -24.0f, 12.0f); reg(satOutput, [this](){ sendStageAll(StSat); });
    satMix.set("Saturation Mix", 1.0f, 0.0f, 1.0f);         reg(satMix, [this](){ sendStageAll(StSat); });
    eqOn.set("EQ On", false);                                reg(eqOn, toStages);
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
    // Space: MiVerb, on by default (the prototype's room)
    spaceOn.set("Space On", true);                   reg(spaceOn, toStages);
    spaceMix.set("Space Mix", 0.25f, 0.0f, 1.0f);    reg(spaceMix, [this](){ sendStageAll(StSpace); });
    spaceTime.set("Space Time", 0.8f, 0.0f, 1.2f);   reg(spaceTime, [this](){ sendStageAll(StSpace); });
    spaceDamp.set("Space Damp", 0.5f, 0.0f, 1.0f);   reg(spaceDamp, [this](){ sendStageAll(StSpace); });
    spaceDiff.set("Space Diffusion", 0.7f, 0.0f, 1.0f); reg(spaceDiff, [this](){ sendStageAll(StSpace); });
    spaceLowCut.set("Space Low Cut", 0.05f, 0.0f, 1.0f); reg(spaceLowCut, [this](){ sendStageAll(StSpace); });
    spaceFreeze.set("Space Freeze", false);          reg(spaceFreeze, [this](){ sendStageAll(StSpace); });

    snaps.setupParameters();
    for(auto* p : {&snaps.snapshotP, &snaps.morphAP, &snaps.morphBP, &snaps.morphCurveP, &snaps.morphSwitchP}) reg(*p, [](){});
    for(auto* p : {&snaps.morphP, &snaps.morphTimeP}) reg(*p, [](){});
    for(auto* p : {&snaps.morphBeatsP, &snaps.morphGlideP}) reg(*p, [](){});
    scModulation::Host h;
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
    mod.setup(h, {{"Pitch", 24, "st"}, {"Material", 1, ""}, {"Excite", 1, ""}, {"Position", 1, ""},
        {"Hardness", 1, ""}, {"Bow Pressure", 1, ""}, {"Bow Speed", 1, ""}, {"Breath", 1, ""},
        {"Scrape Speed", 1, ""}, {"Brightness", 1, ""}, {"Decay", 4, "oct"}, {"Shape", 5, ""},
        {"Couple", 1, ""}, {"Cascade", 1, ""}, {"Amp", 1, ""}, {"Pan", 1, ""}}, true, true, true);
    mod.lfoCount = mod.envCount = mod.extCount = 0;
    {
        static const std::map<std::string, int> targets = {{"material", Material}, {"excite", Excite}, {"pos", Position},
            {"hardness", Hardness}, {"bowpress", BowPressure}, {"bowspeed", BowSpeed}, {"breath", Breath},
            {"scrapespeed", ScrapeSpeed}, {"abright", Brightness}, {"bbright", Brightness}, {"adecay", Decay},
            {"bdecay", Decay}, {"bshape", Shape}, {"couple", Couple}, {"cascade", Cascade}};
        for(const auto& c : soundControls) {
            auto it = targets.find(c->sc);
            if(it != targets.end()) mod.targetOfKey[c->p.getEscapedName()] = it->second;
        }
    }
    mod.changed = [this](){ if(!restoring) sendAll(); };
    initial = capture();
    scSnapshots::Host sh;
    sh.folder = "PhysicalBox"; sh.title = "PhysicalBox";
    sh.capture = [this](){ return capture(); };
    sh.apply = [this](const ofJson& a, const ofJson& b, float t, bool sw){ apply(a, b, t, sw); };
    sh.factory = [this](){ return factory(); };
    sh.bpm = [this](){ return bpm; };
    sh.paramMenu = [this](const std::string& k){ mark(k); menu(k); };
    snaps.setup(sh);
    requireSynthdefs();
}

// ── Server side ──────────────────────────────────────────────────────────────
void scPhysicalBox::sendNotes(ofxSCServer* server, State& st) {
    if(!st.gate) return;
    auto now = scheduled.inEffectNow();   // not a scheduled send: the notes as they stand now
    const int n = st.builtVoices;
    // Pulse: ModGate takes the generator's gates, Gate (Follow Gate) holds its voices
    const bool pulse = st.pulse && st.pulseBus;
    std::vector<float> g(n), ones(n, 1.0f);
    for(int i = 0; i < n; ++i) g[i] = pulse ? 0.0f : first(gate.get(), i, 0);
    st.gate->set("gate", g);
    st.gate->set("velocity", ones);       // the gate's level is the velocity
    st.gate->set("out", st.gates->index);
    st.gate->set("vel", mod.sourceBus(server, scModulation::SRC_VEL));
    st.gate->set("rnd", mod.sourceBus(server, scModulation::SRC_RND));
    st.droneSent = droning() ? 1 : 0;
    st.gate->set("drone", float(st.droneSent));
    const int gbus = gateIn.bus(server);
    st.gate->set("gate_sel", gbus >= 0 || pulse ? 1.0f : 0.0f);
    if(pulse) st.gate->mapan("gate_ar", st.pulseBus->index, n);
    else if(gbus >= 0) st.gate->mapan("gate_ar", gbus, n);
    if(pulse) {
        std::vector<float> held(16, 0.0f);
        for(int i = 0; i < n; ++i) held[i] = first(gate.get(), i, 0);
        st.pulse->set("enable", held);
        st.pulse->set("free", pulseFollow.get() ? 0.0f : 1.0f);
        st.pulse->set("enable_sel", gbus >= 0 ? 1.0f : 0.0f);
        if(gbus >= 0) st.pulse->mapan("enable_ar", gbus, n);
    }
    const int bus = pitchIn.bus(server);
    for(int i = 0; i < n; ++i) if(auto* v = st.voices[i]) {
        v->set("pitch", first(pitch.get(), i, 48));
        v->set("pitch_sel", bus >= 0 ? 1.0f : 0.0f);
        if(bus >= 0) v->mapan("pitch_ar", bus + i, 1);
    }
}

void scPhysicalBox::sendSound(ofxSCServer* server, State& st) {
    if(!st.created) return;
    static const char* names[NUM_TARGETS - 1] = {"mpitch", "mmat", "mexc", "mpos", "mhard", "mpress", "mspeed",
        "mbreath", "mscrape", "mbright", "mdecay", "mshape", "mcouple", "mcascade", "mamp"};
    bool vca = false;
    for(int e = 0; e < mod.envCount; ++e) if(std::abs(mod.matrix[(scModulation::SRC_ENV + e) * NUM_TARGETS + Amp]) > 0.0001f) vca = true;
    const float comp = 1.0f / std::sqrt(std::max(1.0f, float(st.builtVoices) / st.builtChannels));
    const int inBus = st.in >= 0 ? st.in : st.silent->index;
    for(int i = 0; i < st.builtVoices; ++i) if(auto* v = st.voices[i]) {
        auto set = [&](const std::string& name, float value) {
            auto& sent = st.sent[i]; auto it = sent.find(name);
            if(it != sent.end() && it->second == value) return;
            sent[name] = value; v->set(name, value);
        };
        for(const auto& c : soundControls) if(c->voiceInput) set(c->sc, c->sc == "bmodes" ? std::round(c->p.get()) : c->p.get());
        set("gatebus", st.gates->index + i);
        set("out", st.audio->index + 2 * i);
        set("in", inBus + (st.in >= 0 ? i % std::max(1, st.inChannels) : 0));
        set("voice", i);
        set("master", master.get());
        set("normalization", 1.0f / (st.builtVoices * comp));
        set("vca", vca ? 1.0f : 0.0f);
        set("vis", st.vis->index);
        for(int t = 0; t < NUM_TARGETS - 1; ++t) set(names[t], mod.targetBus(server, t) + i);
    }
    if(st.spread) {
        st.spread->set("in", st.audio->index);
        st.spread->set("voices", st.builtVoices);
        const float spread = soundParam("spread").get();
        std::vector<float> pans(16, 0.5f);
        for(int i = 0; i < st.builtVoices; ++i)
            pans[i] = ofClamp(0.5f + (st.builtVoices > 1 ? (float(i) / (st.builtVoices - 1) - 0.5f) * spread : 0), 0.0f, 1.0f);
        st.spread->set("pan", pans);
        st.spread->set("width", soundParam("width").get());
        st.spread->set("mpan", mod.targetBus(server, Pan));
        st.spread->set("vis", st.vis->index + VIS_PAN);
    }
    updateStages(server, st);
}
void scPhysicalBox::sendAll() { for(auto& [s, st] : states) { sendNotes(s, st); sendSound(s, st); sendPulse(s, st); } }

// ── Pulse: the automatic gates ───────────────────────────────────────────────
float scPhysicalBox::pulseHz() const {
    if(pulseSync.get()) return bpm / 60.0f / std::max(0.0625f, pulseBeats.get());
    return std::max(0.01f, pulseRate.get());
}
void scPhysicalBox::sendPulse(ofxSCServer* server, State& st) {
    if(!st.pulse) return;
    scSynthControlBatch c(st.pulse, server);
    c.set("voices", float(st.builtVoices));
    c.set("rate", pulseHz());
    c.set("chaos", pulseChaos.get()); c.set("cluster", pulseCluster.get());
    c.set("chance", pulseChance.get()); c.set("link", pulseLink.get());
    c.set("phasing", pulsePhasing.get()); c.set("strum", pulseStrum.get() * 0.001f);
    c.set("length", pulseLength.get()); c.set("lengthsec", pulseTime.get());
    c.set("fixed", pulseFixed.get() ? 1.0f : 0.0f); c.set("jitter", pulseJitter.get());
    c.set("velocity", pulseVelocity.get()); c.set("veljitter", pulseVelJitter.get());
    c.send();
}
// a new generator (Seed > 0: its pattern from the start) at the head of the gate
// group, and a fresh ModGate after it: a level held from Oceanode gate values
// played before would otherwise floor every pulse's velocity
void scPhysicalBox::startPulse(ofxSCServer* server, State& st) {
    if(!st.created || !st.groups[SlGate] || !liveBus(server, st.vis, false)) return;
    if(!liveBus(server, st.pulseBus, true)) {
        dropBus(server, st.pulseBus, true);
        st.pulseBus = new ofxSCBus(RATE_AUDIO, 16, server);
        if(!liveBus(server, st.pulseBus, true)) { st.pulseBus->free(); delete st.pulseBus; st.pulseBus = nullptr; return; }
    }
    stopPulse(st);
    st.pulse = new ofxSCSynth("PhysicalBoxPulse", server);
    st.pulse->set("seed", pulseSeed.get() > 0 ? pulseSeed.get() : int(ofRandom(1, 99999)));
    st.pulse->set("out", st.pulseBus->index);
    st.pulse->set("vis", st.vis->index + VIS_PULSE);
    sendPulse(server, st);
    st.pulse->createAndRun(0, st.groups[SlGate]->nodeID, true);
    pulseCounts.fill(0);
    for(auto& events : pulseEvents) events.clear();
    if(st.gate) {
        st.gate->free(); delete st.gate;
        st.gate = new ofxSCSynth("ModGate" + ofToString(st.builtVoices), server);
        sendNotes(server, st);
        st.gate->createAndRun(1, st.groups[SlGate]->nodeID, true);
    }
}
void scPhysicalBox::stopPulse(State& st) {
    if(!st.pulse) return;
    st.pulse->free(); delete st.pulse; st.pulse = nullptr;
}
// the characters: a starting point each (Rate stays the user's)
void scPhysicalBox::pulseShape(int shape) {
    struct Shape { float chaos, cluster, chance, link, phasing, strum, length, jitter, veljitter; };
    static const Shape shapes[] = {
        {0, 0, 1, 1, 0, 0, 0.5f, 0, 0},              // Clock
        {0, 0, 1, 1, 0, 30, 0.8f, 0.1f, 0.25f},      // Strum
        {0, 0, 1, 0, 0.03f, 0, 0.3f, 0, 0.1f},       // Phasing
        {1, 0, 1, 0, 0, 0, 0.15f, 0.5f, 0.7f},       // Rain
        {1, 0.6f, 1, 0, 0, 0, 0.05f, 0.3f, 0.3f},    // Geiger
        {0.6f, 0.2f, 0.85f, 0.35f, 0.2f, 0, 0.4f, 0.6f, 0.5f},   // Swarm
        {0.15f, 1, 1, 1, 0, 0, 0.3f, 0.2f, 0.35f},   // Bursts
    };
    const auto& x = shapes[std::clamp(shape, 0, int(std::size(shapes)) - 1)];
    restoring = true;
    pulseChaos = x.chaos; pulseCluster = x.cluster; pulseChance = x.chance; pulseLink = x.link;
    pulsePhasing = x.phasing; pulseStrum = x.strum; pulseLength = x.length; pulseFixed = false;
    pulseJitter = x.jitter; pulseVelJitter = x.veljitter;
    restoring = false;
    for(auto& [s, st] : states) sendPulse(s, st);
}
// the generator's event counts since the last read: new events into the raster
void scPhysicalBox::readPulse() {
    const float now = ofGetElapsedTimef(), window = 6.0f;
    if(live.size() >= size_t(VIS_SIZE)) {
        const float since = std::clamp(now - pulseReadAt, 0.0f, 0.25f);
        for(int v = 0; v < 16; ++v) {
            const float count = live[VIS_PULSE + v];
            if(count < pulseCounts[v]) pulseCounts[v] = count;      // a new generator
            const int fresh = std::min(16, int(count - pulseCounts[v]));
            pulseCounts[v] = count;
            for(int k = 0; k < fresh; ++k)
                pulseEvents[v].push_back({now - since * (1 - float(k + 1) / fresh), live[VIS_PULSE + 16 + v], live[VIS_PULSE + 32 + v]});
        }
    }
    pulseReadAt = now;
    for(auto& events : pulseEvents)
        events.erase(events.begin(), std::find_if(events.begin(), events.end(), [&](const PulseEvent& e){ return e.t + e.dur > now - window; }));
}

void scPhysicalBox::buildSynth(ofxSCServer* server) {
    if(!server) return;
    auto& st = states[server]; release(server, st); st.root = new ofxSCGroup(server);
}
void scPhysicalBox::createSynth(ofxSCServer* server) {
    auto it = states.find(server); if(it == states.end() || !it->second.root) return;
    auto& st = it->second;
    if(placeBefore >= 0) st.root->create(2, placeBefore); else st.root->create(0, 1);
    for(auto*& g : st.groups) { g = new ofxSCGroup(server); g->create(1, st.root->nodeID); }
    st.gates = new ofxSCBus(RATE_AUDIO, 16, server);
    st.audio = new ofxSCBus(RATE_AUDIO, 32, server);
    st.silent = new ofxSCBus(RATE_AUDIO, 1, server);
    st.vis = new ofxSCBus(RATE_CONTROL, VIS_SIZE, server);
    if(!liveBus(server, st.gates, true) || !liveBus(server, st.audio, true) || !liveBus(server, st.silent, true) || !liveBus(server, st.vis, false)) {
        release(server, st); return;
    }
    st.builtVoices = voices.get(); st.builtChannels = channels.get(); st.created = true;
    mod.create(server, st.groups[SlSrc]->nodeID, st.groups[SlRoute]->nodeID, st.gates->index);
    if(pulsing()) startPulse(server, st);
    st.gate = new ofxSCSynth("ModGate" + ofToString(st.builtVoices), server);
    for(int i = 0; i < st.builtVoices; ++i) st.voices[i] = new ofxSCSynth("PhysicalBoxVoice", server);
    st.spread = new ofxSCSynth("PhysicalBoxSpread" + ofToString(st.builtChannels), server);
    sendNotes(server, st); sendSound(server, st);
    st.gate->createAndRun(1, st.groups[SlGate]->nodeID, true);
    for(int i = 0; i < st.builtVoices; ++i) st.voices[i]->createAndRun(1, st.groups[SlVoice]->nodeID, true);
    st.spread->createAndRun(1, st.groups[SlSpread]->nodeID, true);
    routeChain(st);
    if(!getActive()) st.root->run(false);
}
void scPhysicalBox::release(ofxSCServer* server, State& st) {
    if(st.created && st.root) st.root->free();
    if(st.created) mod.free(server, false);
    for(auto& f : st.fx) { delete f.synth; f.synth = nullptr; dropBus(server, f.in, true); f.running = false; f.offAt = -1; }
    delete st.gate; st.gate = nullptr; delete st.spread; st.spread = nullptr;
    delete st.pulse; st.pulse = nullptr; dropBus(server, st.pulseBus, true);
    for(auto*& v : st.voices) { delete v; v = nullptr; }
    for(auto& cache : st.sent) cache.clear();
    for(auto*& g : st.groups) { delete g; g = nullptr; }
    delete st.root; st.root = nullptr; st.created = false; st.droneSent = -1;
    dropBus(server, st.gates, true); dropBus(server, st.audio, true); dropBus(server, st.silent, true); dropBus(server, st.vis, false);
}
void scPhysicalBox::free(ofxSCServer* s) { auto it = states.find(s); if(it != states.end()) { release(s, it->second); it->second.out = -1; } }
// A new voice / speaker count: everything again, but in the old root's place
// (the graph is not recomputed for the same topology: a root at the head would
// run before the nodes feeding Gate / Pitch / In and read silence).
void scPhysicalBox::rebuild() {
    structureDirty = false; mod.voicesChanged();
    for(auto& [s, st] : states) if(st.created) {
        const bool wasLatency = s->getBLatency(); s->setBLatency(true);
        ofxSCGroup* old = st.root; st.root = nullptr;
        release(s, st);
        st.root = new ofxSCGroup(s);
        placeBefore = old->nodeID; createSynth(s); placeBefore = -1;
        old->free(); delete old;
        s->setBLatency(wasLatency);
    }
    refreshOutputs();
}
void scPhysicalBox::moveSynthBefore(ofxSCServer* s, int id) { auto it = states.find(s); if(it != states.end() && it->second.created) it->second.root->moveBefore(id); }
int scPhysicalBox::getLastSynthID(ofxSCServer* s) { auto it = states.find(s); return it != states.end() && it->second.created ? it->second.root->nodeID : -1; }
void scPhysicalBox::setOutputBus(ofxSCServer* s, int, int bus) { auto& st = states[s]; st.out = bus; if(st.created) routeChain(st); }
int scPhysicalBox::getOutputBusIndex(ofxSCServer* s, int) { auto it = states.find(s); return it == states.end() ? -1 : it->second.out; }
// a Gate / Pitch cable (told apart by its source's bus), else the audio input
void scPhysicalBox::setInputBus(ofxSCServer* s, scNode* node, int bus) {
    auto& st = states[s];
    if(gateIn.take(s, node, bus) | pitchIn.take(s, node, bus)) { sendNotes(s, st); return; }
    st.in = bus;
    st.inChannels = node ? std::clamp(node->getOutputNumChannels(0), 1, 16) : 1;
    for(auto& cache : st.sent) cache.erase("in");
    sendSound(s, st);
}
void scPhysicalBox::resetInputBusses(ofxSCServer* s, int) {
    gateIn.reset(s); pitchIn.reset(s);
    auto& st = states[s];
    st.in = -1; st.inChannels = 1;     // the voices read their own silent bus
    for(auto& cache : st.sent) cache.erase("in");
    sendNotes(s, st); sendSound(s, st);
}
void scPhysicalBox::activate() { for(auto& [s, st] : states) if(st.created) st.root->run(true); }
void scPhysicalBox::deactivate() { for(auto& [s, st] : states) if(st.created) st.root->run(false); }
void scPhysicalBox::setBpm(float v) { bpm = std::max(1.0f, v); mod.setBpm(bpm); sendAll(); }
void scPhysicalBox::resendParametersForNRT() { for(auto& [s, st] : states) for(auto& cache : st.sent) cache.clear(); mod.resend(); sendAll(); }
#ifdef OFXOCEANODE_HAS_TIMELINE_INSTRUMENT
bool scPhysicalBox::getTimelineInstrument(ofxOceanodeTimelineInstrument& out) {
    if(!gateIn.handle || !pitchIn.handle) return false;
    out.gate = gateIn.handle.get(); out.pitch = pitchIn.handle.get(); out.voices = voices.get();
    return true;
}
#endif

// ── Effects ──────────────────────────────────────────────────────────────────
void scPhysicalBox::updateStages(ofxSCServer* server, State& st) {
    if(!st.created) return;
    for(int s = 0; s < NumStages; s++){
        auto& f = st.fx[s];
        if(stageOn(s)){
            if(!liveBus(server, f.in, true)){
                delete f.in;
                f.in = new ofxSCBus(RATE_AUDIO, channels.get(), server);
                if(!liveBus(server, f.in, true)){ f.in->free(); delete f.in; f.in = nullptr; continue; }
                if(f.synth) f.synth->set("in", f.in->index);
            }
            if(!f.synth){
                f.synth = new ofxSCSynth(stageDef(s), server);
                f.synth->set("in", f.in->index);
                f.synth->set("out", std::max(0, st.out));
                sendStage(s, f.synth, server);
                f.synth->createAndRun(1, st.groups[SlFx + s]->nodeID, true);
            }else{
                sendStage(s, f.synth, server);
                if(!f.running) f.synth->run(true);
            }
            f.running = true;
            f.offAt = -1.0f;
        }else if(f.synth && f.running && f.offAt < 0.0f){
            if(s != StEq) f.synth->set("on", 0.0f);
            f.offAt = ofGetElapsedTimef() + stageTail(s);
        }
    }
    routeChain(st);
}
// the spreader -> the running effects -> out
void scPhysicalBox::routeChain(State& st) {
    int next = std::max(0, st.out);
    for(int s = NumStages - 1; s >= 0; s--){
        auto& f = st.fx[s];
        if(!f.synth || !f.running || !f.in) continue;
        f.synth->set("out", next);
        next = f.in->index;
    }
    if(st.spread) st.spread->set("out", next);
}
bool scPhysicalBox::stageOn(int s) const {
    switch(s){
        case StSat: return satOn.get();
        case StEq: return eqOn.get();
        case StEcho: return echoOn.get();
        case StRvb: return rvbOn.get();
        default: return spaceOn.get();
    }
}
std::string scPhysicalBox::stageDef(int s) const {
    static const char* names[NumStages] = {"mixerProSat", "mixerProEQ", "mixerProEcho", "mixerProRvb", "PhysicalBoxSpace"};
    return names[s] + ofToString(channels.get());
}
float scPhysicalBox::echoDelaySec() const {
    if(!echoBeats.get()) return ofClamp(echoDelay.get(), 0.005f, 4.0f);
    return ofClamp(echoBeatVal.get() * 60.0f / std::max(1.0f, bpm), 0.005f, 4.0f);
}
float scPhysicalBox::stageTail(int s) const {
    if(s == StEq || s == StSat) return 0.02f;
    if(s == StEcho){
        const float feed = ofClamp(echoFeed.get(), 0.0f, 0.98f);
        const float repeats = feed > 0.001f ? std::log(0.001f) / std::log(feed) : 0.0f;
        return std::min(30.0f, echoDelaySec() * (repeats + 1.0f) + 0.2f);
    }
    if(s == StSpace) return spaceFreeze.get() ? 0.1f : 12.0f;
    return std::min(30.0f, rvbDecay.get() + rvbPredelay.get() * 0.001f + 0.3f);
}
void scPhysicalBox::sendStage(int s, ofxSCSynth* x, ofxSCServer* server) {
    if(!x) return;
    scSynthControlBatch c(x, server);
    const float on = stageOn(s) ? 1.0f : 0.0f;
    if(s == StSat){
        c.set("drive", satDrive.get()); c.set("bias", satBias.get()); c.set("tone", satTone.get());
        c.set("autogain", 1.0f); c.set("output", satOutput.get()); c.set("mix", satMix.get()); c.set("on", on);
    }else if(s == StEq){
        // mixerProEQ: gains dB, freqs Hz, shelves bNslope (= 1/S), peaks bNrq (= 1/Q)
        for(int b = 0; b < 5; b++){
            const std::string k = "b" + ofToString(b + 1);
            c.set(k + "gain", eqGain[b].get());
            c.set(k + "freq", eqFreq[b].get());
            if(scEQEditor::isShelf(b)) c.set(k + "slope", 1.0f / scEQEditor::safeShelfSlope(eqShape[b].get(), eqGain[b].get()));
            else c.set(k + "rq", 1.0f / std::max(0.01f, eqShape[b].get()));
        }
    }else if(s == StEcho){
        c.set("delay", echoDelaySec());
        c.set("feed", ofClamp(echoFeed.get(), 0.0f, 0.98f));
        c.set("cutoff", ofClamp(echoCutoff.get(), 12.0f, 130.0f));
        c.set("resonance", ofClamp(echoResonance.get(), 0.0f, 0.9f));
        c.set("filtertype", (float)ofClamp(echoFilter.get(), 0, 3));
        c.set("pingpong", ofClamp(echoPingPong.get(), 0.0f, 1.0f));
        c.set("mix", ofClamp(echoMix.get(), 0.0f, 1.0f));
        c.set("on", on);
    }else if(s == StRvb){
        c.set("mix", ofClamp(rvbMix.get(), 0.0f, 1.0f));
        c.set("decay", std::max(0.05f, rvbDecay.get()));
        c.set("size", ofClamp(rvbSize.get(), 0.0f, 60.0f));
        c.set("predelay", ofClamp(rvbPredelay.get(), 0.0f, 500.0f));
        c.set("position", ofClamp(rvbLate.get(), 0.0f, 1.0f));
        c.set("spread", ofClamp(rvbSpread.get(), -1.0f, 1.0f));
        c.set("lowpass", ofClamp(rvbLowpass.get(), 20.0f, 20000.0f));
        c.set("highdamp", ofClamp(rvbHiDamp.get(), 0.0f, 60.0f));
        c.set("lowdamp", ofClamp(rvbLoDamp.get(), 0.0f, 60.0f));
        c.set("modfreq", ofClamp(rvbModRate.get(), 0.001f, 20.0f));
        c.set("spin", ofClamp(rvbSpin.get(), 0.0f, 20.0f));
        c.set("wander", ofClamp(rvbWander.get(), 0.0f, 20.0f));
        c.set("on", on);
    }else{
        c.set("mix", spaceMix.get()); c.set("time", spaceTime.get()); c.set("damp", spaceDamp.get());
        c.set("diff", spaceDiff.get()); c.set("hp", spaceLowCut.get()); c.set("freeze", spaceFreeze.get() ? 1.0f : 0.0f);
        c.set("on", on);
    }
    c.send();
}
void scPhysicalBox::sendStageAll(int stage) {
    for(auto& [server, st] : states) if(st.created && st.fx[stage].synth) sendStage(stage, st.fx[stage].synth, server);
}

// ── Publishing, snapshots, presets ───────────────────────────────────────────
bool scPhysicalBox::connected(const std::string& k) const {
    auto it = published.find(k); return it != published.end() && (it->second->hasInConnection() || it->second->hasOutConnections());
}
void scPhysicalBox::publish(const std::string& k) {
    if(published.count(k) || getParameterGroup().contains(k)) return;
    auto it = std::find_if(controls.begin(), controls.end(), [&](const Control& c){ return c.key == k; }); if(it == controls.end()) return;
    if(!separator){ addSeparator("Published", ofColor(200)); separator = true; }
    published[k] = it->add(); dropdowns.apply(k, published[k]); publishedKeys.push_back(k); parameterGroupChanged.notify(this);
}
void scPhysicalBox::unpublish(const std::string& k) {
    auto it = published.find(k); if(it == published.end() || connected(k)) return;
    removeParameter(it->second->getName()); published.erase(it);
    publishedKeys.erase(std::remove(publishedKeys.begin(), publishedKeys.end(), k), publishedKeys.end());
    if(published.empty() && separator){ removeSeparator("Published"); separator = false; }
    parameterGroupChanged.notify(this);
}
void scPhysicalBox::mark(const std::string& k) {
    timeline.mark(k);
    if(published.count(k) && !timeline.isHidden(k)) {
        auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y), b, IM_COL32(106, 212, 196, 255), 2);
    }
}
void scPhysicalBox::menu(const std::string& k) {
    if(!ImGui::BeginPopupContextItem(("##phys" + k).c_str())) return;
    auto it = std::find_if(controls.begin(), controls.end(), [&](const Control& c){ return c.key == k; });
    if(it != controls.end()) scTyped::jsonField(k, it->save, it->load);
    if(timeline.isHidden(k)){ if(ImGui::MenuItem("Publish to Node GUI")) timeline.reveal(k); }
    else if(published.count(k)){
        if(connected(k)) ImGui::TextDisabled("Disconnect it to unpublish");
        else if(ImGui::MenuItem("Unpublish from Node")) timeline.unpublish(k);
    }else if(ImGui::MenuItem("Publish to Node GUI")) publish(k);
    timeline.menuItems(k); mod.modulateMenu(k); ImGui::EndPopup();
}
ofJson scPhysicalBox::capture() const {
    ofJson j = ofJson::object();
    for(const auto& c : controls) if(!scSnapshots::isOwnKey(c.key)) j[c.key] = c.save();
    mod.patchBases(j); mod.save(j, "physicalMod"); return j;
}
void scPhysicalBox::apply(const ofJson& a, const ofJson& b, float t, bool sw) {
    restoring = true;
    for(const auto& c : controls) if(!scSnapshots::isOwnKey(c.key) && !(published.count(c.key) && published.at(c.key)->hasInConnection())) {
        const auto& av = a.contains(c.key) ? a[c.key] : initial[c.key];
        const auto& bv = b.contains(c.key) ? b[c.key] : initial[c.key];
        c.load(blend(av, bv, t, sw, c.discrete));
    }
    const auto& src = sw ? b : a;
    if(modSource != &src || t >= 1) { modSource = &src; mod.load(src, "physicalMod"); mod.afterLoad(); }
    auto count = [&](const char* key, int max) {
        const int av = a.value(key, 0), bv = b.value(key, 0);
        return std::clamp(t <= 0 ? av : t >= 1 ? bv : std::max(av, bv), 0, max);
    };
    const int nl = count("physicalModLfos", scModulation::MAX_LFOS), ne = count("physicalModEnvs", scModulation::MAX_ENVS),
              nx = count("physicalModExts", scModulation::MAX_EXT);
    const bool counts = nl != mod.lfoCount || ne != mod.envCount || nx != mod.extCount;
    mod.lfoCount = nl; mod.envCount = ne; mod.extCount = nx;
    auto ma = a.value("physicalModMatrix", std::vector<float>(scModulation::NUM_SOURCES * NUM_TARGETS, 0));
    auto mb = b.value("physicalModMatrix", std::vector<float>(scModulation::NUM_SOURCES * NUM_TARGETS, 0));
    ma.resize(mod.matrix.size(), 0); mb.resize(mod.matrix.size(), 0);
    bool dirty = false;
    for(size_t i = 0; i < mod.matrix.size(); ++i) { const float value = ofLerp(ma[i], mb[i], t); if(value != mod.matrix[i]) { mod.matrix[i] = value; dirty = true; } }
    if(counts) mod.refresh(); else if(dirty) mod.sendMatrix();
    restoring = false; sendAll();
}
std::vector<ofJson> scPhysicalBox::factory() const {
    std::vector<ofJson> out;
    for(const auto& f : factorySounds()) {
        ofJson j = initial; j["name"] = f.name;
        for(const auto& [sc, value] : f.values)
            for(const auto& c : soundControls) if(c->sc == sc) j[c->p.getEscapedName()] = value;
        out.push_back(j);
    }
    return out;
}
void scPhysicalBox::presetSave(ofJson& j) {
    j["physicalBox"] = capture();
    // Snapshot controls belong in a patch, but never inside a sound snapshot.
    for(const auto& c : controls) if(scSnapshots::isOwnKey(c.key)) j["physicalBox"][c.key] = c.save();
    j["physicalBoxPublished"] = publishedKeys; timeline.save(j, "physicalBoxTimeline");
}
void scPhysicalBox::loadBeforeConnections(ofJson& j) {
    std::vector<std::string> wanted;
    if(j.contains("physicalBoxPublished") && j["physicalBoxPublished"].is_array())
        for(const auto& k : j["physicalBoxPublished"]) if(k.is_string()) wanted.push_back(k.get<std::string>());
    const auto old = publishedKeys; for(const auto& k : old) if(std::find(wanted.begin(), wanted.end(), k) == wanted.end()) unpublish(k);
    for(const auto& k : wanted) publish(k); timeline.load(j, "physicalBoxTimeline");
}
void scPhysicalBox::presetRecallAfterSettingParameters(ofJson& j) {
    if(!j.contains("physicalBox")) return;
    auto state = j["physicalBox"];
    // a loaded patch starts from Auto's own rule: droning unless Gate is
    // connected or timelined (a value it gets from here on switches it to gated)
    gateTouched = false;
    apply(state, state, 1, true);
    restoring = true; for(const auto& c : controls) if(scSnapshots::isOwnKey(c.key) && state.contains(c.key)) c.load(state[c.key]); restoring = false;
}
void scPhysicalBox::requireSynthdefs() {
    for(const auto& entry : std::vector<std::pair<std::string, std::string>>{
        {"PhysicalBoxVoice.scsyndef", "PhysicalBox"}, {"ModGate1.scsyndef", "Modulation"}, {"mixerProEQ2.scsyndef", "MixerPro"}}) {
        std::filesystem::path folder;
        try {
            const std::filesystem::path data = ofToDataPath(SYNTHDEF_DIRECTORY, true);
            if(std::filesystem::exists(data)) for(const auto& f : std::filesystem::recursive_directory_iterator(data, std::filesystem::directory_options::skip_permission_denied))
                if(f.is_regular_file() && f.path().filename() == entry.first) { folder = f.path().parent_path(); break; }
            if(folder.empty()) folder = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "synthdefs/Defaults" / entry.second / "CompiledSynthdefs" / (entry.second == "MixerPro" ? "mixerpro" : entry.second);
            if(!std::filesystem::exists(folder / entry.first)) { ofLogError("physicalBox") << "Compile synthdefs/Defaults/" << entry.second << " first"; continue; }
            for(auto* sm : servers) if(sm) sm->requireSynthdefFolder(folder.string());
        } catch(const std::exception& e) { ofLogError("physicalBox") << e.what(); }
    }
}
void scPhysicalBox::update(ofEventArgs&) {
    timeline.update(); snaps.update();
    {   // the snapshot names, to a published Snapshot / Morph A / B dropdown
        const auto names = snaps.slotNames();
        for(const auto& k : snaps.slotKeys()) dropdowns.set(k, names, published.count(k) ? published[k] : nullptr);
    }
    if(structureDirty) rebuild();
    for(auto& [server, st] : states) if(st.created && st.droneSent != (droning() ? 1 : 0)) sendNotes(server, st);
    // Pulse on / off / again (a new Seed, Reset)
    for(auto& [server, st] : states) if(st.created) {
        const bool want = pulsing();
        if(want && (!st.pulse || pulseRestart)) startPulse(server, st);
        else if(!want && st.pulse) { stopPulse(st); sendNotes(server, st); }
    }
    pulseRestart = false;
    mod.showing = show.get(); mod.update();
    const float now = ofGetElapsedTimef();
    for(auto& [server, st] : states) if(st.created) {
        for(auto& f : st.fx) {
            if(f.offAt < 0 || now < f.offAt) continue;
            f.offAt = -1; f.running = false; routeChain(st); if(f.synth) f.synth->run(false);
        }
    }
    if(!show.get()) return;
    for(auto& [s, st] : states) if(st.created && liveBus(s, st.vis, false)) {
        live = st.vis->readValues;
        if(now >= nextRead) { nextRead = now + 1.0f / (pulsing() ? 60 : 25); s->requestControlValues(st.vis->index, VIS_SIZE); }
        break;
    }
    if(pulsing()) readPulse();
}
bool scPhysicalBox::droning() const {
    if(pulsing()) return false;
    if(gateMode.get() == 2) return true;
    if(gateMode.get() == 1) return false;
    return !(gateTouched || gateIn.connected() || (gateIn.handle && gateIn.handle->isTimelined()));
}

// ── Window ───────────────────────────────────────────────────────────────────
scPhysicalBox::SoundControl* scPhysicalBox::sound(const std::string& sc) {
    for(auto& c : soundControls) if(c->sc == sc) return c.get();
    return nullptr;
}
ofParameter<float>& scPhysicalBox::soundParam(const std::string& sc) { return sound(sc)->p; }

void scPhysicalBox::drawWindow() {
    const float z = zoom();
    ImGui::SetNextWindowSize(ImVec2(1180 * z, 860 * z), ImGuiCond_FirstUseEver);
    bool open = show.get();
    if(!ImGui::Begin(getWindowTitle().c_str(), &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End(); if(!open) show = false; return;
    }
    if(!open) { show = false; ImGui::End(); return; }
    const float w = ImGui::GetContentRegionAvail().x;
    drawHeader();
    snaps.drawBar(w);
    drawDisplay(w);
    if(pulsing()) drawPulse(w);
    ImGui::Spacing();
    if(ImGui::BeginTabBar("##physTabs")) {
        auto tab = [&](const char* name, std::function<void(float)> body) {
            if(!ImGui::BeginTabItem(name)) return;
            ImGui::BeginChild((std::string("##physTab") + name).c_str(), ImVec2(0, 0), false);
            ImGui::Spacing();
            const float tw = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
            labelW = 70;
            body(tw);
            ImGui::EndChild();
            ImGui::EndTabItem();
        };
        tab("Sound", [this](float tw){ drawSoundTab(tw); });
        tab("Modulation", [this](float tw){ mod.drawEnvelopeCards(tw, {}); ImGui::Spacing(); mod.drawLfos(tw); });
        tab("Routing", [this](float tw){ mod.drawRouting(tw); });
        tab("Effects", [this](float tw){ drawFxTab(tw); });
        ImGui::EndTabBar();
    }
    ImGui::End();
}
void scPhysicalBox::draw(ofEventArgs&) { if(show.get()) drawWindow(); }

void scPhysicalBox::drawHeader() {
    const float z = zoom();
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    auto next = [&](float width, float gap = 12.0f) {
        if(ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + gap * z + width <= right) ImGui::SameLine(0, gap * z);
    };
    labelW = 54;
    ImGui::BeginGroup(); sliderInt(voices, "Voices", 170 * z); ImGui::EndGroup();
    next(170 * z);
    ImGui::BeginGroup(); sliderInt(channels, "Channels", 170 * z); ImGui::EndGroup();
    next(170 * z);
    ImGui::BeginGroup(); slider(master, "Master", 170 * z, "%.2f"); ImGui::EndGroup();
    next(190 * z); labelW = 64;
    ImGui::BeginGroup(); combo(gateMode, "Gate Mode", 190 * z); ImGui::EndGroup();
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Auto: drone until Gate is connected, timelined or receives a note.\nGated: follow Gate. Drone: every voice always plays.\nPulse: the voices play by themselves (the Pulse panel).");
    next(ImGui::CalcTextSize("pulsing").x, 6);
    ImGui::AlignTextToFramePadding();
    if(pulsing()) ImGui::TextColored(ImVec4(.42f, .83f, .77f, 1), "pulsing");
    else ImGui::TextColored(droning() ? ImVec4(.96f, .73f, .45f, 1) : ImVec4(.59f, .65f, 1, 1), droning() ? "droning" : "gated");
    next(ImGui::CalcTextSize("Strike").x + ImGui::GetStyle().FramePadding.x * 2);
    if(ImGui::Button("Strike")) for(auto& [s, st] : states) for(int i = 0; i < st.builtVoices; ++i) if(st.voices[i]) st.voices[i]->set("strk", 1.0f);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Strike every voice now (also while droning or held)");
    next(ImGui::CalcTextSize("Reset LFOs").x + ImGui::GetStyle().FramePadding.x * 2);
    if(ImGui::Button(pulsing() ? "Reset" : "Reset LFOs")) resetLFOs.trigger();
    if(ImGui::IsItemHovered()) ImGui::SetTooltip(pulsing() ? "Restart the LFOs and the pulse pattern" : "Restart the LFOs");
    labelW = 70;
}

void scPhysicalBox::drawDisplay(float w) {
    const float z = zoom();
    const float h = 176 * z, pad = h, meters = std::max(60.0f * z, voices.get() * 11.0f * z);
    const ImVec2 a = ImGui::GetCursorScreenPos();
    drawPad(a, pad);
    drawObject(ImVec2(a.x + pad + gapW(), a.y), ImVec2(w - pad - meters - 2 * gapW(), h));
    drawMeters(ImVec2(a.x + w - meters, a.y), ImVec2(meters, h));
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h + 4 * z));
    ImGui::Dummy(ImVec2(w, 0));
}

// Material (x: wood .. glass .. metal) and Excite + Listen (y: up the body, down the waveguide)
void scPhysicalBox::drawPad(ImVec2 a, float s) {
    const float z = zoom();
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(a.x + s, a.y + s);
    const ImU32 wood = IM_COL32(120, 78, 44, 255), glass = IM_COL32(70, 120, 130, 255), metal = IM_COL32(120, 125, 140, 255);
    dl->AddRectFilledMultiColor(a, ImVec2(a.x + s * 0.5f, b.y), wood, glass, glass, wood);
    dl->AddRectFilledMultiColor(ImVec2(a.x + s * 0.5f, a.y), b, glass, metal, metal, glass);
    dl->AddRectFilledMultiColor(a, b, IM_COL32(0, 0, 0, 40), IM_COL32(0, 0, 0, 40), IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 150));
    for(int i = 1; i < 4; ++i) {
        dl->AddLine(ImVec2(a.x + s * i / 4, a.y), ImVec2(a.x + s * i / 4, b.y), pal::grid);
        dl->AddLine(ImVec2(a.x, a.y + s * i / 4), ImVec2(b.x, a.y + s * i / 4), pal::grid);
    }
    dl->AddRect(a, b, pal::border, 3 * z);
    auto label = [&](ImVec2 p, const char* t) { dl->AddText(p, IM_COL32(255, 255, 255, 150), t); };
    label(ImVec2(a.x + 4 * z, b.y - 16 * z), "wood");
    label(ImVec2(a.x + s * 0.5f - ImGui::CalcTextSize("glass").x * 0.5f, b.y - 16 * z), "glass");
    label(ImVec2(b.x - ImGui::CalcTextSize("metal").x - 4 * z, b.y - 16 * z), "metal");
    label(ImVec2(a.x + 4 * z, a.y + 3 * z), "body");
    label(ImVec2(a.x + 4 * z, b.y - 32 * z), "waveguide");
    auto& mat = soundParam("material");
    auto& exc = soundParam("excite");
    const ImVec2 p(a.x + mat.get() * s, b.y - exc.get() * s);
    dl->AddCircle(p, 7 * z, IM_COL32(255, 255, 255, 230), 0, 2 * z);
    dl->AddCircleFilled(p, 3 * z, pal::exciter);
    // listen apart from excite: its own ring
    const float lis = soundParam("listen").get();
    if(std::abs(lis - exc.get()) > 0.01f) dl->AddCircle(ImVec2(p.x, b.y - lis * s), 5 * z, pal::pickup, 0, 1.5f * z);
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##materialPad", ImVec2(s, s));
    if(ImGui::IsItemActive()) {
        const auto m = ImGui::GetMousePos();
        mat = ofClamp((m.x - a.x) / s, 0.0f, 1.0f);
        const float e = ofClamp((b.y - m.y) / s, 0.0f, 1.0f);
        exc = e; soundParam("listen") = e;
    }
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("Material %.2f  (wood > glass > metal: how fast highs die, how long it rings)\n"
                          "Excite / Listen %.2f / %.2f  (0 waveguide, 1 modal body)\nRight-click: publish / modulate Material", mat.get(), exc.get(), lis);
    menu(mat.getEscapedName());
}

// The waveguide's excited shape with the contact and pickups (drag them), and
// the body's modes as the voice computes them (PhysicalModes.h).
void scPhysicalBox::drawObject(ImVec2 a, ImVec2 size) {
    const float z = zoom();
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(a.x + size.x, a.y + size.y);
    dl->AddRectFilled(a, b, pal::canvas, 3 * z);
    dl->AddRect(a, b, pal::border, 3 * z);
    const float exc = soundParam("excite").get(), lis = soundParam("listen").get();
    const float x0 = a.x + 18 * z, x1 = b.x - 18 * z;
    const float rowA = a.y + size.y * 0.27f, ampA = size.y * 0.16f;
    auto& pos = soundParam("pos"); auto& p1 = soundParam("pick1"); auto& p2 = soundParam("pick2");
    const float bore = soundParam("abore").get();
    const char* kind = bore < 0.25f ? "string" : bore < 0.75f ? "closed tube" : "open tube";
    const float pitch0 = first(pitch.get(), 0, 48) + soundParam("atune").get();
    const auto noteName = [](float note) {
        static const char* n[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        const int k = int(std::round(note));
        return std::string(n[((k % 12) + 12) % 12]) + ofToString(k / 12 - 1);
    };
    dl->AddText(ImVec2(a.x + 6 * z, a.y + 4 * z), mixColor(pal::textDim, pal::waveguide, 1 - std::min(exc, lis)),
                ("A  " + std::string(kind) + "   " + noteName(pitch0) + "  " + ofToString(440 * std::pow(2.0f, (pitch0 - 69) / 12), 1) + " Hz").c_str());
    // the waveguide (a tube drawn as two walls)
    const ImU32 wg = mixColor(IM_COL32(150, 165, 255, 70), pal::waveguide, 1 - exc);
    if(bore >= 0.25f) {
        dl->AddLine(ImVec2(x0, rowA - ampA - 4 * z), ImVec2(x1, rowA - ampA - 4 * z), IM_COL32(150, 165, 255, 60), 1.5f * z);
        dl->AddLine(ImVec2(x0, rowA + ampA + 4 * z), ImVec2(x1, rowA + ampA + 4 * z), IM_COL32(150, 165, 255, 60), 1.5f * z);
    }
    dl->AddLine(ImVec2(x0, rowA - ampA), ImVec2(x0, rowA + ampA), pal::headerText, 2 * z);
    dl->AddLine(ImVec2(x1, rowA - ampA), ImVec2(x1, rowA + ampA), pal::headerText, 2 * z);
    // its shape just after a pluck / strike at the contact: sum sin(k pi pos) sin(k pi x) / k^2
    std::vector<ImVec2> line;
    const int steps = 96;
    for(int i = 0; i <= steps; ++i) {
        const float x = float(i) / steps;
        float y = 0;
        for(int k = 1; k <= 16; ++k) y += std::sin(k * PI * pos.get()) * std::sin(k * PI * x) / (k * k);
        line.push_back(ImVec2(ofLerp(x0, x1, x), rowA - y * ampA * 1.15f));
    }
    dl->AddPolyline(line.data(), int(line.size()), wg, 0, 2 * z);
    // the markers: contact (triangle) and pickups (circles); drag them
    struct Marker { ofParameter<float>* p; ImU32 color; const char* name; };
    Marker markers[3] = {{&pos, pal::exciter, "Contact"}, {&p1, pal::pickup, "Pickup 1"}, {&p2, pal::pickup, "Pickup 2"}};
    for(int m = 0; m < 3; ++m) {
        const float x = ofLerp(x0, x1, markers[m].p->get());
        if(m == 0) dl->AddTriangleFilled(ImVec2(x, rowA + ampA + 2 * z), ImVec2(x - 6 * z, rowA + ampA + 12 * z), ImVec2(x + 6 * z, rowA + ampA + 12 * z), pal::exciter);
        else {
            dl->AddCircleFilled(ImVec2(x, rowA - ampA - 7 * z), 4.5f * z, markers[m].color);
            dl->AddText(ImVec2(x - 3 * z, rowA - ampA - 22 * z), markers[m].color, m == 1 ? "1" : "2");
        }
    }
    static int dragging = -1;
    ImGui::SetCursorScreenPos(ImVec2(a.x, a.y));
    ImGui::InvisibleButton("##object", ImVec2(size.x, size.y * 0.55f));
    const auto mouse = ImGui::GetMousePos();
    const float mx = ofClamp((mouse.x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
    if(ImGui::IsItemActivated()) {
        float best = 1e9f; dragging = -1;
        for(int m = 0; m < 3; ++m) {
            const float d = std::abs(ofLerp(x0, x1, markers[m].p->get()) - mouse.x) + (m == 0 ? (mouse.y > rowA ? 0 : 6 * z) : (mouse.y < rowA ? 0 : 6 * z));
            if(d < best) { best = d; dragging = m; }
        }
    }
    if(ImGui::IsItemActive() && dragging >= 0) *markers[dragging].p = mx;
    if(!ImGui::IsItemActive()) dragging = -1;
    if(ImGui::IsItemHovered() && dragging < 0)
        ImGui::SetTooltip("Drag the contact (where it is struck, bowed, blown) or a pickup (where it is heard).\n"
                          "Contact %.2f   Pickup 1 %.2f   Pickup 2 %.2f", pos.get(), p1.get(), p2.get());

    // the body's modes: log frequency, height = how much the contact and pickups get of them
    const float rowTop = a.y + size.y * 0.58f, rowBottom = b.y - 8 * z;
    const auto* sh = sound("bshape");
    const float shape = sh->p.get();
    const int s0 = std::clamp(int(std::floor(shape)), 0, physical::NSHAPES - 1), s1 = std::min(s0 + 1, physical::NSHAPES - 1);
    std::string shapeName = shapeNames[s0];
    if(shape - s0 > 0.04f && s1 != s0) shapeName += " > " + std::string(shapeNames[s1]) + " " + ofToString(shape - s0, 2);
    dl->AddText(ImVec2(a.x + 6 * z, rowTop - 16 * z), mixColor(pal::textDim, pal::body, std::max(exc, lis)), ("B  " + shapeName).c_str());
    const float lo = std::log2(0.4f), hi = std::log2(64.0f);
    auto xOf = [&](float ratio) { return ofLerp(x0, x1, (std::log2(std::max(ratio, 0.01f)) - lo) / (hi - lo)); };
    for(int k = 1; k <= 64; k *= 2) {
        const float x = xOf(float(k));
        dl->AddLine(ImVec2(x, rowTop), ImVec2(x, rowBottom), pal::grid);
    }
    const int modes = std::clamp(int(std::round(soundParam("bmodes").get())), 1, physical::MAXM);
    const float material = soundParam("material").get(), bdecay = soundParam("bdecay").get(), freeze = soundParam("freeze").get();
    const float bexp = ofLerp(1.6f, 0.0f, soundParam("bbright").get());
    const float fb0 = 440 * std::pow(2.0f, (first(pitch.get(), 0, 48) + soundParam("btune").get() - 69) / 12);
    int hovered = -1; float hoverD = 6 * z;
    std::vector<physical::Mode> ms(modes);
    for(int k = 0; k < modes; ++k) {
        ms[k] = physical::mode(k, shape, soundParam("bstretch").get(), material, bdecay, freeze);
        const auto& m = ms[k];
        if(m.ratio > 64 || fb0 * m.ratio > 20000) continue;
        const float e = std::abs(std::sin(PI * m.q * pos.get() + m.ph));
        const float l = 0.5f * (std::abs(std::sin(PI * m.q * p1.get() + m.ph)) + std::abs(std::sin(PI * m.q * p2.get() + m.ph)));
        const float hgt = ofClamp(e * l * std::pow(float(m.ratio), -bexp), 0.03f, 1.0f);
        const float x = xOf(m.ratio);
        const float ring = ofClamp(std::log10(m.t60 * 10) / 2.5f, 0.0f, 1.0f);   // 0.1 s .. 30 s
        const ImU32 col = mixColor(IM_COL32(120, 80, 50, 200), IM_COL32(255, 230, 170, 255), ring);
        dl->AddLine(ImVec2(x, rowBottom), ImVec2(x, rowBottom - hgt * (rowBottom - rowTop)), col, 2 * z);
        if(std::abs(mouse.x - x) < hoverD && mouse.y > rowTop && mouse.y < rowBottom) { hovered = k; hoverD = std::abs(mouse.x - x); }
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, rowTop));
    ImGui::InvisibleButton("##modes", ImVec2(size.x, rowBottom - rowTop));
    if(ImGui::IsItemHovered() && hovered >= 0)
        ImGui::SetTooltip("Mode %d: x%.3f  %.1f Hz  rings %.2f s", hovered + 1, ms[hovered].ratio, fb0 * ms[hovered].ratio, ms[hovered].t60);
    else if(ImGui::IsItemHovered())
        ImGui::SetTooltip("The body's modes (log frequency, octave lines): height = how much the contact\n"
                          "and pickups get of each, colour = how long it rings (Material, B Decay)");
}

void scPhysicalBox::drawMeters(ImVec2 a, ImVec2 size) {
    const float z = zoom();
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(a.x + size.x, a.y + size.y);
    dl->AddRectFilled(a, b, pal::canvas, 3 * z);
    dl->AddRect(a, b, pal::border, 3 * z);
    const int n = voices.get();
    const float cw = (size.x - 8 * z) / std::max(1, n);
    for(int v = 0; v < n; ++v) {
        const size_t o = size_t(v * VIS_VOICE);
        const float level = o < live.size() ? std::sqrt(ofClamp(live[o] * 4, 0.0f, 1.0f)) : 0;
        const bool on = o + 1 < live.size() && live[o + 1] > 0.0001f;
        const float x = a.x + 4 * z + v * cw;
        const float top = a.y + 14 * z, bottom = b.y - 6 * z;
        dl->AddRectFilled(ImVec2(x + 1 * z, top), ImVec2(x + cw - 1 * z, bottom), IM_COL32(255, 255, 255, 10));
        dl->AddRectFilled(ImVec2(x + 1 * z, bottom - level * (bottom - top)), ImVec2(x + cw - 1 * z, bottom), mixColor(pal::waveguide, pal::accent, level));
        dl->AddCircleFilled(ImVec2(x + cw * 0.5f, a.y + 7 * z), 2.5f * z, on ? pal::exciter : IM_COL32(255, 255, 255, 30));
    }
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##meters", size);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Voices: level, gate dot (each voice is its own instrument)");
}

void scPhysicalBox::sectionHeader(const char* title, float w, ofParameter<bool>* on, ImU32 accent) {
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
void scPhysicalBox::slider(ofParameter<float>& p, const char* label, float w, const char* fmt, bool log) {
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
void scPhysicalBox::toggle(ofParameter<bool>& p, const char* label) {
    bool v = p.get();
    if(ImGui::Checkbox(label, &v)) p.set(v);
    mark(p.getEscapedName());
    menu(p.getEscapedName());
}
void scPhysicalBox::sliderInt(ofParameter<int>& p, const char* label, float w) {
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
void scPhysicalBox::combo(ofParameter<int>& p, const char* label, float w) {
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

// one sound control: a slider (Modes: whole numbers, Hold: a checkbox)
void scPhysicalBox::soundSlider(const std::string& sc, const char* label, float w) {
    auto* c = sound(sc);
    if(!c) return;
    const std::string key = c->p.getEscapedName();
    if(c->discrete) {
        ImGui::PushID(key.c_str());
        bool on = c->p.get() > 0.5f;
        if(ImGui::Checkbox(label, &on)) c->p = on ? 1 : 0;
        mark(key); menu(key);
        ImGui::PopID();
        return;
    }
    if(sc == "bmodes") {
        const float z = zoom();
        ImGui::PushID(key.c_str());
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(label); mark(key);
        ImGui::SameLine(); ImGui::SetCursorPosX(x0 + labelW * z);
        ImGui::SetNextItemWidth(std::max(30.0f * z, w - labelW * z));
        int v = int(std::round(c->p.get()));
        if(ImGui::SliderInt("##v", &v, 1, physical::MAXM)) c->p = float(v);
        menu(key);
        ImGui::PopID();
        return;
    }
    std::string fmt = c->fmt;
    if(sc == "bshape") {
        const int s0 = std::clamp(int(std::floor(c->p.get())), 0, physical::NSHAPES - 1);
        fmt = std::string(shapeNames[s0]) + " %.2f";
    }
    slider(c->p, label, w, fmt.c_str(), c->log);
}
void scPhysicalBox::soundGroup(const std::string& group, float w) {
    for(auto& c : soundControls) if(c->group == group) {
        std::string label = c->p.getName();
        for(const char* strip : {"A ", "B "}) if(label.rfind(strip, 0) == 0 && group.size() == 1) label.erase(0, 2);
        soundSlider(c->sc, label.c_str(), w);
    }
}

// Pulse (Gate Mode Pulse): the events as they play, a character to start from,
// then Time / Voices / Notes
void scPhysicalBox::drawPulse(float w) {
    const float z = zoom();
    const float cw = colW(w, 5), rasterW = cw * 2 + gapW();
    auto tip = [](const char* text) { if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text); };
    ImGui::Spacing();
    ImGui::BeginGroup();
    labelW = 66;
    sectionHeader("Pulse", rasterW, nullptr, pal::pickup);
    {
        static const char* names[] = {"Clock", "Strum", "Phasing", "Rain", "Geiger", "Swarm", "Bursts"};
        static const char* tips[] = {
            "Every voice on the same beat",
            "Together, rolled across the voices like a strummed chord",
            "Each voice its own clock, slightly apart: they drift in and out of phase",
            "Each voice its own random drops (Poisson), soft and loud",
            "Random clicks with bursts and silences, each voice its own counter",
            "Loose, partly shared, partly free: a flock",
            "Together, in clusters of quick repeats"};
        const int count = int(std::size(names));
        const float gap = 4 * z, bw = (rasterW - gap * (count - 1)) / count;
        for(int i = 0; i < count; ++i) {
            if(i) ImGui::SameLine(0, gap);
            if(ImGui::Button(names[i], ImVec2(bw, 0))) pulseShape(i);
            tip(tips[i]);
        }
    }
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float rows = 6 * ImGui::GetFrameHeightWithSpacing() - ImGui::GetFrameHeight() - 4 * z;
    drawPulseRaster(at, ImVec2(rasterW, rows));
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + rows));
    ImGui::Dummy(ImVec2(rasterW, 0));
    nextCol();
    sectionHeader("Time", cw, nullptr, pal::pickup);
    toggle(pulseSync, "Sync to tempo");
    tip("Rate from the tempo (a note value) instead of Hz");
    if(pulseSync.get()) {
        static const std::vector<std::pair<const char*, float>> notes = {
            {"1/32", 0.125f}, {"1/16T", 1.0f / 6.0f}, {"1/16", 0.25f}, {"1/8T", 1.0f / 3.0f}, {"1/8", 0.5f},
            {"1/4T", 2.0f / 3.0f}, {"1/4", 1.0f}, {"1/2", 2.0f}, {"1 bar", 4.0f}, {"2 bars", 8.0f}, {"4 bars", 16.0f}};
        int k = 0;
        for(int i = 0; i < (int)notes.size(); i++)
            if(std::abs(notes[i].second - pulseBeats.get()) < std::abs(notes[k].second - pulseBeats.get())) k = i;
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Every");
        mark(pulseBeats.getEscapedName());
        ImGui::SameLine();
        ImGui::SetCursorPosX(x0 + labelW * z);
        ImGui::SetNextItemWidth(cw - labelW * z);
        if(ImGui::SliderInt("##pulseNote", &k, 0, (int)notes.size() - 1, notes[k].first)) pulseBeats = notes[k].second;
        tip("The average time between events, in beats");
        menu(pulseBeats.getEscapedName());
    } else {
        slider(pulseRate, "Rate", cw, "%.2f Hz", true);
        tip("Events per second per voice, on average (whatever Chaos and Cluster are)");
    }
    slider(pulseChaos, "Chaos", cw);
    tip("0: periodic, a clock\n1: random like a Geiger counter (Poisson): same average rate, no pattern");
    slider(pulseCluster, "Cluster", cw);
    tip("Gather the events into bursts of quick repeats and long silences (same average rate)");
    slider(pulseChance, "Chance", cw);
    tip("The chance each event plays (thins the pattern)");
    nextCol();
    sectionHeader("Voices", cw, nullptr, pal::pickup);
    slider(pulseLink, "Link", cw, pulseLink.get() >= 0.995f ? "together" : pulseLink.get() <= 0.005f ? "independent" : "%.2f");
    tip("1: every voice plays the same events (one seed for all)\n0: each voice its own events (its own seed)\n"
        "between: each event is shared or a voice's own by chance");
    slider(pulsePhasing, "Phasing", cw);
    tip("Each voice's own clock a little faster or slower (up to an octave):\nperiodic voices drift in and out of phase. Acts where Link < 1");
    slider(pulseStrum, "Strum", cw, "%+.0f ms");
    tip("Roll the shared events across the voices, ms per voice (negative: downwards)");
    {
        const std::string key = pulseSeed.getEscapedName();
        ImGui::PushID(key.c_str());
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Seed");
        mark(key);
        ImGui::SameLine();
        ImGui::SetCursorPosX(x0 + labelW * z);
        ImGui::SetNextItemWidth(std::max(30.0f * z, cw - labelW * z));
        int v = pulseSeed.get();
        if(ImGui::DragInt("##v", &v, 0.2f, 0, 9999, v == 0 ? "fresh" : "%d")) pulseSeed = std::clamp(v, 0, 9999);
        tip("0: a new pattern every start\nabove 0: the same pattern every time (Reset plays it from the start)");
        menu(key);
        ImGui::PopID();
    }
    toggle(pulseFollow, "Follow Gate");
    tip("Off: the pulses run by themselves\nOn: a voice pulses only while its Gate is held, as loud as the Gate\n"
        "(hold notes from a keyboard or the timeline to play rolls)");
    nextCol();
    sectionHeader("Notes", cw, nullptr, pal::pickup);
    if(pulseFixed.get()) {
        slider(pulseTime, "Length", cw, "%.3f s", true);
        tip("How long each gate stays open, in seconds");
    } else {
        char fmt[48];
        std::snprintf(fmt, sizeof(fmt), "%%.2f x  (%.0f ms)", pulseLength.get() / pulseHz() * 1000);
        slider(pulseLength, "Length", cw, fmt);
        tip("How long each gate stays open, as a fraction of the average period.\n"
            "Struck sounds only need the start; bow, blow and scrape sound while it is open.\n"
            "Past 1 notes run into the next: legato, each event a new strike");
    }
    toggle(pulseFixed, "Length in seconds");
    slider(pulseJitter, "Jitter", cw);
    tip("Random length per event: up to 4x shorter or longer");
    slider(pulseVelocity, "Velocity", cw);
    tip("How hard each event plays (the gate level)");
    slider(pulseVelJitter, "Vel Jitter", cw);
    tip("Random velocity per event: accents and ghost notes");
    ImGui::EndGroup();
    labelW = 70;
}

// the last seconds of events per voice: a block per gate (its length), as
// bright and tall as its velocity
void scPhysicalBox::drawPulseRaster(ImVec2 a, ImVec2 size) {
    const float z = zoom();
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(a.x + size.x, a.y + size.y);
    dl->AddRectFilled(a, b, pal::canvas, 3 * z);
    dl->AddRect(a, b, pal::border, 3 * z);
    const int n = std::max(1, voices.get());
    const float hz = pulseHz();
    const float window = ofClamp(24.0f / hz, 1.5f, 6.0f), now = ofGetElapsedTimef();
    const float x0 = a.x + 4 * z, x1 = b.x - 4 * z, top = a.y + 4 * z, bottom = b.y - 18 * z;
    const float rh = (bottom - top) / n;
    auto xOf = [&](float t) { return ofLerp(x0, x1, ofClamp(1 - (now - t) / window, 0.0f, 1.0f)); };
    // a line per average period (sync: per beat value)
    const float period = 1.0f / hz;
    if(window / period < 64)
        for(float t = now - std::fmod(now, period); t > now - window; t -= period)
            dl->AddLine(ImVec2(xOf(t), top), ImVec2(xOf(t), bottom), pal::grid);
    dl->PushClipRect(ImVec2(x0, top), ImVec2(x1, bottom), true);
    for(int v = 0; v < n; ++v) {
        const float y0 = top + v * rh, y1 = y0 + rh;
        if(v % 2) dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(255, 255, 255, 6));
        for(const auto& e : pulseEvents[v]) {
            const float vel = ofClamp(e.vel, 0.0f, 1.0f);
            const float h = (rh - 2 * z) * (0.3f + 0.7f * vel) * 0.5f, mid = (y0 + y1) * 0.5f;
            const float ex0 = xOf(e.t), ex1 = std::max(ex0 + 2 * z, xOf(e.t + e.dur));
            const ImU32 col = mixColor(IM_COL32(106, 212, 196, 70), IM_COL32(220, 255, 245, 230), vel);
            dl->AddRectFilled(ImVec2(ex0, mid - h), ImVec2(ex1, mid + h), mixColor(IM_COL32(106, 212, 196, 30), IM_COL32(106, 212, 196, 120), vel));
            dl->AddLine(ImVec2(ex0, mid - h), ImVec2(ex0, mid + h), col, 1.5f * z);
        }
    }
    dl->PopClipRect();
    // what it is, in words
    const float chaos = pulseChaos.get(), link = pulseLink.get();
    std::string text = ofToString(hz, hz < 10 ? 2 : 1) + " /s per voice   " +
        (chaos < 0.05f ? "periodic" : chaos > 0.95f ? "random (Poisson)" : "loose " + ofToString(int(chaos * 100)) + "%") +
        (pulseCluster.get() > 0.05f ? ", clustered" : "") + "   " +
        (link > 0.995f ? "together" : link < 0.005f ? "independent" : ofToString(int(link * 100)) + "% shared") +
        (pulseFollow.get() ? "   held by Gate" : "");
    dl->AddText(ImVec2(a.x + 6 * z, b.y - 16 * z), pal::textDim, text.c_str());
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##pulseRaster", size);
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("The pulses as they play: a row per voice, newest on the right (%.1f s).\n"
                          "Each block is a gate, its length and its velocity.", window);
}

void scPhysicalBox::drawSoundTab(float w) {
    const float cw = colW(w, 5);
    auto note = [&](const char* text) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
        ImGui::TextColored(ImColor(pal::textDim), "%s", text);
        ImGui::PopTextWrapPos();
    };
    ImGui::BeginGroup();
    labelW = 78;
    sectionHeader("Strike", cw, nullptr, pal::exciter); soundGroup("Strike", cw);
    ImGui::Spacing();
    sectionHeader("Bow", cw, nullptr, pal::exciter); soundGroup("Bow", cw);
    note("Pressure past 1 goes raucous; light pressure whistles.");
    nextCol();
    sectionHeader("Blow", cw, nullptr, pal::exciter); soundGroup("Blow", cw);
    note("Jet (flute) to reed (clarinet). A flute wants Bore 0.5, Position near 0.");
    ImGui::Spacing();
    sectionHeader("Scrape", cw, nullptr, pal::exciter); soundGroup("Scrape", cw);
    nextCol();
    sectionHeader("Contact", cw, nullptr, pal::exciter); soundGroup("Contact", cw);
    note("Attack / Release shape the sustained exciters (bow, blow, scrape). Hold keeps them on. "
         "Ext In plays the node's In through the object.");
    ImGui::Spacing();
    sectionHeader("Output", cw); soundGroup("Output", cw);
    note("Width: the two pickups apart; Spread: the voices across the speakers.");
    nextCol();
    labelW = 70;
    sectionHeader("A  Waveguide", cw, nullptr, pal::waveguide); soundGroup("A", cw);
    note("Bore: 0 string, 0.5 closed tube (odd harmonics), 1 open tube.");
    ImGui::Spacing();
    sectionHeader("B  Body", cw, nullptr, pal::body); soundGroup("B", cw);
    nextCol();
    labelW = 78;
    sectionHeader("Material & Coupling", cw); soundGroup("Couple", cw);
    ImGui::Spacing();
    sectionHeader("Break", cw); soundGroup("Break", cw);
    ImGui::Spacing();
    sectionHeader("Life", cw); soundGroup("Life", cw);
    ImGui::EndGroup();
    labelW = 70;
}

void scPhysicalBox::drawFxTab(float w) {
    const float z = zoom();
    const float cw = colW(w, 5);
    ImGui::BeginGroup();
    sectionHeader("Saturation", cw, &satOn);
    if(!satOn.get()) ImGui::BeginDisabled();
    slider(satDrive, "Drive", cw, "%.1f dB");
    slider(satBias, "Bias", cw, "%.2f");
    slider(satTone, "Tone", cw, "%.0f Hz", true);
    slider(satOutput, "Output", cw, "%+.1f dB");
    slider(satMix, "Mix", cw, "%.2f");
    if(!satOn.get()) ImGui::EndDisabled();
    ImGui::Spacing();
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
    if(eqEditor.draw(cw, 130.0f * z, scEQEditor::defaultColors(), eqOn.get(), nullptr, 0, "##physEq")){
        for(int b = 0; b < 5; b++){
            eqFreq[b] = eqEditor.bands[b].freqHz;
            eqGain[b] = eqEditor.bands[b].gainDb;
            eqShape[b] = eqEditor.bands[b].shape;
        }
    }
    if(!eqOn.get()) ImGui::EndDisabled();
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
    combo(echoFilter, "Filter", cw);
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
    nextCol();
    sectionHeader("Space", cw, &spaceOn, pal::pickup);
    if(!spaceOn.get()) ImGui::BeginDisabled();
    slider(spaceMix, "Mix", cw, "%.2f");
    slider(spaceTime, "Time", cw, "%.2f");
    slider(spaceDamp, "Damp", cw, "%.2f");
    slider(spaceDiff, "Diffusion", cw, "%.2f");
    slider(spaceLowCut, "Low Cut", cw, "%.2f");
    toggle(spaceFreeze, "Freeze");
    if(!spaceOn.get()) ImGui::EndDisabled();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
    ImGui::TextColored(ImColor(pal::textDim), "MiVerb: Mutable Instruments' Clouds / Rings reverb.");
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::TextColored(ImColor(pal::textDim), "Voices -> Saturation -> EQ -> Echo -> Reverb -> Space, at speaker count.");
}
