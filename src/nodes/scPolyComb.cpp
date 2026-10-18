//
//  scPolyComb.cpp
//  ofxOceanodeSuperCollider
//

#include "scPolyComb.h"
#include "scEnvelopeCurve.h"
#include "scTypedValue.h"
#include "ofxOceanodeShared.h"
#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxSCServer.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <filesystem>
#include <numeric>
#include <random>

namespace {
bool liveControlBus(ofxSCServer* s, ofxSCBus* b) {
    return s && b && b->index >= 0 && b->index < (int)s->controlBusses.size() && s->controlBusses[b->index] == b;
}
bool liveAudioBus(ofxSCServer* s, ofxSCBus* b) {
    return s && b && b->index >= 0 && b->index < (int)s->audioBusses.size() && s->audioBusses[b->index] == b;
}
std::vector<float> expand(const std::vector<float>& v, int n, float fallback = 0.0f) {
    if(v.empty()) return std::vector<float>(n, fallback);
    std::vector<float> out(v);
    out.resize(n, v.back());
    return out;
}
float first(const std::vector<float>& v, float fallback = 0.0f) { return v.empty() ? fallback : v[0]; }

// ── Palette (GrainBox's) ─────────────────────────────────────────────────────
namespace pal {
const ImU32 canvas     = IM_COL32( 11,  12,  14, 255);
const ImU32 border     = IM_COL32( 46,  50,  58, 255);
const ImU32 grid       = IM_COL32(255, 255, 255,  14);
const ImU32 gridStrong = IM_COL32(255, 255, 255,  34);
const ImU32 header     = IM_COL32( 30,  32,  38, 255);
const ImU32 headerText = IM_COL32(205, 205, 215, 255);
const ImU32 textDim    = IM_COL32(150, 150, 160, 255);
const ImU32 accent     = IM_COL32(150, 165, 255, 255);   // periwinkle
const ImU32 envAmp     = IM_COL32(150, 165, 255, 235);
const ImU32 envMod     = IM_COL32(236, 178,  96, 235);
}

// ── Routing ──────────────────────────────────────────────────────────────────
const char* kTargets[scPolyComb::NUM_TARGETS] = {"Pitch", "FM Amt", "FM Ratio", "Cutoff", "Reso", "Comb Pitch",
                                                 "Comb Fb", "Comb Mix", "Ring Mix", "Post Cut", "Drive", "PW", "Amp", "Pan"};
const float kTargetRange[scPolyComb::NUM_TARGETS] = {24, 1, 4, 48, 1, 24, 1, 1, 1, 48, 10, 0.5f, 1, 1};
const char* kTargetUnit[scPolyComb::NUM_TARGETS] = {"st", "", "", "st", "", "st", "", "", "", "st", "", "", "", "of the width"};

const std::vector<const char*> kFilterTypes = {"Low pass", "High pass", "Band pass", "Notch"};
}

// ════════════════════════════════════════════════════════════════════════════
// Setup
// ════════════════════════════════════════════════════════════════════════════

scPolyComb::scPolyComb(std::vector<serverManager*> s) : scNode("polyComb"), servers(std::move(s)) {
    description = "Polyphonic comb-resonator synth. Gate takes Oceanode values and audio-rate gates together. "
                  "Oscillators with FM, filters, comb, ring mod, sine shaper, envelopes, LFOs and a modulation "
                  "matrix, spread over the speakers, then EQ, echo and reverb. Built from small synths: only the "
                  "parts in use run. Show opens the editor; right-click a control there to publish it.";
}

scPolyComb::~scPolyComb() {
    listeners.unsubscribeAll();
    for(auto& [server, st] : states) freeAll(server, st);
    states.clear();
}

void scPolyComb::setup() {
    auto vec = [](ofParameter<std::vector<float>>& p, const std::string& name, float def, float lo, float hi) {
        p.set(name, {def}, {lo}, {hi});
    };
    // what a change resends
    auto toGate = [this]() { forStates([this](ofxSCServer*, ServerState& st){ sendGate(st); }); };
    auto toAmpEnv = [this]() { forStates([this](ofxSCServer*, ServerState& st){ sendAmpEnv(st); }); };
    auto toPitch = [this]() { forStates([this](ofxSCServer*, ServerState& st){ sendPitch(st); }); };
    auto toFm = [this]() { forStates([this](ofxSCServer*, ServerState& st){ sendFm(st); }); };
    auto toAmp = [this]() { forStates([this](ofxSCServer*, ServerState& st){ sendAmp(st); }); };
    auto toSpread = [this]() { forStates([this](ofxSCServer*, ServerState& st){ sendSpread(st); }); };
    auto toOsc = [this](int o) { return [this, o]() { forStates([this, o](ofxSCServer* sv, ServerState& st){ sendOsc(st, o); updateParts(sv, st); }); }; };
    auto toAllOsc = [this]() { forStates([this](ofxSCServer*, ServerState& st){ for(int o = 0; o < NumOsc; o++) sendOsc(st, o); }); };
    auto toIns = [this](int i) { return [this, i]() { forStates([this, i](ofxSCServer*, ServerState& st){ sendIns(st, i); }); }; };
    auto toParts = [this]() { forStates([this](ofxSCServer* sv, ServerState& st){ updateParts(sv, st); }); };

    // ── The node ─────────────────────────────────────────────────────────────
    addParameter(showWindow.set("Show", false));
    addParameter(voices.set("Voices", 4, 1, MAX_VOICES));
    addParameter(outChannels.set("Channels", 2, 1, MAX_VOICES));
    gate.set("Gate", {0.0f}, {0.0f}, {1.0f});
    // Gate (levels: a gate rising to 0.2 plays at 0.2), Pitch, Velocity: Oceanode values or audio cables
    gateIn.attach(addParameter(gate), availableInputs, [this](){ refreshOutputs(); });
    pitchIn.attach(addParameter(pitch.set("Pitch", {60.0f}, {0.0f}, {127.0f})), availableInputs, [this](){ refreshOutputs(); });
    velIn.attach(addParameter(velocity.set("Velocity", {1.0f}, {0.0f}, {1.0f})), availableInputs, [this](){ refreshOutputs(); });
    combPitch.set("Comb Pitch", {0}, {-48}, {127});
    combPitchHandle = addParameter(combPitch);
    addParameter(master.set("Master", 0.8f, 0.0f, 1.0f));
    addParameter(resetLFOs.set("Reset"));
    scNode::addOutput("Out");

    // ── Voices onto the speakers ─────────────────────────────────────────────
    spreadMode.set("Placement", 0, 0, 2);                reg(spreadMode, toSpread);
    spreadWidth.set("Spread", 1.0f, 0.0f, 1.0f);         reg(spreadWidth, toSpread);
    spreadCenter.set("Spread Center", 0.0f, -1.0f, 1.0f); reg(spreadCenter, toSpread);
    panP.set("Pan", {0.5f}, {0.0f}, {1.0f});             reg(panP, toSpread);
    spreadLayout.set("Speaker Layout", 0, 0, 1);         reg(spreadLayout, toSpread);

    // ── Gate, amp envelope, pitch ────────────────────────────────────────────
    // Auto: a drone until something is connected to Gate, then gated
    envMode.set("Env Mode", 0, 0, 2);                      reg(envMode, [this]() { sendEnvMode(true); });
    ampA.set("Amp Attack", 0.01f, 0.001f, 10.0f);          reg(ampA, toAmpEnv);
    ampD.set("Amp Decay", 0.3f, 0.001f, 10.0f);            reg(ampD, toAmpEnv);
    ampS.set("Amp Sustain", 0.8f, 0.0f, 1.0f);             reg(ampS, toAmpEnv);
    ampR.set("Amp Release", 1.0f, 0.001f, 20.0f);          reg(ampR, toAmpEnv);
    ampCA.set("Amp Attack Curve", -4.0f, -12.0f, 12.0f);   reg(ampCA, toAmpEnv);
    ampCD.set("Amp Decay Curve", -4.0f, -12.0f, 12.0f);    reg(ampCD, toAmpEnv);
    ampCR.set("Amp Release Curve", -4.0f, -12.0f, 12.0f);  reg(ampCR, toAmpEnv);
    velSens.set("Vel Sens", 0.5f, 0.0f, 1.0f);             reg(velSens, toAmp);
    glide.set("Glide", 0.0f, 0.0f, 2.0f);                  reg(glide, toPitch);
    vec(transpose, "Transpose", 0, -48, 48);               reg(transpose, toPitch);
    vec(detune, "Detune", 0, 0, 1);                        reg(detune, toPitch);
    scramble.set("Scramble", false);                       reg(scramble, [this, toPitch]() { updateScramble(); toPitch(); });
    vibRate.set("Vib Rate", 5.0f, 0.0f, 20.0f);            reg(vibRate, toPitch);
    vibRand.set("Vib Rate Random", 0.0f, 0.0f, 1.0f);      reg(vibRand, toPitch);
    vibAmt.set("Vib Amount", 0.0f, 0.0f, 2.0f);            reg(vibAmt, toPitch);

    // ── Oscillators (each runs only while its level is up), FM ──────────────
    vec(oscSaw, "Saw", 1, 0, 1);              reg(oscSaw, toOsc(OSaw));
    vec(oscTri, "Tri", 0, 0, 1);              reg(oscTri, toOsc(OTri));
    vec(oscPulse, "Pulse", 0, 0, 1);          reg(oscPulse, toOsc(OPulse));
    vec(oscPw, "Pulse Width", 0.5f, 0.01f, 0.99f); reg(oscPw, toOsc(OPulse));
    vec(oscSine, "Sine", 0, 0, 1);            reg(oscSine, toOsc(OSine));
    vec(oscSSaw, "SuperSaw", 0, 0, 1);        reg(oscSSaw, toOsc(OSSaw));
    vec(oscSSDetune, "SS Detune", 0.5f, 0, 1); reg(oscSSDetune, toOsc(OSSaw));
    vec(oscWhite, "White", 0, 0, 1);          reg(oscWhite, toOsc(OWhite));
    vec(oscPink, "Pink", 0, 0, 1);            reg(oscPink, toOsc(OPink));
    vec(oscDust, "Dust", 0, 0, 1);            reg(oscDust, toOsc(ODust));
    vec(oscImpulse, "Impulse", 0, 0, 1);      reg(oscImpulse, toOsc(OImp));
    vec(oscAmp, "Osc Amp", 0.5f, 0, 1);       reg(oscAmp, toAllOsc);
    fmOn.set("FM On", false);                 reg(fmOn, [this, toParts, toAllOsc]() { toParts(); toAllOsc(); });
    vec(fmAmt, "FM Amount", 0.2f, 0, 1);      reg(fmAmt, toFm);
    vec(fmRatio, "FM Ratio", 1, 0.0625f, 32); reg(fmRatio, toFm);
    fmFb.set("FM Feedback", 0.0f, 0.0f, 1.5f); reg(fmFb, toFm);
    fmSnap.set("FM Ratio Snap", true);        reg(fmSnap, [](){});

    // ── Filters ──────────────────────────────────────────────────────────────
    filterOn.set("Filter On", true);          reg(filterOn, toParts);
    fType.set("Filter Type", 0, 0, 3);        reg(fType, toIns(IFilter));
    vec(cutoff, "Cutoff", 100, 0, 135);       reg(cutoff, toIns(IFilter));
    vec(res, "Resonance", 0.3f, 0, 1);        reg(res, toIns(IFilter));
    keyTrack.set("Key Track", 0.0f, 0.0f, 1.0f); reg(keyTrack, toIns(IFilter));
    postOn.set("Post Filter On", false);      reg(postOn, toParts);
    postType.set("Post Type", 0, 0, 3);       reg(postType, toIns(IPost));
    vec(postCut, "Post Cutoff", 130, 0, 135); reg(postCut, toIns(IPost));
    vec(postRes, "Post Resonance", 0, 0, 1);  reg(postRes, toIns(IPost));

    // ── Resonator, ring, shaper ──────────────────────────────────────────────
    combOn.set("Comb On", false);             reg(combOn, toParts);
    combMode.set("Comb Mode", 0, 0, 1);       reg(combMode, toIns(IComb));
    reg(combPitch, toIns(IComb));
    vec(combTranspose, "Comb Transpose", 0, -48, 48); reg(combTranspose, toIns(IComb));
    vec(combDetune, "Comb Detune", 0, 0, 1);  reg(combDetune, toIns(IComb));
    vec(combFeed, "Comb Feedback", 0.7f, -0.999f, 0.999f); reg(combFeed, toIns(IComb));
    combTone.set("Comb Tone", 1.0f, 0.0f, 1.0f); reg(combTone, toIns(IComb));
    vec(combMix, "Comb Mix", 1, 0, 1);        reg(combMix, toIns(IComb));
    ringOn.set("Ring On", false);             reg(ringOn, toParts);
    vec(ringPitch, "Ring Pitch", 72, 0, 135); reg(ringPitch, toIns(IRing));
    vec(ringTranspose, "Ring Transpose", 0, -48, 48); reg(ringTranspose, toIns(IRing));
    vec(ringDetune, "Ring Detune", 0, 0, 12);  reg(ringDetune, toIns(IRing));
    vec(ringMix, "Ring Mix", 1, 0, 1);        reg(ringMix, toIns(IRing));
    shaperOn.set("Shaper On", false);         reg(shaperOn, toParts);
    vec(drive, "Drive", 1, 0, 30);            reg(drive, toIns(IShaper));
    vec(shaperMix, "Shaper Mix", 1, 0, 1);    reg(shaperMix, toIns(IShaper));

    // ── Effects (MixerPro's inserts) ─────────────────────────────────────────
    auto toStages = [this]() { forStates([this](ofxSCServer* sv, ServerState& st){ if(st.created) updateStages(sv, st); }); };
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

    // ── Snapshots (publishable: Snapshot recalls, Morph blends A into B) ────
    snapshotP.set("Snapshot", 0, 0, NUM_SNAPS);
    reg(snapshotP, [this]() {
        if(snapRecalling || snapshotP.get() <= 0) return;
        if(morphGlideP.get()) startGlide(snapshotP.get() - 1); else recallSnapshot(snapshotP.get() - 1);
    });
    morphP.set("Morph", 0.0f, 0.0f, 1.0f);              reg(morphP, [this]() { applyAB(); });
    morphAP.set("Morph A", 0, 0, NUM_SNAPS);            reg(morphAP, [this]() { applyAB(); });
    morphBP.set("Morph B", 0, 0, NUM_SNAPS);            reg(morphBP, [this]() { applyAB(); });
    morphTimeP.set("Morph Time", 4.0f, 0.0f, 64.0f);    reg(morphTimeP, [](){});
    morphBeatsP.set("Morph Beats", true);               reg(morphBeatsP, [](){});
    morphGlideP.set("Morph Glide", false);              reg(morphGlideP, [](){});

    // ── The shared modulation: LFOs, envelopes, external sources, routing ───
    {
        scModulation::Host h;
        h.regF = [this](ofParameter<float>& p, std::function<void()> f){ reg(p, f); };
        h.control = [this](const std::string& k) -> ofAbstractParameter* {
            for(auto& c : controls) if(c.key == k) return c.param;
            return nullptr;
        };
        h.regI = [this](ofParameter<int>& p, std::function<void()> f){ reg(p, f); };
        h.regB = [this](ofParameter<bool>& p, std::function<void()> f){ reg(p, f); };
        h.regV = [this](ofParameter<std::vector<float>>& p, std::function<void()> f){ reg(p, f); };
        h.publishMenu = [this](const std::string& k){ publishMenu(k); };
        h.publishedUnderline = [this](const std::string& k){ publishedUnderline(k); };
        h.unpublish = [this](const std::string& k){ unpublish(k); };
        h.publishedKeys = [this]() -> const std::vector<std::string>& { return publishedKeys; };
        h.voices = [this](){ return n(); };
        std::vector<scModulation::Target> targets;
        for(int t = 0; t < NUM_TARGETS; t++) targets.push_back({kTargets[t], kTargetRange[t], kTargetUnit[t]});
        modulation.setup(h, targets, true, true, true);
        modulation.lfoCount = 2;
        modulation.envCount = 1;
        modulation.extCount = 2;
        // controls that are routing targets: their right-click menu routes sources to them
        modulation.targetOfKey = {{pitch.getEscapedName(), 0}, {transpose.getEscapedName(), 0}, {fmAmt.getEscapedName(), 1},
                       {fmRatio.getEscapedName(), 2}, {cutoff.getEscapedName(), 3}, {res.getEscapedName(), 4},
                       {combPitch.getEscapedName(), 5}, {combTranspose.getEscapedName(), 5}, {combFeed.getEscapedName(), 6},
                       {combMix.getEscapedName(), 7}, {ringMix.getEscapedName(), 8}, {postCut.getEscapedName(), 9},
                       {drive.getEscapedName(), 10}, {oscPw.getEscapedName(), 11}, {oscAmp.getEscapedName(), 12},
                       {panP.getEscapedName(), 13}};
    }

    // ── Node parameters' listeners ───────────────────────────────────────────
    listeners.push(voices.newListener([this](int&) {
        updateScramble();
        modulation.voicesChanged();   // the LFOs' voice phases, even before anything runs
        rebuildAll();
    }));
    listeners.push(outChannels.newListener([this](int&) { rebuildOutput(); }));
    listeners.push(gate.newListener([this, toGate](std::vector<float>& v) {
        for(float x : v) if(x > 0.0f) gateTouched = true;
        toGate();
    }));
    listeners.push(velocity.newListener([toGate](std::vector<float>&) { toGate(); }));
    listeners.push(pitch.newListener([toPitch](std::vector<float>&) { toPitch(); }));
    listeners.push(master.newListener([toAmp](float&) { toAmp(); }));
    listeners.push(resetLFOs.newListener([this]() { modulation.resetLfos(); }));

    updateScramble();
    requireSynthdefs();
    defaultSnap = captureSnapshot();     // what the factory snapshots start from
    loadSnapshotLibrary();
}

