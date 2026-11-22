#include "scAnimaBox.h"
#include "scAnimaBoxData.h"
#include "../shared/scGlobalPitch.h"
#include "../../../audio/scFxRouting.h"
#include "../../../audio/scSynthControlBatch.h"
#include "../../../ui/scTypedValue.h"
#include "../../../ofxOceanodeSuperColliderConfig.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace {
namespace pal {
const ImU32 canvas     = IM_COL32( 11,  12,  14, 255);
const ImU32 border     = IM_COL32( 46,  50,  58, 255);
const ImU32 header     = IM_COL32( 30,  32,  38, 255);
const ImU32 headerText = IM_COL32(205, 205, 215, 255);
const ImU32 textDim    = IM_COL32(150, 150, 160, 255);
const ImU32 accent     = IM_COL32(120, 200, 190, 255);   // sea green
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
float first(const std::vector<float>& v, int i, float fallback) { return v.empty() ? fallback : v[i % v.size()]; }
int rnd(double x) { return (int)std::floor(x + 0.5); }                       // Reaktor Quantize, step 1
double fmodf_(double a, double b) { return b == 0 ? 0 : a - b * std::floor(a / b); }   // Reaktor Modulo
float tension(float x, float p) {                                             // Reaktor pow macro
    const float a = 0.999f * std::clamp(p, -1.0f, 1.0f), k = 2 * a / (1 - a);
    return x * (k + 1) / (k * std::fabs(x) + 1);
}
// glibc LCG one step from a seed (Reaktor Core "Random [0..1]"), the bands' placement
float lcg(uint32_t seed) { return float(((uint64_t)1103515245u * seed + 12345u) & 0x7FFFFFFFu) / 2147483648.0f; }
ofJson blend(const ofJson& a, const ofJson& b, float t, bool sw, bool discrete) {
    if(!discrete && a.is_number() && b.is_number()) return ofLerp(a.get<float>(), b.get<float>(), t);
    if(!discrete && a.is_array() && b.is_array() && !a.empty() && !b.empty() && a.size() == b.size()) {
        ofJson out = ofJson::array();
        for(size_t i = 0; i < a.size(); ++i) out.push_back(blend(a[i], b[i], t, sw, false));
        return out;
    }
    return sw ? b : a;
}
const char* NOTE_NAMES[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
}

scAnimaBox::scAnimaBox(std::vector<serverManager*> s) : scNode("animaBox"), servers(std::move(s)) {
    description = "AnimaMundi (Reaktor) as a Box: a Body (oscillators or the live input) excites up to 120 "
                  "resonant bands tuned to the Scale input (ChordBox, SC Pitch...), their energy and resonance "
                  "moved by spatial LFOs. Post filter, Delay and MetaVerb from the ensemble, Box effects. "
                  "The original snapshots are its factory bank. Right-click controls to publish or automate.";
}
scAnimaBox::~scAnimaBox() {
    scGlobalPitch::unfollow(this);
    scheduled.disable();
    listeners.unsubscribeAll();
    for(auto& [s, st] : states) release(s, st);
}

template<class T> void scAnimaBox::reg(ofParameter<T>& p, std::function<void()> send) {
    Control c;
    c.key = p.getEscapedName(); c.param = &p;
    c.discrete = std::is_same_v<T, bool> || std::is_same_v<T, int>;
    c.add = [this, &p]() { return addParameter(p); };
    c.save = [&p]() { return ofJson(p.get()); };
    c.load = [&p](const ofJson& j) { try { p.set(j.get<T>()); } catch(const std::exception&) {} };
    controls.push_back(std::move(c));
    listeners.push(p.newListener([this, &p, send](T&) { snaps.morph.edited(p); if(!restoring) send(); }));
}

scAnimaBox::SoundControl& scAnimaBox::add(const std::string& name, float def, float lo, float hi, const std::string& group,
                                          std::function<void()> send, std::vector<std::string> options, const std::string& help) {
    auto c = std::make_unique<SoundControl>();
    c->p.set(name, def, lo, hi); c->group = group; c->help = help;
    c->discrete = !options.empty();
    reg(c->p, send);
    controls.back().discrete = c->discrete;
    if(!options.empty()) dropdowns.set(c->p.getEscapedName(), options);
    soundByName[name] = c.get();
    sound.push_back(std::move(c));
    return *sound.back();
}
float scAnimaBox::v(const std::string& name) const { auto it = soundByName.find(name); return it == soundByName.end() ? 0.0f : it->second->p.get(); }
ofParameter<float>& scAnimaBox::P(const std::string& name) { return soundByName.at(name)->p; }

void scAnimaBox::setup() {
    addOnToggle();
    scGlobalPitch::follow(this, true, [this](){
        for(auto& [s, st] : states) if(st.body) st.body->set("gtrbus", scGlobalPitch::bus(this, s));
        bandsDirty = true;
    });
    timeline.setup(this, [this](const std::string& k){ publish(k); }, [this](const std::string& k){ unpublish(k); }, [this](){ return publishedKeys; });
    addParameter(show.set("Show", false));
    addParameter(bands.set("Bands", 35, 1, MAX_BANDS));
    addParameter(channels.set("Channels", 2, 1, 16));
    gateIn.attach(addParameter(gate.set("Gate", {0.0f}, {0.0f}, {1.0f})), availableInputs, [this](){ refreshOutputs(); });
    pitchIn.attach(addParameter(pitch.set("Pitch", {60.0f}, {0.0f}, {127.0f})), availableInputs, [this](){ refreshOutputs(); });
    // the scale (an outer node's): pitch classes 0..12 (fractions: microtones) or notes
    scaleHandle = addParameter(scale.set("Scale", {0, 2, 3, 7, 8}, {-127.0f}, {127.0f}));
    addParameter(master.set("Master", 1.0f, 0.0f, 1.0f));
    addParameter(resetLFOs.set("Reset"));
    addInput("In");          // the live input of the Body
    addOutput("Out");

    reg(bands, [this](){ structureDirty = true; });
    reg(scale, [this](){ bandsDirty = true; });
    drone.set("Drone", false); reg(drone, [this](){ sendAll(); });
    scheduled.add(gateIn.handle, gate); scheduled.add(pitchIn.handle, pitch);
    scheduled.enable(this, [this](){ for(auto& [s, st] : states) if(st.created) return true; return false; });
    listeners.push(gate.newListener([this](std::vector<float>& g){
        // a new note: the keyboard's pitch transposes the bank, the detune is drawn again
        const float level = first(g, 0, 0);
        float& previous = lastGateLevel;
        if(level > 0 && previous <= 0) { lastNote = first(pitch.get(), 0, 60); if(v("Detune") + v("Detune Extra") > 0) redrawDetune(); bandsDirty = true; }
        previous = level;
        if(scScheduling::isBackendSendSuppressed()) return;
        for(auto& [s, st] : states) sendBody(s, st);
    }));
    listeners.push(pitch.newListener([this](std::vector<float>&){
        if(first(gate.get(), 0, 0) > 0) { lastNote = first(pitch.get(), 0, 60); bandsDirty = true; }
        if(scScheduling::isBackendSendSuppressed()) return;
        for(auto& [s, st] : states) sendBody(s, st);
    }));
    listeners.push(channels.newListener([this](int&){ structureDirty = true; }));
    listeners.push(bands.newListener([this](int&){ structureDirty = true; }));   // also while a sound is applied
    listeners.push(master.newListener([this](float&){ if(!restoring) sendAll(); }));
    listeners.push(resetLFOs.newListener([this](){ mod.resetLfos(); }));

    auto toBody = [this](){ sendAll(); };
    auto toBands = [this](){ bandsDirty = true; };
    // ── Body (Instrument "Body"): saved panel state of the ensemble ─────────────
    const std::vector<std::string> oscs = {"Noise", "Saw", "Pulse", "Impulse", "Geiger"}, srcs = {"Synth", "Input"};
    add("Osc A", 1, 0, 4, "Body", toBody, oscs, "Side A's oscillator (Reaktor's left)");
    add("Osc B", 1, 0, 4, "Body", toBody, oscs, "Side B's oscillator (Reaktor's right)");
    add("Source A", 0, 0, 1, "Body", toBody, srcs, "Synth: the oscillator. Input: the live audio input (the node's In)");
    add("Source B", 0, 0, 1, "Body", toBody, srcs);
    add("Body Octave", -3, -4, 4, "Body", toBody, {}, "The oscillators' octave (the note is also the bank's transposition)");
    add("Body Transpose", 0, -48, 48, "Body", toBody);
    add("Body Detune", 0.24f, 0, 1, "Body", toBody, {}, "Side A + detune, side B - detune (semitones)");
    add("Geiger Regularity", 0, 0, 1, "Body", toBody, {}, "0: random clicks, 1: regular");
    add("Attack", 25, 0, 80, "Body", toBody); add("Decay", 40, 0, 80, "Body", toBody);
    add("Sustain", 0.5f, 0, 1, "Body", toBody); add("Release", 15, 0, 80, "Body", toBody);
    add("Input Gain", 1, 0, 4, "Body", toBody);
    add("Body Level", 1, 0, 4, "Body", toBody);
    // ── Pitch Index: band -> degree ─────────────────────────────────────────────
    add("Step", 1, 0, 12, "Index", toBands, {}, "Scale degrees from one band to the next");
    add("Index Cycles", 1, 0, 16, "Index", toBands); add("Index Offset", 0, 0, 1, "Index", toBands);
    add("Repeat", 1, 1, 6, "Index", toBands, {}, "Neighbouring bands sharing a degree");
    add("Comb", 0, 0, 1, "Index", toBands); add("Index Invert", 0, 0, 1, "Index", toBands, {"Off", "On"});
    add("Symmetry", 1, 1, 16, "Index", toBands); add("Index Random", 0, 0, 127, "Index", [this](){ redrawIndexRandom(); bandsDirty = true; });
    add("Index Pow", 0, -1, 1, "Index", toBands); add("Index Quantize", 0, 0, 127, "Index", toBands);
    add("Index Skew", 1, -1, 1, "Index", toBands, {}, "1: rising order, -1: falling, 0: up then down");
    add("Index Round", 0, 0, 2, "Index", toBands); add("Index Edge", 0, -0.49f, 0.49f, "Index", toBands);
    // ── Harmony: degree -> note (the scale comes from outside) ─────────────────
    add("Tuning", 0, 0, 1, "Harmony", toBands, {"Scale", "Overtones"}, "Scale: the Scale input. Overtones: the harmonic series");
    add("Scale Input", 0, 0, 3, "Harmony", toBands, {"Auto", "Classes + Root", "Notes", "Classes"},
        "Classes + Root: pitch classes from Root (fractions for microtones). Classes: absolute pitch classes (a ChordBox's Output: Root is not added). "
        "Notes: MIDI notes. Auto: Notes when a value is 12 or more, else Classes while a cable brings the Scale, Classes + Root when typed in");
    std::vector<std::string> notes(NOTE_NAMES, NOTE_NAMES + 12);
    add("Root", 0, 0, 11, "Harmony", toBands, notes);
    add("Octave", 4, 0, 8, "Harmony", toBands, {}, "Root + 12 x Octave: the base note (4: C4 = 48)");
    add("Key", -3, -24, 24, "Harmony", toBands, {}, "Degrees added to every band (an inversion)");
    add("Mode", 0, 0, 7, "Harmony", toBands, {}, "Degrees added (the scale's mode)");
    add("Overtones", 0, 0, 2, "Harmony", toBands, {"Even", "Odd", "All"});
    add("Overtone Round", 0, 0, 1, "Harmony", toBands, {"Off", "On"});
    add("Keyboard Transpose", 1, 0, 1, "Harmony", toBands, {"Off", "On"}, "The played note (Pitch) transposes the bank by note - 60");
    add("Transpose", 0, -24, 24, "Harmony", toBands); add("Fine Transpose", 0, -1, 1, "Harmony", toBands);
    add("Random Transpose", 0, 0, 12, "Harmony", toBands, {}, "A random transposition each Random Every (semitones)");
    add("Random Step", 1, 0, 12, "Harmony", toBands); add("Random Abs", 0, 0, 1, "Harmony", toBands, {"Off", "On"});
    add("Random Every", 16, 0.125f, 16, "Harmony", toBands, {}, "Beats between the random draws (transpose, octaves)");
    add("Octave Random", 0, 0, 1, "Harmony", toBands, {}, "Probability of a random octave shift per band");
    add("Octave Range", 2, 0, 4, "Harmony", toBands); add("Octave Up Only", 0, 0, 1, "Harmony", toBands, {"Off", "On"});
    // ── Pitch Modifiers ────────────────────────────────────────────────────────
    add("Band Octave", 0, -3, 3, "Modifiers", toBands); add("Semitone", 0, -12, 12, "Modifiers", toBands);
    add("Fine", 0, -1, 1, "Modifiers", toBands);
    add("Detune", 0, 0, 1, "Modifiers", [this](){ redrawDetune(); bandsDirty = true; }, {}, "A random offset per band, +-semitones (drawn on each note)");
    add("Detune Extra", 0, 0, 24, "Modifiers", [this](){ redrawDetune(); bandsDirty = true; });
    add("Vibrato", 0, 0, 1, "Modifiers", toBody, {}, "Vibrato depth (semitones)");
    add("Vibrato Rate", 6.31f, 0, 8, "Modifiers", toBands); add("Vibrato Poly", 0, 0, 8, "Modifiers", toBands, {}, "Each band its own vibrato rate");
    add("Glide", 0, 0, 4000, "Modifiers", toBody, {}, "Pitch glide when the scale or the transposition changes (ms)");
    // ── Bands ──────────────────────────────────────────────────────────────────
    add("Energy", 0, 0, 1, "Bands", toBody, {}, "Every band's level; LFO 1 adds the moving energy (Routing)");
    add("Reso", 29.9f, 0, 100, "Bands", toBody, {}, "Resonance (dB); LFO 2 adds to it (Routing)");
    add("Resonance Fine", 0, 0, 0.002f, "Bands", toBody);
    add("Stereo Detune", 0.1f, 0, 1, "Bands", toBody, {}, "Each band's two filters at pitch +- this (semitones)");
    add("Gain", 16, 1, 16, "Bands", toBody);
    add("Pan Width", 1, 0, 1, "Bands", toBands, {}, "0: each band's filters on the first / last speaker, 1: scattered");
    add("Speaker Layout", 1, 0, 1, "Bands", toBands, {"Ring", "Line"});
    add("Energy Damp", 0, 0, 1, "Bands", toBands, {"Off", "On"}, "Energy shaped by each band's place in the scale");
    add("Damp Curve", 0, -1, 1, "Bands", toBands); add("Damp Invert", 0, 0, 1, "Bands", toBands, {"Off", "On"});
    add("Pluck", 0, 0, 1, "Bands", toBody, {"Off", "On"}, "Each band plucks (an envelope) when its energy rises past the threshold");
    add("Pluck Threshold", 0.07f, 0, 1, "Bands", toBody);
    add("Pluck Attack", 0, 0, 80, "Bands", toBody); add("Pluck Decay", 52, 0, 80, "Bands", toBody);
    add("Pluck Sustain", 0, 0, 1, "Bands", toBody); add("Pluck Release", 26, 0, 80, "Bands", toBody);
    // ── Post Filter (Reaktor library multimode) ────────────────────────────────
    add("Post Type", 0, 0, 8, "Post", toBody, {"LP1", "LP2", "LP4", "BP2", "BP4", "HP1", "HP2", "HP4", "Off"});
    add("Post Cutoff", 127, -52, 127, "Post", toBody); add("Post Reson", 0, 0, 48, "Post", toBody);
    add("Post Gain Correction", 0.504f, 0, 1, "Post", toBody);
    // ── Effects ────────────────────────────────────────────────────────────────
    auto toStages = [this](){ stagesDirty = true; };
    delayOn.set("Delay On", false); reg(delayOn, toStages);
    add("Delay Feedback", 0.4f, 0, 1, "Delay", toStages); add("Delay Wet", 0.3f, 0, 1, "Delay", toStages);
    add("Delay Mode", 6, 0, 6, "Delay", toStages, {"HP2", "BP2", "LP2", "BP4", "BL4", "LP4", "Peak"});
    add("Delay Cutoff", 58, 20, 120, "Delay", toStages); add("Delay Resonance", 0.57f, 0, 0.98f, "Delay", toStages);
    add("Delay Divider", 2.06f, 1, 16, "Delay", toStages); add("Delay Beats", 3.99f, 1, 256, "Delay", toStages);
    verbOn.set("MetaVerb On", true); reg(verbOn, toStages);
    add("MetaVerb Size", 0.865f, 0, 1, "MetaVerb", toStages); add("MetaVerb Lo Cut", 0.005f, 0, 1, "MetaVerb", toStages);
    add("MetaVerb Hi Cut", 0.595f, 0, 1, "MetaVerb", toStages); add("MetaVerb Mix", 0.25f, 0, 1, "MetaVerb", toStages);
    eqOn.set("EQ On", false); reg(eqOn, toStages);
    {
        const char* names[5] = {"Low", "Lo Mid", "Mid", "Hi Mid", "High"};
        const float freqs[5] = {80, 250, 1000, 4000, 12000};
        for(int b = 0; b < 5; b++){
            const bool shelf = scEQEditor::isShelf(b);
            eqGain[b].set(std::string("EQ ") + names[b], 0.0f, -24.0f, 24.0f);
            eqFreq[b].set(std::string("EQ ") + names[b] + " Freq", freqs[b], 20.0f, 20000.0f);
            eqShape[b].set(std::string("EQ ") + names[b] + (shelf ? " Slope" : " Q"), 1.0f, 0.1f, shelf ? 4.0f : 20.0f);
            for(auto* p : {&eqGain[b], &eqFreq[b], &eqShape[b]}) reg(*p, toStages);
        }
    }
    satOn.set("Saturation On", false); reg(satOn, toStages);
    satDrive.set("Saturation Drive", 6.0f, 0.0f, 36.0f); satBias.set("Saturation Bias", 0.0f, 0.0f, 1.0f);
    satTone.set("Saturation Tone", 20000.0f, 500.0f, 20000.0f); satOutput.set("Saturation Output", 0.0f, -24.0f, 12.0f);
    satMix.set("Saturation Mix", 1.0f, 0.0f, 1.0f);
    for(auto* p : {&satDrive, &satBias, &satTone, &satOutput, &satMix}) reg(*p, toStages);
    rvbOn.set("Reverb On", false); reg(rvbOn, toStages);
    rvbMix.set("Reverb Mix", 0.3f, 0.0f, 1.0f); rvbDecay.set("Reverb Decay", 3.0f, 0.05f, 30.0f);
    rvbSize.set("Reverb Size", 30.0f, 0.0f, 60.0f); rvbPredelay.set("Reverb Predelay", 20.0f, 0.0f, 500.0f);
    rvbLate.set("Reverb Late", 0.8f, 0.0f, 1.0f); rvbSpread.set("Reverb Spread", 0.15f, -1.0f, 1.0f);
    rvbLowpass.set("Reverb Lowpass", 10000.0f, 20.0f, 20000.0f); rvbHiDamp.set("Reverb Hi Damp", 6.0f, 0.0f, 60.0f);
    rvbLoDamp.set("Reverb Lo Damp", 0.0f, 0.0f, 60.0f); rvbModRate.set("Reverb Mod Rate", 0.2f, 0.001f, 20.0f);
    rvbSpin.set("Reverb Spin", 1.0f, 0.0f, 20.0f); rvbWander.set("Reverb Wander", 0.25f, 0.0f, 20.0f);
    for(auto* p : {&rvbMix, &rvbDecay, &rvbSize, &rvbPredelay, &rvbLate, &rvbSpread, &rvbLowpass, &rvbHiDamp, &rvbLoDamp, &rvbModRate, &rvbSpin, &rvbWander})
        reg(*p, toStages);

    snaps.setupParameters();
    for(auto* p : {&snaps.snapshotP, &snaps.morphAP, &snaps.morphBP, &snaps.morphCurveP, &snaps.morphSwitchP}) reg(*p, [](){});
    for(auto* p : {&snaps.morphP, &snaps.morphTimeP}) reg(*p, [](){});
    for(auto* p : {&snaps.morphBeatsP, &snaps.morphGlideP}) reg(*p, [](){});

    // ── Modulation: bands are the voices (the LFOs' spatial phase is the indexer) ──
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
    h.voices = [this](){ return bands.get(); };
    // the classic models (Noise toggle + Random Type): the ensemble's random LFOs are
    // stepped, smooth (brownian) and smooth with Random Same (monobrownian)
    mod.unifiedLfoModels = false;
    mod.maxVoices = 128;
    mod.busWidth = 128;
    mod.setup(h, {{"Energy", 1, "", true}, {"Reso", 100, "dB"}, {"Pitch", 48, "st"}}, true, true, true);
    mod.targetOfKey[P("Energy").getEscapedName()] = Energy;
    mod.targetOfKey[P("Reso").getEscapedName()] = Reso;
    mod.changed = [this](){ if(!restoring) sendAll(); };
    // the ensemble's two spatial LFOs: LFO 1 -> Energy (Band Energy), LFO 2 -> Reso (LFO Q)
    mod.lfoCount = 2; mod.envCount = mod.extCount = 0;
    mod.matrix.assign(size_t(mod.sourceCount()) * NUM_TARGETS, 0.0f);
    mod.matrix[0 * NUM_TARGETS + Energy] = 1.0f;
    mod.matrix[1 * NUM_TARGETS + Reso] = 1.0f;
    mod.refresh();
    initial = capture();

    scSnapshots::Host sh;
    sh.folder = "AnimaBox"; sh.title = "AnimaBox";
    sh.capture = [this](){ return capture(); };
    sh.apply = [this](const ofJson& a, const ofJson& b, float t, bool sw){ apply(a, b, t, sw); };
    sh.factory = [this](){ return factory(); };
    sh.bpm = [this](){ return bpm; };
    sh.paramMenu = [this](const std::string& k){ mark(k); menu(k); };
    for(auto* p : {&satOn, &eqOn, &delayOn, &verbOn, &rvbOn}) snaps.morph.bypass(*p);
    sh.morphChanged = [this](){ sendAll(); stagesDirty = true; bandsDirty = true; };
    sh.morphBlocked = [this](const std::string& k) {
        if(k == scale.getEscapedName()) return scaleConnected();
        auto it = published.find(k); return it != published.end() && it->second->hasInConnection();
    };
    snaps.setup(sh);
    // The factory sounds were converted again (FACTORY_REVISION): a library written from older
    // ones is backed up and replaced once (2: the LFOs' band direction, phase and rate wobble; 3: monobrownian lag)
    {
        static constexpr int FACTORY_REVISION = 3;
        const std::filesystem::path dir = ofToDataPath("nodeSnapshots/AnimaBox", true);
        int written = 0;
        try { written = std::stoi(ofBufferFromFile((dir / "factoryRevision.txt").string()).getText()); } catch(...) {}
        if(written < FACTORY_REVISION) {
            try {
                const auto backup = dir / ("Backup before factory " + ofToString(FACTORY_REVISION) + " - " + ofGetTimestampString("%Y%m%d-%H%M%S"));
                bool any = false;
                for(const auto& e : std::filesystem::directory_iterator(dir))
                    if(e.is_regular_file() && e.path().filename().string().rfind("snapshot_", 0) == 0) {
                        if(!any) std::filesystem::create_directories(backup);
                        std::filesystem::copy_file(e.path(), backup / e.path().filename());
                        any = true;
                    }
            } catch(const std::exception& e) { ofLogWarning("animaBox") << "snapshot backup: " << e.what(); }
            snaps.loadFactory();
            std::ofstream((dir / "factoryRevision.txt").string()) << FACTORY_REVISION;
        }
    }
    if(snaps.active < 0 && snaps.used[0]) { snaps.active = 0; snaps.snapshotP.setWithoutEventNotifications(1); }
    redrawDetune(); redrawIndexRandom();
    for(auto& d : vibDraw) d = std::uniform_real_distribution<float>(0, 1)(rng);
    for(auto& o : octDraw) for(auto& d : o) d = std::uniform_real_distribution<float>(0, 1)(rng);
    requireSynthdefs();
}

// ── The bands' pitches (spec_pitch.md) ────────────────────────────────────────
void scAnimaBox::redrawDetune() {
    std::uniform_real_distribution<float> u(-1, 1);
    for(auto& d : detuneDraw) d = u(rng);
}
void scAnimaBox::redrawIndexRandom() {
    std::uniform_real_distribution<float> u(-1, 1);
    for(auto& d : idxRandDraw) d = u(rng);
}
// Pitch Index: band V (1..B) -> its degree index (a whole number)
float scAnimaBox::pitchIndex(int V, int B) const {
    const float f1 = v("Index Cycles"), f2 = v("Index Offset");
    const int rept = std::max(1, rnd(v("Repeat")));
    const int k = rnd(double(V) / rept);
    const double ph = fmodf_(double(k) / (B + 1) * f1 + f2, 1.0);
    // the deterministic curve: edge clip, skew warp, triangle .. sine .. square
    const double a = v("Index Edge"), sk = std::clamp((double)v("Index Skew"), -0.999999, 0.999999), r = v("Index Round");
    double x = (std::clamp(ph, a, 1 - a) - a) / std::max(1e-6, 1 - 2 * a);
    const double c = (sk + 1) / 2;
    const double w = x <= c ? 0.5 * x / c : 0.5 + 0.5 * (x - c) / (1 - c);
    const double g = 2 - std::fabs(sk);
    const double tri = (w < 0.5 ? 2 * w : 2 - 2 * w) / g, sine = (1 - std::cos(TWO_PI * w)) / 2 / g;
    const double y = tri + (sine - tri) * std::clamp(r, 0.0, 1.0), ex = std::clamp(2 - r, 0.0, 1.0);
    const double u = 2 * y - 1, d = (std::copysign(std::pow(std::fabs(u), ex), u) + 1) / 2;
    int vv = rnd((B + 1) * d);
    if(v("Index Invert") > 0.5f) vv = 1 + (B + 1) - vv;
    const int cm = rnd(std::fabs((B + 1) * fmodf_(k, 2) * v("Comb") - vv));
    const double st = f1 != 0 ? v("Step") / f1 : 0.0;
    double val = cm * st;
    const double symm = std::max(1.0f, v("Symmetry")), per = st * (B + 1) / symm;
    if(per != 0) { const double m = fmodf_(val, per), n = std::floor(val / per); val = std::fabs(fmodf_(n, 2) * per - m) * symm; }
    else val = 0;
    val = rnd(std::fabs(val + v("Index Random") * idxRandDraw[(V - 1) % MAX_BANDS]));
    if(std::fabs(v("Index Pow")) > 1e-6) { const double xx = val / (B + 1); val = (B + 1) * tension(xx, v("Index Pow")); }
    const float q = v("Index Quantize");
    if(q > 0) val = q * rnd(val / q);
    return (float)val;
}
void scAnimaBox::computeBands() {
    bandsDirty = false;
    const int B = bands.get();
    auto& bs = bandState;
    // the scale: pitch classes from Root / Octave, or notes
    std::vector<float> sc = scale.get();
    if(sc.empty()) sc = {0};
    int inputMode = rnd(v("Scale Input"));
    if(inputMode == 0) inputMode = std::any_of(sc.begin(), sc.end(), [](float x){ return x >= 12; }) ? 2 : scaleConnected() ? 3 : 1;
    const bool absolute = inputMode == 2;
    if(absolute || inputMode == 3) std::sort(sc.begin(), sc.end());
    const int n = (int)sc.size();
    const float base = absolute ? 0.0f : (inputMode == 3 ? 0.0f : rnd(v("Root"))) + 12.0f * rnd(v("Octave"));
    // transposition: the played note (Global Pitch applied to it once), the faders, the random draw
    float keyNote = 0;
    if(v("Keyboard Transpose") > 0.5f) keyNote = scGlobalPitch::apply(this, lastNote, 0, false) - 60.0f;
    const float trans = keyNote + v("Transpose") + v("Fine Transpose") + transRand;
    const int key = rnd(v("Key")) + rnd(v("Mode"));
    const bool overtones = v("Tuning") > 0.5f;
    const float det = v("Detune") + v("Detune Extra");
    const float vibF = v("Vibrato Rate"), poly = std::clamp(v("Vibrato Poly"), 0.0f, 1.0f);
    const float octOn = v("Octave Random"), octRange = v("Octave Range");
    const float modOffset = 12.0f * rnd(v("Band Octave")) + v("Semitone") + v("Fine");
    const float maxIdx = std::max(1.0f, B * v("Step"));
    for(int i = 0; i < MAX_BANDS; ++i) {
        const int V = i + 1;
        const float idx = pitchIndex(V, B);
        const int deg = (int)idx + key;
        float p; bool ok = true;
        if(overtones) {
            const int kind = rnd(v("Overtones"));
            const float hh = kind == 0 ? 2.0f * deg + 2 : kind == 1 ? 2.0f * deg + 1 : deg + 1.0f;
            ok = hh > 0;
            p = ok ? 1 + 12 * std::log2(hh) : 0;
            if(v("Overtone Round") > 0.5f) p = (float)rnd(p);
            p += rnd(v("Root")) + 12.0f * rnd(v("Octave"));
        } else {
            const int m = (int)std::floor(double(deg) / n);
            p = sc[deg - m * n] + 12.0f * m + base;
        }
        // Poly8: a random octave shift per band
        if(octOn > 0 && octOn > octDraw[0][i]) {
            const float shift = 12.0f * rnd(octRange * octDraw[1][i]);
            p += v("Octave Up Only") > 0.5f ? shift : shift * (2 * rnd(octDraw[2][i]) - 1);
        }
        p += trans + modOffset + det * detuneDraw[i];
        bs.pitch[i] = std::clamp(p, 0.0f, 140.0f);
        bs.sounding[i] = ok && i < B;
        bs.idx[i] = idx;
        bs.idxNorm[i] = std::clamp(idx / maxIdx, 0.0f, 1.0f);
        bs.vibRate[i] = ofLerp(vibF, vibF * vibDraw[i], poly);
        // the ensemble's placement: each band's A filter at w x U(v), B at 1 - w x U(v + 120)
        const float w = v("Pan Width");
        bs.panA[i] = w * lcg(V); bs.panB[i] = 1 - w * lcg(V + 120);
        // Energy Damp (the Band Energy LFO's Damp: each band's place in the scale)
        if(v("Energy Damp") > 0.5f) {
            const float dp = tension(bs.idxNorm[i], v("Damp Curve"));
            bs.damp[i] = v("Damp Invert") > 0.5f ? dp : 1 - dp;
        } else bs.damp[i] = 1;
    }
    for(auto& [s, st] : states) sendBank(s, st);
}

// ── Server ───────────────────────────────────────────────────────────────────
void scAnimaBox::sendBody(ofxSCServer* server, State& st) {
    if(!st.created || !st.body) return;
    scSynthControlBatch b(st.body, server);
    auto set = [&](const std::string& name, float value) {
        auto it = st.bodySent.find(name);
        if(it != st.bodySent.end() && it->second == value) return;
        st.bodySent[name] = value; b.set(name, value);
    };
    const int gb = gateIn.bus(server), pb = pitchIn.bus(server);
    set("gate", first(gate.get(), 0, 0)); set("gate_sel", gb >= 0 ? 1.0f : 0.0f);
    if(gb >= 0) st.body->mapan("gate_ar", gb, 1);
    set("pitch", first(pitch.get(), 0, 60)); set("pitch_sel", pb >= 0 ? 1.0f : 0.0f);
    if(pb >= 0) st.body->mapan("pitch_ar", pb, 1);
    set("drone", drone.get() ? 1.0f : 0.0f);
    set("gtrbus", scGlobalPitch::bus(this, server));
    set("oct", rnd(v("Body Octave"))); set("trans", v("Body Transpose")); set("det", v("Body Detune"));
    set("atk", v("Attack")); set("dec", v("Decay")); set("sus", v("Sustain")); set("rel", v("Release"));
    set("geiger", v("Geiger Regularity"));
    b.set("osc", std::vector<float>{v("Osc A"), v("Osc B")});
    b.set("src", std::vector<float>{v("Source A"), v("Source B")});
    set("livein", st.liveBus >= 0 ? (float)st.liveBus : 0.0f); set("livech", (float)st.liveChannels);
    set("livegain", v("Input Gain")); set("level", v("Body Level"));
    set("out", st.exc->index); set("gatebus", st.gates->index);
    set("vel", mod.sourceBus(server, scModulation::SRC_VEL)); set("rnd", mod.sourceBus(server, scModulation::SRC_RND));
    set("vis", st.vis->index + VIS_BODY);
    b.send();
}
void scAnimaBox::sendBank(ofxSCServer* server, State& st) {
    if(!st.created) return;
    const auto& bs = bandState;
    const int B = bands.get();
    for(int c = 0; c < MAX_CHUNKS; ++c) if(auto* s = st.bank[c]) {
        scSynthControlBatch b(s, server);
        auto& sent = st.sent[c];
        auto setv = [&](const std::string& name, std::vector<float> value) {
            auto it = sent.find(name);
            if(it != sent.end() && it->second == value) return;
            sent[name] = value; b.set(name, value);
        };
        auto set = [&](const std::string& name, float value) { setv(name, {value}); };
        std::vector<float> pitchV(CHUNK), damp(CHUNK), active(CHUNK), pa(CHUNK), pb(CHUNK), vr(CHUNK), energy(CHUNK, v("Energy"));
        for(int i = 0; i < CHUNK; ++i) {
            const int k = c * CHUNK + i;
            const bool in = k < MAX_BANDS;
            pitchV[i] = in ? bs.pitch[k] : 60; damp[i] = in ? bs.damp[k] : 0;
            active[i] = in && k < B && bs.sounding[k] ? 1.0f : 0.0f;
            pa[i] = in ? bs.panA[k] : 0; pb[i] = in ? bs.panB[k] : 1; vr[i] = in ? bs.vibRate[k] : 6;
        }
        setv("pitch", pitchV); setv("damp", damp); setv("active", active); setv("pana", pa); setv("panb", pb);
        setv("vibrate", vr); setv("energy", energy);
        set("first", (float)c * CHUNK);
        set("exc", (float)st.exc->index); set("out", (float)st.bankOut->index);
        set("menergy", (float)mod.targetBus(server, Energy)); set("mq", (float)mod.targetBus(server, Reso));
        set("mpitch", (float)mod.targetBus(server, Pitch)); set("vis", (float)st.vis->index);
        set("pglide", v("Glide") * 0.001f); set("vibamt", v("Vibrato"));
        set("reso", v("Reso")); set("fine", v("Resonance Fine")); set("sdet", v("Stereo Detune"));
        set("gain", v("Gain") * master.get());
        set("ring", v("Speaker Layout") < 0.5f && channels.get() > 2 ? 1.0f : 0.0f);
        set("pluck", v("Pluck")); set("pthr", v("Pluck Threshold"));
        set("patk", v("Pluck Attack")); set("pdec", v("Pluck Decay")); set("psus", v("Pluck Sustain")); set("prel", v("Pluck Release"));
        b.send();
    }
}
void scAnimaBox::sendPost(ofxSCServer* server, State& st) {
    if(!st.created || !st.post) return;
    scSynthControlBatch b(st.post, server);
    auto set = [&](const std::string& name, float value) {
        auto it = st.postSent.find(name);
        if(it != st.postSent.end() && it->second == value) return;
        st.postSent[name] = value; b.set(name, value);
    };
    set("in", st.bankOut->index);
    set("type", rnd(v("Post Type"))); set("cutoff", v("Post Cutoff")); set("reson", v("Post Reson")); set("gainc", v("Post Gain Correction"));
    b.send();
}
void scAnimaBox::flushSound() {
    if(bandsDirty) computeBands();
    if(soundDirty) {
        soundDirty = false;
        for(auto& [s, st] : states) { sendBody(s, st); sendBank(s, st); sendPost(s, st); }
        stagesDirty = true;
    }
    if(stagesDirty) { stagesDirty = false; for(auto& [s, st] : states) if(st.created) updateStages(s, st); }
}
void scAnimaBox::buildSynth(ofxSCServer* server) {
    if(!server) return;
    auto& st = states[server]; release(server, st); st.root = new ofxSCGroup(server);
}
void scAnimaBox::createSynth(ofxSCServer* server) {
    auto it = states.find(server); if(it == states.end() || !it->second.root) return;
    auto& st = it->second;
    if(placeBefore >= 0) st.root->create(2, placeBefore); else st.root->create(0, 1);
    for(auto*& g : st.groups) { g = new ofxSCGroup(server); g->create(1, st.root->nodeID); }
    st.gates = new ofxSCBus(RATE_AUDIO, 128, server);
    st.exc = new ofxSCBus(RATE_AUDIO, 2, server);
    st.bankOut = new ofxSCBus(RATE_AUDIO, channels.get(), server);
    st.vis = new ofxSCBus(RATE_CONTROL, VIS_SIZE, server);
    if(!liveBus(server, st.gates, true) || !liveBus(server, st.exc, true) || !liveBus(server, st.bankOut, true) || !liveBus(server, st.vis, false)) { release(server, st); return; }
    st.builtBands = bands.get(); st.builtChannels = channels.get(); st.created = true;
    mod.create(server, st.groups[SlSrc]->nodeID, st.groups[SlRoute]->nodeID, st.gates->index);
    st.body = new ofxSCSynth("AnimaBody", server);
    const int chunks = (st.builtBands + CHUNK - 1) / CHUNK;
    for(int c = 0; c < chunks; ++c) st.bank[c] = new ofxSCSynth("AnimaBank16_" + ofToString(st.builtChannels), server);
    st.post = new ofxSCSynth("AnimaPost" + ofToString(st.builtChannels), server);
    if(bandsDirty) computeBands();
    sendBody(server, st); sendBank(server, st); sendPost(server, st);
    st.body->createAndRun(1, st.groups[SlBody]->nodeID, true);
    for(int c = 0; c < chunks; ++c) st.bank[c]->createAndRun(1, st.groups[SlBank]->nodeID, true);
    st.post->createAndRun(1, st.groups[SlPost]->nodeID, true);
    updateStages(server, st);
    if(!runsOn(server)) st.root->run(false);
}
void scAnimaBox::release(ofxSCServer* server, State& st) {
    if(st.created && st.root) st.root->free();
    if(st.created) mod.free(server, false);
    for(auto& f : st.fx) { delete f.synth; f.synth = nullptr; dropBus(server, f.in, true); f.running = false; f.offAt = -1; }
    delete st.body; st.body = nullptr; delete st.post; st.post = nullptr;
    for(auto*& b : st.bank) { delete b; b = nullptr; }
    for(auto& m : st.sent) m.clear();
    st.bodySent.clear(); st.postSent.clear();
    for(auto*& g : st.groups) { delete g; g = nullptr; }
    delete st.root; st.root = nullptr; st.created = false;
    dropBus(server, st.gates, true); dropBus(server, st.exc, true); dropBus(server, st.bankOut, true); dropBus(server, st.vis, false);
}
void scAnimaBox::free(ofxSCServer* s) { auto it = states.find(s); if(it != states.end()) { release(s, it->second); it->second.out = -1; } }
void scAnimaBox::rebuild() {
    structureDirty = false; mod.voicesChanged(); bandsDirty = true;
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
void scAnimaBox::moveSynthBefore(ofxSCServer* s, int id) { auto it = states.find(s); if(it != states.end() && it->second.created) it->second.root->moveBefore(id); }
int scAnimaBox::getLastSynthID(ofxSCServer* s) { auto it = states.find(s); return it != states.end() && it->second.created ? it->second.root->nodeID : -1; }
void scAnimaBox::setOutputBus(ofxSCServer* s, int, int bus) { auto& st = states[s]; st.out = bus; if(st.created) routeChain(st); }
int scAnimaBox::getOutputBusIndex(ofxSCServer* s, int) { auto it = states.find(s); return it == states.end() ? -1 : it->second.out; }
void scAnimaBox::setInputBus(ofxSCServer* s, scNode* node, int bus) {
    auto& st = states[s];
    if(gateIn.take(s, node, bus) | pitchIn.take(s, node, bus)) { st.bodySent.clear(); sendBody(s, st); return; }
    // the live input (In): its channel count from the source port
    st.liveBus = bus; st.liveChannels = 1;
    const auto& in = inputs.empty() ? nodePort() : inputs[0].get();
    if(in.getNodeRef()) st.liveChannels = std::max(1, in.getNodeRef()->getOutputNumChannels(in.getIndex()));
    st.bodySent.erase("livein"); st.bodySent.erase("livech");
    sendBody(s, st);
}
void scAnimaBox::resetInputBusses(ofxSCServer* s, int) {
    gateIn.reset(s); pitchIn.reset(s);
    auto& st = states[s]; st.liveBus = -1; st.bodySent.clear(); sendBody(s, st);
}
void scAnimaBox::activate() { for(auto& [s, st] : states) if(st.created) st.root->run(runsOn(s)); }
void scAnimaBox::deactivate() { for(auto& [s, st] : states) if(st.created) st.root->run(false); }
void scAnimaBox::applyGraphSuspended(ofxSCServer* server, bool suspended) {
    setGraphSuspendedFlag(server, suspended);
    auto it = states.find(server);
    if(it != states.end() && it->second.created && it->second.root) it->second.root->run(runsOn(server));
}
void scAnimaBox::setBpm(float value) {
    const float b = std::max(1.0f, value);
    if(b == bpm) return;
    bpm = b; mod.setBpm(bpm); stagesDirty = true;
}
void scAnimaBox::resendParametersForNRT() {
    for(auto& [s, st] : states) { for(auto& m : st.sent) m.clear(); st.bodySent.clear(); st.postSent.clear(); }
    mod.resend(); bandsDirty = true; sendAll(); flushSound();
}
#ifdef OFXOCEANODE_HAS_TIMELINE_INSTRUMENT
bool scAnimaBox::getTimelineInstrument(ofxOceanodeTimelineInstrument& out) {
    if(!gateIn.handle || !pitchIn.handle) return false;
    out.gate = gateIn.handle.get(); out.pitch = pitchIn.handle.get(); out.velocity = nullptr; out.voices = 1;
    return true;
}
#endif

// ── Effects ──────────────────────────────────────────────────────────────────
void scAnimaBox::updateStages(ofxSCServer* server, State& st) {
    if(!st.created) return;
    for(int s = 0; s < NumStages; s++) {
        auto& f = st.fx[s];
        if(stageOn(s) > 0) {
            if(!f.synth && !scFxDefinitionsReady(servers, server)) continue;
            if(!liveBus(server, f.in, true)) {
                delete f.in;
                f.in = new ofxSCBus(RATE_AUDIO, channels.get(), server);
                if(!liveBus(server, f.in, true)) { f.in->free(); delete f.in; f.in = nullptr; continue; }
                if(f.synth) f.synth->set("in", f.in->index);
            }
            if(!f.synth) {
                f.synth = new ofxSCSynth(stageDef(s), server);
                f.synth->set("in", f.in->index);
                f.synth->set("out", std::max(0, st.out));
                sendStage(s, f.synth, server);
                f.synth->createAndRun(1, st.groups[SlFx + s]->nodeID, true);
            } else {
                sendStage(s, f.synth, server);
                if(!f.running) f.synth->run(true);
            }
            f.running = scFxCanRoute(f.synth, server);
            f.offAt = -1.0f;
        } else if(f.synth && f.running && f.offAt < 0.0f) {
            if(s != StEq) f.synth->set("on", 0.0f);
            f.offAt = ofGetElapsedTimef() + stageTail(s);
        }
    }
    routeChain(st);
}
// the post filter -> the running effects -> out
void scAnimaBox::routeChain(State& st) {
    int next = std::max(0, st.out);
    for(int s = NumStages - 1; s >= 0; s--) {
        auto& f = st.fx[s];
        if(!f.synth || !f.running || !f.in) continue;
        f.synth->set("out", next);
        next = f.in->index;
    }
    if(st.post) st.post->set("out", next);
}
float scAnimaBox::stageOn(int s) const {
    switch(s) {
        case StSat: return snaps.morph.activation(satOn);
        case StEq: return snaps.morph.activation(eqOn);
        case StDelay: return snaps.morph.activation(delayOn);
        case StVerb: return snaps.morph.activation(verbOn);
        default: return snaps.morph.activation(rvbOn);
    }
}
std::string scAnimaBox::stageDef(int s) const {
    static const char* names[NumStages] = {"mixerProSat", "mixerProEQ", "AnimaDelay", "AnimaMetaVerb", "boxMicroSpace"};
    return names[s] + ofToString(channels.get());
}
float scAnimaBox::stageTail(int s) const {
    if(s == StEq || s == StSat) return 0.02f;
    if(s == StDelay) {
        const float sec = v("Delay Beats") / std::max(0.01f, v("Delay Divider")) * 60.0f / std::max(1.0f, bpm);
        const float fb = std::clamp(v("Delay Feedback") * 1.9f, 0.0f, 0.999f);
        return std::min(30.0f, sec * (std::log(0.001f) / std::log(std::max(fb, 0.01f)) + 1) + 0.2f);
    }
    if(s == StVerb) { const float sz = v("MetaVerb Size"); return std::min(30.0f, 0.3f * std::pow(256.0f, sz) + 0.5f); }
    return std::min(30.0f, rvbDecay.get() + rvbPredelay.get() * 0.001f + 0.3f);
}
void scAnimaBox::sendStage(int s, ofxSCSynth* x, ofxSCServer* server) {
    if(!x) return;
    scSynthControlBatch c(x, server);
    const float on = stageOn(s);
    if(s == StSat) {
        c.set("drive", satDrive.get()); c.set("bias", satBias.get()); c.set("tone", satTone.get());
        c.set("autogain", 1.0f); c.set("output", satOutput.get()); c.set("mix", satMix.get()); c.set("on", on);
    } else if(s == StEq) {
        c.set("on", on);
        for(int b = 0; b < 5; b++) {
            const std::string k = "b" + ofToString(b + 1);
            c.set(k + "gain", eqGain[b].get()); c.set(k + "freq", eqFreq[b].get());
            if(scEQEditor::isShelf(b)) c.set(k + "slope", 1.0f / scEQEditor::safeShelfSlope(eqShape[b].get(), eqGain[b].get()));
            else c.set(k + "rq", 1.0f / std::max(0.01f, eqShape[b].get()));
        }
    } else if(s == StDelay) {
        c.set("feedback", v("Delay Feedback")); c.set("wet", v("Delay Wet")); c.set("mode", rnd(v("Delay Mode")));
        c.set("cutoff", v("Delay Cutoff")); c.set("res", v("Delay Resonance"));
        c.set("num", v("Delay Divider")); c.set("den", v("Delay Beats")); c.set("bpm", bpm); c.set("on", on);
    } else if(s == StVerb) {
        c.set("size", v("MetaVerb Size")); c.set("locut", v("MetaVerb Lo Cut")); c.set("hicut", v("MetaVerb Hi Cut"));
        c.set("mix", v("MetaVerb Mix")); c.set("on", on);
    } else {
        c.set("mix", ofClamp(rvbMix.get(), 0.0f, 1.0f)); c.set("decay", std::max(0.05f, rvbDecay.get()));
        c.set("size", rvbSize.get()); c.set("predelay", rvbPredelay.get()); c.set("position", rvbLate.get());
        c.set("spread", rvbSpread.get()); c.set("lowpass", rvbLowpass.get()); c.set("highdamp", rvbHiDamp.get());
        c.set("lowdamp", rvbLoDamp.get()); c.set("modfreq", rvbModRate.get()); c.set("spin", rvbSpin.get());
        c.set("wander", rvbWander.get()); c.set("on", on);
    }
    c.send();
}

// ── Publishing, menus, snapshots ─────────────────────────────────────────────
bool scAnimaBox::connected(const std::string& k) const {
    if(k == scale.getEscapedName()) return scaleConnected();
    auto it = published.find(k); return it != published.end() && (it->second->hasInConnection() || it->second->hasOutConnections());
}
void scAnimaBox::publish(const std::string& k) {
    if(published.count(k) || getParameterGroup().contains(k)) return;
    auto it = std::find_if(controls.begin(), controls.end(), [&](const Control& c){ return c.key == k; }); if(it == controls.end()) return;
    if(!separator) { addSeparator("Published", ofColor(200)); separator = true; }
    published[k] = it->add(); dropdowns.apply(k, published[k]); publishedKeys.push_back(k); parameterGroupChanged.notify(this);
}
void scAnimaBox::unpublish(const std::string& k) {
    auto it = published.find(k); if(it == published.end() || connected(k)) return;
    removeParameter(it->second->getName()); published.erase(it);
    publishedKeys.erase(std::remove(publishedKeys.begin(), publishedKeys.end(), k), publishedKeys.end());
    if(published.empty() && separator) { removeSeparator("Published"); separator = false; }
    parameterGroupChanged.notify(this);
}
void scAnimaBox::mark(const std::string& k) {
    timeline.mark(k);
    if(published.count(k) && !timeline.isHidden(k)) {
        auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y), b, IM_COL32(106, 212, 196, 255), 2);
    }
}
void scAnimaBox::menu(const std::string& k) {
    if(!ImGui::BeginPopupContextItem(("##anima" + k).c_str())) return;
    auto it = std::find_if(controls.begin(), controls.end(), [&](const Control& c){ return c.key == k; });
    if(it != controls.end()) scTyped::jsonField(k, it->save, it->load);
    if(timeline.isHidden(k)) { if(ImGui::MenuItem("Publish to Node GUI")) timeline.reveal(k); }
    else if(published.count(k)) {
        if(connected(k)) ImGui::TextDisabled("Disconnect it to unpublish");
        else if(ImGui::MenuItem("Unpublish from Node")) timeline.unpublish(k);
    } else if(!getParameterGroup().contains(k) && ImGui::MenuItem("Publish to Node GUI")) publish(k);
    timeline.menuItems(k); mod.modulateMenu(k); ImGui::EndPopup();
}
ofJson scAnimaBox::capture() const {
    ofJson j = ofJson::object();
    for(const auto& c : controls) if(!scSnapshots::isOwnKey(c.key) && c.param != &drone) j[c.key] = c.save();
    mod.patchBases(j); mod.save(j, "animaMod"); return j;
}
void scAnimaBox::apply(const ofJson& a, const ofJson& b, float t, bool sw) {
    restoring = true;
    for(const auto& c : controls) {
        if(scSnapshots::isOwnKey(c.key)) continue;
        if(published.count(c.key) && published.at(c.key)->hasInConnection()) continue;
        if(c.param == &scale && scaleConnected()) continue;       // an outer node owns the scale
        if(c.param == &drone) continue;                           // a performance control, not a sound
        auto value = [&](const ofJson& state) -> ofJson { return state.contains(c.key) ? state[c.key] : initial[c.key]; };
        c.load(blend(value(a), value(b), t, sw, c.discrete || c.param == &scale || c.param == &bands));
    }
    // a new band count: the bank and the modulation's voices first, or the modulation sends nothing
    if(structureDirty) rebuild();
    mod.loadMorph(a, b, t, sw, "animaMod", [this](const std::string& key){
        auto it = published.find(key); return it != published.end() && it->second->hasInConnection();
    });
    mod.afterLoad(); mod.refresh();
    restoring = false;
    redrawDetune(); bandsDirty = true; sendAll(); stagesDirty = true;
}
std::vector<ofJson> scAnimaBox::factory() const {
    std::vector<ofJson> out;
    for(const auto& preset : ofJson::parse(animaBoxData::factory)) {
        ofJson j = initial; j["name"] = preset["name"];
        for(auto it = preset["values"].begin(); it != preset["values"].end(); ++it) j[it.key()] = it.value();
        if(preset.contains("matrix")) {
            // sparse: [source, target, amount]
            std::vector<float> m(size_t(mod.sourceCount()) * NUM_TARGETS, 0.0f);
            for(const auto& e : preset["matrix"]) m[size_t(e[0].get<int>()) * NUM_TARGETS + e[1].get<int>()] = e[2].get<float>();
            j["animaModMatrix"] = m;
        }
        out.push_back(j);
    }
    return out;
}
void scAnimaBox::presetSave(ofJson& j) {
    mod.saveLfoOutputs(j);
    j["animaBox"] = capture();
    for(const auto& c : controls) if(scSnapshots::isOwnKey(c.key)) j["animaBox"][c.key] = c.save();
    j["animaBoxPublished"] = publishedKeys; timeline.save(j, "animaBoxTimeline");
    j["animaBoxDrone"] = drone.get();     // a patch's, not a sound's
}
void scAnimaBox::loadBeforeConnections(ofJson& j) {
    mod.loadLfoOutputs(j);
    std::vector<std::string> wanted;
    if(j.contains("animaBoxPublished") && j["animaBoxPublished"].is_array()) for(const auto& k : j["animaBoxPublished"]) if(k.is_string()) wanted.push_back(k.get<std::string>());
    const auto old = publishedKeys; for(const auto& k : old) if(std::find(wanted.begin(), wanted.end(), k) == wanted.end()) unpublish(k);
    for(const auto& k : wanted) publish(k); timeline.load(j, "animaBoxTimeline");
}
void scAnimaBox::presetRecallAfterSettingParameters(ofJson& j) {
    drone = j.value("animaBoxDrone", j.value(drone.getEscapedName(), false));
    if(!j.contains("animaBox")) return;
    auto state = j["animaBox"];
    for(auto it = initial.begin(); it != initial.end(); ++it) if(!state.contains(it.key())) state[it.key()] = it.value();   // a hand-written preset
    apply(state, state, 1, true);
    restoring = true; for(const auto& c : controls) if(scSnapshots::isOwnKey(c.key) && state.contains(c.key)) c.load(state[c.key]); restoring = false;
}
ofxOceanodeNodeModel::MorphKeys scAnimaBox::morphKeys() const { return {{"animaBox"}, {}}; }
ofxOceanodeNodeModel::MorphHint scAnimaBox::morphHint(const ofxOceanodeAbstractParameter& p) const {
    const std::string key = p.getEscapedName();
    if(key == channels.getEscapedName()) return MorphHint::Structural;
    if(key == bands.getEscapedName()) return MorphHint::Switch;
    if(key == show.getEscapedName() || published.count(key)) return MorphHint::Skip;
    return MorphHint::Interpolate;
}
bool scAnimaBox::morphCheck(const ofJson& target, std::string& reason) const {
    if(target.contains("animaBox") && target["animaBox"].is_object()) return true;
    reason = "no saved animaBox sound"; return false;
}
ofJson scAnimaBox::snapshotOf(const ofJson& j) const {
    ofJson s = j.value("animaBox", ofJson::object());
    for(const auto& c : controls) if(scSnapshots::isOwnKey(c.key)) s.erase(c.key);
    return s;
}
bool scAnimaBox::morphBegin(const ofJson& target, std::string& reason) {
    return morphCheck(target, reason) && macroMorph.begin(snaps, snapshotOf(target), reason);
}
void scAnimaBox::requireSynthdefs() {
    for(const auto& entry : std::vector<std::pair<std::string, std::string>>{
        {"AnimaBody.scsyndef", "AnimaBox"}, {"ModLfoX1.scsyndef", "Modulation"}, {"mixerProEQ2.scsyndef", "MixerPro"}}) {
        std::filesystem::path folder;
        try {
            const std::filesystem::path data = ofToDataPath(SYNTHDEF_DIRECTORY, true);
            if(std::filesystem::exists(data)) for(const auto& f : std::filesystem::recursive_directory_iterator(data, std::filesystem::directory_options::skip_permission_denied))
                if(f.is_regular_file() && f.path().filename() == entry.first) { folder = f.path().parent_path(); break; }
            if(folder.empty()) folder = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path().parent_path() / "synthdefs/Defaults" / entry.second / "CompiledSynthdefs" / (entry.second == "MixerPro" ? "mixerpro" : entry.second);
            if(!std::filesystem::exists(folder / entry.first)) { ofLogError("animaBox") << "Compile synthdefs/Defaults/" << entry.second << " first"; continue; }
            for(auto* sm : servers) if(sm) sm->requireSynthdefFolder(folder.string());
        } catch(const std::exception& e) { ofLogError("animaBox") << e.what(); }
    }
}

