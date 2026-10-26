#include "../shared/scDroneMode.h"
//
//  scPolyComb.cpp
//  ofxOceanodeSuperCollider
//

#include "scPolyComb.h"
#include "scPolyCombControlSteps.h"
#include "../../../ui/scQuantizedSlider.h"
#include "scPolyCombMigration.h"
#include "../../../audio/scSynthControlBatch.h"
#include "../../../audio/scSynthStartup.h"
#include "scPitchMapView.h"
#include "../../../ui/scEnvelopeCurve.h"
#include "../../../ui/scTypedValue.h"
#include "ofxOceanodeShared.h"
#include "../../../ofxOceanodeSuperColliderConfig.h"
#include "ofxSCServer.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <utility>
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
// amount 1: the control's whole span (pitches: 48 st); kTargetLegacy, the ranges older saves used
const float kTargetRange[scPolyComb::NUM_TARGETS] = {48, 1, 31.9375f, 135, 1, 48, 2.2f, 1, 1, 135, 30, 0.98f, 1, 1};
const float kTargetLegacy[scPolyComb::NUM_TARGETS] = {24, 1, 4, 48, 1, 24, 1, 1, 1, 48, 10, 0.5f, 1, 1};
const char* kTargetUnit[scPolyComb::NUM_TARGETS] = {"st", "", "", "st", "", "st", "", "", "", "st", "", "", "", "of the width"};

// Preserve the four original indices and their DSP; append fixed pole types.
const std::vector<const char*> kFilterTypes = {"Low pass", "High pass", "Band pass", "Notch",
    "Low pass 12 dB", "Low pass 24 dB", "High pass 12 dB", "High pass 24 dB",
    "Band pass 2-pole", "Band pass 4-pole", "BL4: 6 HP / 18 LP", "Notch 4-pole"};
std::pair<int,int> filterChoice(int type) {
    static const std::pair<int,int> modes[]={{0,1},{0,2},{1,1},{1,2},{2,1},{2,2},{2,3},{3,2}};
    return type<4 ? std::make_pair(type,0) : modes[std::clamp(type-4,0,7)];
}
}

// ════════════════════════════════════════════════════════════════════════════
// Setup
// ════════════════════════════════════════════════════════════════════════════

scPolyComb::scPolyComb(std::vector<serverManager*> s, bool renderer) : scNode("polycombBox"), servers(std::move(s)), voiceRenderer(renderer) {
    description = "Polyphonic comb-resonator synth. Gate takes Oceanode values and audio-rate gates together. "
                  "Oscillators with FM, filters, comb, ring mod, sine shaper, envelopes, LFOs and a modulation "
                  "matrix, spread over the speakers, then EQ, echo and reverb. Built from small synths: only the "
                  "parts in use run. Show opens the editor; right-click a control there to publish it.";
}

scPolyComb::~scPolyComb() {
    voicePeers.clear();
    listeners.unsubscribeAll();
    for(auto& [server, st] : states) freeAll(server, st);
    states.clear();
}