// ════════════════════════════════════════════════════════════════════════════
// Engine: the parts and what they get
// ════════════════════════════════════════════════════════════════════════════

void scPolyComb::forStates(const std::function<void(ofxSCServer*, ServerState&)>& f) {
    for(auto& [server, st] : states) if(st.created) f(server, st);
}
int scPolyComb::src(const ServerState& st, int s) const { return modulation.sourceBus(st.server, s); }
int scPolyComb::mod(const ServerState& st, int t) const { return modulation.targetBus(st.server, t); }

void scPolyComb::sendVec(ofxSCSynth* s, const std::string& name, const std::vector<float>& v) {
    if(!s) return;
    if(v.size() <= 1) s->setMultiple(name, first(v), n());
    else s->set(name, expand(v, n()));
}

void scPolyComb::sendGate(ServerState& st) {
    auto* s = st.gateS;
    if(!s) return;
    sendVec(s, "gate", gate.get());
    sendVec(s, "velocity", velocity.get());
    s->set("drone", droning() ? 1.0f : 0.0f);
    s->set("out", st.gateBus ? st.gateBus->index : 0);
    s->set("vel", src(st, SRC_VEL));
    s->set("rnd", src(st, SRC_RND));
    const int gb = gateIn.bus(st.server), vb = velIn.bus(st.server);
    if(gb >= 0){ s->set("gate_sel", 1.0f); s->mapan("gate_ar", gb, n()); }
    else s->set("gate_sel", 0.0f);
    if(vb >= 0){ s->set("vel_sel", 1.0f); s->mapan("vel_ar", vb, n()); }
    else s->set("vel_sel", 0.0f);
}

void scPolyComb::sendAmpEnv(ServerState& st) {
    auto* s = st.ampEnv;
    if(!s) return;
    s->set("gate", st.gateBus ? st.gateBus->index : 0);
    s->set("out", st.visBlock ? st.visBlock->index + VIS_AMP : 0);
    s->set("aout", st.envBus ? st.envBus->index : 0);
    s->set("atk", ampA.get()); s->set("dec", ampD.get()); s->set("sus", ampS.get()); s->set("rel", ampR.get());
    s->set("cA", ampCA.get()); s->set("cD", ampCD.get()); s->set("cR", ampCR.get());
}




void scPolyComb::sendPitch(ServerState& st) {
    auto* s = st.pitchS;
    if(!s) return;
    s->set("out", st.noteBus ? st.noteBus->index : 0);
    s->set("mpitch", mod(st, 0));
    sendVec(s, "pitch", processedPitch());
    const int pb = pitchIn.bus(st.server);
    if(pb >= 0){ s->set("pitch_sel", 1.0f); s->mapan("pitch_ar", pb, n()); }
    else s->set("pitch_sel", 0.0f);
    sendVec(s, "transpose", transpose.get());
    sendVec(s, "detune", detune.get());
    s->set("glide", glide.get());
    s->set("vibrate", vibRate.get());
    s->set("vibrand", vibRand.get());
    s->set("vibamt", vibAmt.get());
}

void scPolyComb::sendFm(ServerState& st) {
    auto* s = st.fm.synth;
    if(!s) return;
    s->set("note", st.noteBus ? st.noteBus->index : 0);
    s->set("out", st.fmBus ? st.fmBus->index : 0);
    s->set("mamt", mod(st, 1));
    s->set("mratio", mod(st, 2));
    sendVec(s, "fmamt", fmAmt.get());
    sendVec(s, "fmratio", fmRatio.get());
    s->set("fmfb", fmFb.get());
}

namespace {
const char* kOscDefs[] = {"PCOscSaw", "PCOscTri", "PCOscPulse", "PCOscSine", "PCOscSSaw", "PCOscWhite", "PCOscPink", "PCOscDust", "PCOscImp"};
}

bool scPolyComb::oscWanted(int o) const {
    const ofParameter<std::vector<float>>* levels[] = {&oscSaw, &oscTri, &oscPulse, &oscSine, &oscSSaw, &oscWhite, &oscPink, &oscDust, &oscImpulse};
    for(float v : levels[o]->get()) if(v > 0.0001f) return true;
    return false;
}

void scPolyComb::sendOsc(ServerState& st, int o) {
    auto* s = st.osc[o].synth;
    if(!s) return;
    const ofParameter<std::vector<float>>* levels[] = {&oscSaw, &oscTri, &oscPulse, &oscSine, &oscSSaw, &oscWhite, &oscPink, &oscDust, &oscImpulse};
    s->set("note", st.noteBus ? st.noteBus->index : 0);
    s->set("fm", st.fmBus ? st.fmBus->index : 0);
    s->set("fmon", (st.fm.synth && st.fm.running && fmOn.get()) ? 1.0f : 0.0f);
    s->set("out", st.oscBus ? st.oscBus->index : 0);
    sendVec(s, "level", levels[o]->get());
    sendVec(s, "oscamp", oscAmp.get());
    if(o == OPulse){ sendVec(s, "pw", oscPw.get()); s->set("mpw", mod(st, 11)); }
    if(o == OSSaw) sendVec(s, "ssdetune", oscSSDetune.get());
}

bool scPolyComb::insWanted(int i) const {
    switch(i){
        case IFilter: return filterOn.get();
        case IComb: return combOn.get();
        case IRing: return ringOn.get();
        case IPost: return postOn.get();
        default: return shaperOn.get();
    }
}

void scPolyComb::sendIns(ServerState& st, int i) {
    auto* s = st.ins[i].synth;
    if(!s) return;
    s->set("on", insWanted(i) ? 1.0f : 0.0f);
    s->set("note", st.noteBus ? st.noteBus->index : 0);
    switch(i){
        case IFilter:
            s->set("type", (float)fType.get());
            sendVec(s, "cutoff", cutoff.get());
            sendVec(s, "res", res.get());
            s->set("keytrack", keyTrack.get());
            s->set("mcut", mod(st, 3)); s->set("mres", mod(st, 4));
            break;
        case IPost:
            s->set("type", (float)postType.get());
            sendVec(s, "cutoff", postCut.get());
            sendVec(s, "res", postRes.get());
            s->set("keytrack", 0.0f);
            s->set("mcut", mod(st, 9)); s->set("mres", mod(st, ZERO_TARGET));
            break;
        case IComb:
            s->set("combmode", (float)combMode.get());
            sendVec(s, "combpitch", combPitch.get());
            sendVec(s, "combtranspose", combTranspose.get());
            sendVec(s, "combdetune", combDetune.get());
            sendVec(s, "combfeed", combFeed.get());
            s->set("combtone", combTone.get());
            sendVec(s, "combmix", combMix.get());
            s->set("mcpitch", mod(st, 5)); s->set("mcfb", mod(st, 6)); s->set("mcmix", mod(st, 7));
            break;
        case IRing:
            sendVec(s, "ringpitch", ringPitch.get());
            sendVec(s, "ringtranspose", ringTranspose.get());
            sendVec(s, "ringdetune", ringDetune.get());
            sendVec(s, "ringmix", ringMix.get());
            s->set("mrmix", mod(st, 8));
            break;
        default:
            sendVec(s, "drive", drive.get());
            sendVec(s, "shapermix", shaperMix.get());
            s->set("mdrive", mod(st, 10));
            break;
    }
}

void scPolyComb::sendAmp(ServerState& st) {
    auto* s = st.amp;
    if(!s) return;
    s->set("env", st.envBus ? st.envBus->index : 0);
    s->set("gate", st.gateBus ? st.gateBus->index : 0);
    s->set("vel", src(st, SRC_VEL));
    s->set("velsens", velSens.get());
    s->set("level", master.get());
    s->set("mamp", mod(st, 12));
}

void scPolyComb::sendSpread(ServerState& st) {
    auto* s = st.spread;
    if(!s) return;
    s->set("in", st.voiceBus ? st.voiceBus->index : 0);
    s->set("voices", (float)n());
    s->set("line", spreadLayout.get() == 1 ? 1.0f : 0.0f);
    s->set("pan", expand(panPositions(), W, 0.5f));
    s->set("mpan", mod(st, 13));
    s->set("vis", st.visBlock ? st.visBlock->index + VIS_PAN : 0);
}


ofxSCSynth* scPolyComb::make(ofxSCServer* server, ServerState& st, const std::string& defName, int slot) {
    auto* s = new ofxSCSynth(defName, server);
    (void)st; (void)slot;
    return s;
}

// The oscillators (by level), FM and the inserts (by their On): created the
// first time they are wanted, paused when not (inserts after a short fade)
void scPolyComb::updateParts(ofxSCServer* server, ServerState& st) {
    if(!st.created) return;
    const float now = ofGetElapsedTimef();
    auto bring = [&](Part& p, bool wanted, const std::string& defName, int slot, float tail, std::function<void()> send){
        if(wanted){
            if(!p.synth){
                p.synth = make(server, st, defName, slot);
                send();
                p.synth->createAndRun(1, st.groups[slot]->nodeID, true);   // tail of its slot
            }else{
                send();
                if(!p.running) p.synth->run(true);
            }
            p.running = true;
            p.offAt = -1.0f;
        }else if(p.synth && p.running && p.offAt < 0.0f){
            send();                                   // "on" 0: an insert fades to dry
            p.offAt = now + tail;
        }
    };
    bring(st.fm, fmOn.get(), def("PCFm"), SlFm, 0.0f, [&](){ sendFm(st); });
    for(int o = 0; o < NumOsc; o++) bring(st.osc[o], oscWanted(o), def(kOscDefs[o]), SlOsc, 0.0f, [&, o](){ sendOsc(st, o); });
    static const char* insDefs[NumIns] = {"PCFilter", "PCComb", "PCRing", "PCFilter", "PCShaper"};
    static const int insSlots[NumIns] = {SlFilter, SlComb, SlRing, SlPost, SlShaper};
    for(int i = 0; i < NumIns; i++) bring(st.ins[i], insWanted(i), def(insDefs[i]), insSlots[i], i == IComb ? 2.0f : 0.1f,
                                          [&, i](){ sendIns(st, i); });
    for(int o = 0; o < NumOsc; o++) sendOsc(st, o);   // FM on / off
    routeVoice(st);
}




// oscillators -> the running inserts, in order -> amp -> the spreader
void scPolyComb::routeVoice(ServerState& st) {
    if(!st.oscBus) return;
    int from = st.oscBus->index;
    for(int i = 0; i < NumIns; i++){
        auto& p = st.ins[i];
        if(!p.synth || !p.running || !st.insBus[i]) continue;
        p.synth->set("in", from);
        p.synth->set("out", st.insBus[i]->index);
        from = st.insBus[i]->index;
    }
    if(st.amp){
        st.amp->set("in", from);
        st.amp->set("out", st.voiceBus ? st.voiceBus->index : 0);
    }
}

void scPolyComb::ensureBuses(ofxSCServer* server, ServerState& st) {
    auto audio = [&](ofxSCBus*& b, int channels){
        if(liveAudioBus(server, b)) return;
        delete b;
        b = new ofxSCBus(RATE_AUDIO, channels, server);
        if(!liveAudioBus(server, b)){ b->free(); delete b; b = nullptr; }
    };
    auto control = [&](ofxSCBus*& b, int channels){
        if(liveControlBus(server, b)) return;
        delete b;
        b = new ofxSCBus(RATE_CONTROL, channels, server);
        if(!liveControlBus(server, b)){ b->free(); delete b; b = nullptr; }
    };
    st.server = server;
    ofxSCBus* oldGate = st.gateBus;
    audio(st.gateBus, W); audio(st.envBus, W); audio(st.fmBus, W); audio(st.oscBus, W); audio(st.voiceBus, W);
    for(auto*& b : st.insBus) audio(b, W);
    control(st.noteBus, 2 * W);
    control(st.visBlock, 2 * W);
    if(st.modCreated && st.gateBus != oldGate) modulation.setGateBus(server, st.gateBus ? st.gateBus->index : 0);
}

// Everything, in its slot: the gate, the sources, the routes, the voice, the
// spreader, the effects
void scPolyComb::createAll(ofxSCServer* server, ServerState& st) {
    ensureBuses(server, st);
    st.builtVoices = n();
    st.builtChannels = outChannels.get();
    auto fixed = [&](ofxSCSynth*& s, const std::string& defName, int slot, std::function<void()> send){
        s = make(server, st, defName, slot);
        send();
        s->createAndRun(1, st.groups[slot]->nodeID, true);
    };
    // the shared modulation first: its buses are what the parts read
    if(!st.modCreated){
        modulation.create(server, st.groups[SlSrc]->nodeID, st.groups[SlRoute]->nodeID, st.gateBus ? st.gateBus->index : 0);
        st.modCreated = true;
    }
    fixed(st.gateS, def("ModGate"), SlGate, [&](){ sendGate(st); });
    fixed(st.ampEnv, def("ModEnvAr"), SlSrc, [&](){ sendAmpEnv(st); });
    fixed(st.pitchS, def("PCPitch"), SlPitch, [&](){ sendPitch(st); });
    fixed(st.amp, def("PCAmp"), SlAmp, [&](){ sendAmp(st); });
    fixed(st.spread, "PolyCombSpread" + ofToString(outChannels.get()), SlSpread, [&](){ sendSpread(st); });
    updateParts(server, st);
    updateStages(server, st);
}

// Free every synth (the groups and buses stay)
void scPolyComb::freeSynths(ServerState& st) {
    auto drop = [](ofxSCSynth*& s){ if(s){ s->free(); delete s; s = nullptr; } };
    drop(st.gateS); drop(st.ampEnv); drop(st.pitchS); drop(st.amp); drop(st.spread);
    auto dropPart = [&](Part& p){ drop(p.synth); p.running = false; p.offAt = -1.0f; };
    dropPart(st.fm);
    for(auto& p : st.osc) dropPart(p);
    for(auto& p : st.ins) dropPart(p);
    for(auto& f : st.fx){ drop(f.synth); f.running = false; f.offAt = -1.0f; }
}

// Everything, with the groups and buses
void scPolyComb::freeAll(ofxSCServer* server, ServerState& st) {
    if(st.root && st.created) st.root->free();          // frees every synth in it
    auto forget = [](ofxSCSynth*& s){ delete s; s = nullptr; };
    forget(st.gateS); forget(st.ampEnv); forget(st.pitchS); forget(st.amp); forget(st.spread);
    if(st.modCreated) modulation.free(server, false);   // the root's /n_free took its synths
    st.modCreated = false;
    forget(st.fm.synth);
    for(auto& p : st.osc){ forget(p.synth); p.running = false; }
    for(auto& p : st.ins){ forget(p.synth); p.running = false; }
    for(auto& f : st.fx){ forget(f.synth); f.running = false; }
    for(auto*& g : st.groups){ delete g; g = nullptr; }
    delete st.root;
    st.root = nullptr;
    st.created = false;
    auto audio = [&](ofxSCBus*& b){ if(b && liveAudioBus(server, b)) b->free(); delete b; b = nullptr; };
    auto control = [&](ofxSCBus*& b){ if(b && liveControlBus(server, b)) b->free(); delete b; b = nullptr; };
    audio(st.gateBus); audio(st.envBus); audio(st.fmBus); audio(st.oscBus); audio(st.voiceBus);
    for(auto*& b : st.insBus) audio(b);
    for(auto& f : st.fx) audio(f.in);
    control(st.noteBus); control(st.visBlock);
}