// ── Frame ────────────────────────────────────────────────────────────────────
void scAnimaBox::update(ofEventArgs&) {
    timeline.update(); snaps.update();
    {
        const auto& names = snaps.slotNames();
        for(const auto& k : snaps.slotKeys()) dropdowns.set(k, names, published.count(k) ? published[k] : nullptr);
    }
    if(structureDirty) rebuild();
    // the random draws (Random Transpose, Octave Random) on the tempo clock
    clockBeats += ofGetLastFrameTime() * bpm / 60.0;
    const long long step = (long long)std::floor(clockBeats / std::max(0.125f, v("Random Every")));
    if(step != lastStep) {
        lastStep = step;
        const float r = v("Random Transpose");
        if(r > 0) {
            float x = std::uniform_real_distribution<float>(-r, r)(rng);
            if(v("Random Abs") > 0.5f) x = std::fabs(x);
            const float q = v("Random Step");
            transRand = q > 0 ? q * rnd(x / q) : x;
            bandsDirty = true;
        } else if(transRand != 0) { transRand = 0; bandsDirty = true; }
        if(v("Octave Random") > 0) {
            for(auto& o : octDraw) for(auto& d : o) d = std::uniform_real_distribution<float>(0, 1)(rng);
            bandsDirty = true;
        }
    }
    mod.showing = show.get(); mod.update();
    flushSound();
    const float now = ofGetElapsedTimef();
    for(auto& [server, st] : states) if(st.created &&
        scFxAwaitingRoute(st.fx, server, [this](int stage){ return stageOn(stage) > 0; }, scFxDefinitionsReady(servers, server)))
        updateStages(server, st);
    for(auto& [server, st] : states) if(st.created) for(auto& f : st.fx) {
        if(f.offAt < 0 || now < f.offAt) continue;
        f.offAt = -1; f.running = false; routeChain(st); if(f.synth) f.synth->run(false);
    }
    if(!show.get()) return;
    for(auto& [s, st] : states) if(st.created && liveBus(s, st.vis, false)) {
        live = st.vis->readValues;
        if(now >= nextRead) { nextRead = now + 1.0f / 25; s->requestControlValues(st.vis->index, VIS_SIZE); }
        break;
    }
}
void scAnimaBox::draw(ofEventArgs&) {
    if(show.get()) drawWindow();
    keyboardPiano.finishFrame();
}
void scAnimaBox::drawWindow() {
    const float z = zoom();
    ImGui::SetNextWindowSize(ImVec2(1250 * z, 860 * z), ImGuiCond_FirstUseEver);
    bool open = show.get();
    if(!ImGui::Begin(getWindowId().c_str(), &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End(); if(!open) show = false; return;
    }
    if(!open) { show = false; ImGui::End(); return; }
    keyboardPiano.beginContent(pitch, gate, 1);
    const float w = ImGui::GetContentRegionAvail().x;
    drawHeader();
    snaps.drawBar(w);
    ImGui::Spacing();
    if(ImGui::BeginTabBar("##animaTabs")) {
        auto tab = [&](const char* name, std::function<void(float)> body) {
            if(!ImGui::BeginTabItem(name)) return;
            ImGui::BeginChild((std::string("##animaTab") + name).c_str(), ImVec2(0, 0), false);
            ImGui::Spacing();
            const float tw = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
            labelW = 70; body(tw);
            ImGui::EndChild(); ImGui::EndTabItem();
        };
        tab("Sound", [this](float tw){ drawSoundTab(tw); });
        tab("Bands", [this](float tw){ drawBandsTab(tw); });
        tab("LFOs", [this](float tw){ mod.drawLfos(tw); });
        tab("Envelopes", [this](float tw){ mod.drawEnvelopes(tw); });
        tab("Routing", [this](float tw){ mod.drawRouting(tw); });
        tab("Effects", [this](float tw){ drawFxTab(tw); });
        ImGui::EndTabBar();
    }
    scKeyboardPiano::endContent();
    ImGui::End();
}
void scAnimaBox::drawHeader() {
    const float z = zoom();
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    auto next = [&](float width, float gap = 12.0f) {
        if(ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + gap * z + width <= right) ImGui::SameLine(0, gap * z);
    };
    labelW = 54;
    ImGui::BeginGroup(); sliderInt(bands, "Bands", 190 * z); ImGui::EndGroup(); next(170 * z);
    ImGui::BeginGroup(); sliderInt(channels, "Channels", 170 * z); ImGui::EndGroup(); next(170 * z);
    ImGui::BeginGroup(); slider(master, "Master", 170 * z, "%.2f"); ImGui::EndGroup(); next(190 * z);
    toggle(drone, "Drone");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Drone: the Body plays continuously. Unchecked: it follows Gate (the keyboard).");
    next(ImGui::CalcTextSize("Reset LFOs").x + ImGui::GetStyle().FramePadding.x * 2);
    if(ImGui::Button("Reset LFOs")) resetLFOs.trigger();
    labelW = 70;
}
void scAnimaBox::sectionHeader(const char* title, float w, ofParameter<bool>* on, ImU32 accent) {
    const float z = zoom();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    const float bh = ImGui::GetFrameHeight() + 6.0f * z;
    dl->AddRectFilled(cur, ImVec2(cur.x + w, cur.y + bh), pal::header, 3.0f * z);
    dl->AddLine(ImVec2(cur.x, cur.y + bh), ImVec2(cur.x + w, cur.y + bh), accent ? accent : pal::accent, 1.5f * z);
    ImGui::SetCursorScreenPos(ImVec2(cur.x + 6.0f * z, cur.y + 3.0f * z));
    if(on) {
        bool val = on->get();
        ImGui::PushID(title);
        if(ImGui::Checkbox("##on", &val)) *on = val;
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s on / off (off: no CPU)", title);
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
void scAnimaBox::slider(ofParameter<float>& p, const char* label, float w, const char* fmt, bool log) {
    const float z = zoom();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(label); mark(key);
    ImGui::SameLine(); ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(30.0f * z, w - labelW * z));
    float val = p.get();
    if(ImGui::SliderFloat("##v", &val, p.getMin(), p.getMax(), fmt, log ? ImGuiSliderFlags_Logarithmic : 0)) p.set(val);
    menu(key);
    ImGui::PopID();
}
void scAnimaBox::sliderInt(ofParameter<int>& p, const char* label, float w) {
    const float z = zoom();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(label); mark(key);
    ImGui::SameLine(); ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(30.0f * z, w - labelW * z));
    int val = p.get();
    if(ImGui::SliderInt("##v", &val, p.getMin(), p.getMax())) p.set(val);
    menu(key);
    ImGui::PopID();
}
void scAnimaBox::toggle(ofParameter<bool>& p, const char* label) {
    bool val = p.get();
    if(ImGui::Checkbox(label, &val)) p.set(val);
    mark(p.getEscapedName()); menu(p.getEscapedName());
}
// one sound control: a slider, a combo (named options) or a log-time slider
void scAnimaBox::control(const std::string& name, float w, const char* label, const char* fmt, bool log) {
    auto it = soundByName.find(name); if(it == soundByName.end()) return;
    auto& c = *it->second;
    const std::string key = c.p.getEscapedName();
    if(!c.discrete) { slider(c.p, label ? label : name.c_str(), w, fmt, log); }
    else {
        const float z = zoom();
        ImGui::PushID(key.c_str());
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(label ? label : name.c_str()); mark(key);
        ImGui::SameLine(); ImGui::SetCursorPosX(x0 + labelW * z);
        ImGui::SetNextItemWidth(std::max(30.0f * z, w - labelW * z));
        const auto items = dropdowns.names(key);
        int k = std::clamp(rnd(c.p.get()), 0, (int)items.size() - 1);
        if(ImGui::Combo("##v", &k, items.data(), (int)items.size())) c.p = float(k);
        menu(key);
        ImGui::PopID();
    }
    if(ImGui::IsItemHovered() && !c.help.empty()) ImGui::SetTooltip("%s", c.help.c_str());
}
void scAnimaBox::scaleEditor(float w) {
    const float z = zoom();
    const std::string key = scale.getEscapedName();
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Scale");
    ImGui::SameLine(); ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, labelW * z - ImGui::CalcTextSize("Scale").x - ImGui::GetStyle().ItemSpacing.x));
    std::string text;
    for(size_t i = 0; i < scale.get().size(); ++i) text += (i ? ", " : "") + ofToString(scale.get()[i], 3);
    char buf[512]; std::snprintf(buf, sizeof(buf), "%s", text.c_str());
    ImGui::SetNextItemWidth(std::max(30.0f * z, w - labelW * z));
    const bool cabled = scaleConnected();
    if(cabled) ImGui::BeginDisabled();
    if(ImGui::InputText("##scale", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::vector<float> vals;
        for(auto& s : ofSplitString(buf, ",", true, true)) if(!s.empty()) vals.push_back(ofToFloat(s));
        if(!vals.empty()) scale = vals;
    }
    if(cabled) ImGui::EndDisabled();
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Pitch classes (0 2 4 7 9; fractions for microtones) or notes. Usually from an outer node (ChordBox's chord).");
}
void scAnimaBox::drawSoundTab(float w) {
    const float cw = colW(w, 5);
    labelW = 92;
    ImGui::BeginGroup();
    sectionHeader("Body", cw);
    for(auto* n : {"Osc A", "Osc B", "Source A", "Source B"}) control(n, cw);
    control("Body Octave", cw, "Octave", "%.0f"); control("Body Transpose", cw, "Transpose", "%.0f st");
    control("Body Detune", cw, "Detune", "%.2f st"); control("Geiger Regularity", cw, "Geiger Reg.");
    control("Attack", cw, nullptr, "%.0f"); control("Decay", cw, nullptr, "%.0f"); control("Sustain", cw); control("Release", cw, nullptr, "%.0f");
    control("Input Gain", cw); control("Body Level", cw);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Envelope times: Reaktor log time, 0 = 1 ms, 20 = 10 ms, 40 = 100 ms, 60 = 1 s, 80 = 10 s");
    nextCol();
    sectionHeader("Harmony", cw);
    scaleEditor(cw);
    for(auto* n : {"Tuning", "Scale Input", "Root"}) control(n, cw);
    control("Octave", cw, nullptr, "%.0f"); control("Key", cw, nullptr, "%.0f"); control("Mode", cw, nullptr, "%.0f");
    if(v("Tuning") > 0.5f) { control("Overtones", cw); control("Overtone Round", cw, "Round"); }
    control("Keyboard Transpose", cw, "Kbd Transpose");
    control("Transpose", cw, nullptr, "%.0f st"); control("Fine Transpose", cw, "Fine", "%.2f st");
    control("Random Transpose", cw, "Rnd Transpose", "%.0f st"); control("Random Step", cw, "Rnd Step", "%.0f st");
    control("Random Abs", cw, "Rnd Up Only"); control("Random Every", cw, "Every (beats)", "%.3f", true);
    control("Octave Random", cw, "Rnd Octave"); control("Octave Range", cw, "Oct Range", "%.0f"); control("Octave Up Only", cw, "Oct Up Only");
    nextCol();
    sectionHeader("Pitch Index", cw);
    for(auto* n : {"Step", "Index Cycles", "Index Offset", "Repeat", "Comb", "Index Invert", "Symmetry", "Index Random",
                   "Index Pow", "Index Quantize", "Index Skew", "Index Round", "Index Edge"}) control(n, cw);
    drawIndexView(cw, 70 * zoom());
    nextCol();
    sectionHeader("Pitch Modifiers", cw);
    control("Band Octave", cw, "Octave", "%.0f"); control("Semitone", cw, nullptr, "%.0f st"); control("Fine", cw, nullptr, "%.2f st");
    control("Detune", cw, nullptr, "%.2f st"); control("Detune Extra", cw, "Detune +", "%.1f st");
    control("Vibrato", cw, nullptr, "%.3f st"); control("Vibrato Rate", cw, "Vib Rate", "%.2f Hz"); control("Vibrato Poly", cw, "Vib Poly");
    control("Glide", cw, nullptr, "%.0f ms");
    ImGui::Spacing();
    sectionHeader("Post Filter", cw);
    control("Post Type", cw, "Type"); control("Post Cutoff", cw, "Cutoff", "%.1f"); control("Post Reson", cw, "Reson", "%.1f dB");
    control("Post Gain Correction", cw, "Gain Corr.");
    nextCol();
    sectionHeader("Bands", cw);
    control("Energy", cw); control("Reso", cw, nullptr, "%.1f dB"); control("Resonance Fine", cw, "Reso Fine", "%.4f");
    control("Stereo Detune", cw, "Pair Detune", "%.2f st"); control("Gain", cw, nullptr, "%.2f");
    control("Pan Width", cw); if(channels.get() > 2) control("Speaker Layout", cw, "Speakers");
    control("Energy Damp", cw); if(v("Energy Damp") > 0.5f) { control("Damp Curve", cw); control("Damp Invert", cw); }
    control("Pluck", cw);
    if(v("Pluck") > 0.5f) {
        control("Pluck Threshold", cw, "Threshold"); control("Pluck Attack", cw, "Attack", "%.0f"); control("Pluck Decay", cw, "Decay", "%.0f");
        control("Pluck Sustain", cw, "Sustain"); control("Pluck Release", cw, "Release", "%.0f");
    }
    ImGui::EndGroup();
    labelW = 70;
    ImGui::Spacing();
    drawBandView(w, 150 * zoom());
}
// the Pitch Index scheme, as the ensemble's display: a bar per band (in band order) as high as
// its pitch index; hover for the band's index, degree and pitch
void scAnimaBox::drawIndexView(float w, float h) {
    const float z = zoom();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    ImGui::InvisibleButton("##indexView", ImVec2(w, h));
    dl->AddRectFilled(a, b, pal::canvas, 3 * z); dl->AddRect(a, b, pal::border, 3 * z);
    const int B = std::max(1, bands.get());
    float top = 1;
    for(int i = 0; i < B; ++i) top = std::max(top, bandState.idx[i]);
    const float bw = (w - 4 * z) / B, pad = 2 * z;
    for(int i = 0; i < B; ++i) {
        const float x0 = a.x + pad + i * bw, x1 = x0 + std::max(1.0f, bw - (bw > 3 * z ? 1 * z : 0));
        const float t = ofClamp(bandState.idx[i] / top, 0.0f, 1.0f);
        const float level = i < (int)live.size() ? ofClamp(live[i], 0.0f, 1.0f) : 0.0f;
        const ImU32 col = bandState.sounding[i] ? (ImU32)ImColor(0.47f + 0.5f * level, 0.78f, 0.75f, 0.55f + 0.45f * level) : IM_COL32(90, 90, 100, 120);
        dl->AddRectFilled(ImVec2(x0, b.y - pad - t * (h - 2 * pad)), ImVec2(x1, b.y - pad), col);
    }
    if(ImGui::IsItemHovered()) {
        const int i = std::clamp(int((ImGui::GetMousePos().x - a.x - pad) / bw), 0, B - 1);
        const float p = bandState.pitch[i];
        const int note = (int)std::round(p);
        ImGui::SetTooltip("Band %d: index %.0f, degree %.0f\npitch %.2f (%s%d)", i + 1, bandState.idx[i],
                          bandState.idx[i] + std::round(v("Key")) + std::round(v("Mode")), p, NOTE_NAMES[(note % 12 + 12) % 12], note / 12 - 1);
    }
}
// every band at its pitch (x), its level now (height), Bands' range lit
void scAnimaBox::drawBandView(float w, float h) {
    const float z = zoom();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    ImGui::InvisibleButton("##bandView", ImVec2(w, h));
    dl->AddRectFilled(a, b, pal::canvas, 3 * z); dl->AddRect(a, b, pal::border, 3 * z);
    const float lo = 12, hi = 132;
    for(int oct = 1; oct <= 10; ++oct) {
        const float x = a.x + w * (12 * oct - lo) / (hi - lo);
        dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), IM_COL32(255, 255, 255, 16));
        dl->AddText(ImVec2(x + 2, b.y - 14 * z), pal::textDim, ("C" + ofToString(oct - 1)).c_str());
    }
    const int B = bands.get();
    for(int i = 0; i < B; ++i) {
        const float p = bandState.pitch[i], x = a.x + w * (p - lo) / (hi - lo);
        const float level = i < (int)live.size() ? ofClamp(live[i], 0.0f, 1.0f) : 0.0f;
        ImColor col = ImColor::HSV(std::fmod(i / std::max(1.0f, (float)B) * 0.8f, 1.0f), 0.45f, 0.95f);
        dl->AddLine(ImVec2(x, b.y - 2 * z), ImVec2(x, b.y - 2 * z - 6 * z), IM_COL32(255, 255, 255, 50));
        if(bandState.sounding[i]) dl->AddRectFilled(ImVec2(x - 1.5f * z, b.y - 18 * z - level * (h - 24 * z)), ImVec2(x + 1.5f * z, b.y - 18 * z), col);
    }
    if(ImGui::IsItemHovered()) {
        const float p = lo + (ImGui::GetMousePos().x - a.x) / w * (hi - lo);
        ImGui::SetTooltip("%d bands, %.1f .. %.1f\nmouse: %.1f (%s%d)", B, *std::min_element(bandState.pitch.begin(), bandState.pitch.begin() + std::max(1, B)),
                          *std::max_element(bandState.pitch.begin(), bandState.pitch.begin() + std::max(1, B)), p, NOTE_NAMES[((int)std::round(p) % 12 + 12) % 12], (int)std::round(p) / 12 - 1);
    }
}
void scAnimaBox::drawBandsTab(float w) {
    drawBandView(w, 260 * zoom());
    ImGui::Spacing();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::TextColored(ImColor(pal::textDim),
        "Each bar is a band at its pitch; its height is its energy now. The bands are the LFOs' voices: "
        "LFO 1 moves the Energy and LFO 2 the Reso of each band (Routing), with the band's phase set by the LFO's "
        "Spread (cycles across the bank), Invert, Symmetry and Random. Bands above Bands are not run.");
    ImGui::PopTextWrapPos();
}
void scAnimaBox::drawFxTab(float w) {
    const float z = zoom();
    const float cw = colW(w, 5);
    labelW = 80;
    ImGui::BeginGroup();
    sectionHeader("Delay", cw, &delayOn);
    if(!delayOn.get()) ImGui::BeginDisabled();
    control("Delay Beats", cw, "Beats", "%.2f"); control("Delay Divider", cw, "Divider", "%.2f");
    control("Delay Feedback", cw, "Feedback"); control("Delay Wet", cw, "Wet"); control("Delay Mode", cw, "Filter");
    control("Delay Cutoff", cw, "Cutoff", "%.1f"); control("Delay Resonance", cw, "Resonance");
    if(!delayOn.get()) ImGui::EndDisabled();
    ImGui::TextColored(ImColor(pal::textDim), "Time: Beats / Divider beats");
    nextCol();
    sectionHeader("MetaVerb", cw, &verbOn);
    if(!verbOn.get()) ImGui::BeginDisabled();
    control("MetaVerb Size", cw, "Size", "%.3f"); control("MetaVerb Lo Cut", cw, "Lo Cut", "%.3f");
    control("MetaVerb Hi Cut", cw, "Hi Cut", "%.3f"); control("MetaVerb Mix", cw, "Mix");
    if(!verbOn.get()) ImGui::EndDisabled();
    nextCol();
    sectionHeader("EQ", cw, &eqOn);
    for(int b = 0; b < 5; b++) eqEditor.bands[b] = {eqFreq[b].get(), eqGain[b].get(), eqShape[b].get()};
    eqEditor.dbRange = 24.0f; eqEditor.gainLimit = 24.0f;
    eqEditor.qMin = 0.1f; eqEditor.qMax = 20.0f; eqEditor.slopeMin = 0.1f; eqEditor.slopeMax = 4.0f;
    float sr = 48000.0f;
    for(auto* sm : servers) if(sm && sm->getServer()) { sr = (float)serverManager::getSampleRateForServer(sm->getServer()); break; }
    eqEditor.recompute(sr > 0 ? sr : 48000.0f);
    eqEditor.bandMenu = [this](int b){
        ImGui::TextUnformatted(scEQEditor::bandName(b)); ImGui::Separator();
        auto value = [this](ofParameter<float>& p, const char* label){
            float val = p.get(); ImGui::SetNextItemWidth(160.0f * zoom());
            if(ImGui::InputFloat(label, &val, 0, 0, "%.3f")) p.set(ofClamp(val, p.getMin(), p.getMax()));
            const auto key = p.getEscapedName(); const bool pub = published.count(key) != 0;
            if(ImGui::MenuItem((std::string(pub ? "Unpublish " : "Publish ") + label).c_str(), nullptr, false, !connected(key))) { if(pub) unpublish(key); else publish(key); }
        };
        value(eqFreq[b], "Freq (Hz)"); value(eqGain[b], "Gain (dB)"); value(eqShape[b], scEQEditor::isShelf(b) ? "Slope" : "Q");
    };
    if(!eqOn.get()) ImGui::BeginDisabled();
    if(eqEditor.draw(cw, 150.0f * z, scEQEditor::defaultColors(), eqOn.get(), nullptr, 0, "##animaEq"))
        for(int b = 0; b < 5; b++) { eqFreq[b] = eqEditor.bands[b].freqHz; eqGain[b] = eqEditor.bands[b].gainDb; eqShape[b] = eqEditor.bands[b].shape; }
    if(!eqOn.get()) ImGui::EndDisabled();
    nextCol();
    sectionHeader("Saturation", cw, &satOn);
    if(!satOn.get()) ImGui::BeginDisabled();
    slider(satDrive, "Drive", cw, "%.1f dB"); slider(satBias, "Bias", cw, "%.2f"); slider(satTone, "Tone", cw, "%.0f Hz", true);
    slider(satOutput, "Output", cw, "%+.1f dB"); slider(satMix, "Mix", cw, "%.2f");
    if(!satOn.get()) ImGui::EndDisabled();
    nextCol();
    sectionHeader("Reverb", cw, &rvbOn);
    if(!rvbOn.get()) ImGui::BeginDisabled();
    slider(rvbMix, "Mix", cw, "%.2f"); slider(rvbDecay, "Decay", cw, "%.2f s", true); slider(rvbSize, "Size", cw, "%.1f");
    slider(rvbPredelay, "Predelay", cw, "%.0f ms"); slider(rvbLate, "Late", cw, "%.2f"); slider(rvbLowpass, "Lowpass", cw, "%.0f Hz", true);
    if(!rvbOn.get()) ImGui::EndDisabled();
    ImGui::EndGroup();
    ImGui::TextColored(ImColor(pal::textDim), "Bands -> Post Filter -> Saturation -> EQ -> Delay -> MetaVerb -> Reverb. An effect switched off costs no CPU.");
    labelW = 70;
}