void scPolyComb::setup() {
    if(!voiceRenderer) timeline.setup(this, [this](const std::string& k){ publish(k); }, [this](const std::string& k){ unpublish(k); },
                   [this](){ return publishedKeys; });
    auto vec = [](ofParameter<std::vector<float>>& p, const std::string& name, float def, float lo, float hi) {
        p.set(name, {def}, {lo}, {hi});
    };
    // what a change resends. While a snapshot applies, each part is queued
    // once (by key) and sent at the end of the frame, not once per control
    auto deferred = [this](std::string key, std::function<void()> send) {
        return [this, key, send]() { if(applyingSnapshot){ morphSends[key] = send; return; } send(); };
    };
    auto toGate = deferred("gate", [this]() { forStates([this](ofxSCServer*, ServerState& st){ sendGate(st, true); }); });
    auto toAmpEnv = deferred("ampEnv", [this]() { forStates([this](ofxSCServer*, ServerState& st){ sendAmpEnv(st); }); });
    auto toPitch = [this]() { if(!applyingSnapshot) forStates([this](ofxSCServer*, ServerState& st){ sendPitch(st); }); };
    auto toFm = deferred("fm", [this]() { forStates([this](ofxSCServer*, ServerState& st){ sendFm(st); }); });
    auto toAmp = deferred("amp", [this]() { forStates([this](ofxSCServer*, ServerState& st){ sendAmp(st); }); });
    auto toSpread = deferred("spread", [this]() { forStates([this](ofxSCServer*, ServerState& st){ sendSpread(st); }); });
    auto toOsc = [this, deferred](int o) { return deferred("osc" + ofToString(o), [this, o]() {
        forStates([this, o](ofxSCServer* sv, ServerState& st){
            const Part& p = st.osc[o];
            const bool wanted = oscWanted(o);
            // A level edit only changes the graph when it starts/stops an
            // oscillator (or cancels a pending stop). Keep that transition
            // path intact; ordinary automation need not resend every part.
            const bool transition = wanted ? (!p.synth || !p.running || p.offAt >= 0.0f)
                                           : (p.synth && p.running && p.offAt < 0.0f);
            if(transition) updateParts(sv, st);
            else sendOsc(st, o);
        });
    }); };
    auto toAllOsc = deferred("allOsc", [this]() { forStates([this](ofxSCServer*, ServerState& st){ for(int o = 0; o < NumOsc; o++) sendOsc(st, o); }); });
    auto toIns = [this, deferred](int i) { return deferred("ins" + ofToString(i), [this, i]() { forStates([this, i](ofxSCServer*, ServerState& st){ sendIns(st, i); }); }); };
    auto toParts = deferred("parts", [this]() { forStates([this](ofxSCServer* sv, ServerState& st){ updateParts(sv, st); }); });

    // ── The node ─────────────────────────────────────────────────────────────
    addParameter(showWindow.set("Show", false));
    voicesHandle = addParameter(voices.set("Voices", 4, 1, MAX_VOICES));
    addParameter(outChannels.set("Channels", 2, 1, MAX_CHANNELS));
    gate.set("Gate", {0.0f}, {0.0f}, {1.0f});
    // Gate (levels: a gate rising to 0.2 plays at 0.2), Pitch, Velocity: Oceanode values or audio cables
    gateIn.attach(addParameter(gate), availableInputs, [this](){ refreshOutputs(); });
    pitchIn.attach(addParameter(pitch.set("Pitch", {60.0f}, {0.0f}, {127.0f})), availableInputs, [this](){ refreshOutputs(); });
    noteInputs.add(gateIn.handle, gate);
    noteInputs.add(pitchIn.handle, pitch);
    if(!voiceRenderer) noteInputs.enable(this, [this](){ for(auto& [server, st] : states) if(st.created && st.gateS) return true; return false; });
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
    drone.set("Drone", false);                      reg(drone, [this]() { sendEnvMode(true); });
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
    expandPitch.set("Expand Pitches", false);               reg(expandPitch, toPitch);
    for(const auto& c : controls) if(c.key == "Expand_Pitches") pitchDefaults[c.key] = c.save();   // older patches: off, as before
    vibRate.set("Vib Rate", 5.0f, 0.0f, 20.0f);            reg(vibRate, toPitch);
    vibRand.set("Vib Rate Random", 0.0f, 0.0f, 1.0f);      reg(vibRand, toPitch);
    vibAmt.set("Vib Amount", 0.0f, 0.0f, 2.0f);            reg(vibAmt, toPitch);
    tremRate.set("Trem Rate", 5.0f, 0.0f, 20.0f);          reg(tremRate, toAmp);
    tremRand.set("Trem Rate Random", 0.0f, 0.0f, 1.0f);    reg(tremRand, toAmp);
    tremDepth.set("Trem Depth", 0.0f, 0.0f, 1.0f);         reg(tremDepth, toAmp);
    tremRound.set("Trem Roundness", 1.0f, 0.0f, 1.0f);     reg(tremRound, toAmp);   // 1 sine .. 0 square
    for(const auto& c : controls) if(c.key.rfind("Trem_", 0)==0) tremDefaults[c.key]=c.save();

    // ── Pitch & Harmony: internal pitch maps (the original PolyComb's). External
    //    on (the default, and every snapshot without these): the Pitch values as
    //    they are. Older snapshots restore these defaults, as the Sub's. ──────
    auto toPitchComb = [this]() { syncMapScale(oscMap);syncMapScale(combMap);if(!applyingSnapshot)forStates([this](ofxSCServer*, ServerState& st){ sendPitch(st); sendIns(st, IComb); }); };
    pitchExternal.set("Pitch External", true);             reg(pitchExternal, toPitchComb);
    pitchShiftMode.set("Pitch Shift", 0, 0, 1);            reg(pitchShiftMode, toPitchComb);
    combMapOn.set("Comb Map On", false);                   reg(combMapOn, toPitchComb);
    for(auto* mp : {&oscMap, &combMap}){
        const std::string pre = mp == &oscMap ? "Osc Map " : "Comb Map ";
        auto& m = *mp;
        m.shape.set(pre + "Shape", 0, 0, 2);                reg(m.shape, toPitchComb);
        m.first.set(pre + "First", 1.0f, 0.0f, 32.0f);      reg(m.first, toPitchComb);
        m.interval.set(pre + "Interval", 3.0f, -24.0f, 24.0f); reg(m.interval, toPitchComb);
        m.transpose.set(pre + "Transpose", 0.0f, -72.0f, 72.0f); reg(m.transpose, toPitchComb);
        m.bend.set(pre + "Bend", 1.0f, 1.0f, 6.0f);         reg(m.bend, toPitchComb);
        m.mode.set(pre + "Mode", 0, 0, pcmap::NumModes - 1); reg(m.mode, toPitchComb);
        m.quantize.set(pre + "Quantize", 0, 0, 2);          reg(m.quantize, toPitchComb);
        m.root.set(pre + "Root", 0.0f, 0.0f, 11.0f);        reg(m.root, toPitchComb);
        m.tones.set(pre + "Tones", {0, 2, 4, 5, 7, 9, 11}, {0.0f}, {24.0f}); reg(m.tones, toPitchComb);
        m.table.set(pre + "Table", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, {0.0f}, {11.0f}); reg(m.table, toPitchComb);
        m.harmonicBase.set(pre + "Harmonic Base", 36.0f, 0.0f, 127.0f); reg(m.harmonicBase, toPitchComb);
        m.offset.set(pre + "Fine Tune", 0.0f, -1.0f, 1.0f);  reg(m.offset, toPitchComb);
        m.span.set(pre + "Span", 0.0f, 0.0f, 48.0f);        reg(m.span, toPitchComb);
        m.version.set(pre + "Version", 2, 2, 2); reg(m.version, toPitchComb);
        m.degreeOffset.set(pre + "Degree Offset", 0.0f, -256.0f, 256.0f); reg(m.degreeOffset, toPitchComb);
        m.octaveJump.set(pre + "Octave Jump", 1.0f, 0.0f, 4.0f);
        reg(m.octaveJump, toPitchComb);
        m.indexSkew.set(pre+"Skew", -1.f,-1.f,1.f);reg(m.indexSkew,toPitchComb);
        m.indexRoundness.set(pre+"Roundness",0.f,0.f,1.f);reg(m.indexRoundness,toPitchComb);
        m.indexPW.set(pre+"PW",.5f,.001f,1.f);reg(m.indexPW,toPitchComb);
        m.indexPow.set(pre+"Pow",0.f,-1.f,1.f);reg(m.indexPow,toPitchComb);
        m.indexWaves.set(pre + "Num Waves", 1.0f, 0.0f, 16.0f); reg(m.indexWaves, toPitchComb);
        m.indexCycleShift.set(pre+"Cycle Shift",0,-31,31);reg(m.indexCycleShift,toPitchComb);
        m.indexInvert.set(pre + "Invert", false); reg(m.indexInvert, toPitchComb);
        m.indexSymmetry.set(pre + "Symmetry", 0, 0, 32); reg(m.indexSymmetry, toPitchComb);
        m.indexComb.set(pre + "Comb", 0.0f, 0.0f, 1.0f); reg(m.indexComb, toPitchComb);
        m.indexQuantize.set(pre + "Index Quantize", 0.0f, 0.0f, 16.0f); reg(m.indexQuantize, toPitchComb);
        m.indexRandom.set(pre + "Index Random", 0.0f, 0.0f, 16.0f); reg(m.indexRandom, toPitchComb);
        m.indexSeed.set(pre + "Seed", 0, 0, 9999); reg(m.indexSeed, toPitchComb);
        m.indexSeedOnGate.set(pre+"New Seed On Gate",false);reg(m.indexSeedOnGate,[](){});
        m.scaleSelection.set(pre+"Scale",0,0,1024);
        reg(m.scaleSelection,[this,mp](){selectMapScale(*mp);});
        dropdowns.set(m.shape.getEscapedName(), {"Ramp", "Mirror", "Sine"});
        dropdowns.set(m.mode.getEscapedName(), {"Semitones", "User scale", "Snap to scale", "Harmonic series"});
        dropdowns.set(m.quantize.getEscapedName(), {"Nearest", "Down", "Up"});
    }
    updatePitchIndexLimits();
    dropdowns.set(pitchShiftMode.getEscapedName(), {"After the map", "In the scale"});
    for(const auto& c : controls)
        if(c.key.rfind("Osc_Map_", 0) == 0 || c.key.rfind("Comb_Map_", 0) == 0 || c.key.rfind("Pitch_", 0) == 0) pitchDefaults[c.key] = c.save();
    loadPitchLibraries();

    // ── Delta (a modulation source, the matrix's last row): how much a mix of
    //    two LFOs moved over the window. No route from it: no synth. ─────────
    auto toDelta = deferred("delta", [this]() { forStates([this](ofxSCServer*, ServerState& st){ sendDelta(st); }); });
    deltaSrcA.set("Delta LFO A", 0, 0, NUM_LFOS - 1);      reg(deltaSrcA, toDelta);
    deltaSrcB.set("Delta LFO B", 1, 0, NUM_LFOS - 1);      reg(deltaSrcB, toDelta);
    deltaMixA.set("Delta Mix A", 0.0f, 0.0f, 1.0f);        reg(deltaMixA, toDelta);
    deltaMixB.set("Delta Mix B", 1.0f, 0.0f, 1.0f);        reg(deltaMixB, toDelta);
    deltaWindow.set("Delta Window", 1000.0f, 1.0f, 1000.0f); reg(deltaWindow, toDelta);
    deltaGain.set("Delta Gain", 3.0f, 0.0f, 10.0f);        reg(deltaGain, toDelta);
    for(const auto& c : controls) if(c.key.rfind("Delta_", 0) == 0) deltaDefaults[c.key] = c.save();

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
    oscDustDensity.set("Dust Density", 0.0f, -10.0f, 0.0f); reg(oscDustDensity, toOsc(ODust));
    for(const auto& c : controls) if(c.key == "Dust_Density") pitchDefaults[c.key] = c.save();   // older patches: as before
    vec(oscImpulse, "Impulse", 0, 0, 1);      reg(oscImpulse, toOsc(OImp));
    vec(oscAmp, "Osc Amp", 0.5f, 0, 1);       reg(oscAmp, toAllOsc);
    fmOn.set("FM On", false);                 reg(fmOn, [this, toParts, toAllOsc]() { toParts(); toAllOsc(); });
    vec(fmAmt, "FM Amount", 0.2f, 0, 1);      reg(fmAmt, toFm);
    vec(fmRatio, "FM Ratio", 1, 0.0625f, 32); reg(fmRatio, toFm);
    fmFb.set("FM Feedback", 0.0f, 0.0f, 1.5f); reg(fmFb, toFm);
    fmSnap.set("FM Ratio Snap", true);        reg(fmSnap, [](){});

    // ── Filters ──────────────────────────────────────────────────────────────
    filterOn.set("Filter On", true);          reg(filterOn, toParts);
    fType.set("Filter Type", 0, 0, 11);        reg(fType, toParts);
    vec(cutoff, "Cutoff", 100, 0, 135);       reg(cutoff, toIns(IFilter));
    vec(res, "Resonance", 0.3f, 0, 1);        reg(res, toIns(IFilter));
    keyTrack.set("Key Track", 0.0f, 0.0f, 1.0f); reg(keyTrack, toIns(IFilter));
    postOn.set("Post Filter On", false);      reg(postOn, toParts);
    postType.set("Post Type", 0, 0, 11);       reg(postType, toParts);
    // Both filter positions offer the same models, selected independently.
    filterModel.set("Filter Model", 0, 0, 1); reg(filterModel, toParts);
    postFilterModel.set("Post Filter Model", 0, 0, 1); reg(postFilterModel, toParts);
    vec(postCut, "Post Cutoff", 130, 0, 135); reg(postCut, toIns(IPost));
    vec(postRes, "Post Resonance", 0, 0, 1);  reg(postRes, toIns(IPost));
    postKeyTrack.set("Post Key Track", 0.0f, 0.0f, 1.0f); reg(postKeyTrack, toIns(IPost));

    // Legacy snapshot fields are retained for compatibility; the comb always uses PCCombR.
    combSaturation.set("Comb Saturation", false); reg(combSaturation, toParts);
    combSatLevel.set("Comb Sat Level", 12.0f, 0.0f, 48.0f); reg(combSatLevel, toIns(IComb));
    // Fix the legacy model field to Reaktor, including when recalling older snapshots.
    combModel.set("Comb Model", 1, 1, 1); reg(combModel, toParts);

    // ── Resonator, ring, shaper ──────────────────────────────────────────────
    combOn.set("Comb On", false);             reg(combOn, toParts);
    combMode.set("Comb Mode", 0, 0, 1);       reg(combMode, toIns(IComb));
    reg(combPitch, toIns(IComb));
    vec(combTranspose, "Comb Transpose", 0, -48, 48); reg(combTranspose, toIns(IComb));
    vec(combDetune, "Comb Detune", 0, 0, 1);  reg(combDetune, toIns(IComb));
    combGlide.set("Comb Glide", 0.0f, 0.0f, 2.0f); reg(combGlide, toIns(IComb));
    combVibRate.set("Comb Vib Rate", 5.0f, 0.0f, 20.0f); reg(combVibRate, toIns(IComb));
    combVibRand.set("Comb Vib Rate Random", 0.0f, 0.0f, 1.0f); reg(combVibRand, toIns(IComb));
    combVibAmt.set("Comb Vib Amount", 0.0f, 0.0f, 2.0f); reg(combVibAmt, toIns(IComb));
    // Older patches and snapshots restore the new vibrato to Off.
    for(const auto& c : controls) if(c.key.rfind("Comb_Vib_", 0)==0) pitchDefaults[c.key]=c.save();
    vec(combFeed, "Comb Feedback", 0.7f, -1.1f, 1.1f); reg(combFeed, toIns(IComb));
    combTone.set("Comb Tone", 1.0f, 0.0f, 1.0f); reg(combTone, toIns(IComb));
    vec(combMix, "Comb Mix", 1, 0, 1);        reg(combMix, toIns(IComb));
    ringOn.set("Ring On", false);             reg(ringOn, toParts);
    ringPitchSource.set("Ring Pitch Source", 1, 0, 2); reg(ringPitchSource, toIns(IRing));
    dropdowns.set(ringPitchSource.getEscapedName(), {"External", "Oscillators", "Resonators map"});
    vec(ringPitch, "Ring Pitch", 72, 0, 135); reg(ringPitch, toIns(IRing));
    vec(ringTranspose, "Ring Transpose", 0, -48, 48); reg(ringTranspose, toIns(IRing));
    vec(ringDetune, "Ring Detune", 0, 0, 12);  reg(ringDetune, toIns(IRing));
    vec(ringMix, "Ring Mix", 1, 0, 1);        reg(ringMix, toIns(IRing));
    // Legacy model field fixed to the Reaktor ring modulator (PCRingR).
    ringModel.set("Ring Model", 1, 1, 1);     reg(ringModel, toParts);
    ringRatio.set("Ring Ratio", 1.0f, 0.0f, 32.0f); reg(ringRatio, toIns(IRing));
    ringWidth.set("Ring Width", 0.0f, -1.0f, 1.0f); reg(ringWidth, toIns(IRing));
    shaperOn.set("Shaper On", false);         reg(shaperOn, toParts);
    vec(drive, "Drive", 1, 0, 30);            reg(drive, toIns(IShaper));
    vec(shaperMix, "Shaper Mix", 1, 0, 1);    reg(shaperMix, toIns(IShaper));

    // Independent sine layer: no synth runs until enabled. Older snapshots
    // reset every Sub control, so an imported patch cannot change their sound.
    auto toSub = deferred("sub", [this]() { forStates([this](ofxSCServer*, ServerState& st){ sendSub(st); }); });
    subOn.set("Sub On", false); reg(subOn, toParts);
    subLevel.set("Sub Level", 0.25f, 0.0f, 1.0f); reg(subLevel, toSub);
    subTranspose.set("Sub Transpose", -12.0f, -96.0f, 96.0f); reg(subTranspose, toSub);
    subFineTune.set("Sub Fine Tune", 0.0f, -1.0f, 1.0f); reg(subFineTune, toSub);
    subVibRate.set("Sub Vib Rate", 5.0f, 0.0f, 20.0f); reg(subVibRate, toSub);
    subVibAmt.set("Sub Vib Amount", 0.0f, 0.0f, 2.0f); reg(subVibAmt, toSub);
    subVibRand.set("Sub Vib Rate Random", 0.0f, 0.0f, 1.0f); reg(subVibRand, toSub);
    subA.set("Sub Attack", 0.01f, 0.001f, 10.0f); reg(subA, toSub);
    subD.set("Sub Decay", 0.5f, 0.001f, 10.0f); reg(subD, toSub);
    subS.set("Sub Sustain", 1.0f, 0.0f, 1.0f); reg(subS, toSub);
    subR.set("Sub Release", 0.5f, 0.001f, 20.0f); reg(subR, toSub);
    subCA.set("Sub Attack Curve", -4.0f, -12.0f, 12.0f); reg(subCA, toSub);
    subCD.set("Sub Decay Curve", -4.0f, -12.0f, 12.0f); reg(subCD, toSub);
    subCR.set("Sub Release Curve", -4.0f, -12.0f, 12.0f); reg(subCR, toSub);
    for(const auto& c : controls) if(c.key.rfind("Sub_", 0)==0) subDefaults[c.key]=c.save();

    // ── Effects (MixerPro's inserts) ─────────────────────────────────────────
    auto toStages = deferred("stages", [this]() { forStates([this](ofxSCServer* sv, ServerState& st){ if(st.created) updateStages(sv, st); }); });
    auto toStage = [deferred, this](int stage) { return deferred("stage" + ofToString(stage), [this, stage]() { sendStageAll(stage); }); };
    compOn.set("Comp On", false); reg(compOn, toStages);
    compThreshold.set("Comp Threshold", -18.0f, -60.0f, 0.0f);
    compRatio.set("Comp Ratio", 3.0f, 1.0f, 20.0f);
    compKnee.set("Comp Knee", 6.0f, 0.0f, 48.0f);
    compAttack.set("Comp Attack", 10.0f, 0.1f, 200.0f);
    compRelease.set("Comp Release", 150.0f, 5.0f, 2000.0f);
    compMakeup.set("Comp Makeup", 0.0f, -24.0f, 24.0f);
    compAuto.set("Comp Auto Makeup", false);
    compRms.set("Comp RMS", 0.0f, 0.0f, 1.0f);
    compHpf.set("Comp SC HPF", 20.0f, 20.0f, 1000.0f);
    compMix.set("Comp Mix", 1.0f, 0.0f, 1.0f);
    for(auto* p : {&compThreshold, &compRatio, &compKnee, &compAttack, &compRelease,
                   &compMakeup, &compRms, &compHpf, &compMix}) reg(*p, toStage(StComp));
    reg(compAuto, toStage(StComp));
    for(const auto& c : controls) if(c.key.rfind("Comp_", 0)==0) compDefaults[c.key]=c.save();
    eqOn.set("EQ On", false);                 reg(eqOn, toStages);
    {
        const char* names[5] = {"Low", "Lo Mid", "Mid", "Hi Mid", "High"};
        const float freqs[5] = {80, 250, 1000, 4000, 12000};
        for(int b = 0; b < 5; b++){
            const bool shelf = scEQEditor::isShelf(b);
            eqGain[b].set(std::string("EQ ") + names[b], 0.0f, -24.0f, 24.0f);
            eqFreq[b].set(std::string("EQ ") + names[b] + " Freq", freqs[b], 20.0f, 20000.0f);
            eqShape[b].set(std::string("EQ ") + names[b] + (shelf ? " Slope" : " Q"), 1.0f, 0.1f, shelf ? 4.0f : 20.0f);
            for(auto* p : {&eqGain[b], &eqFreq[b], &eqShape[b]}) reg(*p, toStage(StEq));
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
        reg(*p, toStage(StEcho));
    reg(echoBeats, toStage(StEcho));
    reg(echoFilter, toStage(StEcho));
    // the original PolyComb's delay as a second model (older snapshots: MixerPro's)
    echoModel.set("Echo Model", 0, 0, 1);
    reg(echoModel, [this]() {
        forStates([this](ofxSCServer* sv, ServerState& st){
            auto& f = st.fx[StEcho];
            if(f.synth){ f.synth->free(); delete f.synth; f.synth = nullptr; f.running = false; f.offAt = -1.0f; }
            updateStages(sv, st);
        });
    });
    echoHi.set("Echo Hi", 6000.0f, 20.0f, 20000.0f);
    echoLo.set("Echo Lo", 40.0f, 1.0f, 4000.0f);
    echoRes.set("Echo Res", 0.0f, -1.0f, 0.98f);
    echoSpread.set("Echo Spread", 0.0f, -0.33f, 0.33f);
    for(auto* p : {&echoHi, &echoLo, &echoRes, &echoSpread}) reg(*p, toStage(StEcho));
    for(const auto& c : controls)
        if(c.key == "Ring_Pitch_Source" || c.key == "Ring_Model" || c.key == "Ring_Ratio" || c.key == "Ring_Width" || c.key == "Filter_Model" || c.key == "Post_Filter_Model" || c.key == "Post_Key_Track" || c.key == "Comb_Model" || c.key == "Echo_Model" || c.key == "Echo_Hi" || c.key == "Echo_Lo" || c.key == "Echo_Res" || c.key == "Echo_Spread") deltaDefaults[c.key] = c.save();
    dropdowns.set(echoModel.getEscapedName(), {"MixerPro", "PolyComb 1.7"});
    dropdowns.set(filterModel.getEscapedName(), {"Classic", "PolyComb 1.7"});
    dropdowns.set(postFilterModel.getEscapedName(), {"Classic", "PolyComb 1.7"});
    // the combos' names (published: dropdowns)
    dropdowns.set(spreadMode.getEscapedName(), {"Spread", "Channels", "Manual"});
    dropdowns.set(spreadLayout.getEscapedName(), {"Ring", "Line"});

    dropdowns.set(fType.getEscapedName(), kFilterTypes);
    dropdowns.set(postType.getEscapedName(), kFilterTypes);
    dropdowns.set(combMode.getEscapedName(), {"Follows the note", "Fixed note"});
    dropdowns.set(echoFilter.getEscapedName(), {"LowPass", "HighPass", "BandPass", "PeakEQ"});
    // Blur (Effect/Time/Delay/Blur.scd): an eight-stage allpass diffuser, no feedback
    blurOn.set("Blur On", false);             reg(blurOn, toStages);
    blurTime.set("Blur Time", 8.0f, 0.1f, 100.0f);
    blurAmount.set("Blur Amount", 0.75f, 0.0f, 1.0f);
    blurDensity.set("Blur Density", 0.55f, 0.0f, 1.0f);
    blurSpread.set("Blur Spread", 1.0f, 0.0f, 1.0f);
    blurLowCut.set("Blur Low Cut", 20.0f, 20.0f, 20000.0f);
    blurHighCut.set("Blur High Cut", 20000.0f, 20.0f, 20000.0f);
    blurMix.set("Blur Mix", 0.5f, 0.0f, 1.0f);
    for(auto* p : {&blurTime, &blurAmount, &blurDensity, &blurSpread, &blurLowCut, &blurHighCut, &blurMix})
        reg(*p, toStage(StBlur));
    for(const auto& c : controls) if(c.key.rfind("Blur_", 0)==0) blurDefaults[c.key]=c.save();
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
        reg(*p, toStage(StRvb));

    // ── Snapshots (publishable: Snapshot recalls, Morph blends A into B) ────
    // (scSnapshots: the parameters act through their own listeners)
    snaps.setupParameters();
    for(auto* p : {&snaps.snapshotP, &snaps.morphAP, &snaps.morphBP, &snaps.morphCurveP, &snaps.morphSwitchP}) reg(*p, [](){});
    for(auto* p : {&snaps.morphP, &snaps.morphTimeP}) reg(*p, [](){});
    for(auto* p : {&snaps.morphBeatsP, &snaps.morphGlideP}) reg(*p, [](){});

    // ── The shared modulation: LFOs, envelopes, external sources, routing ───
    {
        scModulation::Host h;
        h.node = this;
        h.voiceParamsChanged = [this](){ forStates([this](ofxSCServer*, ServerState& st){ sendAmpEnv(st); sendPitch(st); sendFm(st); sendAmp(st); for(int o=0;o<NumOsc;++o) sendOsc(st,o); for(int i=0;i<NumIns;++i) sendIns(st,i); }); };
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
        for(int t = 0; t < NUM_TARGETS; t++) targets.push_back({kTargets[t], kTargetRange[t], kTargetUnit[t], false, kTargetLegacy[t]});
        modulation.extraSources = {"Delta"};      // SRC_DELTA
        modulation.busWidth = W;
        modulation.maxVoices = MAX_VOICES;
        modulation.unifiedLfoModels = true;
        modulation.setup(h, targets, true, true, true);
        for(auto* p : {&ampA, &ampD, &ampS, &ampR, &ampCA, &ampCD, &ampCR, &velSens, &glide, &vibRate, &vibRand, &vibAmt, &tremRate, &tremRand, &tremDepth, &tremRound, &oscDustDensity, &fmFb, &combSatLevel, &keyTrack, &postKeyTrack, &combTone, &combGlide, &combVibRate, &combVibRand, &combVibAmt, &ringWidth, &ringRatio, &subA, &subD, &subS, &subR, &subCA, &subCD, &subCR}) modulation.addVoiceParam(*p);
        for(int i=1;i<=NUM_LFOS;++i) dropdowns.set("LFO"+ofToString(i)+"_Model", {"Off", "Classic", "Noise", "Random"});
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
    // the LFOs' Random type and Steps are newer than the older snapshots: they restore them off
    for(const auto& c : controls)
        if(c.key.find("_Random_Type") != std::string::npos || c.key.find("_Random_Smooth") != std::string::npos
           || (c.key.rfind("LFO", 0) == 0 && (c.key.find("_Steps") != std::string::npos || c.key.find("_Model") != std::string::npos || c.key.find("_Value") != std::string::npos))) deltaDefaults[c.key] = c.save();

    // ── Node parameters' listeners ───────────────────────────────────────────
    listeners.push(voices.newListener([this](int& v) {
        if(!voicesFromSnapshot) userVoices = v;
        if(!voiceMorphWeights.empty()) {
            if(!voicesFromSnapshot) {
                voiceMorphWeights.assign(MAX_VOICES,0); voiceMorphWeights[v-1]=1;
                voiceHandoff=0; voiceEndpointSince=-1;
                syncVoiceMorph();
            }
            return; // the active audio bank keeps its own width
        }
        updatePitchIndexLimits();
        updateScramble();
        modulation.voicesChanged();   // the LFOs' voice phases, even before anything runs
        requestRebuild();
    }));
    listeners.push(outChannels.newListener([this](int&) {
        if(!voiceMorphWeights.empty()) { releaseVoiceMorph(); requestRebuild(); }
        else rebuildOutput();
    }));
    // The timeline's frame set of a value it already sent timetagged: nothing to do.
    listeners.push(gate.newListener([this, toGate](std::vector<float>& v) {
        if(scScheduling::isBackendSendSuppressed()) return;
        randomizeMapSeedsOnGate(v);
        toGate();
    }));
    listeners.push(pitch.newListener([this](std::vector<float>&) {
        if(scScheduling::isBackendSendSuppressed()) return;
        forStates([this](ofxSCServer*, ServerState& st){ sendPitch(st, true); });
    }));
    listeners.push(master.newListener([toAmp](float&) { toAmp(); }));
    listeners.push(resetLFOs.newListener([this]() {
        modulation.resetLfos();
        for(auto& item:voicePeers) item.second.node->modulation.resetLfos();
    }));

    userVoices = voices.get();
    updateScramble();
    requireSynthdefs();
    for(auto* mode : {&filterModel, &fType}) snaps.morph.neutralDepth(*mode, filterOn, "controls");
    for(auto* mode : {&postFilterModel, &postType}) snaps.morph.neutralDepth(*mode, postOn, "controls");
    for(auto* mode : {&echoModel, &echoFilter}) snaps.morph.neutralDepth(*mode, echoMix, "controls");
    snaps.morph.continuous(combMode, scMorphPolicies::field(combMode.getEscapedName(), "controls"));
    // On switches whose DSP already crossfades a fractional \on (inserts, Sub,
    // FM depth, effect stages): they fade instead of switching at the midpoint
    for(auto* on : {&combOn, &ringOn, &shaperOn, &subOn, &fmOn, &compOn, &eqOn, &blurOn, &echoOn, &rvbOn})
        snaps.morph.bypass(*on, scMorphPolicies::field(on->getEscapedName(), "controls"));
    snaps.morph.mode(ringPitchSource, 3, scMorphPolicies::field(ringPitchSource.getEscapedName(), "controls"));
    snaps.morph.mode(spreadMode,3,scMorphPolicies::field(spreadMode.getEscapedName(),"controls"));
    snaps.morph.mode(spreadLayout,2,scMorphPolicies::field(spreadLayout.getEscapedName(),"controls"));
    if(!voiceRenderer) {
        scSnapshots::Host h;
        h.folder = "PolyComb";
        h.title = "PolyCombBox";
        h.capture = [this](){ return captureSnapshot(); };
        h.apply = [this](const ofJson& a, const ofJson& b, float t, bool sw){ applyState(a, b, t, sw); };
        // Shared banks (Santi's, the Reaktor collections) come from the
        // addon's presets/ through Sync with SynthDefs Git
        h.defaultBank = "SANTI_1.json";
        h.bpm = [this](){ return currentBpm; };
        h.paramMenu = [this](const std::string& k){ publishedUnderline(k); publishMenu(k); };
        h.morphBlocked = [this](const std::string& key){ return drivenByConnection(key); };
        h.morphChanged = [this](){ forStates([this](ofxSCServer* server, ServerState& st){
            updateParts(server,st);sendSpread(st);sendIns(st,IFilter);sendIns(st,IPost);sendIns(st,IComb);sendIns(st,IRing);sendStage(StEcho,st.fx[StEcho].synth,server);
        }); };
        snaps.setup(h);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Engine: the parts and what they get
// ════════════════════════════════════════════════════════════════════════════

void scPolyComb::forStates(const std::function<void(ofxSCServer*, ServerState&)>& f) {
    // While a voice rebuild fades out, controls already describe the requested
    // width. Sending those arrays to the old synths overwrites adjacent controls.
    for(auto& [server, st] : states) if(st.created && st.builtVoices == n()) f(server, st);
    if(!applyingSnapshot) syncVoiceMorph();
}
int scPolyComb::src(const ServerState& st, int s) const { return modulation.sourceBus(st.server, s); }
int scPolyComb::mod(const ServerState& st, int t) const { return modulation.targetBus(st.server, t); }

void scPolyComb::sendVec(ofxSCSynth* s, const std::string& name, const std::vector<float>& v) {
    if(!s) return;
    if(v.size() <= 1) s->setMultiple(name, first(v), n());
    else s->set(name, expand(v, n()));
}

#ifdef OFXOCEANODE_HAS_TIMELINE_INSTRUMENT
bool scPolyComb::getTimelineInstrument(ofxOceanodeTimelineInstrument& out) {
    if(!gateIn.handle || !pitchIn.handle) return false;
    out.gate = gateIn.handle.get();
    out.pitch = pitchIn.handle.get();
    out.voices = n();
    return true;
}
#endif

void scPolyComb::sendGate(ServerState& st, bool notesOnly) {
    if(st.builtVoices != n()) return;
    auto* s = st.gateS;
    if(!s) return;
    auto inEffect = noteInputs.inEffectNow();
    scSynthControlBatch controls(s, st.server);
    controls.set("gate", expand(gate.get(), n()));
    controls.set("drone", droning() ? 1.0f : 0.0f);
    // During playback only the gate changes. Bus wiring and cable mappings
    // are sent on creation/reconnection, outside the note's scheduling budget.
    if(!notesOnly || !s->isCreated()){
        controls.set("velocity", std::vector<float>(n(), 1.0f));
        controls.set("out", st.gateBus ? st.gateBus->index : 0);
        controls.set("vel", src(st, SRC_VEL));
        controls.set("rnd", src(st, SRC_RND));
        const int gb = gateIn.bus(st.server);
        controls.set("gate_sel", gb >= 0 ? 1.0f : 0.0f);
        controls.set("vel_sel", 0.0f);
        if(gb >= 0) s->mapan("gate_ar", gb, n());
    }
    controls.send();
}

void scPolyComb::sendAmpEnv(ServerState& st) {
    auto* s = st.ampEnv;
    if(!s) return;
    s->set("gate", st.gateBus ? st.gateBus->index : 0);
    s->set("out", st.visBlock ? st.visBlock->index + VIS_AMP : 0);
    s->set("aout", st.envBus ? st.envBus->index : 0);
    s->set("atk", modulation.voiceValues(ampA)); s->set("dec", modulation.voiceValues(ampD)); s->set("sus", modulation.voiceValues(ampS)); s->set("rel", modulation.voiceValues(ampR));
    s->set("cA", modulation.voiceValues(ampCA)); s->set("cD", modulation.voiceValues(ampCD)); s->set("cR", modulation.voiceValues(ampCR));
}




void scPolyComb::sendPitch(ServerState& st, bool notesOnly) {
    if(st.builtVoices != n()) return;
    sendSub(st, notesOnly);
    auto* s = st.pitchS;
    if(!s) return;
    auto inEffect = noteInputs.inEffectNow();
    scSynthControlBatch controls(s, st.server);
    controls.set("pitch", oscillatorPitches());
    if(!notesOnly || !s->isCreated()){
        controls.set("out", st.noteBus ? st.noteBus->index : 0);
        controls.set("mpitch", mod(st, 0));
        const int pb = internalPitch() ? -1 : pitchIn.bus(st.server);
        controls.set("pitch_sel", pb >= 0 ? 1.0f : 0.0f);
        if(pb >= 0) s->mapan("pitch_ar", pb, n());
        // the cable's own values, spread over the voices as the Oceanode values are
        controls.set("pitch_k", (float)pitchIn.channels());
        controls.set("pitch_expand", expandPitch.get() ? 1.0f : 0.0f);
        controls.set("transpose", expand(transpose.get(), n()));
        controls.set("detune", expand(detune.get(), n()));
        controls.set("glide", modulation.voiceValues(glide));
        controls.set("vibrate", modulation.voiceValues(vibRate));
        controls.set("vibrand", modulation.voiceValues(vibRand));
        controls.set("vibamt", modulation.voiceValues(vibAmt));
    }
    controls.send();
    if(ringPitchSource.get()==2 && st.ins[IRing].synth)
        sendVec(st.ins[IRing].synth,"ringpitch",resonatorPitches());
    if(combMapped()){
        // The resonators' mapped notes move with Pitch; their other controls do not.
        if(notesOnly && st.ins[IComb].synth && st.ins[IComb].synth->isCreated())
            sendVec(st.ins[IComb].synth, "combpitch", combMapPitches());
        else sendIns(st, IComb);
    }
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
    s->set("fmfb", modulation.voiceValues(fmFb));
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
    s->set("fmon", (st.fm.synth && st.fm.running) ? snaps.morph.activation(fmOn) : 0.0f);
    s->set("out", st.oscBus ? st.oscBus->index : 0);
    sendVec(s, "level", levels[o]->get());
    sendVec(s, "oscamp", oscAmp.get());
    if(o == OPulse){ sendVec(s, "pw", oscPw.get()); s->set("mpw", mod(st, 11)); }
    if(o == OSSaw) sendVec(s, "ssdetune", oscSSDetune.get());
    if(o == ODust) s->set("dustoct", modulation.voiceValues(oscDustDensity));
}

bool scPolyComb::insWanted(int i) const {
    switch(i){
        case IFilter: return snaps.morph.activation(filterOn)>1e-6f;
        case IComb: return snaps.morph.activation(combOn)>1e-6f;
        case IRing: return snaps.morph.activation(ringOn)>1e-6f;
        case IPost: return snaps.morph.activation(postOn)>1e-6f;
        default: return snaps.morph.activation(shaperOn)>1e-6f;
    }
}

void scPolyComb::sendIns(ServerState& st, int i) {
    auto* s = st.ins[i].synth;
    if(!s) return;
    s->set("on", snaps.morph.activation(i==IFilter ? filterOn : i==IPost ? postOn : i==IComb ? combOn : i==IRing ? ringOn : shaperOn));
    s->set("note", st.noteBus ? st.noteBus->index : 0);
    switch(i){
        case IFilter:
            s->set("type",float(filterModel.get()==1 ? fType.get() : filterChoice(fType.get()).first));
            if(fType.get()>=4 && filterModel.get()!=1) s->set("slope",float(filterChoice(fType.get()).second));
            sendVec(s, "cutoff", cutoff.get());
            sendVec(s, "res", res.get());
            s->set("keytrack", modulation.voiceValues(keyTrack));
            s->set("mcut", mod(st, 3)); s->set("mres", mod(st, 4));
            break;
        case IPost:
            s->set("type",float(postFilterModel.get()==1 ? postType.get() : filterChoice(postType.get()).first));
            if(postType.get()>=4 && postFilterModel.get()!=1) s->set("slope",float(filterChoice(postType.get()).second));
            sendVec(s, "cutoff", postCut.get());
            sendVec(s, "res", postRes.get());
            s->set("keytrack", modulation.voiceValues(postKeyTrack));
            s->set("mcut", mod(st, 9)); s->set("mres", mod(st, ZERO_TARGET));
            break;
        case IComb:
            s->set("satlevel", modulation.voiceValues(combSatLevel));
            if(combMapped()){              // map contribution plus the remaining live oscillator follow
                s->set("combmode", combMapAmount());
                sendVec(s, "combpitch", combMapPitches());
            }else{
                // Follow the oscillator note bus so glide and live pitch inputs
                // keep working; the shared-map controls are an added interval.
                s->set("combmode",internalPitch()?0.f:snaps.morph.activation(combMode));
                auto interval=internalPitch()?std::vector<float>{0.f}:combPitch.get();
                for(auto& value:interval)value+=combMap.transpose.get()+combMap.offset.get();
                sendVec(s,"combpitch",interval);
            }
            sendVec(s, "combtranspose", combTranspose.get());
            sendVec(s, "combdetune", combDetune.get());
            s->set("combglide", modulation.voiceValues(combGlide));
            s->set("combvibrate", modulation.voiceValues(combVibRate));
            s->set("combvibrand", modulation.voiceValues(combVibRand));
            s->set("combvibamt", modulation.voiceValues(combVibAmt));
            sendVec(s, "combfeed", combFeed.get());
            s->set("combtone", modulation.voiceValues(combTone));
            sendVec(s, "combmix", combMix.get());
            s->set("mcpitch", mod(st, 5)); s->set("mcfb", mod(st, 6)); s->set("mcmix", mod(st, 7));
            break;
        case IRing:
            {
                const auto weights=snaps.morph.weights(ringPitchSource);
                const float fixed=weights[0]+weights[2];
                auto pitches=expand(ringPitch.get(),n(),72.f), resonators=expand(resonatorPitches(),n(),72.f);
                if(fixed>0) for(int v=0;v<n();++v) pitches[v]=(weights[0]*pitches[v]+weights[2]*resonators[v])/fixed;
                s->set("ringmode",fixed);sendVec(s,"ringpitch",pitches);
            }
            sendVec(s, "ringtranspose", ringTranspose.get());
            sendVec(s, "ringdetune", ringDetune.get());
            sendVec(s, "ringmix", ringMix.get());
            s->set("ringratio", modulation.voiceValues(ringRatio));
            s->set("ringwidth", modulation.voiceValues(ringWidth));
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
    sendSub(st);
    auto* s = st.amp;
    if(!s) return;
    s->set("env", st.envBus ? st.envBus->index : 0);
    s->set("gate", st.gateBus ? st.gateBus->index : 0);
    s->set("vel", src(st, SRC_VEL));
    s->set("velsens", modulation.voiceValues(velSens));
    s->set("level", master.get());
    s->set("mamp", mod(st, 12));
    s->set("tremrate", modulation.voiceValues(tremRate));
    s->set("tremrand", modulation.voiceValues(tremRand));
    s->set("tremdepth", modulation.voiceValues(tremDepth));
    s->set("tremround", modulation.voiceValues(tremRound));
}

void scPolyComb::sendSub(ServerState& st, bool notesOnly) {
    auto* s = st.sub.synth;
    if(!s) return;
    auto inEffect = noteInputs.inEffectNow();
    scSynthControlBatch controls(s, st.server);
    controls.set("pitch", expand(pitch.get(), n()));
    if(notesOnly && s->isCreated()){ controls.send(); return; }
    // Join PCAmp output directly: all voice inserts have already run.
    // SlSub executes after SlAmp and before SlSpread / shared effects.
    controls.set("out", st.voiceBus ? st.voiceBus->index : 0);
    controls.set("gate", st.gateBus ? st.gateBus->index : 0);
    // PCSub uses the first input pitch as its root, independently of the main pitch map.
    const int pb = pitchIn.bus(st.server);
    if(pb >= 0){ controls.set("pitch_sel", 1.0f); s->mapan("pitch_ar", pb, n()); }
    else controls.set("pitch_sel", 0.0f);
    controls.set("vis", st.visBlock ? st.visBlock->index + VIS_SUB : 0);
    controls.set("transpose", subTranspose.get() + subFineTune.get());
    controls.set("vibrate", subVibRate.get()); controls.set("vibamt", subVibAmt.get()); controls.set("vibrand", subVibRand.get());
    controls.set("level", subLevel.get()); controls.set("master", master.get());
    controls.set("vel", src(st, SRC_VEL)); controls.set("velsens", modulation.voiceValues(velSens));
    controls.set("on", snaps.morph.activation(subOn));
    controls.set("atk", modulation.voiceValues(subA)); controls.set("dec", modulation.voiceValues(subD));
    controls.set("sus", modulation.voiceValues(subS)); controls.set("rel", modulation.voiceValues(subR));
    controls.set("cA", modulation.voiceValues(subCA)); controls.set("cD", modulation.voiceValues(subCD)); controls.set("cR", modulation.voiceValues(subCR));
    controls.send();
}

void scPolyComb::sendSpread(ServerState& st) {
    if(st.builtVoices != n()) return;
    auto* s = st.spread;
    if(!s) return;
    s->set("in", st.voiceBus ? st.voiceBus->index : 0);
    s->set("voices", (float)n());
    s->set("line", snaps.morph.weights(spreadLayout)[1]);
    s->set("pan", expand(panPositions(), st.spreadLanes, 0.5f));   // as many as its \\pan has
    s->set("mpan", mod(st, 13));
    s->set("vis", st.visBlock ? st.visBlock->index + VIS_PAN : 0);
}


ofxSCSynth* scPolyComb::make(ofxSCServer* server, ServerState& st, const std::string& defName, int slot) {
    auto* s = new ofxSCSynth(defName, server);
    // The voice chain's parts get their in / out from routeVoice, after they
    // exist: until then a bus nobody listens to, never the hardware's bus 0
    // (a raw, loud voice for a moment when the voices are rebuilt).
    if(st.scratch && slot >= SlFilter && slot <= SlAmp){
        s->set("in", st.scratch->index);
        s->set("out", st.scratch->index);
    }
    return s;
}

// The oscillators (by level), FM and the inserts (by their On): created the
// first time they are wanted, paused when not (inserts after a short fade)
void scPolyComb::updateParts(ofxSCServer* server, ServerState& st) {
    if(!st.created || st.builtVoices != n()) return;
    const float now = ofGetElapsedTimef();
    auto bring = [&](Part& p, bool wanted, const std::string& defName, int slot, float tail, std::function<void()> send){
        if(p.synth && p.definition != defName){
            p.synth->free(); delete p.synth; p.synth=nullptr;
            p.running=false; p.offAt=-1.0f;
        }
        if(wanted){
            if(!p.synth){
                p.synth = make(server, st, defName, slot);
                p.definition = defName;
                p.running = true;
                scStartConfiguredSynth(p.synth, server, 1, st.groups[slot]->nodeID, [&](){
                    send();
                    routeVoice(st);
                });
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
    bring(st.sub, snaps.morph.activation(subOn) > 1e-6f, def("PCSub"), SlSub, 0.05f, [&](){ sendSub(st); });
    bring(st.fm, snaps.morph.activation(fmOn) > 1e-6f, def("PCFm"), SlFm, 0.0f, [&](){ sendFm(st); });
    for(int o = 0; o < NumOsc; o++) bring(st.osc[o], oscWanted(o), def(kOscDefs[o]), SlOsc, 0.0f, [&, o](){ sendOsc(st, o); });
    static const char* insDefs[NumIns] = {"PCFilter", "PCCombR", "PCRingR", "PCFilter", "PCShaper"};
    static const int insSlots[NumIns] = {SlFilter, SlComb, SlRing, SlPost, SlShaper};
    for(int i = 0; i < NumIns; i++) {
        const char* name=insDefs[i];
        if((i==IFilter && fType.get()>=4) || (i==IPost && postType.get()>=4)) name="PCFilterSlopes";
        if((i==IFilter && filterModel.get()==1) || (i==IPost && postFilterModel.get()==1)) name="PCFilterR";
        bring(st.ins[i],insWanted(i),def(name),insSlots[i],i==IComb ? 2.0f : 0.1f,[&,i](){sendIns(st,i);});
    }
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
        if(!p.synth->isCreated()) p.synth->resendStoredArgs();
        from = st.insBus[i]->index;
    }
    if(st.amp){
        st.amp->set("in", from);
        st.amp->set("out", st.voiceBus ? st.voiceBus->index : 0);
        if(!st.amp->isCreated()) st.amp->resendStoredArgs();
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
    audio(st.scratch, W);
    control(st.noteBus, 3 * W);       // PCPitch: the note, before vibrato, before the glide
    control(st.visBlock, 3 * W + 1);
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
        scStartConfiguredSynth(s, server, 1, st.groups[slot]->nodeID, [&](){
            send();
            if(slot == SlAmp) routeVoice(st);
            if(slot == SlSpread) routeChain(st);
        });
    };
    // the shared modulation first: its buses are what the parts read
    if(!st.modCreated){
        modulation.create(server, st.groups[SlSrc]->nodeID, st.groups[SlRoute]->nodeID, st.gateBus ? st.gateBus->index : 0);
        st.modCreated = true;
    }
    fixed(st.gateS, def("ModGate"), SlGate, [&](){ sendGate(st); });
    fixed(st.ampEnv, def("ModEnvAr"), SlSrc, [&](){ sendAmpEnv(st); });
    fixed(st.pitchS, def("PCPitch"), SlPitch, [&](){ sendPitch(st); });
    fixed(st.amp, def("PCAmp"), SlAmp, [&](){ sendAmp(st); if(fadingIn) st.amp->set("level", 0.0f); });
    if(fadingIn) st.amp->set("level", master.get());   // \level glides up (50 ms)
    // up to 16 voices the original spreader, more: its 32-lane version
    const bool wide = n() > LEGACY_VOICES;
    st.spreadLanes = wide ? W : LEGACY_VOICES;
    const std::string spreadDef = std::string(wide ? "PCSpreadWide" : "PolyCombSpread") + ofToString(outChannels.get());
    fixed(st.spread, spreadDef, SlSpread, [&](){ sendSpread(st); });
    updateParts(server, st);
    updateStages(server, st);
    updateDelta(server, st);
}

// Free every synth (the groups and buses stay)
void scPolyComb::freeSynths(ServerState& st) {
    auto drop = [](ofxSCSynth*& s){ if(s){ s->free(); delete s; s = nullptr; } };
    drop(st.gateS); drop(st.ampEnv); drop(st.pitchS); drop(st.amp); drop(st.spread); drop(st.delta);
    auto dropPart = [&](Part& p){ drop(p.synth); p.running = false; p.offAt = -1.0f; };
    dropPart(st.fm); dropPart(st.sub);
    for(auto& p : st.osc) dropPart(p);
    for(auto& p : st.ins) dropPart(p);
    for(auto& f : st.fx){ drop(f.synth); f.running = false; f.offAt = -1.0f; }
}

// Everything, with the groups and buses
void scPolyComb::freeAll(ofxSCServer* server, ServerState& st) {
    releaseVoiceMorphServer(server, st);
    if(st.root && st.created) st.root->free();          // frees every synth in it
    auto forget = [](ofxSCSynth*& s){ delete s; s = nullptr; };
    forget(st.gateS); forget(st.ampEnv); forget(st.pitchS); forget(st.amp); forget(st.spread); forget(st.delta);
    if(st.modCreated) modulation.free(server, false);   // the root's /n_free took its synths
    st.modCreated = false;
    forget(st.fm.synth); forget(st.sub.synth); st.sub.running=false; st.sub.offAt=-1.0f;
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
    audio(st.scratch);
    for(auto& f : st.fx) audio(f.in);
    control(st.noteBus); control(st.visBlock);
}

// A new voice count: every synth again, in its slot (the groups keep the
// node's place in the graph)
void scPolyComb::rebuildAll() {
    // Unlike ordinary control sends, rebuilds must visit mismatched widths.
    for(auto& [server, st] : states){
        if(!st.created) continue;
        const bool wasLatency = server->getBLatency();
        server->setBLatency(true);
        freeSynths(st);
        modulation.rebuild(server);     // N voices wide
        createAll(server, st);
        server->setBLatency(wasLatency);
    }
}

// A new voice count, while playing: the amp glides to silence (its \level
// lag, 50 ms), then everything is rebuilt and glides back (update())
void scPolyComb::requestRebuild() {
    bool playing = false;
    for(auto& [server, st] : states) if(st.created && st.amp){ st.amp->set("level", 0.0f); playing = true; }
    if(!playing){ rebuildAll(); return; }
    rebuildAt = ofGetElapsedTimef() + 0.06f;
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
        // up to 16 voices the original spreader, more: its 32-lane version
        const bool wide = n() > LEGACY_VOICES;
        st.spreadLanes = wide ? W : LEGACY_VOICES;
        const std::string spreadDef = std::string(wide ? "PCSpreadWide" : "PolyCombSpread") + ofToString(outChannels.get());
        st.spread = make(server, st, spreadDef, SlSpread);
        scStartConfiguredSynth(st.spread, server, 1, st.groups[SlSpread]->nodeID, [&](){
            sendSpread(st);
            routeChain(st);
        });
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
    if(voiceRenderer && rendererBefore.count(server)) st.root->create(2, rendererBefore[server]);
    else st.root->create(0, 1);
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
    bool any=false; for(const auto& item:states) any|=item.second.created;
    if(!any && !voiceRenderer) {
        voicePeers.clear(); voiceMorphWeights.clear(); morphRenderVoices=0; voiceAliasCount=0;
        voiceHandoff=0; voiceEndpointSince=-1; morphDomainVoices=0;
    }
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
    if(gateIn.take(server, node, bus)) sendGate(st);
    if(pitchIn.take(server, node, bus)) sendPitch(st);
}
void scPolyComb::resetInputBusses(ofxSCServer* server, int) {
    auto& st = states[server];
    st.server = server;
    gateIn.reset(server); pitchIn.reset(server);
    sendGate(st);
    sendPitch(st);
}

void scPolyComb::resendParametersForNRT() {
    forStates([this](ofxSCServer* server, ServerState& st){
        sendGate(st); sendAmpEnv(st); sendPitch(st); sendFm(st); sendAmp(st); sendSpread(st); sendDelta(st);
        for(int o = 0; o < NumOsc; o++) sendOsc(st, o);
        for(int i = 0; i < NumIns; i++) sendIns(st, i);
        for(int s = 0; s < NumStages; s++) if(st.fx[s].synth) sendStage(s, st.fx[s].synth, server);
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
    if(voiceRenderer) return;
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
                f.running = true;
                scStartConfiguredSynth(f.synth, server, 1, st.groups[SlComp + s]->nodeID, [&](){
                    f.synth->set("in", f.in->index);
                    sendStage(s, f.synth, server);
                    routeChain(st);
                });
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
void scPolyComb::routeChain(ServerState& st) {
    int next = st.outBus;
    for(int s = NumStages - 1; s >= 0; s--){
        auto& f = st.fx[s];
        if(!f.synth || !f.running || !f.in) continue;
        f.synth->set("out", next);
        if(!f.synth->isCreated()) f.synth->resendStoredArgs();
        next = f.in->index;
    }
    if(st.voiceMix && st.voiceMixReady>=0 && ofGetElapsedTimef() >= st.voiceMixReady) {
        st.voiceMix->set("out", next);
        if(!st.voiceMix->isCreated()) st.voiceMix->resendStoredArgs();
        next=st.voiceMainBus->index;
    }
    if(st.spread){
        st.spread->set("out", next);
        if(!st.spread->isCreated()) st.spread->resendStoredArgs();
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Update: parts that faded out pause; the window's live values
// ════════════════════════════════════════════════════════════════════════════

void scPolyComb::update(ofEventArgs& args) {
    if(!voiceRenderer) timeline.update();
    {   // the snapshot names, to a published Snapshot / Morph A / B dropdown
        const auto& names = snaps.slotNames();
        for(const auto& key : snaps.slotKeys()) { auto it = published.find(key); dropdowns.set(key, names, it == published.end() ? nullptr : it->second); }
    }
    sendEnvMode();      // Auto follows Gate's connections
    if(!internalPitch() && pitchIn.channels() != pitchCableChannels){   // the cable's source changed its voices
        pitchCableChannels = pitchIn.channels();
        forStates([this](ofxSCServer*, ServerState& st){ sendPitch(st); });
    }
    if(rebuildAt >= 0.0f && ofGetElapsedTimef() >= rebuildAt){
        rebuildAt = -1.0f;
        fadingIn = true;
        rebuildAll();
        fadingIn = false;
    }
    modulation.showing = showWindow.get();
    modulation.update();   // the LFO / envelope live values (window)
    // snapshots: a file dialog (outside ImGui), another node's library change, a glide
    if(!voiceRenderer) snaps.update();
    updateVoiceMorph(args);
    const float now = ofGetElapsedTimef();
    for(auto& [server, st] : states){
        if(!st.created || st.builtVoices != n()) continue;
        if(deltaRouted() != (st.delta != nullptr)) updateDelta(server, st);   // a Delta route made / removed
        bool reroute = false;
        auto settle = [&](Part& p){
            if(p.offAt < 0.0f || now < p.offAt) return;
            p.offAt = -1.0f;
            p.running = false;
            if(p.synth) p.synth->run(false);
            reroute = true;
        };
        settle(st.fm); settle(st.sub);
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
                if(st.fx[StComp].running) server->requestControlValues(st.visBlock->index + VIS_GR, 1);
                if(st.sub.running) server->requestControlValues(st.visBlock->index + VIS_SUB, n());
            }
            break;
        }
    }
}

bool scPolyComb::stageOn(int s) const { return stageAmount(s) > 1e-6f; }
float scPolyComb::stageAmount(int s) const {
    return snaps.morph.activation(s == StComp ? compOn : s == StEq ? eqOn : s == StBlur ? blurOn : s == StEcho ? echoOn : rvbOn);
}

std::string scPolyComb::stageDef(int s) const {
    static const char* names[NumStages] = {"mixerProComp", "mixerProEQ", "PCBlur", "mixerProEcho", "boxMicroSpace"};
    if(s == StEcho && echoModel.get() == 1) return "PCEchoR" + ofToString(outChannels.get());
    return names[s] + ofToString(outChannels.get());
}

float scPolyComb::echoDelaySec() const {
    if(!echoBeats.get()) return ofClamp(echoDelay.get(), 0.005f, 4.0f);
    return ofClamp(echoBeatVal.get() * 60.0f / std::max(1.0f, currentBpm), 0.005f, 4.0f);
}

float scPolyComb::stageTail(int s) const {
    if(s == StEq) return 0.0f;
    if(s == StComp) return 0.04f;
    if(s == StBlur){
        // the eight allpass tails one after another (stage delays x their decay)
        const float g = ofLerp(0.25f, 0.72f, ofClamp(blurDensity.get(), 0.0f, 1.0f));
        return std::min(30.0f, blurTime.get() * 0.001f * 13.43f * std::log(0.001f) / std::log(g) + 0.1f);
    }
    if(s == StEcho){
        const float feed = ofClamp(echoFeed.get(), 0.0f, 0.98f);
        const float repeats = feed > 0.001f ? std::log(0.001f) / std::log(feed) : 0.0f;
        return std::min(30.0f, echoDelaySec() * (repeats + 1.0f) + 0.2f);
    }
    return std::min(30.0f, rvbDecay.get() + rvbPredelay.get() * 0.001f + 0.3f);
}

void scPolyComb::sendStage(int s, ofxSCSynth* x, ofxSCServer* server) {
    if(!x) return;
    scSynthControlBatch controls(x, server);
    const float on = stageAmount(s);
    if(s == StComp){
        controls.set("threshold", compThreshold.get()); controls.set("ratio", compRatio.get());
        controls.set("knee", compKnee.get()); controls.set("attack", compAttack.get());
        controls.set("release", compRelease.get()); controls.set("makeup", compMakeup.get());
        controls.set("automakeup", compAuto.get() ? 1.0f : 0.0f); controls.set("rms", compRms.get());
        controls.set("schpf", compHpf.get()); controls.set("mix", compMix.get()); controls.set("on", on);
        auto it=states.find(server);
        controls.set("grBus", it!=states.end() && it->second.visBlock ? (float)(it->second.visBlock->index+VIS_GR) : -1.0f);
    }else if(s == StEq){
        // mixerProEQ: gains dB, freqs Hz, shelves bNslope (= 1/S), peaks bNrq (= 1/Q)
        for(int b = 0; b < 5; b++){
            // the bands' gains fade with EQ On (0 dB: the exact dry signal)
            const std::string k = "b" + ofToString(b + 1);
            controls.set(k + "gain", eqGain[b].get() * on);
            controls.set(k + "freq", eqFreq[b].get());
            if(scEQEditor::isShelf(b)) controls.set(k + "slope", 1.0f / scEQEditor::safeShelfSlope(eqShape[b].get(), eqGain[b].get() * on));
            else controls.set(k + "rq", 1.0f / std::max(0.01f, eqShape[b].get()));
        }
    }else if(s == StBlur){
        controls.set("time", blurTime.get()); controls.set("blur", blurAmount.get());
        controls.set("density", blurDensity.get()); controls.set("spread", blurSpread.get());
        controls.set("hipass", blurLowCut.get()); controls.set("lowpass", blurHighCut.get());
        controls.set("mix", blurMix.get()); controls.set("on", on);
    }else if(s == StEcho && echoModel.get() == 1){
        controls.set("delay", echoDelaySec());
        controls.set("spread", echoSpread.get());
        controls.set("feed", ofClamp(echoFeed.get(), 0.0f, 0.98f));
        controls.set("hi", echoHi.get());
        controls.set("lo", echoLo.get());
        controls.set("res", echoRes.get());
        controls.set("mix", ofClamp(snaps.morph.value(echoMix), 0.0f, 1.0f));
        controls.set("on", on);
    }else if(s == StEcho){
        controls.set("delay", echoDelaySec());
        controls.set("feed", ofClamp(echoFeed.get(), 0.0f, 0.98f));
        controls.set("cutoff", ofClamp(echoCutoff.get(), 12.0f, 130.0f));
        controls.set("resonance", ofClamp(echoResonance.get(), 0.0f, 0.9f));
        controls.set("filtertype", (float)ofClamp(echoFilter.get(), 0, 3));
        controls.set("pingpong", ofClamp(echoPingPong.get(), 0.0f, 1.0f));
        controls.set("mix", ofClamp(snaps.morph.value(echoMix), 0.0f, 1.0f));
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

void scPolyComb::sendStageAll(int s) {
    for(auto& [server, st] : states){
        auto& f = st.fx[s];
        if(!f.synth) continue;
        sendStage(s, f.synth, server);
        if(!stageOn(s) && s != StEq) f.synth->set("on", 0.0f);
    }
}

void scPolyComb::presetSave(ofJson& j) {
    modulation.saveLfoOutputs(j);
    modulation.captureGeneratorState(j, "polyComb");
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
    timeline.save(j, "polyCombTimeline");
    const auto sound = captureSnapshot();
    if(sound.contains(scMorphPolicies::metadata)) j["polyCombMorphAudio"] = sound[scMorphPolicies::metadata];
}

void scPolyComb::loadBeforeConnections(ofJson& j) {
    scDroneMode::migrate(j);
    modulation.loadLfoOutputs(j);
    if(!j.contains("polyCombPublished") || !j["polyCombPublished"].is_array()) return;
    std::vector<std::string> wanted;
    for(const auto& k : j["polyCombPublished"]) if(k.is_string()) wanted.push_back(k.get<std::string>());
    const auto old = publishedKeys;
    for(const auto& k : old) if(std::find(wanted.begin(), wanted.end(), k) == wanted.end()) unpublish(k);
    for(const auto& k : wanted) publish(k);
    timeline.load(j, "polyCombTimeline");
}

void scPolyComb::presetRecallAfterSettingParameters(ofJson& j) {
    scDroneMode::migrate(j);
    if(!j.contains(drone.getEscapedName())) drone = false;
    pitchMorph.clear();
    combSaturation.setWithoutEventNotifications(false);
    combSatLevel.setWithoutEventNotifications(12.0f);
    restoring=true;
    for(const auto& c : controls) if(compDefaults.contains(c.key)) c.load(compDefaults[c.key]);
    for(const auto& c : controls) if(blurDefaults.contains(c.key)) c.load(blurDefaults[c.key]);
    for(const auto& c : controls) if(subDefaults.contains(c.key)) c.load(subDefaults[c.key]);
    for(const auto& c : controls) if(tremDefaults.contains(c.key)) c.load(tremDefaults[c.key]);
    for(const auto& c : controls) if(pitchDefaults.contains(c.key)) c.load(pitchDefaults[c.key]);
    for(const auto& c : controls) if(deltaDefaults.contains(c.key)) c.load(deltaDefaults[c.key]);
    restoring=false;
    if(j.contains("polyComb") && j["polyComb"].is_object()){
        restoring = true;
        const auto saved = pcmigration::controls(j["polyComb"]);
        for(const auto& control : controls)
            if(saved.contains(control.key)) control.load(saved[control.key]);
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
                std::vector<float> wide(ALL_SOURCES * NUM_TARGETS, 0.0f);
                for(int r = 0; r < o.rows; r++)
                    for(int t = 0; t < o.cols; t++) wide[o.map[r] * NUM_TARGETS + t] = m[r * o.cols + t];
                m = wide;
                break;
            }
            m.resize(ALL_SOURCES * NUM_TARGETS, 0.0f);   // (no Delta row before it existed)
            modulation.rescaleSavedMatrix(m, j.value("polyCombParams", ofJson::array()));
            modulation.matrix = m;
        } catch(...) {}
    }
    if(j.contains("polyCombLfos") && j["polyCombLfos"].is_number()) modulation.lfoCount = std::clamp(j["polyCombLfos"].get<int>(), 1, NUM_LFOS);
    if(j.contains("polyCombEnvs") && j["polyCombEnvs"].is_number()) modulation.envCount = std::clamp(j["polyCombEnvs"].get<int>(), 0, NUM_ENVS);
    if(j.contains("polyCombExts") && j["polyCombExts"].is_number()) modulation.extCount = std::clamp(j["polyCombExts"].get<int>(), 0, NUM_EXT);
    sendEnvMode(true);
    modulation.loadParamsJson(j.contains("polyCombParams") ? j["polyCombParams"] : ofJson::array());
    updateScramble();
    modulation.loadGeneratorState(j, "polyComb");
    const auto sound = snapshotOf(j);
    snaps.morph.begin(sound,sound,0,[this](const std::string& key){return drivenByConnection(key);});
    snaps.morph.end();
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
    dropdowns.apply(key, published[key]);
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
    if(timeline.isHidden(key)){
        if(ImGui::MenuItem("Publish to Node GUI")) timeline.reveal(key);
    }else if(published.count(key)){
        if(isConnected(key)) ImGui::TextDisabled("Disconnect it to unpublish");
        else if(ImGui::MenuItem("Unpublish from Node")) timeline.unpublish(key);
    }else if(getParameterGroup().contains(key)) ImGui::TextDisabled("On the node");
    else if(ImGui::MenuItem("Publish to Node GUI")) publish(key);
    timeline.menuItems(key);
    modulation.modulateMenu(key);
    ImGui::EndPopup();
}

void scPolyComb::publishedUnderline(const std::string& key) const {
    timeline.mark(key);
    if(!published.count(key) || timeline.isHidden(key)) return;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y - 1.0f), ImVec2(b.x, b.y - 1.0f), ImGui::GetColorU32(ImGuiCol_Text));
}

std::vector<float> scPolyComb::panPositions() const {
    return snaps.morph.evaluateVector(spreadLayout,[&](int layoutMode){
        return snaps.morph.evaluateVector(spreadMode,[&](int placementMode){
    const int nv = n(), c = outChannels.get();
    const bool ring = c > 2 && layoutMode == 0;
    std::vector<float> p(nv, 0.5f);
    if(placementMode == 2) return expand(panP.get(), nv, 0.5f);
    for(int i = 0; i < nv; i++){
        if(placementMode == 1){                       // each voice on a channel
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

        });
    });
}

// The incoming pitches over the voices: as many as there are; the rest repeat
// the last, or (Expand) the whole vector again, 12 semitones up each time.
std::vector<float> scPolyComb::spreadPitch(const std::vector<float>& v) const {
    const int nv = n();
    if(v.empty()) return std::vector<float>(nv, 60.0f);
    std::vector<float> out(nv);
    const int k = (int)v.size();
    for(int i = 0; i < nv; i++)
        out[i] = i < k ? v[i] : expandPitch.get() ? v[i % k] + 12.0f * (i / k) : v.back();
    return out;
}
// ... then scrambled over the voices
std::vector<float> scPolyComb::processedPitch() const {
    const auto v = spreadPitch(pitch.get());
    if(!scramble.get() || v.size() <= 1 || scrambleOrder.size() < v.size()) return v;
    std::vector<float> out(v.size());
    for(size_t i = 0; i < v.size(); i++) out[i] = v[std::clamp(scrambleOrder[i], 0, (int)v.size() - 1)];
    return out;
}

// A scheduled Gate invokes this listener once with its timetag in effect; the
// later GUI delivery is suppressed. Generate one seed per map, not per server
// or per voice, and send only the affected pitch controls before the new gate.
void scPolyComb::randomizeMapSeedsOnGate(const std::vector<float>& values) {
    bool rising=false;
    for(int i=0;i<n();++i){
        const float now=values.empty()?0.f:values[std::min((size_t)i,values.size()-1)];
        const float before=previousMapGate.empty()?0.f:previousMapGate[std::min((size_t)i,previousMapGate.size()-1)];
        if(now>0.f && before<=0.f)rising=true;
    }
    previousMapGate=values;
    if(!rising)return;
    auto reseed=[](MapParams& m){
        if(!m.indexSeedOnGate.get())return false;
        int seed=(int)ofRandom(10000.f);
        if(seed==m.indexSeed.get())seed=(seed+1)%10000;
        m.indexSeed.setWithoutEventNotifications(seed);
        return true;
    };
    const bool oscillators=reseed(oscMap),resonators=reseed(combMap);
    if(!oscillators && !resonators)return;
    if(oscillators)pitchMorph.reseed("Osc_Map_",oscMap.indexSeed.get());
    if(resonators)pitchMorph.reseed("Comb_Map_",combMap.indexSeed.get());
    auto inEffect=noteInputs.inEffectNow();
    forStates([&](ofxSCServer*,ServerState& st){
        if(oscillators && internalPitch() && st.pitchS){
            scSynthControlBatch controls(st.pitchS,st.server);
            controls.set("pitch",oscillatorPitches());controls.send();
        }
        if(resonators && combMapped() && st.ins[IComb].synth)
            sendVec(st.ins[IComb].synth,"combpitch",combMapPitches());
        if((combMapped()?resonators:(oscillators && internalPitch())) && ringPitchSource.get()==2 && st.ins[IRing].synth)
            sendVec(st.ins[IRing].synth,"ringpitch",resonatorPitches());
    });
}

// The index domain and its published controls follow the active voice count.
void scPolyComb::updatePitchIndexLimits() {
    const int count=std::clamp(std::max(n(),morphDomainVoices),1,(int)MAX_VOICES);
    for(auto* m:{&oscMap,&combMap}){
        m->first.setMin(1.f);m->first.setMax((float)count);
        m->first.setWithoutEventNotifications(std::clamp(std::round(m->first.get()),1.f,(float)count));
        m->indexCycleShift.setMin(1-count);m->indexCycleShift.setMax(count-1);
        m->indexCycleShift.setWithoutEventNotifications(std::clamp(m->indexCycleShift.get(),1-count,count-1));
        m->indexSymmetry.setMax(count-1);
        m->indexSymmetry.setWithoutEventNotifications(std::clamp(m->indexSymmetry.get(),0,count-1));
        m->indexQuantize.setMax((float)count);
        m->indexQuantize.setWithoutEventNotifications(std::clamp(std::round(m->indexQuantize.get()),0.f,(float)count));
        m->indexRandom.setMax((float)(count-1));
        m->indexRandom.setWithoutEventNotifications(std::clamp(m->indexRandom.get(),0.f,(float)(count-1)));
    }
}

// ── Pitch & Harmony ─────────────────────────────────────────────────────────

pcmap::Map scPolyComb::mapOf(const MapParams& m) const {
    pcmap::Map r;
    r.morphGeometry=true;
    r.skew=m.indexSkew.get();r.roundness=m.indexRoundness.get();
    r.pulseWidth=m.indexPW.get();r.pow=m.indexPow.get();
    r.shape = m.shape.get();
    r.first = m.first.get();
    r.interval = m.interval.get();
    r.transpose = m.transpose.get();
    r.bend = m.bend.get();
    r.offset = m.offset.get();
    r.mode = m.mode.get();
    r.quantize = m.quantize.get();
    r.root = std::round(m.root.get());
    r.tones = m.tones.get();
    r.table = m.table.get();
    r.harmonicBase = m.harmonicBase.get();
    r.span = m.span.get();
    r.degreeOffset = m.degreeOffset.get();
    r.octaveJump = m.octaveJump.get();
    r.waves = m.indexWaves.get();
    r.cycleShift = m.indexCycleShift.get();
    r.invert = m.indexInvert.get();
    r.symmetry = m.indexSymmetry.get();
    r.comb = m.indexComb.get();
    r.indexQuantize = m.indexQuantize.get();
    r.indexRandom = m.indexRandom.get();
    r.seed = m.indexSeed.get();
    return r;
}

// A map's pitch per voice with the played Pitch around 60 (61: +1, one value
// for all voices or one each): added after the map, or before its quantization
// ("in the scale"). Scrambled as Pitch's own values would be.
std::vector<float> scPolyComb::mapPitches(const MapParams& m) const {
    // A map expands one played root into voices. Incoming chord positions must
    // not add a second, unrelated per-voice pitch shape after indexation.
    return pcmap::playedPitches(mapOf(m),n(),first(pitch.get(),60.0f),pitchShiftMode.get()==1,
                               scramble.get() ? scrambleOrder : std::vector<int>{});
}

// Shared map means the same voice order and note choices, with only the
// resonators' own coarse/fine pitch adjustment applied after that source.
ofJson scPolyComb::pitchMorphOverrides() const {
    ofJson out=ofJson::object();
    for(const auto& c:controls) if(scPolyCombPitchMorph::control(c.key) && drivenByConnection(c.key)) out[c.key]=c.save();
    return out;
}
float scPolyComb::combMapAmount() const {
    return pitchMorph.active()?pitchMorph.ownWeight(pitchMorphOverrides()):(combMapOn.get()?1.f:0.f);
}
std::vector<float> scPolyComb::combMapPitches() const {
    if(pitchMorph.active() && internalPitch()) {
        auto notes=pitchMorph.evaluate(n(),first(pitch.get(),60.f),pitchMorphOverrides());
        // Retain the live oscillator note bus (including transpose/detune) for
        // the inherited fraction. The residual makes the resulting pitches the
        // exact weighted endpoint map pitches, rather than a second map blend.
        for(int i=0;i<n();++i) notes.resonator[i]-=(1-notes.own)*notes.oscillator[i];
        return notes.resonator;
    }
    return resonatorPitches();
}
std::vector<float> scPolyComb::oscillatorPitches() const {
    if(internalPitch() && pitchMorph.active())
        return pitchMorph.evaluate(n(),first(pitch.get(),60.f),pitchMorphOverrides()).oscillator;
    return internalPitch()?mapPitches(oscMap):processedPitch();
}
std::vector<float> scPolyComb::resonatorPitches() const {
    if(internalPitch() && pitchMorph.active())
        return pitchMorph.evaluate(n(),first(pitch.get(),60.f),pitchMorphOverrides()).resonator;
    if(combMapped())return mapPitches(combMap);
    auto notes=oscillatorPitches();
    for(auto& note:notes)note=ofClamp(note+combMap.transpose.get()+combMap.offset.get(),0.f,135.f);
    return notes;
}

std::vector<float> scPolyComb::ringSourcePitches() const {
    if(ringPitchSource.get()==0)return expand(ringPitch.get(),n());
    if(ringPitchSource.get()==2)return resonatorPitches();
    auto notes=oscillatorPitches();
    const auto coarse=expand(transpose.get(),n());
    for(int i=0;i<n();++i)notes[i]+=coarse[i];
    return notes;
}

// One registered parameter backs both the editor library and the node dropdown.
const chordbox::LibItem* scPolyComb::mapScaleAt(int index) const {
    if(index<=0)return nullptr;
    --index;
    if(index<(int)scaleLibrary.size())return &scaleLibrary[index];
    index-=(int)scaleLibrary.size();
    return index<(int)chordLibrary.size()?&chordLibrary[index]:nullptr;
}
void scPolyComb::selectMapScale(MapParams& m) {
    const auto* item=mapScaleAt(m.scaleSelection.get());
    if(!item)return; // User: keep the editable notes.
    std::vector<float> notes;
    for(float value:item->values){const float pc=pcmap::floorMod(std::round(value),12);
        if(std::find(notes.begin(),notes.end(),pc)==notes.end())notes.push_back(pc);}
    if(notes.empty())notes={0};
    if(m.mode.get()==pcmap::ModeTones)m.tones.set(notes);else m.table.set(notes);
}
void scPolyComb::syncMapScale(MapParams& m) {
    auto minimal=[](const std::vector<float>& input){std::vector<float> out;
        for(float value:input){const float pc=pcmap::floorMod(std::round(value),12);
            if(std::find(out.begin(),out.end(),pc)==out.end())out.push_back(pc);}
        return out.empty()?std::vector<float>{0}:out;};
    const auto notes=minimal(m.mode.get()==pcmap::ModeTones?m.tones.get():m.table.get());
    const auto* selected=mapScaleAt(m.scaleSelection.get());
    if(selected && minimal(selected->values)==notes)return;
    int match=0;
    for(int i=1;i<=int(scaleLibrary.size()+chordLibrary.size());++i)
        if(minimal(mapScaleAt(i)->values)==notes){match=i;break;}
    m.scaleSelection.setWithoutEventNotifications(match);
}

// ChordBox's libraries (Supercollider/Pitchclass): the scales and chords a map takes its tones from
void scPolyComb::loadPitchLibraries() {
    const std::string dir = ofToDataPath("Supercollider/Pitchclass/", true);
    scaleLibrary = chordbox::Engine::parsePitchClassFile(dir + "scales.txt");
    chordLibrary = chordbox::Engine::parsePitchClassFile(dir + "chords.txt");
    std::vector<std::string> names{"User"};
    for(const auto& item:scaleLibrary)names.push_back("Scale: "+item.name);
    for(const auto& item:chordLibrary)names.push_back("Chord: "+item.name);
    for(auto* m:{&oscMap,&combMap}){
        m->scaleSelection.setMax((int)names.size()-1);
        dropdowns.set(m->scaleSelection.getEscapedName(),names);
        syncMapScale(*m);
    }
}

// ── Delta ───────────────────────────────────────────────────────────────────

bool scPolyComb::deltaRouted() const {
    const auto& m = modulation.matrix;
    for(int t = 0; t < NUM_TARGETS; t++){
        const size_t i = (size_t)SRC_DELTA * NUM_TARGETS + t;
        if(i < m.size() && std::abs(m[i]) > 1e-4f) return true;
    }
    return false;
}

void scPolyComb::sendDelta(ServerState& st) {
    auto* s = st.delta;
    if(!s) return;
    s->set("a", src(st, std::clamp(deltaSrcA.get(), 0, NUM_LFOS - 1)));
    s->set("b", src(st, std::clamp(deltaSrcB.get(), 0, NUM_LFOS - 1)));
    s->set("mixa", deltaMixA.get());
    s->set("mixb", deltaMixB.get());
    s->set("window", deltaWindow.get());
    s->set("gain", deltaGain.get());
    s->set("out", src(st, SRC_DELTA));
}

// PCDelta runs only while a route takes it: at the head of the routes' slot
// (after the LFOs, before the routes that read it)
void scPolyComb::updateDelta(ofxSCServer* server, ServerState& st) {
    if(!st.created || !st.modCreated) return;
    const bool wanted = deltaRouted();
    if(wanted && !st.delta){
        st.delta = make(server, st, def("PCDelta"), SlRoute);
        scStartConfiguredSynth(st.delta, server, 0, st.groups[SlRoute]->nodeID, [&](){ sendDelta(st); });
    }else if(!wanted && st.delta){
        st.delta->free(); delete st.delta; st.delta = nullptr;
        ofxOscMessage m;                       // its bus keeps the last value: a later route starts from 0
        m.setAddress("/c_fill");
        m.addIntArg(src(st, SRC_DELTA));
        m.addIntArg(W);
        m.addFloatArg(0.0f);
        server->sendMsg(m);
    }
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
                const auto bundled = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path().parent_path() / bundledDir;
                if(std::filesystem::exists(bundled / expected)) folder = bundled;
            }
        } catch(const std::exception& e) { ofLogWarning("scPolyComb") << e.what(); }
        return folder;
    };
    for(const auto& [expected, dir] : std::vector<std::pair<std::string, std::string>>{
            {"PCVoiceMorph2.scsyndef", "synthdefs/Defaults/PolyComb/CompiledSynthdefs/PolyComb"},
            {"PCSub2.scsyndef", "synthdefs/Defaults/PolyComb/CompiledSynthdefs/PolyComb"},
            {"PCAmp2.scsyndef", "synthdefs/Defaults/PolyComb/CompiledSynthdefs/PolyComb"},
            {"ModLfo2.scsyndef", "synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation"},
            {"PCFilterSlopes1.scsyndef", "synthdefs/Defaults/PolyComb/CompiledSynthdefs/PolyComb"},
            {"mixerProComp2.scsyndef", "synthdefs/Defaults/MixerPro/CompiledSynthdefs/mixerpro"}}){
        const auto folder = find(expected, dir);
        if(folder.empty()){ ofLogError("scPolyComb") << "Compile the SynthDefs of " << dir; continue; }
        for(auto* sm : servers) if(sm) sm->requireSynthdefFolder(folder.string());
    }
}

bool scPolyComb::droning() const {
    return drone.get();
}

template<typename T>
void scPolyComb::reg(ofParameter<T>& p, std::function<void()> send) {
    Control c;
    c.key = p.getEscapedName();
    c.name = p.getName();
    c.param = &p;
    c.add = [this, &p]() { return addParameter(p); };
    c.save = [&p]() { return ofJson(p.get()); };
    c.load = [&p,key=c.key](const ofJson& j) {
        try { p.set(pccontrol::normalize(key,j.get<T>(),p.getMin(),p.getMax())); } catch(...) {}
    };
    controls.push_back(std::move(c));
    listeners.push(p.newListener([this,&p,key=p.getEscapedName(),send](T& value) {
        snaps.morph.edited(p);
        const auto normalized=pccontrol::normalize(key,value,p.getMin(),p.getMax());
        if(normalized!=value)p.setWithoutEventNotifications(normalized);
        if(!applyingSnapshot && !drivenByConnection(key) &&
            (scPolyCombPitchMorph::control(key) || key==pitchExternal.getEscapedName())) pitchMorph.clear();
        if(!restoring)send();
    }));
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
    const float step=pccontrol::step(key);
    ImGuiSliderFlags flags=log?ImGuiSliderFlags_Logarithmic:0;
    if(key=="Osc_Map_Fine_Tune" || key=="Comb_Map_Fine_Tune" || key=="Sub_Fine_Tune")flags|=ImGuiSliderFlags_NoRoundToFormat;
    const bool changed=step>0?scwidgets::quantizedSlider("##v",v,p.getMin(),p.getMax(),step,fmt)
        :ImGui::SliderFloat("##v",&v,p.getMin(),p.getMax(),fmt,flags);
    if(changed)p.set(v);
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
            float value=lo+t*(hi-lo);
            if(std::string(label)=="Transpose")value=std::round(value*4.f)*.25f;
            p.set({value});
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%d values, one per voice (from a connection)\nDrag: one value for every voice", (int)values.size());
    }else{
        ImGui::SetNextItemWidth(sw);
        float v = first(values, lo);
        const float step=pccontrol::step(key);
        const bool changed=step>0?scwidgets::quantizedSlider("##v",v,lo,hi,step,fmt)
            :ImGui::SliderFloat("##v",&v,lo,hi,fmt,log?ImGuiSliderFlags_Logarithmic:0);
        if(changed)p.set({v});
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

void scPolyComb::combo(ofParameter<int>& p, const char* label, float w) {
    const float z = zoom();
    const std::string key = p.getEscapedName();
    const auto items = dropdowns.names(key);
    if(items.empty()) return;
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
        const float level = (visOffset != VIS_SUB || subOn.get()) && i >= 0 && i < (int)visValues.size() ? ofClamp(visValues[i], 0.0f, 1.0f) : 0.0f;
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
    keyboardPiano.finishFrame();
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
    keyboardPiano.beginContent(pitch, gate, n());
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
        toggle(drone, "Drone");
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Drone: every voice plays continuously. Unchecked: follow Gate.");
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
            const float level = i >= 0 && i < (int)visValues.size() ? ofClamp(visValues[i], 0.0f, 1.0f) : 0.0f;
            const ImVec2 b0(a.x + v * bw + 1, a.y), b1(a.x + (v + 1) * bw - 1, a.y + mh);
            dl->AddRectFilled(b0, b1, pal::canvas, 2.0f * z);
            dl->AddRectFilled(ImVec2(b0.x, b1.y - level * mh), b1, pal::envAmp, 2.0f * z);
        }
        ImGui::Dummy(ImVec2(mw, mh));
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Each voice's amp envelope");
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    snaps.drawBar(w);

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
        tab("Pitch & Harmony", [this](float tw){ drawPitchTab(tw); });
        tab("Envelopes", [this](float tw){ drawEnvTab(tw); });
        tab("Filters", [this](float tw){ drawFilterTab(tw); });
        tab("Resonator", [this](float tw){ drawResonatorTab(tw); });
        tab("LFOs", [this](float tw){ modulation.drawLfos(tw); drawDeltaCard(tw); });
        tab("Routing", [this](float tw){ modulation.drawRouting(tw); });
        tab("Panning", [this](float tw){ drawPanTab(tw); });
        tab("Effects", [this](float tw){ drawFxTab(tw); });
        ImGui::EndTabBar();
    }
    scKeyboardPiano::endContent();
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
    const float third = (mixW - 2 * gapW()) / 3;
    ImGui::BeginGroup(); sliderVec(oscPw, "Pulse W", third, "%.2f"); ImGui::EndGroup();
    ImGui::SameLine(0, gapW());
    ImGui::BeginGroup(); sliderVec(oscSSDetune, "SS Detune", third, "%.2f"); ImGui::EndGroup();
    ImGui::SameLine(0, gapW());
    ImGui::BeginGroup(); slider(oscDustDensity, "Dust Dens", third, "%+.1f oct");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Dust grains per second: the note's frequency, down this many octaves\n(0: as the note; -10: about a thousand times sparser)");
    ImGui::EndGroup();
    nextCol();
    labelW = 80.0f;
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
    nextCol();
    sectionHeader("Tremolo", qw);
    slider(tremRate, "Trem Rate", qw, "%.2f Hz");
    slider(tremRand, "Rate Rnd", qw, "%.2f");
    slider(tremDepth, "Trem Depth", qw, "%.2f");
    slider(tremRound, "Roundness", qw, "%.2f");
    labelW = 70.0f;
    ImGui::EndGroup();
    ImGui::Spacing();
    const auto divider=ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(divider.x,divider.y+4*z),ImVec2(divider.x+w,divider.y+4*z),pal::accent,2*z);
    ImGui::Dummy(ImVec2(w,12*z));
    const float sw = colW(w, 3);
    ImGui::BeginGroup();
    sectionHeader("Independent Sub", sw, &subOn);
    slider(subLevel, "Level", sw, "%.3f");
    slider(subTranspose, "Transpose", sw, "%+.0f st");
    slider(subFineTune, "Fine Tune", sw, "%+.3f st");
    slider(subA, "Attack", sw, "%.3f s", true);
    slider(subD, "Decay", sw, "%.3f s", true);
    slider(subS, "Sustain", sw, "%.2f");
    slider(subR, "Release", sw, "%.3f s", true);
    ImGui::EndGroup(); ImGui::SameLine(0, gapW()); ImGui::BeginGroup();
    sectionHeader("Sub Vibrato", sw);
    slider(subVibRate, "Vib Rate", sw, "%.2f Hz");
    slider(subVibAmt, "Vib Amp", sw, "%.2f st");
    slider(subVibRand, "Rate Rnd", sw, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("A random rate offset for the independent sub's single vibrato oscillator");
    ImGui::EndGroup(); ImGui::SameLine(0, gapW()); ImGui::BeginGroup();
    sectionHeader("Sub Envelope", sw, nullptr, pal::envAmp);
    envelopeEditor("##subEnv", subA, subD, subS, subR, subCA, subCD, subCR, sw, 90*z, pal::envAmp, VIS_SUB);
    labelW = 100.0f;
    slider(subCA, "Attack curve", sw, "%.1f");
    slider(subCD, "Decay curve", sw, "%.1f");
    slider(subCR, "Release curve", sw, "%.1f");
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
            float db;
            if(type<4){
                db=biquadDb(type,f,fc,q);
                if(type<=1) db*=2.0f;
            }else{
                const auto choice=filterChoice(type);
                const float rq=ofClamp(1-resonance,0.02f,1.0f);
                const bool four=choice.second>=2;
                const float rq2=(choice.first<=1 && four) ? std::sqrt(rq) : rq;
                db=biquadDb(choice.first,f,fc,1.0f/rq2);
                if(four) db*=2.0f;
                if(choice.second==3){
                    const double pole=std::exp(-TWO_PI*fc/48000.0);
                    const auto zz=std::polar(1.0,-TWO_PI*f/48000.0);
                    const auto lp=(1-pole)/(1.0-pole*zz);
                    db=20*std::log10(std::max(1e-8,std::abs((1.0-lp)*lp*lp*lp)));
                }
            }
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
    combo(filterModel, "Model", cw);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("PolyComb 1.7: the original's 4-pole ladder\n(resonance takes some of the lows, 1 self-oscillates)");
    combo(fType, "Type", cw);
    std::string f1 = hzFormat(cutoff.get());
    sliderVec(cutoff, "Cutoff", cw, f1.c_str());
    sliderVec(res, "Resonance", cw, "%.2f");
    slider(keyTrack, "Key Track", cw, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("1: the cutoff follows the note (one octave per octave)");
    response(fType.get(), first(cutoff.get(), 100.0f), first(res.get(), 0.3f), pal::accent);
    nextCol();
    sectionHeader("Post Filter", cw, &postOn);
    combo(postFilterModel, "Model", cw);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("PolyComb 1.7: the original's 4-pole ladder\n(resonance takes some of the lows, 1 self-oscillates)");
    combo(postType, "Type", cw);
    std::string f2 = hzFormat(postCut.get());
    sliderVec(postCut, "Cutoff", cw, f2.c_str());
    sliderVec(postRes, "Resonance", cw, "%.2f");
    slider(postKeyTrack, "Key Track", cw, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("1: the cutoff follows the note (one octave per octave)");
    response(postType.get(), first(postCut.get(), 130.0f), first(postRes.get(), 0.0f), pal::envMod);
    ImGui::EndGroup();
    ImGui::Spacing();
    ImGui::TextColored(ImColor(pal::textDim), "Envelopes and LFOs move the cutoffs from Routing (or right-click a Cutoff: Modulate with).");
}

void scPolyComb::drawRingPitchVector(float w) {
    scPitchMapView::pitchVector(ringSourcePitches(),expand(ringTranspose.get(),n()),modulation.voiceValues(ringRatio),w,82*zoom(),zoom(),pal::accent);
}

void scPolyComb::drawResonatorTab(float w) {
    const float z = zoom();
    const float cw = colW(w, 3);
    ImGui::BeginGroup();
    sectionHeader("Comb", cw, &combOn);
    const bool follow = combMode.get() == 0;
    if(!internalPitch()){
        combo(combMode, "Pitch", cw);
        sliderVec(combPitch, follow ? "Interval" : "Note", cw, follow ? "%+.2f st" : "%.2f");
    }
    slider(combSatLevel,"Sat Level",cw,"%.1f dB");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Level at which the feedback loop saturates. Lower: stronger saturation.\nHigher levels allow louder resonant peaks.");
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
        const float g = ofClamp(first(combFeed.get(), 0.7f), -0.999f, 0.999f);   // past 1: drawn at the edge
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
        std::snprintf(t, sizeof(t), "%.1f Hz%s / saturated", f0, follow ? " at C4" : "");
        dl->AddText(ImVec2(a.x + 4 * z, a.y + 2 * z), pal::textDim, t);
        ImGui::Dummy(ImVec2(cw, h));
    }
    nextCol();
    sectionHeader("Ring Modulator", cw, &ringOn);
    combo(ringPitchSource,"Source",cw);
    if(ringPitchSource.get()==0)sliderVec(ringPitch,"Pitch",cw,"%.1f MIDI");
    sliderVec(ringTranspose,"Transpose",cw,"%+.2f st");
    slider(ringRatio, "Ratio", cw, "x%.3f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("The modulator frequency: selected pitch + Transpose, converted to Hz, multiplied by Ratio");
    slider(ringWidth, "Width", cw, "%+.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("-1: flat (no modulation), 0: a sine, 1: narrow peaks");
    sliderVec(ringDetune, "Detune", cw, "%.2f st");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("A random offset per voice, up to this many semitones");
    sliderVec(ringMix, "Amount", cw, "%.2f");
    drawRingPitchVector(cw);
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
    const float cw = colW(w, 5);
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
    sectionHeader("Blur", cw, &blurOn);
    if(!blurOn.get()) ImGui::BeginDisabled();
    slider(blurAmount, "Amount", cw, "%.2f");
    slider(blurTime, "Time", cw, "%.1f ms", true);
    slider(blurDensity, "Density", cw, "%.2f");
    slider(blurSpread, "Spread", cw, "%.2f");
    slider(blurLowCut, "Low Cut", cw, "%.0f Hz", true);
    slider(blurHighCut, "High Cut", cw, "%.0f Hz", true);
    slider(blurMix, "Mix", cw, "%.2f");
    if(!blurOn.get()) ImGui::EndDisabled();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
    ImGui::TextColored(ImColor(pal::textDim), "Smears attacks through eight allpasses: Amount = how many, Time = the shortest, Density = their tails. No echo, no feedback.");
    ImGui::PopTextWrapPos();
    nextCol();
    sectionHeader("Echo", cw, &echoOn);
    if(!echoOn.get()) ImGui::BeginDisabled();
    combo(echoModel, "Model", cw);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("MixerPro: the shared echo. PolyComb 1.7: the original's delay\n"
                                                 "(a resonant lowpass and a highpass in the loop, L / R lines).");
    const bool original = echoModel.get() == 1;
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
    if(original){
        slider(echoHi, "Hi", cw, "%.0f Hz", true);
        slider(echoRes, "Res", cw, "%.2f");
        slider(echoLo, "Lo", cw, "%.0f Hz", true);
        slider(echoSpread, "L / R", cw, "%+.2f");
    }else{
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
        publishMenu(echoFilter.getEscapedName());
    }
    slider(echoPingPong, "Ping-pong", cw, "%.2f");
    }
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
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    drawCompPanel(w);
    ImGui::TextColored(ImColor(pal::textDim), "Voices -> Compressor -> EQ -> Blur -> Echo -> Reverb");
}

// MixerPro's transfer curve, reduction meter and compressor controls.
void scPolyComb::drawCompPanel(float w) {
    const float z=zoom(), cw=colW(w,3);
    sectionHeader("Compressor", w, &compOn, IM_COL32(255,190,90,255));
    ImGui::BeginGroup();
    const bool enabled=compOn.get();
    const float side=std::min(cw-20*z,180*z);
    const ImVec2 p0=ImGui::GetCursorScreenPos(), p1(p0.x+side,p0.y+side);
    auto* dl=ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0,p1,IM_COL32(20,20,24,255),3*z);
    auto px=[&](float db){return p0.x+(db+60)/60*side;};
    auto py=[&](float db){return p1.y-(ofClamp(db,-60.0f,0.0f)+60)/60*side;};
    for(int g=-48;g<0;g+=12){
        dl->AddLine(ImVec2(px(g),p0.y),ImVec2(px(g),p1.y),IM_COL32(45,45,52,255));
        dl->AddLine(ImVec2(p0.x,py(g)),ImVec2(p1.x,py(g)),IM_COL32(45,45,52,255));
    }
    dl->AddLine(ImVec2(p0.x,p1.y),ImVec2(p1.x,p0.y),IM_COL32(70,70,80,255));
    const float thr=compThreshold.get(), knee=std::max(.01f,compKnee.get());
    const float slope=1-1/std::max(1.0f,compRatio.get());
    auto grAt=[&](float x){
        const float over=x-thr, xk=ofClamp(over+knee*.5f,0.0f,knee);
        return std::min(40.0f,slope*(xk*xk/(2*knee)+std::max(0.0f,over-knee*.5f)));
    };
    const float makeup=compMakeup.get()+(compAuto.get() ? .5f*grAt(0) : 0);
    ImVec2 prev;
    for(int i=0;i<=120;i++){
        const float x=-60+i*.5f;
        const ImVec2 pt(px(x),py(enabled ? x-grAt(x)+makeup : x));
        if(i) dl->AddLine(prev,pt,IM_COL32(255,190,90,255),2*z);
        prev=pt;
    }
    dl->AddLine(ImVec2(px(thr),p0.y),ImVec2(px(thr),p1.y),IM_COL32(255,190,90,70));
    dl->AddText(ImVec2(p0.x+4*z,p0.y+2*z),pal::textDim,"out");
    dl->AddText(ImVec2(p1.x-18*z,p1.y-16*z),pal::textDim,"in");
    ImGui::Dummy(ImVec2(side,side)); ImGui::SameLine(0,6*z);
    const ImVec2 m=ImGui::GetCursorScreenPos();
    const float gr=enabled && visValues.size()>VIS_GR ? std::max(0.0f,visValues[VIS_GR]) : 0;
    dl->AddRectFilled(m,ImVec2(m.x+10*z,m.y+side),IM_COL32(20,20,24,255),2*z);
    dl->AddRectFilled(m,ImVec2(m.x+10*z,m.y+side*ofClamp(gr/24,0.0f,1.0f)),IM_COL32(255,200,80,255),2*z);
    for(int g=6;g<24;g+=6) dl->AddLine(ImVec2(m.x,m.y+side*g/24),ImVec2(m.x+10*z,m.y+side*g/24),IM_COL32(60,60,68,255));
    ImGui::Dummy(ImVec2(10*z,side));
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Gain reduction %.1f dB (scale 0..24 dB)",gr);
    nextCol();
    if(!enabled) ImGui::BeginDisabled();
    slider(compThreshold,"Threshold",cw,"%.1f dB");
    slider(compRatio,"Ratio",cw,"%.1f : 1",true);
    slider(compKnee,"Knee",cw,"%.1f dB");
    slider(compMakeup,"Makeup",cw,"%.1f dB");
    toggle(compAuto,"Auto makeup");
    if(!enabled) ImGui::EndDisabled();
    nextCol();
    if(!enabled) ImGui::BeginDisabled();
    slider(compAttack,"Attack",cw,"%.1f ms",true);
    slider(compRelease,"Release",cw,"%.0f ms",true);
    slider(compRms,"Peak/RMS",cw,"%.2f");
    slider(compHpf,"SC HPF",cw,"%.0f Hz",true);
    slider(compMix,"Mix",cw,"%.2f");
    if(!enabled) ImGui::EndDisabled();
    ImGui::EndGroup();
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
    combo(spreadMode, "Mode", cw);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Spread: evenly across the speakers\nChannels: voice 1 on speaker 1, voice 2 on speaker 2...\n"
                                                 "Manual: each voice where you put it (drag it, or the Pan vector)");
    if(spreadMode.get() == 0){
        slider(spreadWidth, "Spread", cw, "%.2f");
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

// ── Pitch & Harmony: External, then the oscillators' and resonators' maps ────
void scPolyComb::drawPitchTab(float w) {
    const float z=zoom(),savedLabel=labelW;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(6*z,3*z));
    toggle(pitchExternal,"Incoming pitches");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("On: use the incoming Pitch values directly. Off: expand a scale with the maps below.");
    ImGui::SameLine(0,12*z);
    if(!pitchExternal.get())ImGui::BeginDisabled();
    toggle(expandPitch,"Expand pitches");
    if(!pitchExternal.get())ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("More voices than incoming pitches: repeat the pitches, 12 semitones higher each time.\nOff: the extra voices take the last pitch. Also for an audio-rate Pitch cable.");
    ImGui::SameLine(0,12*z);
    toggle(scramble,"Scramble pitches");
    ImGui::SameLine(0,12*z);
    labelW=72;
    ImGui::BeginGroup();combo(pitchShiftMode,"Transpose",245*z);ImGui::EndGroup();
    ImGui::SameLine(0,12*z);
    toggle(combMapOn,"Resonators: own map");
    if(w<1100*z)ImGui::NewLine();else ImGui::SameLine(0,12*z);
    ImGui::TextDisabled("Pitch %s / %.1f   |   %d voices",scPitchMapView::noteName(first(pitch.get(),60)).c_str(),first(pitch.get(),60),n());
    const bool sideBySide=w>=1320*z;
    const float cw=sideBySide?(w-gapW())*.5f:w;
    ImGui::BeginGroup();
    drawMapColumn(oscMap,"osc",cw,false);
    ImGui::EndGroup();
    if(sideBySide)ImGui::SameLine(0,gapW());else ImGui::Spacing();
    ImGui::BeginGroup();
    drawMapColumn(combMap,"comb",cw,true);
    ImGui::EndGroup();
    labelW=savedLabel;ImGui::PopStyleVar();
}

void scPolyComb::drawMapColumn(MapParams& edits,const char* id,float w,bool comb) {
    const bool inherited=comb&&!combMapped();
    const bool disableMap=inherited||(!comb&&!internalPitch());
    auto& m=inherited?oscMap:edits;
    const float z=zoom(),savedLabel=labelW;
    const ImU32 accent=comb?pal::envMod:pal::accent;
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(4*z,1*z));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(5*z,2*z));
    sectionHeader(comb?"Resonators map":"Oscillators map",w,nullptr,accent);
    const float columnGap=12*z;
    const float left=(w-2*columnGap)/3, right=left, tuning=left;
    ImGui::BeginGroup();
    ImGui::TextColored(ImColor(accent),"1  SCALE / SEQUENCE");
    if(disableMap)ImGui::BeginDisabled();
    labelW=45;
    const int previousMode=m.mode.get();
    const bool previousRestoring=restoring;restoring=true;
    combo(m.mode,"Map",left);
    if(previousMode!=m.mode.get()){
        if(m.mode.get()==pcmap::ModeTable){
            m.interval.set(1);m.octaveJump.set(1);
            m.degreeOffset.set(0);m.span.set(0);
        }else if(m.mode.get()==pcmap::ModeHarmonic){m.interval.set(1);m.span.set(0);}
    }
    restoring=previousRestoring;
    if(previousMode!=m.mode.get()&&!restoring)forStates([this](ofxSCServer*,ServerState& st){sendPitch(st);sendIns(st,IComb);});
    const int mode=m.mode.get();
    const bool user=mode==pcmap::ModeTable,tones=mode==pcmap::ModeTones,harmonic=mode==pcmap::ModeHarmonic;
    if(harmonic){
        labelW=65;slider(m.harmonicBase,"Base note",left,"%.0f");
        ImGui::TextDisabled("%s  /  MIDI %.0f",scPitchMapView::noteName(m.harmonicBase.get()).c_str(),m.harmonicBase.get());
    }else{
        const float x0=ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();ImGui::TextUnformatted("Root");
        publishedUnderline(m.root.getEscapedName());ImGui::SameLine();ImGui::SetCursorPosX(x0+50*z);
        ImGui::SetNextItemWidth(std::max(40*z,left-50*z));
        int root=std::clamp((int)std::lround(m.root.get()),0,11);
        if(!user&&!tones)ImGui::BeginDisabled();
        if(ImGui::Combo("##root",&root,chordbox::keyNames(),12))m.root.set((float)root);
        if(!user&&!tones)ImGui::EndDisabled();
        publishMenu(m.root.getEscapedName());
    }
    auto minimal=[](const std::vector<float>& values){
        std::vector<float> out;
        for(float v:values){const float pc=pcmap::floorMod(std::round(v),12);if(std::find(out.begin(),out.end(),pc)==out.end())out.push_back(pc);}
        return out.empty()?std::vector<float>{0}:out;
    };
    auto scale=minimal(tones?m.tones.get():m.table.get());
    if(user||tones){
        syncMapScale(m);
        const auto* selected=mapScaleAt(m.scaleSelection.get());
        const std::string current=selected?selected->name:"User";
        ImGui::SetNextItemWidth(left);
        if(ImGui::BeginCombo("##library",current.c_str(),ImGuiComboFlags_HeightLarge)){
            if(ImGui::Selectable("User",m.scaleSelection.get()==0))m.scaleSelection.set(0);
            int index=0;
            auto choose=[&](const chordbox::LibItem& item){
                ++index;
                if(ImGui::Selectable(item.name.c_str(),m.scaleSelection.get()==index)){
                    m.scaleSelection.set(index);scale=minimal(item.values);
                }
            };
            ImGui::TextDisabled("Scales");for(const auto& item:scaleLibrary)choose(item);
            ImGui::Separator();ImGui::TextDisabled("Chords");for(const auto& item:chordLibrary)choose(item);
            ImGui::EndCombo();
        }
        publishedUnderline(m.scaleSelection.getEscapedName());
        publishMenu(m.scaleSelection.getEscapedName());
        int count=(int)scale.size();ImGui::SetNextItemWidth(left);
        if(ImGui::SliderInt("##scale-size",&count,1,12,"%d notes per octave")){
            while((int)scale.size()>count)scale.pop_back();
            for(int note=0;(int)scale.size()<count&&note<12;++note)
                if(std::find(scale.begin(),scale.end(),(float)note)==scale.end())scale.push_back((float)note);
            if(user)m.table.set(scale);else m.tones.set(scale);
        }
        if(scPitchMapView::scaleEditor(scale,(int)std::lround(m.root.get()),left,120*z,z,accent)){
            if(user)m.table.set(scale);else m.tones.set(scale);
        }
    }else if(harmonic){
        // A compact source reference instead of an irrelevant scale editor.
        const auto a=ImGui::GetCursorScreenPos();auto* dl=ImGui::GetWindowDrawList();
        const float h=120*z;scPitchMapView::frame(dl,a,ImVec2(a.x+left,a.y+h),z);
        for(int i=1;i<=8;++i){
            const float y=a.y+h-(i/8.f)*(h-16*z);
            const float x=a.x+(i-1)*left/8;
            dl->AddLine(ImVec2(x+5*z,a.y+h-5*z),ImVec2(x+5*z,y),accent,3*z);
            scPitchMapView::smallText(dl,ImVec2(x+2*z,y-12*z),pal::textDim,"x"+ofToString(i),z);
        }
        ImGui::Dummy(ImVec2(left,h));
        ImGui::TextDisabled("1, 2, 3... = harmonic multiples");
        ImGui::TextDisabled("2x = +12 st, 4x = +24 st");
    }
    // Drag integer ticks; a float slider renders its unsnapped drag value before
    // the caller can round it, making the displayed number disagree with the map.
    auto ticks=[&](ofParameter<float>& p,const char* label,float width,float quantum,const char* format){
        const float x0=ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();ImGui::TextUnformatted(label);publishedUnderline(p.getEscapedName());
        ImGui::SameLine();ImGui::SetCursorPosX(x0+labelW*z);
        ImGui::SetNextItemWidth(std::max(30*z,width-labelW*z));
        const std::string widget="##ticks-"+p.getEscapedName();
        float value=p.get();
        if(scwidgets::quantizedSlider(widget.c_str(),value,p.getMin(),p.getMax(),quantum,format))p.set(value);
        publishMenu(p.getEscapedName());
    };
    labelW=78;
    if(user){
        ticks(m.octaveJump,"Octave jump",left,.25f,"%.2f oct");
        if(ImGui::IsItemHovered())ImGui::SetTooltip("Distance between repeats of the small scale. 1 = next octave; 0.25 = three semitones. Step selects whole scale degrees independently.");
    }
    labelW=90;
    {   // Present the one-based stored offset as a zero-based UI position.
        const float x0=ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();ImGui::TextUnformatted("Start index");publishedUnderline(m.first.getEscapedName());
        ImGui::SameLine();ImGui::SetCursorPosX(x0+labelW*z);ImGui::SetNextItemWidth(std::max(30*z,right-labelW*z));
        int start=(int)std::lround(m.first.get()-1);
        if(ImGui::SliderInt("##start-index",&start,0,n()-1))m.first.set((float)start+1);
        publishMenu(m.first.getEscapedName());
    }
    ticks(m.interval,"Step",right,1.f,user?"%.0f deg":harmonic?"x%.0f":"%.0f st");
    if(!user)ImGui::BeginDisabled();
    ticks(m.degreeOffset,"Degree shift",right,1.f,"%.0f");
    if(!user)ImGui::EndDisabled();
    if(harmonic)ImGui::BeginDisabled();
    slider(m.span,"Repeat after",right,m.span.get()>0?(user?"%.1f deg":"%.1f st"):"off");
    if(harmonic)ImGui::EndDisabled();
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Wrap the pitch sequence after this many degrees/semitones. 0: keep expanding into higher octaves.");
    if(disableMap)ImGui::EndDisabled();
    ImGui::EndGroup();
    ImGui::SameLine(0,columnGap);
    ImGui::BeginGroup();
    ImGui::TextColored(ImColor(accent),"2  INDEX GEOMETRY");
    if(disableMap)ImGui::BeginDisabled();
    labelW=90;
    slider(m.indexSkew,"Skew",right,"%+.2f");
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Same morph waveform as Classic LFO. -1: rising; 0: symmetric; +1: falling.");
    slider(m.indexRoundness,"Roundness",right,"%.2f");
    if(ImGui::IsItemHovered())ImGui::SetTooltip("0: straight edges; 0.5: sine; 1: pulse.");
    slider(m.indexPW,"PW",right,"%.2f");
    ticks(m.indexWaves,"Num waves",right,.25f,"%.2f");
    if(ImGui::IsItemHovered())ImGui::SetTooltip("How many copies of the index geometry fit across the voices. 1: one pass; 2: repeat twice; 0: all at the start.");
    sliderInt(m.indexCycleShift,"Cycle shift",right);
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Whole-index shift added on each waveform cycle, before the remaining geometry and pitch mapping. 0: exact repeats; +1: each repeat starts one index higher; -1: lower. Wraps within the active voice range.");
    sliderInt(m.indexSymmetry,"Symmetry",right);
    slider(m.indexComb,"Comb",right,"%.2f");
    ticks(m.indexQuantize,"Group size",right,1.f,m.indexQuantize.get()>0?"%.0f":"off");
    slider(m.indexRandom,"Scatter",right,"%.2f");
    sliderInt(m.indexSeed,"Seed",right);
    toggle(m.indexSeedOnGate,"New seed on gate");
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Choose a fresh Scatter seed when any Oceanode/timeline Gate voice rises from zero. Held gates and note-offs keep it. Audio-rate gate cables do not regenerate this CPU pitch map.");
    toggle(m.indexInvert,"Reverse");
    slider(m.indexPow,"Pow",right,"%+.2f");
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Applied last, normalized to the final index range. 0: linear; +1: maximum logarithmic; -1: maximum exponential. Preserves symmetry and range.");
    if(disableMap)ImGui::EndDisabled();
    ImGui::EndGroup();
    ImGui::SameLine(0,columnGap);
    ImGui::BeginGroup();
    ImGui::TextColored(ImColor(accent),"3  TUNING / VIBRATO");
    labelW=90;
    if(!comb&&!internalPitch())ImGui::BeginDisabled();
    ticks(edits.transpose,"Transpose",tuning,1.f,"%+.0f st");
    slider(edits.offset,"Fine tune",tuning,"%+.3f st");
    if(!comb&&!internalPitch())ImGui::EndDisabled();
    if(disableMap||(!user&&!tones))ImGui::BeginDisabled();
    combo(m.quantize,"Note snap",tuning);
    if(disableMap||(!user&&!tones))ImGui::EndDisabled();
    ImGui::Spacing();
    slider(comb?combGlide:glide,"Glide",tuning,"%.3f s",true);
    // These sound controls stay editable for incoming pitches and inherited maps.
    sliderVec(comb?combDetune:detune,"Detune",tuning,"%.2f st");
    if(ImGui::IsItemHovered())ImGui::SetTooltip("A random offset per voice, up to this many semitones");
    slider(comb?combVibRate:vibRate,"Vib Rate",tuning,"%.2f Hz");
    slider(comb?combVibRand:vibRand,"Rate Rnd",tuning,"%.2f");
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Each voice has its own vibrato rate: Rate x (1 +- a random offset up to this amount)");
    slider(comb?combVibAmt:vibAmt,"Vib Amp",tuning,"%.2f st");
    ImGui::EndGroup();
    const auto map=mapOf(m);
    std::vector<float> positions;
    for(int i=0;i<n();++i){
        const float index=pcmap::voiceIndex(map,i,n());
        positions.push_back(user?std::floor(map.interval*(index-1)+map.degreeOffset+1e-5f):map.interval*index);
    }
    // Keep hover information in the same voice order as the final notes.
    if(scramble.get() && (int)scrambleOrder.size()>=n()){
        const auto rawPositions=positions;
        for(int i=0;i<n();++i){const int j=std::clamp(scrambleOrder[i],0,n()-1);positions[i]=rawPositions[j];}
    }
    ImGui::TextColored(ImColor(accent),"GENERATED NOTES  /  %d voices, indices 0..%d",n(),n()-1);
    scPitchMapView::result(comb?resonatorPitches():oscillatorPitches(),positions,w,132*z,z,inherited?pal::textDim:accent);
    labelW=savedLabel;ImGui::PopStyleVar(2);ImGui::PopID();
}

// ── Delta: the matrix's last source, set here (the LFOs it watches) ───────────
void scPolyComb::drawDeltaCard(float w) {
    const float cw = std::min(w, colW(w, 3));
    ImGui::Spacing();
    sectionHeader("Delta", cw);
    ImGui::TextColored(ImColor(pal::textDim), "How much two LFOs moved: route it in Routing.");
    auto lfoCombo = [&](ofParameter<int>& p, const char* label){
        const float z = zoom();
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        publishedUnderline(p.getEscapedName());
        ImGui::SameLine();
        ImGui::SetCursorPosX(x0 + labelW * z);
        ImGui::SetNextItemWidth(std::max(30.0f * z, cw - labelW * z));
        const char* names[NUM_LFOS] = {"LFO 1", "LFO 2", "LFO 3", "LFO 4", "LFO 5", "LFO 6", "LFO 7", "LFO 8"};
        int v = std::clamp(p.get(), 0, NUM_LFOS - 1);
        if(ImGui::Combo((std::string("##") + p.getEscapedName()).c_str(), &v, names, NUM_LFOS)) p.set(v);
        publishMenu(p.getEscapedName());
    };
    lfoCombo(deltaSrcA, "Source A");
    slider(deltaMixA, "Mix A", cw, "%.2f");
    lfoCombo(deltaSrcB, "Source B");
    slider(deltaMixB, "Mix B", cw, "%.2f");
    slider(deltaWindow, "Window", cw, "%.0f ms");
    slider(deltaGain, "Gain", cw, "%.2f");
    {   // the source now: one bar per voice (as the LFO cards), 0 .. 1 (the top: more)
        const float z = zoom();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float h = 56.0f * z;
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + cw, a.y + h);
        canvas(dl, a, b);
        const int nv = n();
        const float bw = (cw - 8 * z) / std::max(1, nv);
        float top = 0.0f;
        const ImU32 col = modulation.sourceColor(SRC_DELTA);
        for(int v = 0; v < nv; v++){
            const float raw = deltaRouted() && modulation.hasLiveValues() ? modulation.liveValue(SRC_DELTA, v) : 0.0f;
            top = std::max(top, raw);
            const float val = ofClamp(raw, 0.0f, 1.0f);
            const float x0 = a.x + 4 * z + v * bw, y = b.y - 4 * z - val * (h - 8 * z);
            dl->AddRectFilled(ImVec2(x0 + 1, y), ImVec2(x0 + bw - 1, b.y - 4 * z), (col & 0x00FFFFFF) | 0x55000000, 2.0f * z);
            dl->AddLine(ImVec2(x0 + 1, y), ImVec2(x0 + bw - 1, y), col, 2.0f * z);
        }
        char t[40];
        std::snprintf(t, sizeof(t), deltaRouted() ? "max %.2f" : "not routed: not running", top);
        dl->AddText(ImVec2(a.x + 4 * z, a.y + 2 * z), pal::textDim, t);
        ImGui::Dummy(ImVec2(cw, h));
    }
}

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
    modulation.captureGeneratorState(c);
    pitchMorph.capture(s,pitchMorphOverrides());
    s["params"] = modulation.paramsJson();
    s["matrix"] = modulation.matrix;
    s["lfos"] = modulation.lfoCount;
    s["envs"] = modulation.envCount;
    s["exts"] = modulation.extCount;
    if(!(voicesHandle && voicesHandle->hasInConnection())) {
        s["voices"] = voices.get();
        scVoiceCountMorph::capture(s,voiceMorphWeights);
    }
    snaps.morph.capture(s);
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
ofJson blendJson(const std::string& key, const ofJson& x, const ofJson& y, float t, bool sw) {
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
    return sw ? y : x;   // switches and modes: at the switch point
}
}

void scPolyComb::applyState(const ofJson& aSaved, const ofJson& bSaved, float t, bool sw) {
    const auto a = scDroneMode::migrated(aSaved), b = scDroneMode::migrated(bSaved);
    if(!a.is_object() || !b.is_object()) return;
    struct RendererPolicyScope { scMorphPolicies& policies; bool active; ~RendererPolicyScope(){ if(active)policies.end(); } } policyScope{snaps.morph,voiceRenderer};
    if(voiceRenderer)snaps.morph.begin(a,b,t,[this](const std::string& key){return drivenByConnection(key);});
    t = ofClamp(t, 0.0f, 1.0f);
    // Restore listener behavior even if malformed saved state throws. The
    // explicit finish restores it before forwarding the sound to voice peers.
    struct ApplicationScope {
        bool& flag;
        bool previous;
        explicit ApplicationScope(bool& value):flag(value),previous(value) { flag=true; }
        ~ApplicationScope() { finish(); }
        void finish() { flag=previous; }
    } application(applyingSnapshot);
    if(!voiceRenderer) prepareVoiceMorph(a,b,t);
    auto mapDefaults=pitchDefaults;
    mapDefaults["Scramble"]=scramble.get(); // older snapshots leave this existing control alone
    pitchMorph.begin(a,b,t,mapDefaults);
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
    // Expose all contributing voices from the first nonzero morph step. The
    // audio banks retain their own widths and fade independently of Morph Switch.
    // Legacy snapshots without a count still use the patch's own count.
    if(!(voicesHandle && voicesHandle->hasInConnection())){
        const int fallback = userVoices > 0 ? userVoices : voices.get();
        const int v = scVoiceCountMorph::activeCount(
            scVoiceCountMorph::blend(a,b,t,fallback,MAX_VOICES));
        if(v != voices.get()){ voicesFromSnapshot = true; voices.set(v); voicesFromSnapshot = false; }
    }

    // the controls: only what changes is set (each sends what it needs)
    static const ofJson none = ofJson::object();
    auto pinnedA=a,pinnedB=b;snaps.morph.pinSelectors(pinnedA,pinnedB,t);
    const ofJson ca = pcmigration::controls(pinnedA.contains("controls") ? pinnedA["controls"] : none);
    const ofJson cb = pcmigration::controls(pinnedB.contains("controls") ? pinnedB["controls"] : none);
    for(const auto& control : controls){
        const std::string& k = control.key;
        if(snapExcluded(k) || drivenByConnection(k)) continue;
        const bool ha = ca.contains(k), hb = cb.contains(k);
        const bool option = k==combSaturation.getEscapedName() || k==combSatLevel.getEscapedName() || compDefaults.contains(k) || blurDefaults.contains(k) || subDefaults.contains(k)
                         || pitchDefaults.contains(k) || deltaDefaults.contains(k) || tremDefaults.contains(k);
        if(option){
            const ofJson fallback=subDefaults.contains(k) ? subDefaults[k] : compDefaults.contains(k) ? compDefaults[k] : blurDefaults.contains(k) ? blurDefaults[k] : pitchDefaults.contains(k) ? pitchDefaults[k]
                                : deltaDefaults.contains(k) ? deltaDefaults[k] : tremDefaults.contains(k) ? tremDefaults[k] : k==combSatLevel.getEscapedName() ? ofJson(12.0f) : ofJson(false);
            auto v=blendJson(k,ha?ca[k]:fallback,hb?cb[k]:fallback,t,sw);
            if(v!=control.save()) control.load(v);
            continue;
        }
        ofJson v;
        if(ha && hb) v = blendJson(k, ca[k], cb[k], t, sw);
        else if(hb && sw) v = cb[k];
        else if(ha && !sw) v = ca[k];
        else continue;
        if(v != control.save()) control.load(v);
    }

    M.morphGenerators(ca, cb, t, "", [this](const std::string& key){ return drivenByConnection(key); });
    M.sendChanged();

    // the matrix
    std::vector<float> ma(M.matrix.size(), 0.0f), mb = ma;
    try { if(a.contains("matrix")) ma = a["matrix"].get<std::vector<float>>(); } catch(...) {}
    try { if(b.contains("matrix")) mb = b["matrix"].get<std::vector<float>>(); } catch(...) {}
    ma.resize(M.matrix.size(), 0.0f);   // older snapshots: no Delta row
    mb.resize(M.matrix.size(), 0.0f);
    M.rescaleSavedMatrix(ma, a.value("params", ofJson::array()));   // older snapshots: amounts in the old ranges
    M.rescaleSavedMatrix(mb, b.value("params", ofJson::array()));
    bool changed = countsChanged;
    for(size_t i = 0; i < M.matrix.size() && i < ma.size(); i++){
        const float v = ofLerp(ma[i], mb[i], t);
        if(std::abs(v - M.matrix[i]) > 1e-5f){ M.matrix[i] = v; changed = true; }
    }
    if(countsChanged) M.refresh();       // sources created / freed, and the routes
    else if(changed) M.sendMatrix();
    // the controls any source moves ("Modulate"): the arrival's
    M.loadParamsJson(scModulationMorph::parameters(a.value("params",ofJson::array()),
        b.value("params",ofJson::array()),t,ca,cb,[this](const std::string& key){return drivenByConnection(key);}));
    M.applyParamBases();
    // the parts the changed controls queued, each once
    auto queued = std::move(morphSends);
    morphSends.clear();
    for(auto& entry : queued) entry.second();
    // Modes can change in the editor without changing any numeric control.
    // Always resend the evaluated blend, even on either side of Morph Switch.
    forStates([this](ofxSCServer* server,ServerState& st){
        sendPitch(st);
        if(!combMapped()) sendIns(st,IComb);
        // Temporary voice banks apply snapshots directly, without the public
        // host's morphChanged callback. Their effective controls still move
        // when the integer editor selection stays on the same side of .5.
        if(voiceRenderer){
            updateParts(server,st);sendSpread(st);
            sendIns(st,IFilter);sendIns(st,IPost);sendIns(st,IRing);
            sendStage(StEcho,st.fx[StEcho].synth,server);
        }
    });
    pitchMorph.finish();
    application.finish();
    syncVoiceMorph();
}


// ── MacroMixer: a saved sound blends on the running synths ─────────────────
ofxOceanodeNodeModel::MorphKeys scPolyComb::morphKeys() const {
    return {{"polyComb", "polyCombGenerators", "polyCombMorphAudio", "polyCombParams", "polyCombMatrix", "polyCombLfos", "polyCombEnvs", "polyCombExts", voices.getEscapedName()}, {}};
}

ofxOceanodeNodeModel::MorphHint scPolyComb::morphHint(const ofxOceanodeAbstractParameter& p) const {
    const std::string key = p.getEscapedName();
    if(key == outChannels.getEscapedName()) return MorphHint::Structural;
    if(key == voices.getEscapedName()) return MorphHint::Skip; // owned by the sound morph, including temporary banks
    // the window; a published control blends with the rest of the sound
    if(key == showWindow.getEscapedName() || published.count(key)) return MorphHint::Skip;
    return MorphHint::Interpolate;
}

bool scPolyComb::morphCheck(const ofJson& target, std::string& reason) const {
    if(target.contains("polyComb") && target["polyComb"].is_object()) return true;
    reason = "no saved polyComb sound";
    return false;
}

ofJson scPolyComb::snapshotOf(const ofJson& j) const {
    ofJson s = ofJson::object();
    s["controls"] = j.value("polyComb", ofJson::object());
    if(j.contains("polyCombMorphAudio")) s[scMorphPolicies::metadata] = j["polyCombMorphAudio"];
    if(j.contains("polyCombGenerators")) s["controls"]["Generators"] = j["polyCombGenerators"];
    s["params"] = j.value("polyCombParams", ofJson::array());
    if(j.contains("polyCombMatrix")) s["matrix"] = j["polyCombMatrix"];
    for(const auto& [from, to] : {std::pair<const char*, const char*>{"polyCombLfos", "lfos"}, {"polyCombEnvs", "envs"}, {"polyCombExts", "exts"}})
        if(j.contains(from) && j[from].is_number()) s[to] = j[from];
    const auto voiceKey=voices.getEscapedName();
    if(j.contains(voiceKey) && j[voiceKey].is_number()) s["voices"]=j[voiceKey];
    return s;
}

bool scPolyComb::morphBegin(const ofJson& target, std::string& reason) {
    if(!morphCheck(target, reason)) return false;
    morphFrom = snaps.captureForExternalMorph(); // current bases and effective morph state
    morphTo = snapshotOf(target);
    return true;
}

void scPolyComb::morphApply(float t, bool switched) {
    if(morphFrom.is_object()) snaps.applyExternalMorph(morphFrom, morphTo, t, switched);
}

void scPolyComb::morphEnd(bool completed) {
    if(!completed && morphFrom.is_object()) snaps.applyExternalMorph(morphFrom, morphFrom, 1.0f, true);
    morphFrom = morphTo = ofJson();
}

// ── The bar under the header: a slot menu (recall), store, glide, A/B morph,
//    banks. Every one of them publishable. ─────────────────────────────────────

#include "scPolyCombVoiceMorph.inl"