// A new voice count: every synth again, in its slot (the groups keep the
// node's place in the graph)
void scPolyComb::rebuildAll() {
    forStates([this](ofxSCServer* server, ServerState& st){
        const bool wasLatency = server->getBLatency();
        server->setBLatency(true);
        freeSynths(st);
        modulation.rebuild(server);     // N voices wide
        createAll(server, st);
        server->setBLatency(wasLatency);
    });
}

// A new speaker count: the spreader and the effects again
void scPolyComb::rebuildOutput() {
    forStates([this](ofxSCServer* server, ServerState& st){
        const bool wasLatency = server->getBLatency();
        server->setBLatency(true);
        if(st.spread){ st.spread->free(); delete st.spread; st.spread = nullptr; }
        for(auto& f : st.fx){
            if(f.synth){ f.synth->free(); delete f.synth; f.synth = nullptr; }
            if(f.in && liveAudioBus(server, f.in)) f.in->free();
            delete f.in;
            f.in = nullptr;
            f.running = false; f.offAt = -1.0f;
        }
        st.spread = make(server, st, "PolyCombSpread" + ofToString(outChannels.get()), SlSpread);
        sendSpread(st);
        st.spread->createAndRun(1, st.groups[SlSpread]->nodeID, true);
        st.builtChannels = outChannels.get();
        updateStages(server, st);
        server->setBLatency(wasLatency);
    });
}

void scPolyComb::sendEnvMode(bool force) {
    const int d = droning() ? 1 : 0;
    if(!force && d == sentDrone) return;
    sentDrone = d;
    forStates([d](ofxSCServer*, ServerState& st){ if(st.gateS) st.gateS->set("drone", (float)d); });
}

// ════════════════════════════════════════════════════════════════════════════
// The graph: the node is one group (its slots inside)
// ════════════════════════════════════════════════════════════════════════════

void scPolyComb::buildSynth(ofxSCServer* server) {
    if(!server) return;
    auto& st = states[server];
    freeAll(server, st);
    st.root = new ofxSCGroup(server);
}

void scPolyComb::createSynth(ofxSCServer* server) {
    auto it = states.find(server);
    if(it == states.end() || !it->second.root) return;
    auto& st = it->second;
    st.root->create(0, 1);
    for(auto*& g : st.groups){
        g = new ofxSCGroup(server);
        g->create(1, st.root->nodeID);       // in slot order
    }
    st.created = true;
    if(!getActive()) st.root->run(false);
    createAll(server, st);
}

void scPolyComb::free(ofxSCServer* server) {
    auto it = states.find(server);
    if(it == states.end()) return;
    freeAll(server, it->second);
    it->second.outBus = -1;
}

void scPolyComb::moveSynthBefore(ofxSCServer* server, int nodeID) {
    auto it = states.find(server);
    if(it == states.end() || !it->second.created) return;
    routeChain(it->second);
    it->second.root->moveBefore(nodeID);
}
int scPolyComb::getLastSynthID(ofxSCServer* server) {
    auto it = states.find(server);
    return (it != states.end() && it->second.created && it->second.root) ? it->second.root->nodeID : -1;
}

void scPolyComb::setOutputBus(ofxSCServer* server, int, int bus) {
    auto& st = states[server];
    st.outBus = bus;
    routeChain(st);
}
int scPolyComb::getOutputBusIndex(ofxSCServer* server, int) {
    auto it = states.find(server);
    return it != states.end() ? it->second.outBus : -1;
}

void scPolyComb::setInputBus(ofxSCServer* server, scNode* node, int bus) {
    auto& st = states[server];
    st.server = server;
    if(gateIn.take(server, node, bus) | velIn.take(server, node, bus)) sendGate(st);
    if(pitchIn.take(server, node, bus)) sendPitch(st);
}
void scPolyComb::resetInputBusses(ofxSCServer* server, int) {
    auto& st = states[server];
    st.server = server;
    gateIn.reset(server); pitchIn.reset(server); velIn.reset(server);
    sendGate(st);
    sendPitch(st);
}

void scPolyComb::resendParametersForNRT() {
    forStates([this](ofxSCServer* server, ServerState& st){
        sendGate(st); sendAmpEnv(st); sendPitch(st); sendFm(st); sendAmp(st); sendSpread(st);
        for(int o = 0; o < NumOsc; o++) sendOsc(st, o);
        for(int i = 0; i < NumIns; i++) sendIns(st, i);
        for(int s = 0; s < NumStages; s++) if(st.fx[s].synth) sendStage(s, st.fx[s].synth);
        routeVoice(st);
        routeChain(st);
    });
    modulation.resend();
}

void scPolyComb::activate() { for(auto& [s, st] : states) if(st.created) st.root->run(true); }
void scPolyComb::deactivate() { for(auto& [s, st] : states) if(st.created) st.root->run(false); }

void scPolyComb::setBpm(float bpm) {
    currentBpm = bpm;
    modulation.setBpm(bpm);
    if(echoBeats.get()) sendStageAll(StEcho);
}

// ── Effects (EQ / echo / reverb, at the speaker count) ─────────────────────

void scPolyComb::updateStages(ofxSCServer* server, ServerState& st) {
    if(!st.created) return;
    for(int s = 0; s < NumStages; s++){
        auto& f = st.fx[s];
        if(stageOn(s)){
            if(!liveAudioBus(server, f.in)){
                delete f.in;
                f.in = new ofxSCBus(RATE_AUDIO, outChannels.get(), server);
                if(!liveAudioBus(server, f.in)){ f.in->free(); delete f.in; f.in = nullptr; continue; }
                if(f.synth) f.synth->set("in", f.in->index);
            }
            if(!f.synth){
                f.synth = new ofxSCSynth(stageDef(s), server);
                f.synth->set("in", f.in->index);
                f.synth->set("out", st.outBus);
                sendStage(s, f.synth);
                f.synth->createAndRun(1, st.groups[SlEq + s]->nodeID, true);
            }else{
                sendStage(s, f.synth);
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
void scPolyComb::routeChain(ServerState& st) {
    int next = st.outBus;
    for(int s = NumStages - 1; s >= 0; s--){
        auto& f = st.fx[s];
        if(!f.synth || !f.running || !f.in) continue;
        f.synth->set("out", next);
        next = f.in->index;
    }
    if(st.spread) st.spread->set("out", next);
}

// ════════════════════════════════════════════════════════════════════════════
// Update: parts that faded out pause; the window's live values
// ════════════════════════════════════════════════════════════════════════════

void scPolyComb::update(ofEventArgs&) {
    sendEnvMode();      // Auto follows Gate's connections
    modulation.showing = showWindow.get();
    modulation.update();   // the LFO / envelope live values (window)
    // snapshots: a file dialog (outside ImGui), another node's library change, a glide
    if(snapLoadDialog){
        snapLoadDialog = false;
        ofFileDialogResult r = ofSystemLoadDialog("Snapshots from a bank or an Oceanode preset file", false, snapDir());
        if(r.bSuccess) loadSnapshotBank(r.filePath);
    }
    if(ofGetElapsedTimef() >= snapLibCheckAt){
        snapLibCheckAt = ofGetElapsedTimef() + 1.0f;
        if(snapSetTime() != snapLibTime) loadSnapshotLibrary();
    }
    if(morphRun){
        const float u = (ofGetElapsedTimef() - morphT0) / std::max(0.001f, morphDur);
        morphProgress = ofClamp(u, 0.0f, 1.0f);
        const float e = morphProgress * morphProgress * (3.0f - 2.0f * morphProgress);
        applyState(morphFrom, morphTo, morphProgress >= 1.0f ? 1.0f : e);
        if(morphProgress >= 1.0f){
            morphRun = false;
            activeSnap = morphTarget;
            snapRecalling = true;
            snapshotP.set(morphTarget + 1);
            snapRecalling = false;
        }
    }
    const float now = ofGetElapsedTimef();
    for(auto& [server, st] : states){
        if(!st.created) continue;
        bool reroute = false;
        auto settle = [&](Part& p){
            if(p.offAt < 0.0f || now < p.offAt) return;
            p.offAt = -1.0f;
            p.running = false;
            if(p.synth) p.synth->run(false);
            reroute = true;
        };
        settle(st.fm);
        for(auto& p : st.osc) settle(p);
        for(auto& p : st.ins) settle(p);
        if(reroute){ routeVoice(st); for(int o = 0; o < NumOsc; o++) sendOsc(st, o); }
        for(auto& f : st.fx){
            if(f.offAt < 0.0f || now < f.offAt) continue;
            f.offAt = -1.0f;
            f.running = false;
            routeChain(st);
            if(f.synth) f.synth->run(false);
        }
    }
    visValues.clear();
    if(showWindow.get()){
        for(auto& [server, st] : states){
            if(!st.created || !liveControlBus(server, st.visBlock)) continue;
            visValues = st.visBlock->readValues;
            if(now >= nextVisRead){
                nextVisRead = now + 1.0f / 30.0f;
                server->requestControlValues(st.visBlock->index, n());
                server->requestControlValues(st.visBlock->index + VIS_PAN, n());
            }
            break;
        }
    }
}

bool scPolyComb::stageOn(int s) const {
    return s == StEq ? eqOn.get() : s == StEcho ? echoOn.get() : rvbOn.get();
}

std::string scPolyComb::stageDef(int s) const {
    static const char* names[NumStages] = {"mixerProEQ", "mixerProEcho", "mixerProRvb"};
    return names[s] + ofToString(outChannels.get());
}

float scPolyComb::echoDelaySec() const {
    if(!echoBeats.get()) return ofClamp(echoDelay.get(), 0.005f, 4.0f);
    return ofClamp(echoBeatVal.get() * 60.0f / std::max(1.0f, currentBpm), 0.005f, 4.0f);
}

float scPolyComb::stageTail(int s) const {
    if(s == StEq) return 0.0f;
    if(s == StEcho){
        const float feed = ofClamp(echoFeed.get(), 0.0f, 0.98f);
        const float repeats = feed > 0.001f ? std::log(0.001f) / std::log(feed) : 0.0f;
        return std::min(30.0f, echoDelaySec() * (repeats + 1.0f) + 0.2f);
    }
    return std::min(30.0f, rvbDecay.get() + rvbPredelay.get() * 0.001f + 0.3f);
}

void scPolyComb::sendStage(int s, ofxSCSynth* x) {
    if(!x) return;
    const float on = stageOn(s) ? 1.0f : 0.0f;
    if(s == StEq){
        // mixerProEQ: gains dB, freqs Hz, shelves bNslope (= 1/S), peaks bNrq (= 1/Q)
        for(int b = 0; b < 5; b++){
            const std::string k = "b" + ofToString(b + 1);
            x->set(k + "gain", eqGain[b].get());
            x->set(k + "freq", eqFreq[b].get());
            if(scEQEditor::isShelf(b)) x->set(k + "slope", 1.0f / scEQEditor::safeShelfSlope(eqShape[b].get(), eqGain[b].get()));
            else x->set(k + "rq", 1.0f / std::max(0.01f, eqShape[b].get()));
        }
    }else if(s == StEcho){
        x->set("delay", echoDelaySec());
        x->set("feed", ofClamp(echoFeed.get(), 0.0f, 0.98f));
        x->set("cutoff", ofClamp(echoCutoff.get(), 12.0f, 130.0f));
        x->set("resonance", ofClamp(echoResonance.get(), 0.0f, 0.9f));
        x->set("filtertype", (float)ofClamp(echoFilter.get(), 0, 3));
        x->set("pingpong", ofClamp(echoPingPong.get(), 0.0f, 1.0f));
        x->set("mix", ofClamp(echoMix.get(), 0.0f, 1.0f));
        x->set("on", on);
    }else{
        x->set("mix", ofClamp(rvbMix.get(), 0.0f, 1.0f));
        x->set("decay", std::max(0.05f, rvbDecay.get()));
        x->set("size", ofClamp(rvbSize.get(), 0.0f, 60.0f));
        x->set("predelay", ofClamp(rvbPredelay.get(), 0.0f, 500.0f));
        x->set("position", ofClamp(rvbLate.get(), 0.0f, 1.0f));
        x->set("spread", ofClamp(rvbSpread.get(), -1.0f, 1.0f));
        x->set("lowpass", ofClamp(rvbLowpass.get(), 20.0f, 20000.0f));
        x->set("highdamp", ofClamp(rvbHiDamp.get(), 0.0f, 60.0f));
        x->set("lowdamp", ofClamp(rvbLoDamp.get(), 0.0f, 60.0f));
        x->set("modfreq", ofClamp(rvbModRate.get(), 0.001f, 20.0f));
        x->set("spin", ofClamp(rvbSpin.get(), 0.0f, 20.0f));
        x->set("wander", ofClamp(rvbWander.get(), 0.0f, 20.0f));
        x->set("on", on);
    }
}

void scPolyComb::sendStageAll(int s) {
    for(auto& [server, st] : states){
        auto& f = st.fx[s];
        if(!f.synth) continue;
        sendStage(s, f.synth);
        if(!stageOn(s) && s != StEq) f.synth->set("on", 0.0f);
    }
}

void scPolyComb::presetSave(ofJson& j) {
    auto& c = j["polyComb"] = ofJson::object();
    for(const auto& control : controls) c[control.key] = control.save();
    modulation.patchBases(c);        // a modulated control: its own value
    modulation.patchBases(j);        // (published ones)
    j["polyCombParams"] = modulation.paramsJson();
    j["polyCombMatrix"] = modulation.matrix;
    j["polyCombLfos"] = modulation.lfoCount;
    j["polyCombEnvs"] = modulation.envCount;
    j["polyCombExts"] = modulation.extCount;
    j["polyCombPublished"] = publishedKeys;
}

void scPolyComb::loadBeforeConnections(ofJson& j) {
    if(!j.contains("polyCombPublished") || !j["polyCombPublished"].is_array()) return;
    std::vector<std::string> wanted;
    for(const auto& k : j["polyCombPublished"]) if(k.is_string()) wanted.push_back(k.get<std::string>());
    const auto old = publishedKeys;
    for(const auto& k : old) if(std::find(wanted.begin(), wanted.end(), k) == wanted.end()) unpublish(k);
    for(const auto& k : wanted) publish(k);
}

void scPolyComb::presetRecallAfterSettingParameters(ofJson& j) {
    if(j.contains("polyComb") && j["polyComb"].is_object()){
        restoring = true;
        for(const auto& control : controls)
            if(j["polyComb"].contains(control.key)) control.load(j["polyComb"][control.key]);
        restoring = false;
    }
    if(j.contains("polyCombMatrix") && j["polyCombMatrix"].is_array()){
        try {
            auto m = j["polyCombMatrix"].get<std::vector<float>>();
            // older layouts, to today's sources (LFO 1-8, Env 1-4, Velocity, Random,
            // Ext 1-8, Voice) and targets (with Pan)
            struct Old { int rows, cols; std::vector<int> map; };
            const std::vector<Old> olds = {
                {10, 13, {0, 1, 2, 3, SRC_ENV, SRC_VEL, SRC_RND, SRC_EXT, SRC_EXT + 1, SRC_VOICE}},
                {14, 13, {0, 1, 2, 3, 4, 5, 6, 7, SRC_ENV, SRC_VEL, SRC_RND, SRC_EXT, SRC_EXT + 1, SRC_VOICE}},
                {14, 14, {0, 1, 2, 3, 4, 5, 6, 7, SRC_ENV, SRC_VEL, SRC_RND, SRC_EXT, SRC_EXT + 1, SRC_VOICE}},
                {17, 14, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, SRC_VEL, SRC_RND, SRC_EXT, SRC_EXT + 1, SRC_VOICE}},
            };
            for(const auto& o : olds){
                if((int)m.size() != o.rows * o.cols) continue;
                std::vector<float> wide(NUM_SOURCES * NUM_TARGETS, 0.0f);
                for(int r = 0; r < o.rows; r++)
                    for(int t = 0; t < o.cols; t++) wide[o.map[r] * NUM_TARGETS + t] = m[r * o.cols + t];
                m = wide;
                break;
            }
            m.resize(NUM_SOURCES * NUM_TARGETS, 0.0f);
            modulation.matrix = m;
        } catch(...) {}
    }
    if(j.contains("polyCombLfos") && j["polyCombLfos"].is_number()) modulation.lfoCount = std::clamp(j["polyCombLfos"].get<int>(), 1, NUM_LFOS);
    if(j.contains("polyCombEnvs") && j["polyCombEnvs"].is_number()) modulation.envCount = std::clamp(j["polyCombEnvs"].get<int>(), 0, NUM_ENVS);
    if(j.contains("polyCombExts") && j["polyCombExts"].is_number()) modulation.extCount = std::clamp(j["polyCombExts"].get<int>(), 0, NUM_EXT);
    // a loaded preset starts from Auto's own rule: droning unless Gate is connected
    // (a value it gets from here on switches it to gated)
    gateTouched = false;
    sendEnvMode(true);
    modulation.loadParamsJson(j.contains("polyCombParams") ? j["polyCombParams"] : ofJson::array());
    updateScramble();
    modulation.afterLoad();          // its phases, sources and routes, everything resent
    resendParametersForNRT();
    for(auto& [server, st] : states){
        if(!st.created) continue;
        updateParts(server, st);
        sendSpread(st);
        updateStages(server, st);
    }
}

void scPolyComb::publish(const std::string& key) {
    if(published.count(key)) return;
    auto it = std::find_if(controls.begin(), controls.end(), [&](const Control& c){ return c.key == key; });
    if(it == controls.end() || getParameterGroup().contains(key)) return;
    if(!publishedSeparator){ addSeparator("Published", ofColor(200)); publishedSeparator = true; }
    published[key] = it->add();
    publishedKeys.push_back(key);
    parameterGroupChanged.notify(this);
}

void scPolyComb::unpublish(const std::string& key) {
    auto it = published.find(key);
    if(it == published.end() || isConnected(key)) return;
    removeParameter(it->second->getName());
    published.erase(it);
    publishedKeys.erase(std::remove(publishedKeys.begin(), publishedKeys.end(), key), publishedKeys.end());
    if(published.empty() && publishedSeparator){ removeSeparator("Published"); publishedSeparator = false; }
    parameterGroupChanged.notify(this);
}

bool scPolyComb::isConnected(const std::string& key) const {
    auto it = published.find(key);
    return it != published.end() && it->second && (it->second->hasInConnection() || it->second->hasOutConnections());
}

void scPolyComb::publishMenu(const std::string& key) {
    if(!ImGui::BeginPopupContextItem(("##pcPublish" + key).c_str())) return;
    ImGui::TextDisabled("%s", key.c_str());
    ImGui::Separator();
    // the value, typed
    auto it = std::find_if(controls.begin(), controls.end(), [&](const Control& c){ return c.key == key; });
    if(it != controls.end()) scTyped::jsonField(key, it->save, it->load);
    if(published.count(key)){
        if(isConnected(key)) ImGui::TextDisabled("Disconnect it to unpublish");
        else if(ImGui::MenuItem("Unpublish from Node")) unpublish(key);
    }else if(getParameterGroup().contains(key)) ImGui::TextDisabled("On the node");
    else if(ImGui::MenuItem("Publish to Node")) publish(key);
    modulation.modulateMenu(key);
    ImGui::EndPopup();
}

void scPolyComb::publishedUnderline(const std::string& key) const {
    if(!published.count(key)) return;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y - 1.0f), ImVec2(b.x, b.y - 1.0f), ImGui::GetColorU32(ImGuiCol_Text));
}

std::vector<float> scPolyComb::panPositions() const {
    const int nv = n(), c = outChannels.get();
    const bool ring = c > 2 && spreadLayout.get() == 0;
    std::vector<float> p(nv, 0.5f);
    if(spreadMode.get() == 2) return expand(panP.get(), nv, 0.5f);
    for(int i = 0; i < nv; i++){
        if(spreadMode.get() == 1){                       // each voice on a channel
            const int ch = i % std::max(1, c);
            p[i] = c <= 1 ? 0.5f : ring ? (float)ch / c : (float)ch / (c - 1);
        }else if(ring){                                  // evenly around the ring
            p[i] = (float)i / nv * spreadWidth.get() + spreadCenter.get() * 0.5f;
        }else{                                           // evenly from left to right
            const float t = nv > 1 ? (float)i / (nv - 1) : 0.5f;
            p[i] = 0.5f + (t - 0.5f) * spreadWidth.get() + spreadCenter.get() * 0.5f;
        }
    }
    return p;
}

std::vector<float> scPolyComb::processedPitch() const {
    const auto& v = pitch.get();
    if(!scramble.get() || v.size() <= 1 || scrambleOrder.empty()) return v;
    std::vector<float> out(v.size());
    for(size_t i = 0; i < v.size(); i++) out[i] = v[std::min((size_t)scrambleOrder[i % scrambleOrder.size()], v.size() - 1)];
    return out;
}

void scPolyComb::updateScramble() {
    scrambleOrder.resize(n());
    std::iota(scrambleOrder.begin(), scrambleOrder.end(), 0);
    if(scramble.get() && n() > 1){
        std::mt19937 rng(0x504f4c59 + n() * 31);
        std::shuffle(scrambleOrder.begin(), scrambleOrder.end(), rng);
    }
}


void scPolyComb::requireSynthdefs() {
    // the voice parts and the shared modulators: found in the data folder, or
    // beside their sources during development
    auto find = [](const std::string& expected, const std::string& bundledDir){
        std::filesystem::path folder;
        try {
            const std::filesystem::path root = ofToDataPath(SYNTHDEF_DIRECTORY, true);
            if(std::filesystem::exists(root))
                for(const auto& e : std::filesystem::recursive_directory_iterator(root,
                        std::filesystem::directory_options::skip_permission_denied))
                    if(e.is_regular_file() && e.path().filename() == expected){ folder = e.path().parent_path(); break; }
            if(folder.empty()){
                const auto bundled = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / bundledDir;
                if(std::filesystem::exists(bundled / expected)) folder = bundled;
            }
        } catch(const std::exception& e) { ofLogWarning("scPolyComb") << e.what(); }
        return folder;
    };
    for(const auto& [expected, dir] : std::vector<std::pair<std::string, std::string>>{
            {"PCAmp2.scsyndef", "synthdefs/Defaults/PolyComb/CompiledSynthdefs/PolyComb"},
            {"ModLfo2.scsyndef", "synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation"}}){
        const auto folder = find(expected, dir);
        if(folder.empty()){ ofLogError("scPolyComb") << "Compile the SynthDefs of " << dir; continue; }
        for(auto* sm : servers) if(sm) sm->requireSynthdefFolder(folder.string());
    }
}

bool scPolyComb::droning() const {
    if(envMode.get() == 2) return true;
    if(envMode.get() == 1) return false;
    if(gateTouched || gateIn.connected()) return false;
    return true;
}

template<typename T>
void scPolyComb::reg(ofParameter<T>& p, std::function<void()> send) {
    Control c;
    c.key = p.getEscapedName();
    c.name = p.getName();
    c.param = &p;
    c.add = [this, &p]() { return addParameter(p); };
    c.save = [&p]() { return ofJson(p.get()); };
    c.load = [&p](const ofJson& j) { try { p.set(j.get<T>()); } catch(...) {} };
    controls.push_back(std::move(c));
    listeners.push(p.newListener([this, send](T&) { if(!restoring) send(); }));
}

// ════════════════════════════════════════════════════════════════════════════
// Window widgets (GrainBox's look: label column | slider, filled headers)
// ════════════════════════════════════════════════════════════════════════════

namespace {
float zoom() { return ofxOceanodeShared::getZoomLevel(); }
float gapW() { return 12.0f * zoom(); }
float colW(float w, int n) { return (w - gapW() * (n - 1)) / n; }
void nextCol() { ImGui::EndGroup(); ImGui::SameLine(0, gapW()); ImGui::BeginGroup(); }
void canvas(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    dl->AddRectFilled(a, b, pal::canvas, 3.0f * zoom());
    dl->AddRect(a, b, pal::border, 3.0f * zoom());
}
// magnitude (dB) of an RBJ 2-pole filter at f
float biquadDb(int type, float f, float fc, float q, float sr = 48000.0f) {
    const float w0 = TWO_PI * fc / sr, alpha = std::sin(w0) / (2.0f * q), c = std::cos(w0);
    float b0, b1, b2, a0 = 1 + alpha, a1 = -2 * c, a2 = 1 - alpha;
    switch(type){
        case 0: b0 = (1 - c) / 2; b1 = 1 - c; b2 = (1 - c) / 2; break;
        case 1: b0 = (1 + c) / 2; b1 = -(1 + c); b2 = (1 + c) / 2; break;
        case 2: b0 = alpha; b1 = 0; b2 = -alpha; break;
        default: b0 = 1; b1 = -2 * c; b2 = 1; break;
    }
    const std::complex<float> zz = std::polar(1.0f, -TWO_PI * f / sr);
    const auto h = (b0 + b1 * zz + b2 * zz * zz) / (a0 + a1 * zz + a2 * zz * zz);
    return 20.0f * std::log10(std::max(1e-6f, std::abs(h)));
}
}



void scPolyComb::sectionHeader(const char* title, float w, ofParameter<bool>* on, ImU32 accent) {
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
        publishMenu(on->getEscapedName());
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

void scPolyComb::slider(ofParameter<float>& p, const char* label, float w, const char* fmt, bool log) {
    const float z = zoom();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    publishedUnderline(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(30.0f * z, w - labelW * z));
    float v = p.get();
    if(ImGui::SliderFloat("##v", &v, p.getMin(), p.getMax(), fmt, log ? ImGuiSliderFlags_Logarithmic : 0)) p.set(v);
    publishMenu(key);
    ImGui::PopID();
}

// One slider for every voice; a connected (per voice) vector shows its values
// as bars instead (dragging on them sets every voice to one value)
void scPolyComb::sliderVec(ofParameter<std::vector<float>>& p, const char* label, float w, const char* fmt, bool log) {
    const float z = zoom();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    publishedUnderline(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    const float sw = std::max(30.0f * z, w - labelW * z);
    const float lo = first(p.getMin(), 0.0f), hi = first(p.getMax(), 1.0f);
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
        float v = first(values, lo);
        if(ImGui::SliderFloat("##v", &v, lo, hi, fmt, log ? ImGuiSliderFlags_Logarithmic : 0)) p.set({v});
    }
    publishMenu(key);
    ImGui::PopID();
}

void scPolyComb::sliderInt(ofParameter<int>& p, const char* label, float w) {
    const float z = zoom();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    publishedUnderline(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(30.0f * z, w - labelW * z));
    int v = p.get();
    if(ImGui::SliderInt("##v", &v, p.getMin(), p.getMax())) p.set(v);
    publishMenu(key);
    ImGui::PopID();
}

void scPolyComb::combo(ofParameter<int>& p, const char* label, float w, const std::vector<const char*>& items) {
    const float z = zoom();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    publishedUnderline(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(30.0f * z, w - labelW * z));
    int v = std::clamp(p.get(), 0, (int)items.size() - 1);
    if(ImGui::Combo("##v", &v, items.data(), (int)items.size())) p.set(v);
    publishMenu(key);
    ImGui::PopID();
}

void scPolyComb::toggle(ofParameter<bool>& p, const char* label) {
    bool v = p.get();
    if(ImGui::Checkbox(label, &v)) p.set(v);
    publishedUnderline(p.getEscapedName());
    publishMenu(p.getEscapedName());
}

// The ADSR display: drag its points (times, sustain), Alt+drag a segment
// (its curve); each voice's live level as a dot on the right
void scPolyComb::envelopeEditor(const char* id, ofParameter<float>& a, ofParameter<float>& d, ofParameter<float>& s,
                                ofParameter<float>& r, ofParameter<float>& ca, ofParameter<float>& cd, ofParameter<float>& cr,
                                float w, float h, ImU32 color, int visOffset) {
    const float z = zoom();
    const float meterW = 14.0f * z;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    scEnvCurve::ADSR e{a.get(), d.get(), s.get(), r.get(), {ca.get(), cd.get(), cr.get()}};
    if(scEnvCurve::editADSR(id, ImVec2(w - meterW - 4.0f * z, h), e, color, pal::canvas, false, r.getMax())){
        a = ofClamp(e.a, a.getMin(), a.getMax());
        d = ofClamp(e.d, d.getMin(), d.getMax());
        s = e.s;
        r = ofClamp(e.r, r.getMin(), r.getMax());
        ca = e.curve[0]; cd = e.curve[1]; cr = e.curve[2];
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ma(o.x + w - meterW, o.y), mb(o.x + w, o.y + h);
    canvas(dl, ma, mb);
    const int nv = n();
    for(int v = 0; v < nv; v++){
        const int i = visOffset + v;
        const float level = i < (int)visValues.size() ? ofClamp(visValues[i], 0.0f, 1.0f) : 0.0f;
        const float y = mb.y - 4.0f * z - level * (h - 8.0f * z);
        const float x = ma.x + meterW * (0.25f + 0.5f * (nv > 1 ? (float)v / (nv - 1) : 0.5f));
        dl->AddCircleFilled(ImVec2(x, y), 2.2f * z, level > 0.001f ? color : pal::gridStrong);
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + h + ImGui::GetStyle().ItemSpacing.y));
    ImGui::Dummy(ImVec2(w, 0));
}

// ════════════════════════════════════════════════════════════════════════════
// Window
// ════════════════════════════════════════════════════════════════════════════

void scPolyComb::draw(ofEventArgs&) {
    if(showWindow.get()) drawWindow();
}

void scPolyComb::drawWindow() {
    const float z = zoom();
    ImGui::SetNextWindowSize(ImVec2(1150 * z, 760 * z), ImGuiCond_FirstUseEver);
    bool open = showWindow.get();
    const std::string title = getWindowTitle();
    if(!ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)){
        ImGui::End();
        if(!open) showWindow = false;
        return;
    }
    if(!open){ showWindow = false; ImGui::End(); return; }
    const float w = ImGui::GetContentRegionAvail().x;

    // ── Header: voices, channels, master, gate mode, voices' levels ─────────
    {
        const float x0 = ImGui::GetCursorPosX();
        labelW = 54.0f;
        ImGui::BeginGroup(); sliderInt(voices, "Voices", 170 * z); ImGui::EndGroup();
        ImGui::SameLine(0, 12 * z);
        ImGui::BeginGroup(); sliderInt(outChannels, "Channels", 170 * z); ImGui::EndGroup();
        ImGui::SameLine(0, 12 * z);
        ImGui::BeginGroup(); slider(master, "Master", 170 * z, "%.2f"); ImGui::EndGroup();
        ImGui::SameLine(0, 12 * z);
        labelW = 64.0f;
        ImGui::BeginGroup(); combo(envMode, "Gate Mode", 190 * z, {"Auto", "Gated", "Drone"}); ImGui::EndGroup();
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Auto: a drone while nothing is connected to Gate, gated once something is\n"
                              "Gated: a voice plays while its gate is high\nDrone: every voice always plays");
        ImGui::SameLine(0, 6 * z);
        ImGui::TextColored(ImColor(droning() ? pal::envMod : pal::accent), droning() ? "droning" : "gated");
        ImGui::SameLine(0, 12 * z);
        if(ImGui::Button("Reset LFOs")) resetLFOs.trigger();
        const int nv = n();
        const float mw = std::min(200.0f * z, nv * 14.0f * z);
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + w - mw));
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const float mh = ImGui::GetFrameHeight();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float bw = mw / nv;
        for(int v = 0; v < nv; v++){
            const int i = VIS_AMP + v;
            const float level = i < (int)visValues.size() ? ofClamp(visValues[i], 0.0f, 1.0f) : 0.0f;
            const ImVec2 b0(a.x + v * bw + 1, a.y), b1(a.x + (v + 1) * bw - 1, a.y + mh);
            dl->AddRectFilled(b0, b1, pal::canvas, 2.0f * z);
            dl->AddRectFilled(ImVec2(b0.x, b1.y - level * mh), b1, pal::envAmp, 2.0f * z);
        }
        ImGui::Dummy(ImVec2(mw, mh));
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Each voice's amp envelope");
    }
    drawSnapshotBar(w);
    ImGui::Spacing();

    if(ImGui::BeginTabBar("##pcTabs")){
        auto tab = [&](const char* name, std::function<void(float)> body){
            if(!ImGui::BeginTabItem(name)) return;
            ImGui::BeginChild((std::string("##pcTab") + name).c_str(), ImVec2(0, 0), false);
            ImGui::Spacing();
            const float tw = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
            labelW = 70.0f;
            body(tw);
            ImGui::EndChild();
            ImGui::EndTabItem();
        };
        tab("Oscillators", [this](float tw){ drawOscTab(tw); });
        tab("Envelopes", [this](float tw){ drawEnvTab(tw); });
        tab("Filters", [this](float tw){ drawFilterTab(tw); });
        tab("Resonator", [this](float tw){ drawResonatorTab(tw); });
        tab("LFOs", [this](float tw){ modulation.drawLfos(tw); });
        tab("Routing", [this](float tw){ modulation.drawRouting(tw); });
        tab("Panning", [this](float tw){ drawPanTab(tw); });
        tab("Effects", [this](float tw){ drawFxTab(tw); });
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ── Envelopes: the amp envelope (it follows the gate), then the shared mod
//    envelopes, added one by one and routed in Routing ─────────────────────
void scPolyComb::drawAmpCard(float w) {
    const float z = zoom();
    ImGui::PushID("ampCard");
    ImGui::BeginGroup();
    const ImU32 col = pal::envAmp;
    sectionHeader("Amp Envelope", w, nullptr, col);
    labelW = 58.0f;
    envelopeEditor("##ampEnv", ampA, ampD, ampS, ampR, ampCA, ampCD, ampCR, w, 110 * z, col, VIS_AMP);
    slider(ampA, "Attack", w, "%.3f s", true);
    slider(ampD, "Decay", w, "%.3f s", true);
    slider(ampS, "Sustain", w, "%.2f");
    slider(ampR, "Release", w, "%.3f s", true);
    // the three curves on one row
    {
        ofParameter<float>* cs[3] = {&ampCA, &ampCD, &ampCR};
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Curves");
        ImGui::SameLine();
        ImGui::SetCursorPosX(x0 + labelW * z);
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        const float cw = (w - labelW * z - 2 * sp) / 3.0f;
        const char* fmts[3] = {"A %.1f", "D %.1f", "R %.1f"};
        for(int k = 0; k < 3; k++){
            if(k) ImGui::SameLine();
            ImGui::PushID(k);
            ImGui::SetNextItemWidth(cw);
            float v = cs[k]->get();
            if(ImGui::SliderFloat("##c", &v, -12.0f, 12.0f, fmts[k])) cs[k]->set(v);
            publishedUnderline(cs[k]->getEscapedName());
            publishMenu(cs[k]->getEscapedName());
            ImGui::PopID();
        }
    }
    slider(velSens, "Vel Sens", w, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("How much Velocity sets the level (0: not at all)");
    ImGui::TextColored(ImColor(pal::textDim), "Follows each voice's gate.");
    labelW = 70.0f;
    ImGui::EndGroup();
    ImGui::PopID();
}

void scPolyComb::drawEnvTab(float w) {
    modulation.drawEnvelopes(w, [this](float cw){ drawAmpCard(cw); });
}

void scPolyComb::drawOscTab(float w) {
    const float z = zoom();
    const float qw = colW(w, 4);
    const float mixW = qw * 2 + gapW();
    ImGui::BeginGroup();
    sectionHeader("Oscillators", mixW);
    struct Fader { ofParameter<std::vector<float>>* p; const char* name; ImU32 color; };
    const Fader faders[] = {
        {&oscSaw, "Saw", IM_COL32(150, 165, 255, 255)}, {&oscTri, "Tri", IM_COL32(120, 200, 255, 255)},
        {&oscPulse, "Pulse", IM_COL32(96, 200, 176, 255)}, {&oscSine, "Sine", IM_COL32(170, 208, 112, 255)},
        {&oscSSaw, "S.Saw", IM_COL32(236, 178, 96, 255)}, {&oscWhite, "White", IM_COL32(220, 220, 225, 255)},
        {&oscPink, "Pink", IM_COL32(228, 122, 146, 255)}, {&oscDust, "Dust", IM_COL32(186, 146, 232, 255)},
        {&oscImpulse, "Imp", IM_COL32(232, 146, 104, 255)}, {&oscAmp, "LEVEL", IM_COL32(255, 255, 255, 255)},
    };
    const int nf = (int)(sizeof(faders) / sizeof(faders[0]));
    const float fw = (mixW - (nf - 1) * 5.0f * z) / nf;
    const float fh = 150.0f * z;
    for(int i = 0; i < nf; i++){
        if(i) ImGui::SameLine(0, 5.0f * z);
        auto& p = *faders[i].p;
        const std::string key = p.getEscapedName();
        ImGui::PushID(i);
        ImGui::BeginGroup();
        float v = first(p.get(), 0.0f);
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, faders[i].color);
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, faders[i].color);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.06f, 0.07f, 0.08f, 1.0f));
        if(ImGui::VSliderFloat("##f", ImVec2(fw, fh), &v, 0.0f, 1.0f, p.get().size() > 1 ? "vec" : "%.2f")) p.set({v});
        ImGui::PopStyleColor(3);
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + 2, b.y - (b.y - a.y) * ofClamp(v, 0.f, 1.f)), ImVec2(b.x - 2, b.y - 1),
                                                  (faders[i].color & 0x00FFFFFF) | 0x50000000, 2.0f);
        publishMenu(key);
        const ImVec2 ts = ImGui::CalcTextSize(faders[i].name);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (fw - ts.x) * 0.5f));
        ImGui::TextColored(ImColor(faders[i].color), "%s", faders[i].name);
        publishedUnderline(key);
        ImGui::EndGroup();
        ImGui::PopID();
    }
    const float half = (mixW - gapW()) * 0.5f;
    ImGui::BeginGroup(); sliderVec(oscPw, "Pulse W", half, "%.2f"); ImGui::EndGroup();
    ImGui::SameLine(0, gapW());
    ImGui::BeginGroup(); sliderVec(oscSSDetune, "SS Detune", half, "%.2f"); ImGui::EndGroup();
    nextCol();
    sectionHeader("Pitch", qw);
    labelW = 62.0f;
    slider(glide, "Glide", qw, "%.3f s", true);
    sliderVec(transpose, "Transpose", qw, "%+.2f st");
    sliderVec(detune, "Detune", qw, "%.2f st");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("A random offset per voice, up to this many semitones");
    slider(vibRate, "Vib Rate", qw, "%.2f Hz");
    slider(vibRand, "Rate Rnd", qw, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Each voice its own vibrato rate: Rate x (1 +- up to this much, at random)");
    slider(vibAmt, "Vib Depth", qw, "%.2f st");
    toggle(scramble, "Scramble pitches");
    nextCol();
    sectionHeader("FM", qw, &fmOn);
    sliderVec(fmAmt, "Amount", qw, "%.3f");
    {   // ratio, snapped to useful ratios unless Snap is off
        const std::string key = fmRatio.getEscapedName();
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Ratio");
        publishedUnderline(key);
        ImGui::SameLine();
        ImGui::SetCursorPosX(x0 + labelW * z);
        ImGui::SetNextItemWidth(qw - labelW * z);
        float v = first(fmRatio.get(), 1.0f);
        if(ImGui::SliderFloat("##ratio", &v, 0.0625f, 32.0f, "%.3f", ImGuiSliderFlags_Logarithmic)){
            if(fmSnap.get()){
                static const float ratios[] = {0.0625f, 0.125f, 0.25f, 0.5f, 0.75f, 1, 1.5f, 2, 2.5f, 3, 3.5f, 4, 5, 6, 7, 8,
                                               9, 10, 11, 12, 13, 14, 15, 16, 20, 24, 32};
                float best = ratios[0];
                for(float r : ratios) if(std::abs(std::log(r / v)) < std::abs(std::log(best / v))) best = r;
                v = best;
            }
            fmRatio.set({v});
        }
        publishMenu(key);
    }
    slider(fmFb, "Feedback", qw, "%.2f");
    toggle(fmSnap, "Snap ratio");
    {   // operators: modulator (with its feedback loop) -> oscillators
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float h = 70.0f * z;
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + qw, a.y + h);
        canvas(dl, a, b);
        const float bw = qw * 0.36f, bh = 26.0f * z;
        const ImVec2 m0(a.x + qw * 0.06f, a.y + (h - bh) * 0.5f + 6 * z), m1(m0.x + bw, m0.y + bh);
        const ImVec2 c0(b.x - qw * 0.06f - bw, m0.y), c1(c0.x + bw, m1.y);
        const float amt = ofClamp(first(fmAmt.get()), 0.0f, 1.0f);
        dl->AddRectFilled(m0, m1, IM_COL32(40, 44, 60, 255), 4.0f * z);
        dl->AddRect(m0, m1, pal::accent, 4.0f * z);
        dl->AddRectFilled(c0, c1, IM_COL32(40, 44, 60, 255), 4.0f * z);
        dl->AddRect(c0, c1, pal::headerText, 4.0f * z);
        char t[32];
        std::snprintf(t, sizeof(t), "x%.3g", first(fmRatio.get(), 1.0f));
        dl->AddText(ImVec2(m0.x + 5 * z, m0.y + (bh - ImGui::GetTextLineHeight()) * 0.5f), pal::headerText, t);
        dl->AddText(ImVec2(c0.x + 5 * z, c0.y + (bh - ImGui::GetTextLineHeight()) * 0.5f), pal::headerText, "Oscs");
        const ImU32 arrow = (pal::accent & 0x00FFFFFF) | ((ImU32)(60 + 195 * amt) << 24);
        dl->AddLine(ImVec2(m1.x, (m0.y + m1.y) * 0.5f), ImVec2(c0.x - 4 * z, (c0.y + c1.y) * 0.5f), arrow, (1.0f + 3.0f * amt) * z);
        dl->AddTriangleFilled(ImVec2(c0.x, (c0.y + c1.y) * 0.5f), ImVec2(c0.x - 8 * z, (c0.y + c1.y) * 0.5f - 5 * z),
                              ImVec2(c0.x - 8 * z, (c0.y + c1.y) * 0.5f + 5 * z), arrow);
        if(fmFb.get() > 0.001f){
            const ImVec2 p0(m0.x + bw * 0.75f, m0.y), p3(m0.x + bw * 0.25f, m0.y);
            dl->AddBezierCubic(p0, ImVec2(p0.x, p0.y - 18 * z), ImVec2(p3.x, p3.y - 18 * z), p3, pal::accent, (1.0f + fmFb.get()) * z);
        }
        ImGui::Dummy(ImVec2(qw, h));
    }
    labelW = 70.0f;
    ImGui::EndGroup();
}

void scPolyComb::drawFilterTab(float w) {
    const float z = zoom();
    const float cw = colW(w, 3);
    auto response = [&](int type, float note, float resonance, ImU32 color){
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float h = 90.0f * z;
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + cw, a.y + h);
        canvas(dl, a, b);
        const float fc = ofClamp(440.0f * std::pow(2.0f, (note - 69.0f) / 12.0f), 20.0f, 20000.0f);
        const float q = 0.707f + resonance * resonance * 18.0f;
        for(float f : {100.0f, 1000.0f, 10000.0f}){
            const float x = a.x + cw * std::log(f / 20.0f) / std::log(1000.0f);
            dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), pal::grid);
        }
        const float zeroY = a.y + h * 0.35f;
        dl->AddLine(ImVec2(a.x, zeroY), ImVec2(b.x, zeroY), pal::gridStrong);
        std::vector<ImVec2> pts;
        for(int i = 0; i <= 96; i++){
            const float f = 20.0f * std::pow(1000.0f, i / 96.0f);
            float db = biquadDb(type, f, fc, q);
            if(type <= 1) db *= 2.0f;
            pts.push_back(ImVec2(a.x + cw * i / 96.0f, ofClamp(zeroY - db * h / 60.0f, a.y + 1, b.y - 1)));
        }
        dl->AddPolyline(pts.data(), (int)pts.size(), color, 0, 2.0f * z);
        char t[32];
        std::snprintf(t, sizeof(t), "%.0f Hz", fc);
        dl->AddText(ImVec2(a.x + 4 * z, b.y - ImGui::GetTextLineHeight() - 2 * z), pal::textDim, t);
        ImGui::Dummy(ImVec2(cw, h));
    };
    auto hzFormat = [](const std::vector<float>& v){
        const float hz = 440.0f * std::pow(2.0f, (first(v, 60.0f) - 69.0f) / 12.0f);
        return ofToString(first(v, 60.0f), 1) + "  (" + (hz >= 1000 ? ofToString(hz / 1000.0f, 2) + " kHz)" : ofToString(hz, 0) + " Hz)");
    };
    ImGui::BeginGroup();
    sectionHeader("Filter", cw, &filterOn);
    combo(fType, "Type", cw, kFilterTypes);
    std::string f1 = hzFormat(cutoff.get());
    sliderVec(cutoff, "Cutoff", cw, f1.c_str());
    sliderVec(res, "Resonance", cw, "%.2f");
    slider(keyTrack, "Key Track", cw, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("1: the cutoff follows the note (one octave per octave)");
    response(fType.get(), first(cutoff.get(), 100.0f), first(res.get(), 0.3f), pal::accent);
    nextCol();
    sectionHeader("Post Filter", cw, &postOn);
    combo(postType, "Type", cw, kFilterTypes);
    std::string f2 = hzFormat(postCut.get());
    sliderVec(postCut, "Cutoff", cw, f2.c_str());
    sliderVec(postRes, "Resonance", cw, "%.2f");
    response(postType.get(), first(postCut.get(), 130.0f), first(postRes.get(), 0.0f), pal::envMod);
    ImGui::EndGroup();
    ImGui::Spacing();
    ImGui::TextColored(ImColor(pal::textDim), "Envelopes and LFOs move the cutoffs from Routing (or right-click a Cutoff: Modulate with).");
}

void scPolyComb::drawResonatorTab(float w) {
    const float z = zoom();
    const float cw = colW(w, 3);
    ImGui::BeginGroup();
    sectionHeader("Comb", cw, &combOn);
    combo(combMode, "Pitch", cw, {"Follows the note", "Fixed note"});
    const bool follow = combMode.get() == 0;
    sliderVec(combPitch, follow ? "Interval" : "Note", cw, follow ? "%+.2f st" : "%.2f");
    sliderVec(combTranspose, "Transpose", cw, "%+.2f st");
    sliderVec(combDetune, "Detune", cw, "%.2f st");
    sliderVec(combFeed, "Feedback", cw, "%+.3f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Negative: odd harmonics only (hollow, an octave lower)");
    slider(combTone, "Tone", cw, "%.2f");
    sliderVec(combMix, "Mix", cw, "%.2f");
    {   // the comb's response at middle C (or its fixed note)
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float h = 80.0f * z;
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + cw, a.y + h);
        canvas(dl, a, b);
        const float note = (follow ? 60.0f + first(combPitch.get()) : first(combPitch.get(), 60.0f)) + first(combTranspose.get());
        const float f0 = 440.0f * std::pow(2.0f, (note - 69.0f) / 12.0f);
        const float g = ofClamp(first(combFeed.get(), 0.7f), -0.999f, 0.999f);
        std::vector<ImVec2> pts;
        for(int i = 0; i <= 160; i++){
            const float f = 20.0f * std::pow(1000.0f, i / 160.0f);
            const std::complex<float> e = std::polar(1.0f, -TWO_PI * f / f0);
            const float mag = 1.0f / std::max(1e-3f, std::abs(1.0f - g * e));
            const float db = 20.0f * std::log10(mag * (1.0f - std::abs(g) * 0.9f));
            pts.push_back(ImVec2(a.x + cw * i / 160.0f, ofClamp(b.y - 6 * z - (db + 12.0f) * h / 40.0f, a.y + 1, b.y - 1)));
        }
        dl->AddPolyline(pts.data(), (int)pts.size(), pal::accent, 0, 1.5f * z);
        char t[32];
        std::snprintf(t, sizeof(t), "%.1f Hz%s", f0, follow ? " at C4" : "");
        dl->AddText(ImVec2(a.x + 4 * z, a.y + 2 * z), pal::textDim, t);
        ImGui::Dummy(ImVec2(cw, h));
    }
    nextCol();
    sectionHeader("Ring Modulator", cw, &ringOn);
    sliderVec(ringPitch, "Note", cw, "%.1f");
    sliderVec(ringTranspose, "Transpose", cw, "%+.2f st");
    sliderVec(ringDetune, "Detune", cw, "%.2f st");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("A random offset per voice, up to this many semitones");
    sliderVec(ringMix, "Mix", cw, "%.2f");
    nextCol();
    sectionHeader("Sine Shaper", cw, &shaperOn);
    sliderVec(drive, "Drive", cw, "%.2f");
    sliderVec(shaperMix, "Mix", cw, "%.2f");
    {   // its transfer curve: in -> out
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float h = std::min(cw, 150.0f * z);
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + cw, a.y + h);
        canvas(dl, a, b);
        dl->AddLine(ImVec2(a.x, (a.y + b.y) * 0.5f), ImVec2(b.x, (a.y + b.y) * 0.5f), pal::grid);
        dl->AddLine(ImVec2((a.x + b.x) * 0.5f, a.y), ImVec2((a.x + b.x) * 0.5f, b.y), pal::grid);
        const float dr = first(drive.get(), 1.0f), mix = ofClamp(first(shaperMix.get()), 0.0f, 1.0f);
        const float dry = std::cos(mix * HALF_PI), wet = std::sin(mix * HALF_PI);
        std::vector<ImVec2> pts;
        for(int i = 0; i <= 120; i++){
            const float x = -1.0f + 2.0f * i / 120.0f;
            const float y = ofClamp(dry * x + wet * std::sin(x * dr), -1.2f, 1.2f);
            pts.push_back(ImVec2(a.x + cw * i / 120.0f, (a.y + b.y) * 0.5f - y * h * 0.42f));
        }
        dl->AddPolyline(pts.data(), (int)pts.size(), pal::envMod, 0, 2.0f * z);
        ImGui::Dummy(ImVec2(cw, h));
    }
    ImGui::EndGroup();
}




void scPolyComb::drawFxTab(float w) {
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
            if(ImGui::MenuItem((std::string(pub ? "Unpublish " : "Publish ") + label).c_str(), nullptr, false, !isConnected(key))){
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
        publishedUnderline(echoBeatVal.getEscapedName());
        ImGui::SameLine();
        ImGui::SetCursorPosX(x0 + labelW * z);
        ImGui::SetNextItemWidth(cw - labelW * z);
        if(ImGui::SliderInt("##echoNote", &k, 0, (int)notes.size() - 1, notes[k].first)) echoBeatVal = notes[k].second;
        publishMenu(echoBeatVal.getEscapedName());
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
        const char* types[4] = {"LowPass", "HighPass", "BandPass", "PeakEQ"};
        int ft = std::clamp(echoFilter.get(), 0, 3);
        if(ImGui::Combo("##echoFilter", &ft, types, 4)) echoFilter = ft;
        publishMenu(echoFilter.getEscapedName());
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
    ImGui::TextColored(ImColor(pal::textDim), "Voices -> EQ -> Echo -> Reverb (MixerPro's), at the speaker count. "
                       "A stage switched off lets its tail ring out, then stops using CPU.");
}

// ════════════════════════════════════════════════════════════════════════════
// Panning: where each voice sits on the speakers
// ════════════════════════════════════════════════════════════════════════════

void scPolyComb::drawPanTab(float w) {
    const float z = zoom();
    const int nv = n(), c = outChannels.get();
    const bool ring = c > 2 && spreadLayout.get() == 0;
    const float cw = colW(w, 3);
    auto voiceCol = [](int v, int alpha){
        ImColor col = ImColor::HSV(std::fmod(v * 0.61803f, 1.0f), 0.5f, 0.98f);
        col.Value.w = alpha / 255.0f;
        return (ImU32)col;
    };
    ImGui::BeginGroup();
    sectionHeader("Placement", cw);
    combo(spreadMode, "Mode", cw, {"Spread", "Channels", "Manual"});
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Spread: evenly across the speakers\nChannels: voice 1 on speaker 1, voice 2 on speaker 2...\n"
                                                 "Manual: each voice where you put it (drag it, or the Pan vector)");
    if(spreadMode.get() == 0){
        slider(spreadWidth, "Spread", cw, "%.2f");
        slider(spreadCenter, "Center", cw, "%+.2f");
    }
    if(spreadMode.get() == 2) sliderVec(panP, "Pan", cw, "%.2f");
    if(c > 2) combo(spreadLayout, "Speakers", cw, {"Ring", "Line"});
    ImGui::Spacing();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
    ImGui::TextColored(ImColor(pal::textDim), "%d voices on %d speaker%s. Drag a voice to place it (Shift: onto a speaker). "
                       "To move them, route an LFO (or anything) to Pan in Routing: the filled dots show where each voice is now.",
                       nv, c, c == 1 ? "" : "s");
    ImGui::PopTextWrapPos();
    if(c == 1) ImGui::TextColored(ImColor(pal::envMod), "One channel: nothing to pan.");
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
        auto live = [&](int v){ const int i = VIS_PAN + v; return i < (int)visValues.size() ? visValues[i] : base[v]; };
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
            auto v = expand(panP.get(), nv, 0.5f);
            v[panDragVoice] = p;
            panP.set(v);
        }
        if(ImGui::IsItemDeactivated()) panDragVoice = -1;
        if(hovered && hoverVoice >= 0 && panDragVoice < 0)
            ImGui::SetTooltip("Voice %d at %.2f (now %.2f)\nDrag to place it (Shift: onto a speaker)", hoverVoice + 1, base[hoverVoice], live(hoverVoice));
    }
    ImGui::EndGroup();
}

// ════════════════════════════════════════════════════════════════════════════
// Snapshots
// {"name", "controls": {key: value}, "matrix": [...], "lfos", "envs", "exts"}
// ════════════════════════════════════════════════════════════════════════════

bool scPolyComb::snapExcluded(const std::string& key) const {
    return key.rfind("Snapshot", 0) == 0 || key.rfind("Morph", 0) == 0 || key.rfind("Ext_", 0) == 0
        || key == spreadLayout.getEscapedName();
}

// a connection drives it: a recall leaves it alone
bool scPolyComb::drivenByConnection(const std::string& key) const {
    if(key == combPitch.getEscapedName() && combPitchHandle && combPitchHandle->hasInConnection()) return true;
    auto it = published.find(key);
    return it != published.end() && it->second && it->second->hasInConnection();
}

ofJson scPolyComb::captureSnapshot() const {
    ofJson s = ofJson::object();
    auto& c = s["controls"] = ofJson::object();
    for(const auto& control : controls) if(!snapExcluded(control.key)) c[control.key] = control.save();
    modulation.patchBases(c);
    s["params"] = modulation.paramsJson();
    s["matrix"] = modulation.matrix;
    s["lfos"] = modulation.lfoCount;
    s["envs"] = modulation.envCount;
    s["exts"] = modulation.extCount;
    return s;
}

namespace {
// a value between two snapshots' values: numbers glide (times, rates and
// frequencies in log), switches / modes jump half way
bool logKey(const std::string& k) {
    if(k.find("Curve") != std::string::npos) return false;
    for(const char* w : {"Hz", "Freq", "Lowpass", "Ratio", "Beats", "Delay", "Attack", "Decay", "Release", "Time"})
        if(k.find(w) != std::string::npos) return true;
    return false;
}
float blendNum(float x, float y, float t, bool lg) {
    if(lg && x > 0.0f && y > 0.0f) return std::exp(ofLerp(std::log(x), std::log(y), t));
    return ofLerp(x, y, t);
}
ofJson blendJson(const std::string& key, const ofJson& x, const ofJson& y, float t) {
    if(t <= 0.0f) return x;
    if(t >= 1.0f) return y;
    const bool lg = logKey(key);
    if(x.is_number() && y.is_number() && (x.is_number_float() || y.is_number_float()))
        return ofJson(blendNum(x.get<float>(), y.get<float>(), t, lg));
    if(x.is_array() && y.is_array() && !x.empty() && !y.empty() && x[0].is_number() && y[0].is_number()){
        const size_t n = std::max(x.size(), y.size());
        ofJson r = ofJson::array();
        for(size_t i = 0; i < n; i++)
            r.push_back(blendNum(x[std::min(i, x.size() - 1)].get<float>(), y[std::min(i, y.size() - 1)].get<float>(), t, lg));
        return r;
    }
    return t < 0.5f ? x : y;
}
}

void scPolyComb::applyState(const ofJson& a, const ofJson& b, float t) {
    if(!a.is_object() || !b.is_object()) return;
    t = ofClamp(t, 0.0f, 1.0f);
    // the counts first (the routes need their sources): while blending, the larger
    auto count = [&](const char* k, int current, int lo, int hi) {
        const int x = a.value(k, current), y = b.value(k, current);
        return std::clamp(t <= 0.0f ? x : t >= 1.0f ? y : std::max(x, y), lo, hi);
    };
    auto& M = modulation;
    const int nl = count("lfos", M.lfoCount, 1, NUM_LFOS), ne = count("envs", M.envCount, 0, NUM_ENVS),
              nx = count("exts", M.extCount, 0, NUM_EXT);
    const bool countsChanged = nl != M.lfoCount || ne != M.envCount || nx != M.extCount;
    M.lfoCount = nl; M.envCount = ne; M.extCount = nx;

    // the controls: only what changes is set (each sends what it needs)
    static const ofJson none = ofJson::object();
    const ofJson& ca = a.contains("controls") ? a["controls"] : none;
    const ofJson& cb = b.contains("controls") ? b["controls"] : none;
    for(const auto& control : controls){
        const std::string& k = control.key;
        if(snapExcluded(k) || drivenByConnection(k)) continue;
        const bool ha = ca.contains(k), hb = cb.contains(k);
        ofJson v;
        if(ha && hb) v = blendJson(k, ca[k], cb[k], t);
        else if(hb && t >= 0.5f) v = cb[k];
        else if(ha && t < 0.5f) v = ca[k];
        else continue;
        if(v != control.save()) control.load(v);
    }

    // the matrix
    std::vector<float> ma(NUM_SOURCES * NUM_TARGETS, 0.0f), mb = ma;
    try { if(a.contains("matrix")) ma = a["matrix"].get<std::vector<float>>(); } catch(...) {}
    try { if(b.contains("matrix")) mb = b["matrix"].get<std::vector<float>>(); } catch(...) {}
    ma.resize(NUM_SOURCES * NUM_TARGETS, 0.0f);
    mb.resize(NUM_SOURCES * NUM_TARGETS, 0.0f);
    bool changed = countsChanged;
    for(size_t i = 0; i < M.matrix.size() && i < ma.size(); i++){
        const float v = ofLerp(ma[i], mb[i], t);
        if(std::abs(v - M.matrix[i]) > 1e-5f){ M.matrix[i] = v; changed = true; }
    }
    if(countsChanged) M.refresh();       // sources created / freed, and the routes
    else if(changed) M.sendMatrix();
    // the controls any source moves ("Modulate"): the arrival's
    if(t >= 1.0f) M.loadParamsJson(b.contains("params") ? b["params"] : ofJson::array());
}

void scPolyComb::storeSnapshot(int slot, const std::string& name) {
    if(slot < 0 || slot >= NUM_SNAPS) return;
    const std::string keep = name.empty() ? snapName(slot) : name;
    snapSlots[slot] = captureSnapshot();
    snapSlots[slot]["name"] = keep.empty() ? "Snapshot " + ofToString(slot + 1) : keep;
    snapUsed[slot] = true;
    activeSnap = slot;
    saveSnapshotLibrary();
}

void scPolyComb::recallSnapshot(int slot) {
    if(slot < 0 || slot >= NUM_SNAPS || !snapUsed[slot]) return;
    morphRun = false;
    applyState(snapSlots[slot], snapSlots[slot], 1.0f);
    activeSnap = slot;
    snapRecalling = true;
    snapshotP.set(slot + 1);      // shows / outputs it, without a second recall
    snapRecalling = false;
}

void scPolyComb::startGlide(int slot) {
    if(slot < 0 || slot >= NUM_SNAPS || !snapUsed[slot]) return;
    morphDur = morphBeatsP.get() ? morphTimeP.get() * 60.0f / std::max(1.0f, currentBpm) : morphTimeP.get();
    if(morphDur <= 0.001f){ recallSnapshot(slot); return; }
    morphFrom = captureSnapshot();
    morphTo = snapSlots[slot];
    morphTarget = slot;
    morphT0 = ofGetElapsedTimef();
    morphProgress = 0.0f;
    morphRun = true;
}

void scPolyComb::applyAB() {
    const int a = morphAP.get() - 1, b = morphBP.get() - 1;
    if(a < 0 || b < 0 || a >= NUM_SNAPS || b >= NUM_SNAPS || !snapUsed[a] || !snapUsed[b]) return;
    morphRun = false;              // the slider takes over from a glide
    applyState(snapSlots[a], snapSlots[b], morphP.get());
}

std::string scPolyComb::snapName(int k) const {
    if(k < 0 || k >= NUM_SNAPS || !snapUsed[k] || !snapSlots[k].contains("name")) return "";
    try { return snapSlots[k]["name"].get<std::string>(); } catch(...) { return ""; }
}

// ── Library (shared files) and banks ─────────────────────────────────────────
std::string scPolyComb::snapDir() const {
    const std::string dir = ofToDataPath("nodeSnapshots/PolyComb", true);
    try { std::filesystem::create_directories(std::filesystem::path(dir) / "banks"); } catch(...) {}
    return dir;
}

std::filesystem::file_time_type scPolyComb::snapSetTime() const {
    std::filesystem::file_time_type t{};
    try {
        for(auto& e : std::filesystem::directory_iterator(snapDir()))
            if(e.is_regular_file() && e.path().filename().string().rfind("snapshot_", 0) == 0)
                t = std::max(t, std::filesystem::last_write_time(e.path()));
    } catch(...) {}
    return t;
}

void scPolyComb::saveSnapshotLibrary() {
    const std::filesystem::path dir = snapDir();
    for(int k = 0; k < NUM_SNAPS; k++){
        const auto f = dir / ("snapshot_" + ofToString(k) + ".json");
        try {
            if(snapUsed[k]) ofSavePrettyJson(f.string(), snapSlots[k]);
            else if(std::filesystem::exists(f)) std::filesystem::remove(f);
        } catch(...) {}
    }
    // a marker: the library exists (empty slots stay empty, no factory refill)
    try { ofSaveJson((dir / "library.json").string(), ofJson{{"polyCombSnapshots", 1}}); } catch(...) {}
    snapLibTime = snapSetTime();
}

void scPolyComb::loadSnapshotLibrary() {
    const std::filesystem::path dir = snapDir();
    const auto bank = dir / "banks" / "Factory.json";
    {
        // the factory collection, as a bank anyone can reload (rewritten every
        // start, so it always holds this build's sounds)
        ofJson j;
        j["polyCombSnapshotBank"] = 1;
        j["slots"] = ofJson::array();
        for(auto& s : factorySnapshots()) j["slots"].push_back(s);
        ofSavePrettyJson(bank.string(), j);
    }
    if(!std::filesystem::exists(dir / "library.json")){ loadFactory(); return; }   // first run
    snapUsed.fill(false);
    for(int k = 0; k < NUM_SNAPS; k++){
        const auto f = dir / ("snapshot_" + ofToString(k) + ".json");
        if(!std::filesystem::exists(f)) continue;
        try {
            ofJson j = ofLoadJson(f.string());
            if(j.is_object() && j.contains("controls")){ snapSlots[k] = j; snapUsed[k] = true; }
        } catch(...) {}
    }
    if(activeSnap >= 0 && !snapUsed[activeSnap]) activeSnap = -1;
    snapLibTime = snapSetTime();
}

void scPolyComb::saveSnapshotBank(const std::string& path) {
    ofJson j;
    j["polyCombSnapshotBank"] = 1;
    j["slots"] = ofJson::array();
    for(int k = 0; k < NUM_SNAPS; k++) j["slots"].push_back(snapUsed[k] ? snapSlots[k] : ofJson(nullptr));
    std::string p = path;
    if(std::filesystem::path(p).extension().string() != ".json") p += ".json";
    snapMsg = ofSavePrettyJson(p, j) ? "Saved " + std::filesystem::path(p).stem().string() : "Could not save " + p;
}

bool scPolyComb::loadSnapshotBank(const std::string& path) {
    ofJson j;
    try { j = ofLoadJson(path); } catch(...) {}
    if(!j.contains("slots") || !j["slots"].is_array()){
        snapMsg = "No snapshots in " + std::filesystem::path(path).filename().string();
        return false;
    }
    snapUsed.fill(false);
    for(int k = 0; k < std::min(NUM_SNAPS, (int)j["slots"].size()); k++)
        if(j["slots"][k].is_object() && j["slots"][k].contains("controls")){ snapSlots[k] = j["slots"][k]; snapUsed[k] = true; }
    activeSnap = -1;
    snapMsg = "Loaded " + std::filesystem::path(path).stem().string();
    saveSnapshotLibrary();          // becomes the shared current set
    return true;
}

void scPolyComb::loadFactory() {
    snapUsed.fill(false);
    auto f = factorySnapshots();
    for(int k = 0; k < std::min(NUM_SNAPS, (int)f.size()); k++){ snapSlots[k] = f[k]; snapUsed[k] = true; }
    activeSnap = -1;
    snapMsg = "Loaded the factory snapshots";
    saveSnapshotLibrary();
}

// ── The factory collection: each one the defaults plus its own settings
//    (control names as in the window), its routes [source, target, amount]
//    (sources: LFO 0-7, Env 8-11, Velocity 12, Random 13; targets: Pitch 0,
//    FM Amt 1, FM Ratio 2, Cutoff 3, Reso 4, Comb Pitch 5, Comb Fb 6, Comb Mix
//    7, Ring Mix 8, Post Cut 9, Drive 10, PW 11, Amp 12, Pan 13) and counts ──
std::vector<ofJson> scPolyComb::factorySnapshots() const {
    static const char* src = R"JSON([
 {"name": "Mother", "lfos": 4, "envs": 0,
  "routes": [[0, 6, 0.45], [1, 12, 1.0], [2, 3, 0.4], [3, 10, 0.35]],
  "set": {
   "Saw": 0.0, "SuperSaw": 1.0, "SS Detune": 0.45, "Osc Amp": 0.5,
   "Transpose": [-24, -12, -5, 0, 3, 7, 10, 12, 14, 15, 19, 22, 24, 26, 27, 31],
   "Detune": 0.06, "Vib Amount": 0.2, "Vib Rate": 5.0, "Vib Rate Random": 0.6,
   "Cutoff": 74, "Resonance": 0.05, "Key Track": 0.4,
   "Comb On": true, "Comb Feedback": 0.45, "Comb Tone": 0.7, "Comb Mix": 1.0,
   "Shaper On": true, "Drive": 3.0, "Shaper Mix": 0.5,
   "Amp Attack": 3.0, "Amp Sustain": 1.0, "Amp Release": 6.0,
   "LFO1 Noise": true, "LFO1 Sync": false, "LFO1 Hz": 0.6, "LFO1 Noise Scale": 8.0, "LFO1 Gain": 1.6,
   "LFO2 Sync": false, "LFO2 Hz": 0.05, "LFO2 Shuffle": 1.0, "LFO2 Pow": -0.85,
   "LFO3 Noise": true, "LFO3 Sync": false, "LFO3 Hz": 0.25, "LFO3 Noise Scale": 34.0, "LFO3 Gain": 1.5,
   "LFO4 Sync": false, "LFO4 Hz": 0.12, "LFO4 Shuffle": 1.0, "LFO4 Phase": 0.3,
   "Echo On": true, "Echo Beats": 0.75, "Echo Feed": 0.45, "Echo Mix": 0.25,
   "Reverb On": true, "Reverb Mix": 0.45, "Reverb Decay": 8.0, "Reverb Size": 50.0}},
 {"name": "Harmonic FM", "lfos": 3, "envs": 0,
  "routes": [[0, 12, 1.0], [1, 1, 0.5], [2, 6, 0.4]],
  "set": {
   "Saw": 0.0, "Sine": 1.0, "Osc Amp": 0.6,
   "Transpose": [-24, -12, -4.98, 0, 3.86, 7.02, 9.69, 12, 14.04, 15.86, 17.51, 19.02, 20.41, 21.69, 22.88, 24],
   "FM On": true, "FM Amount": 0.15, "FM Ratio": 8.0, "FM Feedback": 0.1,
   "Cutoff": 96, "Resonance": 0.9, "Key Track": 0.8,
   "Comb On": true, "Comb Feedback": 0.4, "Comb Mix": 0.7, "Comb Tone": 0.9,
   "Vib Amount": 0.12, "Vib Rate": 4.5, "Vib Rate Random": 0.7,
   "Amp Attack": 4.0, "Amp Sustain": 1.0, "Amp Release": 6.0,
   "LFO1 Sync": false, "LFO1 Hz": 0.04, "LFO1 Shuffle": 1.0, "LFO1 Pow": -0.85,
   "LFO2 Noise": true, "LFO2 Sync": false, "LFO2 Hz": 0.3, "LFO2 Noise Scale": 12.0, "LFO2 Gain": 2.0,
   "LFO3 Noise": true, "LFO3 Sync": false, "LFO3 Hz": 0.5, "LFO3 Noise Scale": 8.0,
   "Reverb On": true, "Reverb Mix": 0.5, "Reverb Decay": 10.0, "Reverb Size": 55.0}},
 {"name": "Tremolo Pulses", "lfos": 3, "envs": 0,
  "routes": [[0, 12, 0.85], [1, 11, 0.7], [2, 9, 0.6]],
  "set": {
   "Saw": 0.0, "Pulse": 1.0, "Pulse Width": 0.35, "Osc Amp": 0.5,
   "Transpose": [-12, 0, 7, 12, 16, 19, 24, -5, 3, 10, 14, 22, 26, 31, 15, 27],
   "Cutoff": 88, "Resonance": 0.45, "Key Track": 0.5,
   "Shaper On": true, "Drive": 1.5, "Shaper Mix": 0.4,
   "Post Filter On": true, "Post Type": 2, "Post Cutoff": 80, "Post Resonance": 0.6,
   "Amp Attack": 1.5, "Amp Sustain": 1.0, "Amp Release": 4.0,
   "LFO1 Sync": false, "LFO1 Hz": [2, 3, 4.5, 6, 2.5, 8, 3.5, 12, 5, 2.2, 7, 9, 4, 10, 3, 6.5], "LFO1 Round": 1.0, "LFO1 Width": 0.35,
   "LFO2 Noise": true, "LFO2 Sync": false, "LFO2 Hz": 0.4, "LFO2 Noise Scale": 13.0, "LFO2 Gain": 1.5,
   "LFO3 Noise": true, "LFO3 Sync": false, "LFO3 Hz": 0.15, "LFO3 Noise Scale": 4.0, "LFO3 Gain": 2.0,
   "Echo On": true, "Echo Beats": 0.5, "Echo Feed": 0.35, "Echo Mix": 0.2,
   "Reverb On": true, "Reverb Mix": 0.3, "Reverb Decay": 4.0}},
 {"name": "Saw Shimmer", "lfos": 3, "envs": 0,
  "routes": [[0, 12, 0.35], [1, 10, 0.25], [2, 6, 0.3]],
  "set": {
   "Saw": 1.0, "Osc Amp": 0.45, "Detune": 0.08,
   "Transpose": [-12, 0, 7, 12, 4, 16, 19, 24, -5, 11, 14, 23, 2, 26, 28, 31],
   "Cutoff": 120, "Resonance": 0.0,
   "Comb On": true, "Comb Feedback": 0.2, "Comb Mix": 0.35,
   "Shaper On": true, "Drive": 2.0, "Shaper Mix": 0.3,
   "Vib Amount": 0.2, "Vib Rate": 5.5, "Vib Rate Random": 0.4,
   "Amp Attack": 1.0, "Amp Sustain": 1.0, "Amp Release": 3.0,
   "LFO1 Sync": false, "LFO1 Hz": [2, 2.6, 3.1, 4.2, 5.0, 2.3, 3.7, 6.0, 2.9, 4.6, 5.5, 3.3, 2.1, 4.9, 3.9, 5.8],
   "LFO2 Noise": true, "LFO2 Sync": false, "LFO2 Hz": 0.3, "LFO2 Noise Scale": 10.0,
   "LFO3 Noise": true, "LFO3 Sync": false, "LFO3 Hz": 0.2, "LFO3 Noise Scale": 20.0,
   "Echo On": true, "Echo Beats": 0.75, "Echo Feed": 0.3, "Echo Mix": 0.2,
   "Reverb On": true, "Reverb Mix": 0.35, "Reverb Decay": 5.0}},
 {"name": "Vangelis", "lfos": 4, "envs": 0,
  "routes": [[0, 1, 0.6], [1, 11, 0.8], [2, 9, 0.45], [3, 10, 0.4]],
  "set": {
   "Saw": 0.0, "Pulse": 0.7, "Pink": 0.5, "Osc Amp": 0.55,
   "Transpose": [-12, 0, 7, 12, 3, 15, 19, 24, 10, 22, -5, 14, 26, 27, 31, 34],
   "FM On": true, "FM Amount": 0.1, "FM Ratio": 0.5,
   "Cutoff": 82, "Resonance": 0.0,
   "Comb On": true, "Comb Feedback": 0.97, "Comb Mix": 1.0, "Comb Tone": 0.6,
   "Shaper On": true, "Drive": 2.5, "Shaper Mix": 0.5,
   "Post Filter On": true, "Post Type": 0, "Post Cutoff": 64, "Post Resonance": 0.3,
   "Amp Attack": 2.5, "Amp Sustain": 1.0, "Amp Release": 7.0,
   "LFO1 Noise": true, "LFO1 Sync": false, "LFO1 Hz": 0.2, "LFO1 Noise Scale": 12.0,
   "LFO2 Sync": false, "LFO2 Hz": 0.08, "LFO2 Shuffle": 1.0,
   "LFO3 Sync": true, "LFO3 Beats": 32, "LFO3 Spread": 0.0,
   "LFO4 Noise": true, "LFO4 Sync": false, "LFO4 Hz": 0.1, "LFO4 Noise Scale": 30.0,
   "Reverb On": true, "Reverb Mix": 0.55, "Reverb Decay": 12.0, "Reverb Size": 60.0}},
 {"name": "Random Bells", "lfos": 2, "envs": 0,
  "routes": [[0, 12, 1.0], [1, 8, 0.6]],
  "set": {
   "Saw": 0.0, "Sine": 1.0, "Osc Amp": 0.6,
   "Transpose": [0, 7, 12, 19, 24, 28, 31, 36, -12, 14, 16, 26, 33, 38, 40, 43],
   "FM On": true, "FM Amount": 0.35, "FM Ratio": 3.5, "FM Feedback": 0.15,
   "Ring On": true, "Ring Pitch": 96, "Ring Detune": 3.0, "Ring Mix": 0.2,
   "Cutoff": 125,
   "Amp Attack": 0.01, "Amp Sustain": 1.0, "Amp Release": 2.0,
   "LFO1 Noise": true, "LFO1 Sync": false, "LFO1 Hz": 1.5, "LFO1 Noise Scale": 20.0, "LFO1 Gain": 6.0,
   "LFO1 S&H": 3.0, "LFO1 S&H Glide": 0.02, "LFO1 Pow": 0.6,
   "LFO2 Noise": true, "LFO2 Sync": false, "LFO2 Hz": 0.2, "LFO2 Noise Scale": 9.0,
   "Echo On": true, "Echo Beats": 0.75, "Echo Feed": 0.5, "Echo Mix": 0.3, "Echo Ping-pong": 0.8,
   "Reverb On": true, "Reverb Mix": 0.45, "Reverb Decay": 7.0, "Reverb Size": 45.0}},
 {"name": "Comb Choir", "lfos": 3, "envs": 0,
  "routes": [[0, 12, 0.9], [1, 3, 0.35], [2, 5, 0.01]],
  "set": {
   "Saw": 0.0, "Pink": 1.0, "Osc Amp": 0.7,
   "Transpose": [-12, 0, 3, 7, 10, 12, 15, 19, 22, 24, -5, 14, 17, 26, 27, 31],
   "Cutoff": 90, "Key Track": 0.6,
   "Comb On": true, "Comb Feedback": 0.985, "Comb Tone": 0.65, "Comb Mix": 1.0, "Comb Detune": 0.05,
   "Amp Attack": 4.0, "Amp Sustain": 1.0, "Amp Release": 8.0,
   "LFO1 Sync": false, "LFO1 Hz": 0.03, "LFO1 Shuffle": 1.0, "LFO1 Pow": -0.6,
   "LFO2 Noise": true, "LFO2 Sync": false, "LFO2 Hz": 0.15, "LFO2 Noise Scale": 25.0,
   "LFO3 Noise": true, "LFO3 Sync": false, "LFO3 Hz": 0.4, "LFO3 Noise Scale": 10.0,
   "Reverb On": true, "Reverb Mix": 0.5, "Reverb Decay": 9.0, "Reverb Size": 55.0}},
 {"name": "Polyrhythm Grid", "lfos": 2, "envs": 0,
  "routes": [[0, 12, 1.0], [1, 3, 0.4]],
  "set": {
   "Saw": 0.8, "Pulse": 0.3, "Osc Amp": 0.5,
   "Transpose": [-12, 0, 7, 12, 3, 15, 10, 19, -5, 24, 14, 22, 2, 26, 17, 29],
   "Cutoff": 70, "Resonance": 0.5, "Key Track": 0.5,
   "Comb On": true, "Comb Feedback": -0.6, "Comb Mix": 0.4,
   "Amp Attack": 0.3, "Amp Sustain": 1.0, "Amp Release": 2.0,
   "LFO1 Sync": true, "LFO1 Beats": [1, 0.75, 0.5, 1.5, 0.667, 2, 1.25, 0.333, 3, 0.25, 1.75, 0.6, 2.5, 0.4, 1.33, 4],
   "LFO1 Round": 1.0, "LFO1 Width": 0.2,
   "LFO2 Noise": true, "LFO2 Sync": true, "LFO2 Beats": 4, "LFO2 Noise Scale": 6.0, "LFO2 Gain": 1.5,
   "Echo On": true, "Echo Beats": 0.75, "Echo Feed": 0.4, "Echo Mix": 0.25, "Echo Ping-pong": 1.0,
   "Reverb On": true, "Reverb Mix": 0.25, "Reverb Decay": 3.0}},
 {"name": "Slow Tides", "lfos": 3, "envs": 0,
  "routes": [[0, 3, 0.45], [1, 13, 1.0], [2, 12, 0.6]],
  "set": {
   "Saw": 0.5, "SuperSaw": 0.5, "Tri": 0.4, "SS Detune": 0.3, "Osc Amp": 0.45,
   "Transpose": [-24, -12, 0, 7, 12, 16, 19, 24, -5, 4, 11, 14, 23, 26, 28, 31],
   "Cutoff": 72, "Resonance": 0.35,
   "Amp Attack": 6.0, "Amp Sustain": 1.0, "Amp Release": 10.0,
   "LFO1 Sync": true, "LFO1 Beats": 64, "LFO1 Spread": 1.0,
   "LFO2 Sync": true, "LFO2 Beats": 128, "LFO2 Spread": 1.0, "LFO2 Shuffle": 1.0,
   "LFO3 Noise": true, "LFO3 Sync": false, "LFO3 Hz": 0.05, "LFO3 Noise Scale": 15.0,
   "Reverb On": true, "Reverb Mix": 0.55, "Reverb Decay": 14.0, "Reverb Size": 60.0}},
 {"name": "Dust Resonators", "lfos": 2, "envs": 0,
  "routes": [[0, 6, 0.3], [1, 12, 0.8]],
  "set": {
   "Saw": 0.0, "Dust": 1.0, "Osc Amp": 0.9,
   "Transpose": [-36, -36, -36, -36, -36, -36, -36, -36, -36, -36, -36, -36, -36, -36, -36, -36],
   "Filter On": false,
   "Comb On": true, "Comb Transpose": [36, 43, 48, 52, 55, 60, 31, 40, 45, 50, 57, 62, 64, 67, 38, 53],
   "Comb Feedback": 0.65, "Comb Tone": 0.8, "Comb Mix": 1.0,
   "Amp Attack": 1.0, "Amp Sustain": 1.0, "Amp Release": 4.0,
   "LFO1 Noise": true, "LFO1 Sync": false, "LFO1 Hz": 0.2, "LFO1 Noise Scale": 12.0, "LFO1 Gain": 1.4,
   "LFO2 Sync": false, "LFO2 Hz": 0.06, "LFO2 Shuffle": 1.0, "LFO2 Pow": -0.7,
   "Reverb On": true, "Reverb Mix": 0.45, "Reverb Decay": 6.0}},
 {"name": "Drone Organ", "lfos": 2, "envs": 0,
  "routes": [[0, 12, 0.25], [1, 3, 0.2]],
  "set": {
   "Saw": 0.0, "Tri": 0.7, "Sine": 0.5, "Osc Amp": 0.5,
   "Transpose": [-24, -12, 0, 7, 12, 19, 24, 31, 36, -5, 4, 16, 28, 14, 21, 26],
   "Detune": 0.12, "Vib Amount": 0.08, "Vib Rate": 4.0, "Vib Rate Random": 0.8,
   "Cutoff": 100, "Key Track": 0.3,
   "Amp Attack": 5.0, "Amp Sustain": 1.0, "Amp Release": 8.0,
   "LFO1 Noise": true, "LFO1 Sync": false, "LFO1 Hz": 0.3, "LFO1 Noise Scale": 20.0,
   "LFO2 Noise": true, "LFO2 Sync": false, "LFO2 Hz": 0.08, "LFO2 Noise Scale": 3.0,
   "Echo On": true, "Echo Beats": 1.5, "Echo Feed": 0.4, "Echo Mix": 0.2,
   "Reverb On": true, "Reverb Mix": 0.5, "Reverb Decay": 10.0, "Reverb Size": 55.0}},
 {"name": "Metallic Swarm", "lfos": 3, "envs": 0,
  "routes": [[0, 2, 0.05], [1, 13, 1.0], [2, 12, 0.9]],
  "set": {
   "Saw": 0.0, "Pulse": 0.6, "Pulse Width": 0.2, "Osc Amp": 0.45,
   "Transpose": [0, 6, 11, 13, 18, 23, 25, 30, -6, -1, 4, 9, 16, 20, 27, 35],
   "FM On": true, "FM Amount": 0.4, "FM Ratio": 3.73, "FM Feedback": 0.3,
   "Comb On": true, "Comb Feedback": -0.8, "Comb Mix": 0.5,
   "Ring On": true, "Ring Pitch": 79, "Ring Detune": 4.0, "Ring Mix": 0.35,
   "Cutoff": 110, "Resonance": 0.2,
   "Amp Attack": 0.5, "Amp Sustain": 1.0, "Amp Release": 3.0,
   "LFO1 Noise": true, "LFO1 Sync": false, "LFO1 Hz": 0.4, "LFO1 Noise Scale": 20.0,
   "LFO2 Noise": true, "LFO2 Sync": false, "LFO2 Hz": 0.25, "LFO2 Noise Scale": 30.0,
   "LFO3 Noise": true, "LFO3 Sync": false, "LFO3 Hz": 2.5, "LFO3 Noise Scale": 25.0, "LFO3 Gain": 3.0, "LFO3 Pow": 0.7,
   "Echo On": true, "Echo Beats": 0.375, "Echo Feed": 0.5, "Echo Mix": 0.25, "Echo Ping-pong": 1.0,
   "Reverb On": true, "Reverb Mix": 0.35, "Reverb Decay": 5.0}},
 {"name": "Breathing Pad", "lfos": 2, "envs": 0,
  "routes": [[0, 12, 0.8], [1, 9, 0.5]],
  "set": {
   "Saw": 0.6, "Pink": 0.4, "Osc Amp": 0.45,
   "Transpose": [-12, 0, 3, 7, 12, 15, 19, 22, 24, 10, 14, -5, 26, 27, 31, 17],
   "Cutoff": 84, "Resonance": 0.2,
   "Post Filter On": true, "Post Type": 2, "Post Cutoff": 76, "Post Resonance": 0.5,
   "Amp Attack": 3.0, "Amp Sustain": 1.0, "Amp Release": 6.0,
   "LFO1 Sync": true, "LFO1 Beats": 8, "LFO1 Spread": 1.0, "LFO1 Round": 0.5,
   "LFO2 Noise": true, "LFO2 Sync": false, "LFO2 Hz": 0.12, "LFO2 Noise Scale": 18.0, "LFO2 Gain": 1.6,
   "Reverb On": true, "Reverb Mix": 0.5, "Reverb Decay": 9.0, "Reverb Size": 50.0}},
 {"name": "Distant Radio", "lfos": 3, "envs": 0,
  "routes": [[0, 8, 0.5], [1, 9, 0.7], [2, 12, 0.8]],
  "set": {
   "Saw": 0.4, "White": 0.6, "Osc Amp": 0.5,
   "Transpose": [0, 12, 7, 19, -12, 24, 3, 15, 10, 22, 5, 17, 14, 26, 2, 29],
   "Cutoff": 100,
   "Ring On": true, "Ring Pitch": 70, "Ring Detune": 6.0, "Ring Mix": 0.4,
   "Shaper On": true, "Drive": 6.0, "Shaper Mix": 0.6,
   "Post Filter On": true, "Post Type": 2, "Post Cutoff": 82, "Post Resonance": 0.75,
   "Amp Attack": 0.5, "Amp Sustain": 1.0, "Amp Release": 2.0,
   "LFO1 Noise": true, "LFO1 Sync": false, "LFO1 Hz": 1.0, "LFO1 Noise Scale": 10.0,
   "LFO2 Noise": true, "LFO2 Sync": false, "LFO2 Hz": 0.3, "LFO2 Noise Scale": 8.0, "LFO2 Gain": 2.0,
   "LFO3 Noise": true, "LFO3 Sync": false, "LFO3 Hz": 4.0, "LFO3 Noise Scale": 30.0, "LFO3 Gain": 4.0,
   "LFO3 S&H": 6.0, "LFO3 S&H Glide": 0.03,
   "Echo On": true, "Echo Beats": 0.25, "Echo Feed": 0.55, "Echo Mix": 0.3,
   "Reverb On": true, "Reverb Mix": 0.3, "Reverb Decay": 3.0}},
 {"name": "Glass Harmonics", "lfos": 2, "envs": 0,
  "routes": [[0, 12, 1.0], [1, 4, 0.3]],
  "set": {
   "Saw": 0.0, "Sine": 0.7, "Tri": 0.3, "Osc Amp": 0.6,
   "Transpose": [0, 12, 19.02, 24, 27.86, 31.02, 33.69, 36, 38.04, 39.86, 41.51, 43.02, 44.41, 45.69, 46.88, 48],
   "Cutoff": 118, "Resonance": 0.3,
   "Comb On": true, "Comb Feedback": 0.9, "Comb Tone": 0.95, "Comb Mix": 0.5,
   "Amp Attack": 2.0, "Amp Sustain": 1.0, "Amp Release": 5.0,
   "LFO1 Sync": false, "LFO1 Hz": 0.09, "LFO1 Shuffle": 1.0, "LFO1 Pow": -0.9,
   "LFO2 Noise": true, "LFO2 Sync": false, "LFO2 Hz": 0.3, "LFO2 Noise Scale": 12.0,
   "Echo On": true, "Echo Beats": 1.0, "Echo Feed": 0.5, "Echo Mix": 0.25, "Echo Ping-pong": 0.7,
   "Reverb On": true, "Reverb Mix": 0.55, "Reverb Decay": 11.0, "Reverb Size": 58.0}},
 {"name": "Deep Comb Bass", "lfos": 2, "envs": 0,
  "routes": [[0, 6, 0.35], [1, 10, 0.3]],
  "set": {
   "Saw": 1.0, "Pulse": 0.4, "Pulse Width": 0.3, "Osc Amp": 0.5,
   "Transpose": [-24, -12, -24, -12, -17, -5, -24, -12, -19, -7, -24, -12, -22, -10, -24, -12],
   "Cutoff": 62, "Resonance": 0.45, "Key Track": 0.7,
   "Comb On": true, "Comb Feedback": 0.55, "Comb Mix": 0.6, "Comb Tone": 0.5,
   "Shaper On": true, "Drive": 3.0, "Shaper Mix": 0.5,
   "Amp Attack": 0.2, "Amp Sustain": 1.0, "Amp Release": 1.5,
   "LFO1 Noise": true, "LFO1 Sync": false, "LFO1 Hz": 0.25, "LFO1 Noise Scale": 9.0,
   "LFO2 Sync": true, "LFO2 Beats": 4, "LFO2 Spread": 1.0, "LFO2 Pow": 0.5}}
])JSON";
    std::vector<ofJson> out;
    ofJson list;
    try { list = ofJson::parse(src); } catch(const std::exception& e) { ofLogError("scPolyComb") << "factory snapshots: " << e.what(); return out; }
    std::map<std::string, std::string> keyOf;
    for(const auto& c : controls) keyOf[c.name] = c.key;
    for(const auto& f : list){
        ofJson s = defaultSnap;
        auto& c = s["controls"];
        for(auto it = f["set"].begin(); it != f["set"].end(); ++it){
            auto k = keyOf.find(it.key());
            if(k == keyOf.end() || !c.contains(k->second)){ ofLogWarning("scPolyComb") << "factory: no control " << it.key(); continue; }
            const ofJson& d = c[k->second];
            const ofJson& v = it.value();
            // in the control's own type
            if(d.is_array()) c[k->second] = v.is_array() ? v : ofJson::array({v.get<float>()});
            else if(d.is_boolean()) c[k->second] = v.is_boolean() ? v.get<bool>() : v.get<float>() > 0.5f;
            else if(d.is_number_integer()) c[k->second] = (int)std::lround(v.get<float>());
            else c[k->second] = v.get<float>();
        }
        std::vector<float> m(NUM_SOURCES * NUM_TARGETS, 0.0f);
        for(const auto& r : f["routes"]){
            const int so = r[0].get<int>(), ta = r[1].get<int>();
            if(so >= 0 && so < NUM_SOURCES && ta >= 0 && ta < NUM_TARGETS) m[so * NUM_TARGETS + ta] = r[2].get<float>();
        }
        s["matrix"] = m;
        s["lfos"] = f.value("lfos", 1);
        s["envs"] = f.value("envs", 0);
        s["exts"] = defaultSnap.value("exts", 2);
        s["name"] = f["name"];
        out.push_back(s);
    }
    return out;
}

// ── The bar under the header: a slot menu (recall), store, glide, A/B morph,
//    banks. Every one of them publishable. ─────────────────────────────────────
void scPolyComb::drawSnapshotBar(float w) {
    const float z = zoom();
    const float m = 10.0f * z;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImColor(pal::accent), "Snapshot");
    publishedUnderline(snapshotP.getEscapedName());
    publishMenu(snapshotP.getEscapedName());
    ImGui::SameLine();
    auto label = [this](int k) {
        return ofToString(k + 1) + "  " + (snapUsed[k] ? snapName(k) : std::string("(empty)"));
    };
    ImGui::SetNextItemWidth(220 * z);
    const std::string prev = activeSnap >= 0 ? label(activeSnap) : std::string("-");
    if(ImGui::BeginCombo("##pcSnap", prev.c_str(), ImGuiComboFlags_HeightLarge)){
        for(int k = 0; k < NUM_SNAPS; k++){
            if(!snapUsed[k]) continue;
            if(ImGui::Selectable(label(k).c_str(), k == activeSnap)){
                if(morphGlideP.get() || ImGui::GetIO().KeyAlt) startGlide(k); else recallSnapshot(k);
            }
            // right-click: replace with the current sound, or delete
            if(ImGui::BeginPopupContextItem(("##pcSnapItem" + ofToString(k)).c_str())){
                ImGui::TextDisabled("%s", label(k).c_str());
                if(ImGui::MenuItem("Replace with the current sound")) storeSnapshot(k, snapName(k));
                if(ImGui::MenuItem("Delete")){
                    snapUsed[k] = false;
                    if(activeSnap == k) activeSnap = -1;
                    saveSnapshotLibrary();
                }
                ImGui::EndPopup();
            }
        }
        ImGui::EndCombo();
    }
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("Recall a snapshot (Alt: glide to it over Time).\nRight-click a snapshot in the list: replace or delete it.");
    if(morphRun){
        ImGui::SameLine();
        ImGui::ProgressBar(morphProgress, ImVec2(60 * z, ImGui::GetFrameHeight()), "");
    }
    ImGui::SameLine();
    if(ImGui::Button("Store...")){
        const std::string n = activeSnap >= 0 ? snapName(activeSnap) : std::string();
        std::snprintf(snapNameBuf, sizeof(snapNameBuf), "%s", n.c_str());
        ImGui::OpenPopup("##pcSnapStore");
    }
    if(ImGui::BeginPopup("##pcSnapStore")){
        ImGui::TextDisabled("Store the current sound");
        ImGui::SetNextItemWidth(200 * z);
        if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if(ImGui::InputTextWithHint("##pcSnapName", "name", snapNameBuf, sizeof(snapNameBuf),
                                    ImGuiInputTextFlags_EnterReturnsTrue)){
            // Enter: over the snapshot with this name, else the first empty slot
            const std::string nm = ofTrim(std::string(snapNameBuf));
            int slot = -1;
            for(int k = 0; k < NUM_SNAPS && slot < 0 && !nm.empty(); k++)
                if(snapUsed[k] && snapName(k) == nm) slot = k;
            for(int k = 0; k < NUM_SNAPS && slot < 0; k++) if(!snapUsed[k]) slot = k;
            if(slot < 0) slot = activeSnap >= 0 ? activeSnap : 0;
            storeSnapshot(slot, nm);
            ImGui::CloseCurrentPopup();
        }
        ImGui::TextDisabled("Enter: a new snapshot (or over the one with this name)");
        ImGui::Separator();
        int firstFree = -1;
        for(int k = 0; k < NUM_SNAPS && firstFree < 0; k++) if(!snapUsed[k]) firstFree = k;
        if(firstFree >= 0 && ImGui::Selectable("+ New snapshot")){ storeSnapshot(firstFree, snapNameBuf); ImGui::CloseCurrentPopup(); }
        ImGui::TextDisabled("Replace");
        for(int k = 0; k < NUM_SNAPS; k++){
            if(!snapUsed[k]) continue;
            if(ImGui::Selectable(label(k).c_str(), k == activeSnap)){ storeSnapshot(k, snapNameBuf); ImGui::CloseCurrentPopup(); }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if(ImGui::Button("Snapshots")) ImGui::OpenPopup("##pcSnapMenu");
    if(ImGui::BeginPopup("##pcSnapMenu")){
        if(activeSnap >= 0 && snapUsed[activeSnap]){
            ImGui::TextDisabled("%s", label(activeSnap).c_str());
            if(ImGui::MenuItem("Recall again")) recallSnapshot(activeSnap);
            if(ImGui::MenuItem("Delete this snapshot")){ snapUsed[activeSnap] = false; activeSnap = -1; saveSnapshotLibrary(); }
            ImGui::Separator();
        }
        if(ImGui::MenuItem("Factory collection")) loadFactory();
        if(ImGui::BeginMenu("Load bank")){
            bool any = false;
            try {
                for(auto& e : std::filesystem::directory_iterator(std::filesystem::path(snapDir()) / "banks")){
                    if(e.path().extension() != ".json") continue;
                    any = true;
                    if(ImGui::MenuItem(e.path().stem().string().c_str())) loadSnapshotBank(e.path().string());
                }
            } catch(...) {}
            if(!any) ImGui::TextDisabled("No banks yet");
            ImGui::EndMenu();
        }
        if(ImGui::BeginMenu("Save bank as")){
            ImGui::SetNextItemWidth(180 * z);
            const bool enter = ImGui::InputText("##pcBankName", snapBankName, sizeof(snapBankName),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
            if((ImGui::Button("Save") || enter) && snapBankName[0]){
                saveSnapshotBank((std::filesystem::path(snapDir()) / "banks" / snapBankName).string());
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndMenu();
        }
        if(ImGui::MenuItem("From file...")) snapLoadDialog = true;
        if(ImGui::MenuItem("Clear all")){ snapUsed.fill(false); activeSnap = -1; saveSnapshotLibrary(); }
        if(!snapMsg.empty()){ ImGui::Separator(); ImGui::TextDisabled("%s", snapMsg.c_str()); }
        ImGui::EndPopup();
    }

    // glide
    ImGui::SameLine(0, m);
    {
        bool g = morphGlideP.get();
        if(ImGui::Checkbox("Glide", &g)) morphGlideP.set(g);
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("On: every recall glides from the current sound over Time.\nOff: recalls jump (Alt to glide).");
        publishedUnderline(morphGlideP.getEscapedName());
        publishMenu(morphGlideP.getEscapedName());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90 * z);
        float v = morphTimeP.get();
        const bool beats = morphBeatsP.get();
        if(ImGui::SliderFloat("##pcMorphT", &v, 0.0f, 64.0f, beats ? "%.2f beats" : "%.2f s", ImGuiSliderFlags_Logarithmic))
            morphTimeP.set(v);
        publishedUnderline(morphTimeP.getEscapedName());
        publishMenu(morphTimeP.getEscapedName());
        ImGui::SameLine(0, 2 * z);
        if(ImGui::SmallButton(beats ? "b##pcMTU" : "s##pcMTU")) morphBeatsP.set(!beats);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Time in beats (b) or seconds (s)");
    }

    // A / B morph
    ImGui::SameLine(0, m);
    auto slotCombo = [&](const char* id, ofParameter<int>& p) {
        ImGui::SetNextItemWidth(44 * z);
        const std::string cur = p.get() > 0 ? ofToString(p.get()) : std::string("-");
        if(ImGui::BeginCombo(id, cur.c_str(), ImGuiComboFlags_HeightLarge)){
            if(ImGui::Selectable("-", p.get() == 0)) p.set(0);
            for(int k = 0; k < NUM_SNAPS; k++)
                if(snapUsed[k] && ImGui::Selectable(label(k).c_str(), p.get() == k + 1)) p.set(k + 1);
            ImGui::EndCombo();
        }
        publishedUnderline(p.getEscapedName());
        publishMenu(p.getEscapedName());
    };
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("A"); ImGui::SameLine(); slotCombo("##pcMorphA", morphAP);
    ImGui::SameLine();
    ImGui::TextUnformatted("B"); ImGui::SameLine(); slotCombo("##pcMorphB", morphBP);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(80.0f * z, w - ImGui::GetCursorPosX()));
    float mv = morphP.get();
    if(ImGui::SliderFloat("##pcMorph", &mv, 0.0f, 1.0f, "Morph %.3f")) morphP.set(mv);
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
        ImGui::SetTooltip("Blends snapshot A (0) into B (1). Publish it (right-click) to drive it from a connection.");
    publishedUnderline(morphP.getEscapedName());
    publishMenu(morphP.getEscapedName());
}
