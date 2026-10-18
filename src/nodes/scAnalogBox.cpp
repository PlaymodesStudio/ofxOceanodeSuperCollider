//
//  scAnalogBox.cpp
//  ofxOceanodeSuperCollider
//

#include "scAnalogBox.h"
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
const ImU32 osc[6]     = {IM_COL32(150, 165, 255, 255), IM_COL32(236, 178, 96, 255), IM_COL32(96, 200, 176, 255),
                          IM_COL32(228, 122, 146, 255), IM_COL32(170, 208, 112, 255), IM_COL32(186, 146, 232, 255)};
}

// ── Routing ──────────────────────────────────────────────────────────────────
const char* kTargets[scAnalogBox::NUM_TARGETS] = {"Pitch", "Slave Pitch", "Width", "Skew", "Round", "FM", "Noise",
                                                  "Cutoff", "Reso", "Drive", "Filter Fb", "Amp", "Pan",
                                                  "F2 Cutoff", "F2 Reso", "F3 Cutoff", "F3 Reso", "F4 Cutoff", "F4 Reso"};
const float kTargetRange[scAnalogBox::NUM_TARGETS] = {24, 24, 0.5f, 1, 0.5f, 2, 1, 48, 1, 24, 1, 1, 1, 48, 1, 48, 1, 48, 1};
const char* kTargetUnit[scAnalogBox::NUM_TARGETS] = {"st", "st", "", "", "", "cycles", "", "st", "", "dB", "", "", "of the width",
                                                     "st", "", "st", "", "st", ""};

const std::vector<const char*> kFilterModes = {"SVF 12 dB", "SVF 24 dB", "Ladder 12 dB", "Ladder 24 dB", "Comb"};
const std::vector<const char*> kFilterTypes = {"Low pass", "Band pass", "High pass", "Notch"};
const std::vector<const char*> kCombTypes = {"Feedback +", "Feedback -", "Feedforward +", "Feedforward -"};
constexpr int kComb = 4;

// ── MorphOsc's shape (OceanodeAnalog.cpp / ModLfo), for the window ──────────
double clampd(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
double lin(double x, double a, double b, double c, double d) { return b == a ? c : c + (x - a) / (b - a) * (d - c); }
double customPow(double v, double amt) {
    const double k1 = 2.0 * amt * 0.99999, k2 = k1 / ((-amt * 0.999999) + 1.0);
    return v * (k2 + 1.0) / std::max(k2 * std::fabs(v) + 1.0, 1e-6);
}
float morphShape(double ph, double skew, double round, double pw, bool centered) {
    pw = clampd(pw, 0.001, 1.0);
    skew = clampd(skew, -1.0, 1.0);
    const double midpoint = clampd(lin(skew, -1, 1, 1, 0), 0, 1);
    double w;
    if(pw < 0.5){
        w = centered ? clampd(lin(ph, clampd(0.5 - pw, 0, 1), clampd(0.5 + pw, 0, 1), 0, 1), 0, 1)
                     : clampd(lin(ph, 0, std::min(std::max(pw * 2, 1e-7), 1.0), 0, 1), 0, 1);
        if(w >= 0.99999) w = 0;   // after the window: rests at the cycle's start
    }else if(pw >= 0.99999) w = midpoint;
    else{
        const double leftMax = clampd(lin(1 - pw, 0, 0.5, 0, midpoint), 0, 1), rightMin = clampd(lin(pw, 0.5, 1, midpoint, 1), 0, 1);
        w = ph < midpoint ? clampd(lin(ph, 0, std::max(leftMax, 1e-7), 0, midpoint), 0, 1)
                          : clampd(lin(ph, std::min(rightMin, 1 - 1e-7), 1, midpoint, 1), 0, 1);
    }
    const double th = clampd(0.5 - skew * 0.5, 0, 1);
    w = w >= th ? clampd(lin(w, std::min(th, 1 - 1e-7), 1, 0.5, 1), 0.5, 1) : clampd(lin(w, 0, std::max(th, 1e-7), 0, 0.5), 0, 0.5);
    double s;
    const double cosRaw = std::cos(w * TWO_PI + PI);
    if(round >= 1.0) s = (w >= 0.25 && w < 0.75) ? 1.0 : 0.0;
    else if(round >= 0.5) s = clampd((customPow(cosRaw, (round - 0.5) * 2.0) + 1.0) * 0.5, 0, 1);
    else{
        const double tri = std::max(0.0, 1.0 - std::fabs(2.0 * w - 1.0)), cm = clampd((cosRaw + 1.0) * 0.5, 0, 1);
        s = tri + (cm - tri) * (round / 0.5);
    }
    return (float)(s * 2.0 - 1.0);
}

// ── AnalogFilter's response (its analog prototypes; the comb's delay), at f ─
std::complex<float> filterH(int mode, int type, float f, float fc, float res) {
    if(mode == kComb){
        // one period of fc; feedback: peaks on the harmonics (-: odd ones of fc / 2)
        const float g = (type == 1 || type == 3 ? -1.0f : 1.0f) * res * 0.995f;
        const std::complex<float> e = std::polar(1.0f, -2.0f * (float)M_PI * f / std::max(1.0f, fc));
        if(type <= 1) return (1.0f - 0.75f * res) / (1.0f - g * e);
        return (1.0f + g * e) / (1.0f + 0.5f * res);
    }
    const std::complex<float> s(0.0f, f / std::max(1.0f, fc));
    std::complex<float> h;
    auto svf = [&](float k, int t){
        const std::complex<float> d = s * s + k * s + 1.0f;
        switch(t){
            case 0: return 1.0f / d;
            case 1: return (k * s) / d;
            case 2: return (s * s) / d;
            default: return (s * s + 1.0f) / d;
        }
    };
    const std::complex<float> G = 1.0f / (1.0f + s);
    if(mode == 0) h = svf(std::max(0.005f, 2.0f * (1.0f - res)), type);
    else if(mode == 1) h = svf(1.8478f, type) * svf(std::max(0.005f, 0.7654f * 2.0f * (1.0f - res)), type);
    else if(mode == 2){
        const float k = res * res * 24.0f;
        const std::complex<float> u = 1.0f / (1.0f + k * G * G);
        const std::complex<float> y1 = G * u, y2 = G * G * u;
        switch(type){
            case 0: h = y2 * (1.0f + k); break;
            case 1: h = 2.0f * (y1 - y2) * std::sqrt(1.0f + k * k * 0.25f); break;
            case 2: h = u - 2.0f * y1 + y2; break;
            default: h = y2 + (u - 2.0f * y1 + y2); break;
        }
    }else{
        const float k = res * 4.2f;
        const std::complex<float> u = 1.0f / (1.0f + k * G * G * G * G);
        const std::complex<float> y1 = G * u, y2 = G * y1, y3 = G * y2, y4 = G * y3;
        const std::complex<float> hp = u - 4.0f * y1 + 6.0f * y2 - 4.0f * y3 + y4;
        switch(type){
            case 0: h = y4 * (1.0f + 0.5f * k); break;
            case 1: h = 4.0f * (y2 - 2.0f * y3 + y4) / (1.0f + 0.6f * k); break;
            case 2: h = hp; break;
            default: h = y4 - hp; break;
        }
    }
    return h;
}
float filterDb(int mode, int type, float f, float fc, float res) {
    return 20.0f * std::log10(std::max(1e-6f, std::abs(filterH(mode, type, f, fc, res))));
}
}

// ════════════════════════════════════════════════════════════════════════════
// Setup
// ════════════════════════════════════════════════════════════════════════════

scAnalogBox::scAnalogBox(std::vector<serverManager*> s) : scNode("analogBox"), servers(std::move(s)) {
    description = "Polyphonic subtractive synth. Up to 6 band-limited morphing oscillators (the LFOs' width / skew / "
                  "round shape at audio rate) with sync and FM, noise, ring, an SVF or ladder filter (2 / 4 poles, "
                  "drive, saturation, feedback), envelopes, LFOs and a modulation matrix, spread over the speakers, "
                  "then sine shaper, saturation, ring mod, chorus, EQ, echo and reverb. Show opens the editor; "
                  "right-click a control there to publish or modulate it.";
}

scAnalogBox::~scAnalogBox() {
    listeners.unsubscribeAll();
    for(auto& [server, st] : states) freeAll(server, st);
    states.clear();
}

void scAnalogBox::setup() {
    auto vec = [](ofParameter<std::vector<float>>& p, const std::string& name, float def, float lo, float hi) {
        p.set(name, {def}, {lo}, {hi});
    };
    auto toGate = [this]() { forStates([this](ofxSCServer*, ServerState& st){ sendGate(st); }); };
    auto toPitch = [this]() { forStates([this](ofxSCServer*, ServerState& st){ sendPitch(st); }); };
    auto toAmp = [this]() { forStates([this](ofxSCServer*, ServerState& st){ sendAmp(st); }); };
    auto toSpread = [this]() { forStates([this](ofxSCServer*, ServerState& st){ sendSpread(st); }); };
    auto toFilter = [this]() { forStates([this](ofxSCServer*, ServerState& st){ sendFilter(st); }); };
    auto toOsc = [this](int k) { return [this, k]() { forStates([this, k](ofxSCServer*, ServerState& st){ sendOsc(st, k); }); }; };
    auto toAllOsc = [this]() { forStates([this](ofxSCServer*, ServerState& st){ for(int k = 0; k < MAX_OSCS; k++) sendOsc(st, k); }); };
    auto toParts = [this]() { forStates([this](ofxSCServer* sv, ServerState& st){ updateParts(sv, st); }); };

    // ── The node ─────────────────────────────────────────────────────────────
    addParameter(showWindow.set("Show", false));
    addParameter(voices.set("Voices", 4, 1, MAX_VOICES));
    addParameter(outChannels.set("Channels", 2, 1, MAX_VOICES));
    // Gate (levels: a gate rising to 0.2 plays at 0.2), Pitch, Velocity: Oceanode values or audio cables
    gateIn.attach(addParameter(gate.set("Gate", {0.0f}, {0.0f}, {1.0f})), availableInputs, [this](){ refreshOutputs(); });
    pitchIn.attach(addParameter(pitch.set("Pitch", {48.0f}, {0.0f}, {127.0f})), availableInputs, [this](){ refreshOutputs(); });
    velIn.attach(addParameter(velocity.set("Velocity", {1.0f}, {0.0f}, {1.0f})), availableInputs, [this](){ refreshOutputs(); });
    addParameter(master.set("Master", 0.7f, 0.0f, 1.0f));
    addParameter(resetLFOs.set("Reset"));
    scNode::addOutput("Out");

    // ── Voices onto the speakers ─────────────────────────────────────────────
    spreadMode.set("Placement", 0, 0, 2);                reg(spreadMode, toSpread);
    spreadWidth.set("Spread", 1.0f, 0.0f, 1.0f);         reg(spreadWidth, toSpread);
    spreadCenter.set("Spread Center", 0.0f, -1.0f, 1.0f); reg(spreadCenter, toSpread);
    panP.set("Pan", {0.5f}, {0.0f}, {1.0f});             reg(panP, toSpread);
    spreadLayout.set("Speaker Layout", 0, 0, 1);         reg(spreadLayout, toSpread);

    // ── Gate, amp envelope, pitch ────────────────────────────────────────────
    envMode.set("Env Mode", 0, 0, 2);                      reg(envMode, [this]() { sendEnvMode(true); });
    extraVoices.set("Extra Voices", true);                 reg(extraVoices, [](){});   // (update() rebuilds at the new pool size)
    velSens.set("Vel Sens", 0.5f, 0.0f, 1.0f);             reg(velSens, toAmp);
    glide.set("Glide", 0.0f, 0.0f, 2.0f);                  reg(glide, toPitch);
    vec(transpose, "Transpose", 0, -48, 48);               reg(transpose, toPitch);
    vec(detune, "Detune", 0, 0, 1);                        reg(detune, toPitch);
    vibRate.set("Vib Rate", 5.0f, 0.0f, 20.0f);            reg(vibRate, toPitch);
    vibRand.set("Vib Rate Random", 0.0f, 0.0f, 1.0f);      reg(vibRand, toPitch);
    vibAmt.set("Vib Amount", 0.0f, 0.0f, 2.0f);            reg(vibAmt, toPitch);
    drift.set("Drift", 0.1f, 0.0f, 1.0f);                  reg(drift, toAllOsc);

    // ── Oscillators: as many as Oscillators says ─────────────────────────────
    oscCount.set("Oscillators", 2, 1, MAX_OSCS);           reg(oscCount, [this, toParts]() { toParts(); });
    for(int k = 0; k < MAX_OSCS; k++){
        Osc& O = osc[k];
        const std::string p = "Osc" + ofToString(k + 1) + " ";
        O.oct.set(p + "Octave", 0, -4, 4);                  reg(O.oct, toOsc(k));
        O.semi.set(p + "Semi", 0.0f, -24.0f, 24.0f);        reg(O.semi, toOsc(k));
        O.fine.set(p + "Fine", k == 1 ? 7.0f : 0.0f, -100.0f, 100.0f); reg(O.fine, toOsc(k));
        O.ratio.set(p + "Ratio", 1.0f, 0.125f, 16.0f);      reg(O.ratio, toOsc(k));
        vec(O.level, p + "Level", k == 0 ? 0.6f : 0.4f, 0, 1); reg(O.level, toOsc(k));
        // 1 and 2: saws (2 a little sharp), the others squares
        O.skew.set(p + "Skew", k < 2 ? 1.0f : 0.0f, -1.0f, 1.0f); reg(O.skew, toOsc(k));
        O.round.set(p + "Round", k < 2 ? 0.0f : 1.0f, 0.0f, 1.0f); reg(O.round, toOsc(k));
        O.width.set(p + "Width", 0.5f, 0.0f, 1.0f);         reg(O.width, toOsc(k));
        O.centered.set(p + "Centered", true);               reg(O.centered, toOsc(k));
        O.sync.set(p + "Sync", false);                      reg(O.sync, toOsc(k));
        vec(O.fm, p + "FM", 0, 0, 4);                       reg(O.fm, toOsc(k));
        O.fmFrom.set(p + "FM From", 0, 0, MAX_OSCS);        reg(O.fmFrom, toOsc(k));
        O.noise.set(p + "Noise", 0.0f, 0.0f, 1.0f);         reg(O.noise, toOsc(k));
    }
    vec(noiseLevel, "Noise", 0, 0, 1);                      reg(noiseLevel, [this, toParts]() { toParts(); });
    noiseColor.set("Noise Color", 0.0f, -1.0f, 1.0f);       reg(noiseColor, [this]() { forStates([this](ofxSCServer*, ServerState& st){ sendNoise(st); }); });
    vec(ringLevel, "Ring", 0, 0, 1);                        reg(ringLevel, [this, toParts]() { toParts(); });

    // ── Filter ───────────────────────────────────────────────────────────────
    filterOn.set("Filter On", true);                        reg(filterOn, toFilter);
    fMode.set("Filter Mode", 3, 0, kComb);                      reg(fMode, toFilter);
    fType.set("Filter Type", 0, 0, 3);                      reg(fType, toFilter);
    vec(cutoff, "Cutoff", 84, 0, 140);                      reg(cutoff, toFilter);
    vec(res, "Resonance", 0.25f, 0, 1);                     reg(res, toFilter);
    keyTrack.set("Key Track", 0.5f, 0.0f, 1.0f);            reg(keyTrack, toFilter);
    fDrive.set("Filter Drive", 0.0f, 0.0f, 36.0f);          reg(fDrive, toFilter);
    fSat.set("Filter Saturation", 0.3f, 0.0f, 1.0f);        reg(fSat, toFilter);
    fFeedback.set("Filter Feedback", 0.0f, 0.0f, 1.0f);     reg(fFeedback, toFilter);
    hpf.set("HPF", 10.0f, 10.0f, 2000.0f);                  reg(hpf, toFilter);
    fLevel.set("Filter Level", 1.0f, 0.0f, 2.0f);           reg(fLevel, toFilter);
    // more filters, added at will: in series (each into the next) or in parallel (summed)
    filterCount.set("Filters", 1, 1, MAX_FILTERS);          reg(filterCount, toParts);
    filterRouting.set("Filter Routing", 0, 0, 1);          reg(filterRouting, toFilter);
    for(int k = 0; k < MAX_FILTERS - 1; k++){
        auto& f = moreF[k];
        const std::string p = "F" + ofToString(k + 2) + " ";
        f.on.set(p + "On", true);                            reg(f.on, toFilter);
        f.mode.set(p + "Mode", 0, 0, kComb);                 reg(f.mode, toFilter);
        f.type.set(p + "Type", 1, 0, 3);                     reg(f.type, toFilter);
        vec(f.cutoff, p + "Cutoff", 96, 0, 140);             reg(f.cutoff, toFilter);
        vec(f.res, p + "Resonance", 0.3f, 0, 1);             reg(f.res, toFilter);
        f.keyTrack.set(p + "Key Track", 0.5f, 0.0f, 1.0f);   reg(f.keyTrack, toFilter);
        f.drive.set(p + "Drive", 0.0f, 0.0f, 36.0f);         reg(f.drive, toFilter);
        f.sat.set(p + "Saturation", 0.0f, 0.0f, 1.0f);       reg(f.sat, toFilter);
        f.feedback.set(p + "Feedback", 0.0f, 0.0f, 1.0f);    reg(f.feedback, toFilter);
        f.level.set(p + "Level", 1.0f, 0.0f, 2.0f);          reg(f.level, toFilter);
    }

    // ── Effects ──────────────────────────────────────────────────────────────
    auto toStages = [this]() { forStates([this](ofxSCServer* sv, ServerState& st){ if(st.created) updateStages(sv, st); }); };
    auto toStage = [this](int s) { return [this, s]() { sendStageAll(s); }; };
    shaperOn.set("Shaper On", false);                       reg(shaperOn, toStages);
    shaperDrive.set("Shaper Drive", 2.0f, 0.0f, 20.0f);     reg(shaperDrive, toStage(StShaper));
    shaperMix.set("Shaper Mix", 1.0f, 0.0f, 1.0f);          reg(shaperMix, toStage(StShaper));
    satOn.set("Saturation On", false);                      reg(satOn, toStages);
    satDrive.set("Saturation Drive", 6.0f, 0.0f, 36.0f);    reg(satDrive, toStage(StSat));
    satBias.set("Saturation Bias", 0.0f, 0.0f, 1.0f);       reg(satBias, toStage(StSat));
    satTone.set("Saturation Tone", 20000.0f, 500.0f, 20000.0f); reg(satTone, toStage(StSat));
    satOutput.set("Saturation Output", 0.0f, -24.0f, 12.0f); reg(satOutput, toStage(StSat));
    satMix.set("Saturation Mix", 1.0f, 0.0f, 1.0f);         reg(satMix, toStage(StSat));
    ringFxOn.set("Ring Mod On", false);                     reg(ringFxOn, toStages);
    ringFreq.set("Ring Mod Freq", 440.0f, 0.1f, 15000.0f);  reg(ringFreq, toStage(StRing));
    ringSpread.set("Ring Mod Spread", 0.0f, 0.0f, 12.0f);   reg(ringSpread, toStage(StRing));
    ringMix.set("Ring Mod Mix", 0.5f, 0.0f, 1.0f);          reg(ringMix, toStage(StRing));
    chorusOn.set("Chorus On", false);                       reg(chorusOn, toStages);
    shiftOn.set("Freq Shift On", false);                    reg(shiftOn, toStages);
    shiftHz.set("Freq Shift", 50.0f, -2000.0f, 2000.0f);   reg(shiftHz, toStage(StShift));
    shiftSpread.set("Freq Shift Spread", 0.0f, 0.0f, 50.0f); reg(shiftSpread, toStage(StShift));
    shiftBoth.set("Freq Shift Both", false);               reg(shiftBoth, toStage(StShift));
    shiftFeed.set("Freq Shift Feedback", 0.0f, 0.0f, 0.95f); reg(shiftFeed, toStage(StShift));
    shiftDelay.set("Freq Shift Delay", 0.0f, 0.0f, 2.0f);  reg(shiftDelay, toStage(StShift));
    shiftTone.set("Freq Shift Tone", 12000.0f, 200.0f, 20000.0f); reg(shiftTone, toStage(StShift));
    shiftMix.set("Freq Shift Mix", 1.0f, 0.0f, 1.0f);      reg(shiftMix, toStage(StShift));
    chorusRate.set("Chorus Rate", 0.6f, 0.01f, 10.0f);      reg(chorusRate, toStage(StChorus));
    chorusDepth.set("Chorus Depth", 3.0f, 0.0f, 20.0f);     reg(chorusDepth, toStage(StChorus));
    chorusDelay.set("Chorus Delay", 8.0f, 1.0f, 40.0f);     reg(chorusDelay, toStage(StChorus));
    chorusSpread.set("Chorus Spread", 0.5f, 0.0f, 1.0f);    reg(chorusSpread, toStage(StChorus));
    chorusMix.set("Chorus Mix", 0.5f, 0.0f, 1.0f);          reg(chorusMix, toStage(StChorus));
    eqOn.set("EQ On", false);                               reg(eqOn, toStages);
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
    for(auto* p : {&echoDelay, &echoBeatVal, &echoFeed, &echoCutoff, &echoResonance, &echoPingPong, &echoMix}) reg(*p, toStage(StEcho));
    reg(echoBeats, toStage(StEcho));
    reg(echoFilter, toStage(StEcho));
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
                   &rvbHiDamp, &rvbLoDamp, &rvbModRate, &rvbSpin, &rvbWander}) reg(*p, toStage(StRvb));

    // ── Snapshots ────────────────────────────────────────────────────────────
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

    // ── The shared modulation ───────────────────────────────────────────────
    {
        scModulation::Host h;
        h.regF = [this](ofParameter<float>& p, std::function<void()> f){ reg(p, f); };
        h.regI = [this](ofParameter<int>& p, std::function<void()> f){ reg(p, f); };
        h.regB = [this](ofParameter<bool>& p, std::function<void()> f){ reg(p, f); };
        h.regV = [this](ofParameter<std::vector<float>>& p, std::function<void()> f){ reg(p, f); };
        h.control = [this](const std::string& k) -> ofAbstractParameter* {
            if(k == gate.getEscapedName()) return nullptr;
            for(auto& c : controls) if(c.key == k) return c.param;
            return nullptr;
        };
        h.publishMenu = [this](const std::string& k){ publishMenu(k); };
        h.publishedUnderline = [this](const std::string& k){ publishedUnderline(k); };
        h.unpublish = [this](const std::string& k){ unpublish(k); };
        h.publishedKeys = [this]() -> const std::vector<std::string>& { return publishedKeys; };
        h.voices = [this](){ return n(); };
        std::vector<scModulation::Target> targets;
        for(int t = 0; t < NUM_TARGETS; t++) targets.push_back({kTargets[t], kTargetRange[t], kTargetUnit[t]});
        modulation.setup(h, targets, true, true, true);
        modulation.lfoCount = 1;
        modulation.envCount = 2;
        modulation.extCount = 0;
        // Env 1 -> Amp (the VCA), Env 2 -> Cutoff (the filter envelope)
        modulation.matrix[SRC_ENV * NUM_TARGETS + TAmp] = 1.0f;
        modulation.matrix[(SRC_ENV + 1) * NUM_TARGETS + TCut] = 0.4f;
        modulation.changed = [this]() { ampDirty = true; };
        auto setControl = [this](const std::string& name, float v){
            for(auto& c : controls) if(c.name == name){ c.load(ofJson(v)); return; }
        };
        setControl("Env1 Attack", 0.005f); setControl("Env1 Decay", 0.4f);
        setControl("Env1 Sustain", 0.75f); setControl("Env1 Release", 0.4f);
        setControl("Env2 Attack", 0.005f); setControl("Env2 Decay", 0.35f);
        setControl("Env2 Sustain", 0.2f); setControl("Env2 Release", 0.4f);
        modulation.targetOfKey = {{pitch.getEscapedName(), TPitch}, {transpose.getEscapedName(), TPitch},
                                  {cutoff.getEscapedName(), TCut}, {res.getEscapedName(), TRes},
                                  {fDrive.getEscapedName(), TDrive}, {fFeedback.getEscapedName(), TFb},
                                  {moreF[0].cutoff.getEscapedName(), TCut2}, {moreF[0].res.getEscapedName(), TRes2},
                                  {moreF[1].cutoff.getEscapedName(), TCut3}, {moreF[1].res.getEscapedName(), TRes3},
                                  {moreF[2].cutoff.getEscapedName(), TCut4}, {moreF[2].res.getEscapedName(), TRes4},
                                  {noiseLevel.getEscapedName(), TNoise}, {panP.getEscapedName(), TPan}};
    }

    // ── Node parameters' listeners ───────────────────────────────────────────
    listeners.push(voices.newListener([this](int&) { modulation.voicesChanged(); rebuildAll(); }));
    listeners.push(outChannels.newListener([this](int&) { rebuildOutput(); }));
    listeners.push(gate.newListener([this, toGate](std::vector<float>& v) {
        for(float x : v) if(x > 0.0f) gateTouched = true;
        toGate();
    }));
    listeners.push(velocity.newListener([toGate](std::vector<float>&) { toGate(); }));
    listeners.push(pitch.newListener([this, toPitch](std::vector<float>&) {
        toPitch();
        if(allocating()) forStates([this](ofxSCServer*, ServerState& st){ sendGate(st); });   // the pitches go through the allocator
    }));
    listeners.push(master.newListener([toAmp](float&) { toAmp(); }));
    listeners.push(resetLFOs.newListener([this]() { modulation.resetLfos(); }));

    requireSynthdefs();
    defaultSnap = captureSnapshot();
    loadSnapshotLibrary();
}

// ════════════════════════════════════════════════════════════════════════════
// Engine: the parts and what they get
// ════════════════════════════════════════════════════════════════════════════

void scAnalogBox::forStates(const std::function<void(ofxSCServer*, ServerState&)>& f) {
    for(auto& [server, st] : states) if(st.created) f(server, st);
}
int scAnalogBox::src(const ServerState& st, int s) const { return modulation.sourceBus(st.server, s); }
int scAnalogBox::mod(const ServerState& st, int t) const { return modulation.targetBus(st.server, t); }

void scAnalogBox::sendVec(ofxSCSynth* s, const std::string& name, const std::vector<float>& v) {
    if(!s) return;
    if(v.size() <= 1) s->setMultiple(name, first(v), n());
    else s->set(name, expand(v, n()));
}

void scAnalogBox::sendGate(ServerState& st) {
    auto* s = st.gateS;
    if(!s) return;
    if(allocating()){
        // ABGateAlloc<Voices>: the inputs (Voices wide) -> the pool (n() wide)
        const int ni = nIn();
        auto vec = [&](const std::string& name, const std::vector<float>& v, float def){
            if(v.size() <= 1) s->setMultiple(name, first(v, def), ni); else s->set(name, expand(v, ni));
        };
        vec("gate", gate.get(), 0.0f);
        vec("velocity", velocity.get(), 1.0f);
        vec("pitch", pitch.get(), 48.0f);
        s->set("out", st.gateBus ? st.gateBus->index : 0);
        s->set("pitchout", st.poolPitch ? st.poolPitch->index : 0);
        s->set("vel", src(st, SRC_VEL));
        s->set("rnd", src(st, SRC_RND));
        const int gb = gateIn.bus(st.server), vb = velIn.bus(st.server), pb = pitchIn.bus(st.server);
        if(gb >= 0){ s->set("gate_sel", 1.0f); s->mapan("gate_ar", gb, ni); } else s->set("gate_sel", 0.0f);
        if(vb >= 0){ s->set("vel_sel", 1.0f); s->mapan("vel_ar", vb, ni); } else s->set("vel_sel", 0.0f);
        if(pb >= 0){ s->set("pitch_sel", 1.0f); s->mapan("pitch_ar", pb, ni); } else s->set("pitch_sel", 0.0f);
        return;
    }
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


void scAnalogBox::sendPitch(ServerState& st) {
    auto* s = st.pitchS;
    if(!s) return;
    s->set("out", st.noteBus ? st.noteBus->index : 0);
    s->set("mpitch", mod(st, TPitch));
    sendVec(s, "pitch", pitch.get());
    const int pb = allocating() ? (st.poolPitch ? st.poolPitch->index : -1) : pitchIn.bus(st.server);   // pooled: the allocator's
    if(pb >= 0){ s->set("pitch_sel", 1.0f); s->mapan("pitch_ar", pb, n()); }
    else s->set("pitch_sel", 0.0f);
    sendVec(s, "transpose", transpose.get());
    sendVec(s, "detune", detune.get());
    s->set("glide", glide.get());
    s->set("vibrate", vibRate.get());
    s->set("vibrand", vibRand.get());
    s->set("vibamt", vibAmt.get());
}

void scAnalogBox::sendOsc(ServerState& st, int k) {
    auto* s = st.osc[k];
    if(!s) return;
    const Osc& O = osc[k];
    s->set("note", st.noteBus ? st.noteBus->index : 0);
    s->set("out", st.mixBus ? st.mixBus->index : 0);
    s->set("raw", st.raw[k] ? st.raw[k]->index : 0);
    s->set("freqout", st.freq[k] ? st.freq[k]->index : 0);
    s->set("oct", (float)O.oct.get());
    s->set("semi", O.semi.get());
    s->set("fine", O.fine.get());
    s->set("ratio", O.ratio.get());
    s->set("skew", O.skew.get());
    s->set("round", O.round.get());
    s->set("pw", O.width.get());
    s->set("centered", O.centered.get() ? 1.0f : 0.0f);
    s->set("noise", O.noise.get());
    s->set("drift", drift.get());
    s->set("slave", k > 0 ? 1.0f : 0.0f);
    // hard sync: to oscillator 1's frequency
    s->set("sync", (k > 0 && O.sync.get()) ? 1.0f : 0.0f);
    s->set("syncfreq", st.freq[0] ? st.freq[0]->index : 0);
    // FM: from any oscillator in use (an earlier one this block, a later one / itself a block late)
    const int from = O.fmFrom.get() - 1;
    const bool fmOn = from >= 0 && from < oscs() && st.raw[from];
    s->set("fmsrc", fmOn ? st.raw[from]->index : (st.raw[k] ? st.raw[k]->index : 0));
    s->set("fmon", fmOn ? 1.0f : 0.0f);
    sendVec(s, "fmamt", O.fm.get());
    sendVec(s, "level", O.level.get());
    s->set("mslave", mod(st, TSlave)); s->set("mpw", mod(st, TWidth)); s->set("mskew", mod(st, TSkew));
    s->set("mround", mod(st, TRound)); s->set("mfm", mod(st, TFm));
}

void scAnalogBox::sendNoise(ServerState& st) {
    auto* s = st.noise;
    if(!s) return;
    s->set("out", st.mixBus ? st.mixBus->index : 0);
    sendVec(s, "level", noiseLevel.get());
    s->set("color", noiseColor.get());
    s->set("mnoise", mod(st, TNoise));
}

void scAnalogBox::sendRing(ServerState& st) {
    auto* s = st.ring;
    if(!s) return;
    s->set("a", st.raw[0] ? st.raw[0]->index : 0);
    s->set("b", st.raw[1] ? st.raw[1]->index : 0);
    s->set("out", st.mixBus ? st.mixBus->index : 0);
    sendVec(s, "level", ringLevel.get());
}

bool scAnalogBox::vcaOn() const {
    for(int e = 0; e < modulation.envCount; e++)
        if(std::abs(modulation.matrix[(SRC_ENV + e) * NUM_TARGETS + TAmp]) > 1e-4f) return true;
    return false;
}

scAnalogBox::FilterRef scAnalogBox::filterRef(int k) {
    if(k <= 0) return {&filterOn, &fMode, &fType, &cutoff, &res, &keyTrack, &fDrive, &fSat, &fFeedback, &fLevel};
    auto& f = moreF[std::min(k, MAX_FILTERS - 1) - 1];
    return {&f.on, &f.mode, &f.type, &f.cutoff, &f.res, &f.keyTrack, &f.drive, &f.sat, &f.feedback, &f.level};
}

// Filter k: in series from the one before (the first from the mix) into the
// next (the last into the amp); in parallel all from the mix, summed
void scAnalogBox::sendFilterK(ServerState& st, int k) {
    auto* s = k == 0 ? st.filter : st.moreFilters[k - 1];
    if(!s) return;
    const int nf = filters();
    auto bus = [](ofxSCBus* b){ return b ? b->index : 0; };
    const bool series = filterRouting.get() == 0;
    s->set("in", series && k > 0 ? bus(st.chainBus[k - 1]) : bus(st.mixBus));
    s->set("out", series && k < nf - 1 ? bus(st.chainBus[k]) : bus(st.filtBus));
    s->set("note", bus(st.noteBus));
    const FilterRef f = filterRef(k);
    s->set("on", f.on->get() ? 1.0f : 0.0f);
    s->set("mode", (float)f.mode->get());
    s->set("type", (float)f.type->get());
    sendVec(s, "cutoff", f.cutoff->get());
    sendVec(s, "res", f.res->get());
    s->set("keytrack", f.keyTrack->get());
    s->set("drive", f.drive->get());
    s->set("sat", f.sat->get());
    s->set("fb", f.feedback->get());
    s->set("level", f.level->get());
    s->set("hpf", k == 0 ? hpf.get() : 10.0f);
    s->set("vis", st.visBlock ? st.visBlock->index + VIS_CUT : 0);
    s->set("vison", k == 0 ? 1.0f : 0.0f);
    static const Target cuts[MAX_FILTERS] = {TCut, TCut2, TCut3, TCut4}, ress[MAX_FILTERS] = {TRes, TRes2, TRes3, TRes4};
    s->set("mcut", mod(st, cuts[k])); s->set("mres", mod(st, ress[k]));
    // Drive and Filter Fb modulation: every filter
    s->set("mdrive", mod(st, TDrive)); s->set("mfb", mod(st, TFb));
}

void scAnalogBox::sendFilter(ServerState& st) {
    for(int k = 0; k < filters(); k++) sendFilterK(st, k);
}

void scAnalogBox::sendAmp(ServerState& st) {
    auto* s = st.amp;
    if(!s) return;
    s->set("in", st.filtBus ? st.filtBus->index : 0);
    s->set("out", st.voiceBus ? st.voiceBus->index : 0);
    s->set("vca", vcaOn() ? 1.0f : 0.0f);
    s->set("gate", st.gateBus ? st.gateBus->index : 0);
    s->set("vel", src(st, SRC_VEL));
    s->set("velsens", velSens.get());
    s->set("level", master.get());
    s->set("mamp", mod(st, TAmp));
}

void scAnalogBox::sendSpread(ServerState& st) {
    auto* s = st.spread;
    if(!s) return;
    s->set("in", st.voiceBus ? st.voiceBus->index : 0);
    s->set("voices", (float)n());
    s->set("line", spreadLayout.get() == 1 ? 1.0f : 0.0f);
    s->set("pan", expand(panPositions(), W, 0.5f));
    s->set("mpan", mod(st, TPan));
    s->set("vis", st.visBlock ? st.visBlock->index + VIS_PAN : 0);
}

bool scAnalogBox::noiseWanted() const {
    for(float v : noiseLevel.get()) if(v > 0.0001f) return true;
    return false;
}
bool scAnalogBox::ringWanted() const {
    if(oscs() < 2) return false;
    for(float v : ringLevel.get()) if(v > 0.0001f) return true;
    return false;
}

// The oscillators in use (each in its own slot, in order), the noise and the
// ring while their levels are up: created / freed to match
void scAnalogBox::updateParts(ofxSCServer* server, ServerState& st) {
    if(!st.created) return;
    for(int k = 0; k < MAX_OSCS; k++){
        auto*& s = st.osc[k];
        if(k < oscs() && !s){
            s = new ofxSCSynth(def("ABOsc"), server);
            sendOsc(st, k);
            s->createAndRun(1, st.groups[SlOsc + k]->nodeID, true);
        }else if(k >= oscs() && s){ s->free(); delete s; s = nullptr; }
    }
    for(int k = 0; k < oscs(); k++) sendOsc(st, k);    // FM sources may have come or gone
    auto part = [&](ofxSCSynth*& s, bool wanted, const std::string& defName, std::function<void()> send){
        if(wanted && !s){
            s = new ofxSCSynth(defName, server);
            send();
            s->createAndRun(1, st.groups[SlNoise]->nodeID, true);
        }else if(wanted) send();
        else if(s){ s->free(); delete s; s = nullptr; }
    };
    part(st.noise, noiseWanted(), def("ABNoise"), [&](){ sendNoise(st); });
    part(st.ring, ringWanted(), def("ABRing"), [&](){ sendRing(st); });
    // filters 2..: after the first, in order (series: each reads the one before)
    for(int k = 1; k < MAX_FILTERS; k++){
        auto*& s = st.moreFilters[k - 1];
        if(k < filters() && !s){
            s = new ofxSCSynth(def("ABFilter"), server);
            sendFilterK(st, k);
            s->createAndRun(1, st.groups[SlFilter]->nodeID, true);
        }else if(k >= filters() && s){ s->free(); delete s; s = nullptr; }
    }
    sendFilter(st);   // the buses follow the count
}

void scAnalogBox::ensureBuses(ofxSCServer* server, ServerState& st) {
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
    audio(st.gateBus, W); audio(st.poolPitch, W); audio(st.mixBus, W); audio(st.filtBus, W); audio(st.voiceBus, W);
    for(auto*& b : st.raw) audio(b, W);
    for(auto*& b : st.chainBus) audio(b, W);
    for(auto*& b : st.freq) control(b, W);
    control(st.noteBus, 2 * W);
    control(st.visBlock, 4 * W);
    if(st.modCreated && st.gateBus != oldGate) modulation.setGateBus(server, st.gateBus ? st.gateBus->index : 0);
}

void scAnalogBox::createAll(ofxSCServer* server, ServerState& st) {
    ensureBuses(server, st);
    st.builtVoices = n();
    st.builtChannels = outChannels.get();
    auto fixed = [&](ofxSCSynth*& s, const std::string& defName, int slot, std::function<void()> send){
        s = new ofxSCSynth(defName, server);
        send();
        s->createAndRun(1, st.groups[slot]->nodeID, true);
    };
    if(!st.modCreated){
        modulation.create(server, st.groups[SlSrc]->nodeID, st.groups[SlRoute]->nodeID, st.gateBus ? st.gateBus->index : 0);
        st.modCreated = true;
    }
    fixed(st.gateS, allocating() ? "ABGateAlloc" + ofToString(nIn()) : def("ModGate"), SlGate, [&](){ sendGate(st); });
    fixed(st.pitchS, def("ABPitch"), SlPitch, [&](){ sendPitch(st); });
    fixed(st.filter, def("ABFilter"), SlFilter, [&](){ sendFilter(st); });
    fixed(st.amp, def("ABAmp"), SlAmp, [&](){ sendAmp(st); });
    fixed(st.spread, "PolyCombSpread" + ofToString(outChannels.get()), SlSpread, [&](){ sendSpread(st); });
    updateParts(server, st);
    updateStages(server, st);
}

void scAnalogBox::freeSynths(ServerState& st) {
    auto drop = [](ofxSCSynth*& s){ if(s){ s->free(); delete s; s = nullptr; } };
    drop(st.gateS); drop(st.pitchS); drop(st.filter); drop(st.amp); drop(st.spread);
    drop(st.noise); drop(st.ring);
    for(auto*& s : st.osc) drop(s);
    for(auto*& s : st.moreFilters) drop(s);
    for(auto& f : st.fx){ drop(f.synth); f.running = false; f.offAt = -1.0f; }
}

void scAnalogBox::freeAll(ofxSCServer* server, ServerState& st) {
    if(st.root && st.created) st.root->free();
    auto forget = [](ofxSCSynth*& s){ delete s; s = nullptr; };
    forget(st.gateS); forget(st.pitchS); forget(st.filter); forget(st.amp); forget(st.spread);
    forget(st.noise); forget(st.ring);
    for(auto*& s : st.osc) forget(s);
    for(auto*& s : st.moreFilters) forget(s);
    if(st.modCreated) modulation.free(server, false);
    st.modCreated = false;
    for(auto& f : st.fx){ forget(f.synth); f.running = false; }
    for(auto*& g : st.groups){ delete g; g = nullptr; }
    delete st.root;
    st.root = nullptr;
    st.created = false;
    auto audio = [&](ofxSCBus*& b){ if(b && liveAudioBus(server, b)) b->free(); delete b; b = nullptr; };
    auto control = [&](ofxSCBus*& b){ if(b && liveControlBus(server, b)) b->free(); delete b; b = nullptr; };
    audio(st.gateBus); audio(st.poolPitch); audio(st.mixBus); audio(st.filtBus); audio(st.voiceBus);
    for(auto*& b : st.raw) audio(b);
    for(auto*& b : st.chainBus) audio(b);
    for(auto*& b : st.freq) control(b);
    for(auto& f : st.fx) audio(f.in);
    control(st.noteBus); control(st.visBlock);
}

void scAnalogBox::rebuildAll() {
    forStates([this](ofxSCServer* server, ServerState& st){
        const bool wasLatency = server->getBLatency();
        server->setBLatency(true);
        freeSynths(st);
        modulation.rebuild(server);
        createAll(server, st);
        server->setBLatency(wasLatency);
    });
}

void scAnalogBox::rebuildOutput() {
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
        st.spread = new ofxSCSynth("PolyCombSpread" + ofToString(outChannels.get()), server);
        sendSpread(st);
        st.spread->createAndRun(1, st.groups[SlSpread]->nodeID, true);
        st.builtChannels = outChannels.get();
        updateStages(server, st);
        server->setBLatency(wasLatency);
    });
}

void scAnalogBox::sendEnvMode(bool force) {
    const int d = droning() ? 1 : 0;
    if(!force && d == sentDrone) return;
    sentDrone = d;
    forStates([d](ofxSCServer*, ServerState& st){ if(st.gateS) st.gateS->set("drone", (float)d); });
}

float scAnalogBox::matrixCell(int s, int t) const { return modulation.matrix[s * NUM_TARGETS + t]; }
void scAnalogBox::setMatrixCell(int s, int t, float v) {
    modulation.matrix[s * NUM_TARGETS + t] = ofClamp(v, -1.0f, 1.0f);
    modulation.sendMatrix();
}

// ════════════════════════════════════════════════════════════════════════════
// The graph: the node is one group (its slots inside)
// ════════════════════════════════════════════════════════════════════════════

void scAnalogBox::buildSynth(ofxSCServer* server) {
    if(!server) return;
    auto& st = states[server];
    freeAll(server, st);
    st.root = new ofxSCGroup(server);
}

void scAnalogBox::createSynth(ofxSCServer* server) {
    auto it = states.find(server);
    if(it == states.end() || !it->second.root) return;
    auto& st = it->second;
    st.root->create(0, 1);
    for(auto*& g : st.groups){
        g = new ofxSCGroup(server);
        g->create(1, st.root->nodeID);
    }
    st.created = true;
    if(!getActive()) st.root->run(false);
    createAll(server, st);
}

void scAnalogBox::free(ofxSCServer* server) {
    auto it = states.find(server);
    if(it == states.end()) return;
    freeAll(server, it->second);
    it->second.outBus = -1;
}

void scAnalogBox::moveSynthBefore(ofxSCServer* server, int nodeID) {
    auto it = states.find(server);
    if(it == states.end() || !it->second.created) return;
    routeChain(it->second);
    it->second.root->moveBefore(nodeID);
}
int scAnalogBox::getLastSynthID(ofxSCServer* server) {
    auto it = states.find(server);
    return (it != states.end() && it->second.created && it->second.root) ? it->second.root->nodeID : -1;
}

void scAnalogBox::setOutputBus(ofxSCServer* server, int, int bus) {
    auto& st = states[server];
    st.outBus = bus;
    routeChain(st);
}
int scAnalogBox::getOutputBusIndex(ofxSCServer* server, int) {
    auto it = states.find(server);
    return it != states.end() ? it->second.outBus : -1;
}

void scAnalogBox::setInputBus(ofxSCServer* server, scNode* node, int bus) {
    auto& st = states[server];
    if(gateIn.take(server, node, bus) | velIn.take(server, node, bus)) sendGate(st);
    if(pitchIn.take(server, node, bus)) sendPitch(st);
}
void scAnalogBox::resetInputBusses(ofxSCServer* server, int) {
    auto& st = states[server];
    gateIn.reset(server); pitchIn.reset(server); velIn.reset(server);
    sendGate(st);
    sendPitch(st);
}

void scAnalogBox::resendParametersForNRT() {
    forStates([this](ofxSCServer* server, ServerState& st){
        sendGate(st); sendPitch(st); sendFilter(st); sendAmp(st); sendSpread(st);
        for(int k = 0; k < MAX_OSCS; k++) sendOsc(st, k);
        sendNoise(st); sendRing(st);
        for(int s = 0; s < NumStages; s++) if(st.fx[s].synth) sendStage(s, st.fx[s].synth);
        routeChain(st);
    });
    modulation.resend();
}

void scAnalogBox::activate() { for(auto& [s, st] : states) if(st.created) st.root->run(true); }
void scAnalogBox::deactivate() { for(auto& [s, st] : states) if(st.created) st.root->run(false); }

void scAnalogBox::setBpm(float bpm) {
    currentBpm = bpm;
    modulation.setBpm(bpm);
    if(echoBeats.get()) sendStageAll(StEcho);
}

// ── Effects (at the speaker count) ──────────────────────────────────────────

void scAnalogBox::updateStages(ofxSCServer* server, ServerState& st) {
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
                f.synth->createAndRun(1, st.groups[SlFx + s]->nodeID, true);
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
void scAnalogBox::routeChain(ServerState& st) {
    int next = st.outBus;
    for(int s = NumStages - 1; s >= 0; s--){
        auto& f = st.fx[s];
        if(!f.synth || !f.running || !f.in) continue;
        f.synth->set("out", next);
        next = f.in->index;
    }
    if(st.spread) st.spread->set("out", next);
}

void scAnalogBox::update(ofEventArgs&) {
    sendEnvMode();
    // the pool's size follows Voices, Extra Voices and gated / droning: rebuilt at the new one
    {
        bool rebuild = false;
        for(auto& [server, st] : states) if(st.created && st.builtVoices != n()) rebuild = true;
        if(rebuild){ modulation.voicesChanged(); rebuildAll(); }
    }
    if(ampDirty){ ampDirty = false; forStates([this](ofxSCServer*, ServerState& st){ sendAmp(st); }); }
    modulation.showing = showWindow.get();
    modulation.update();
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
                server->requestControlValues(st.visBlock->index + VIS_PAN, n());
                if(filterOn.get()){
                    server->requestControlValues(st.visBlock->index + VIS_CUT, n());
                    server->requestControlValues(st.visBlock->index + VIS_CUT + W, n());
                }
            }
            break;
        }
    }
}

bool scAnalogBox::stageOn(int s) const {
    switch(s){
        case StShaper: return shaperOn.get();
        case StSat: return satOn.get();
        case StRing: return ringFxOn.get();
        case StShift: return shiftOn.get();
        case StChorus: return chorusOn.get();
        case StEq: return eqOn.get();
        case StEcho: return echoOn.get();
        default: return rvbOn.get();
    }
}

std::string scAnalogBox::stageDef(int s) const {
    static const char* names[NumStages] = {"ABFxShaper", "mixerProSat", "ABFxRing", "ABFxShift", "ABFxChorus", "mixerProEQ", "mixerProEcho", "mixerProRvb"};
    return names[s] + ofToString(outChannels.get());
}

float scAnalogBox::echoDelaySec() const {
    if(!echoBeats.get()) return ofClamp(echoDelay.get(), 0.005f, 4.0f);
    return ofClamp(echoBeatVal.get() * 60.0f / std::max(1.0f, currentBpm), 0.005f, 4.0f);
}

float scAnalogBox::stageTail(int s) const {
    if(s == StEq) return 0.0f;
    if(s == StEcho){
        const float feed = ofClamp(echoFeed.get(), 0.0f, 0.98f);
        const float repeats = feed > 0.001f ? std::log(0.001f) / std::log(feed) : 0.0f;
        return std::min(30.0f, echoDelaySec() * (repeats + 1.0f) + 0.2f);
    }
    if(s == StRvb) return std::min(30.0f, rvbDecay.get() + rvbPredelay.get() * 0.001f + 0.3f);
    if(s == StChorus) return 0.1f;
    if(s == StShift){
        const float feed = ofClamp(shiftFeed.get(), 0.0f, 0.95f);
        const float repeats = feed > 0.001f ? std::log(0.001f) / std::log(feed) : 0.0f;
        return std::min(30.0f, (shiftDelay.get() + 0.002f) * (repeats + 1.0f) + 0.1f);
    }
    return 0.05f;
}

void scAnalogBox::sendStage(int s, ofxSCSynth* x) {
    if(!x) return;
    const float on = stageOn(s) ? 1.0f : 0.0f;
    switch(s){
        case StShaper:
            x->set("drive", shaperDrive.get());
            x->set("mix", shaperMix.get());
            x->set("on", on);
            break;
        case StSat:
            x->set("drive", satDrive.get());
            x->set("bias", satBias.get());
            x->set("autogain", 1.0f);
            x->set("tone", satTone.get());
            x->set("output", satOutput.get());
            x->set("mix", satMix.get());
            x->set("on", on);
            break;
        case StRing:
            x->set("freq", ringFreq.get());
            x->set("spread", ringSpread.get());
            x->set("mix", ringMix.get());
            x->set("on", on);
            break;
        case StShift:
            x->set("shift", shiftHz.get());
            x->set("spread", shiftSpread.get());
            x->set("both", shiftBoth.get() ? 1.0f : 0.0f);
            x->set("fb", shiftFeed.get());
            x->set("delay", shiftDelay.get());
            x->set("tone", shiftTone.get());
            x->set("mix", shiftMix.get());
            x->set("on", on);
            break;
        case StChorus:
            x->set("rate", chorusRate.get());
            x->set("depth", chorusDepth.get());
            x->set("delay", chorusDelay.get());
            x->set("spread", chorusSpread.get());
            x->set("mix", chorusMix.get());
            x->set("on", on);
            break;
        case StEq:
            for(int b = 0; b < 5; b++){
                const std::string k = "b" + ofToString(b + 1);
                x->set(k + "gain", eqGain[b].get());
                x->set(k + "freq", eqFreq[b].get());
                if(scEQEditor::isShelf(b)) x->set(k + "slope", 1.0f / scEQEditor::safeShelfSlope(eqShape[b].get(), eqGain[b].get()));
                else x->set(k + "rq", 1.0f / std::max(0.01f, eqShape[b].get()));
            }
            break;
        case StEcho:
            x->set("delay", echoDelaySec());
            x->set("feed", ofClamp(echoFeed.get(), 0.0f, 0.98f));
            x->set("cutoff", ofClamp(echoCutoff.get(), 12.0f, 130.0f));
            x->set("resonance", ofClamp(echoResonance.get(), 0.0f, 0.9f));
            x->set("filtertype", (float)ofClamp(echoFilter.get(), 0, 3));
            x->set("pingpong", ofClamp(echoPingPong.get(), 0.0f, 1.0f));
            x->set("mix", ofClamp(echoMix.get(), 0.0f, 1.0f));
            x->set("on", on);
            break;
        default:
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
            break;
    }
}

void scAnalogBox::sendStageAll(int s) {
    for(auto& [server, st] : states){
        auto& f = st.fx[s];
        if(!f.synth) continue;
        sendStage(s, f.synth);
        if(!stageOn(s) && s != StEq) f.synth->set("on", 0.0f);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Presets
// ════════════════════════════════════════════════════════════════════════════

void scAnalogBox::presetSave(ofJson& j) {
    auto& c = j["analogBox"] = ofJson::object();
    for(const auto& control : controls) c[control.key] = control.save();
    modulation.patchBases(c);
    modulation.patchBases(j);
    modulation.save(j, "analogBoxMod");
    j["analogBoxPublished"] = publishedKeys;
}

void scAnalogBox::loadBeforeConnections(ofJson& j) {
    if(!j.contains("analogBoxPublished") || !j["analogBoxPublished"].is_array()) return;
    std::vector<std::string> wanted;
    for(const auto& k : j["analogBoxPublished"]) if(k.is_string()) wanted.push_back(k.get<std::string>());
    const auto old = publishedKeys;
    for(const auto& k : old) if(std::find(wanted.begin(), wanted.end(), k) == wanted.end()) unpublish(k);
    for(const auto& k : wanted) publish(k);
}

void scAnalogBox::presetRecallAfterSettingParameters(ofJson& j) {
    if(j.contains("analogBox") && j["analogBox"].is_object()){
        restoring = true;
        for(const auto& control : controls)
            if(j["analogBox"].contains(control.key)) control.load(j["analogBox"][control.key]);
        restoring = false;
    }
    // a loaded preset starts from Auto's own rule: droning unless Gate is connected
    // (a value it gets from here on switches it to gated)
    gateTouched = false;
    sendEnvMode(true);
    modulation.load(j, "analogBoxMod");
    modulation.afterLoad();
    resendParametersForNRT();
    for(auto& [server, st] : states){
        if(!st.created) continue;
        updateParts(server, st);
        updateStages(server, st);
    }
}

void scAnalogBox::requireSynthdefs() {
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
        } catch(const std::exception& e) { ofLogWarning("scAnalogBox") << e.what(); }
        return folder;
    };
    for(const auto& [expected, dir] : std::vector<std::pair<std::string, std::string>>{
            {"ABOsc2.scsyndef", "synthdefs/Defaults/AnalogBox/CompiledSynthdefs/AnalogBox"},
            {"PolyCombSpread2.scsyndef", "synthdefs/Defaults/PolyComb/CompiledSynthdefs/PolyComb"},
            {"ModLfo2.scsyndef", "synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation"}}){
        const auto folder = find(expected, dir);
        if(folder.empty()){ ofLogError("scAnalogBox") << "Compile the SynthDefs of " << dir; continue; }
        for(auto* sm : servers) if(sm) sm->requireSynthdefFolder(folder.string());
    }
}

bool scAnalogBox::droning() const {
    if(envMode.get() == 2) return true;
    if(envMode.get() == 1) return false;
    if(gateTouched || gateIn.connected()) return false;
    return true;
}

template<typename T>
void scAnalogBox::reg(ofParameter<T>& p, std::function<void()> send) {
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

void scAnalogBox::publish(const std::string& key) {
    if(published.count(key)) return;
    auto it = std::find_if(controls.begin(), controls.end(), [&](const Control& c){ return c.key == key; });
    if(it == controls.end() || getParameterGroup().contains(key)) return;
    if(!publishedSeparator){ addSeparator("Published", ofColor(200)); publishedSeparator = true; }
    published[key] = it->add();
    publishedKeys.push_back(key);
    parameterGroupChanged.notify(this);
}

void scAnalogBox::unpublish(const std::string& key) {
    auto it = published.find(key);
    if(it == published.end() || isConnected(key)) return;
    removeParameter(it->second->getName());
    published.erase(it);
    publishedKeys.erase(std::remove(publishedKeys.begin(), publishedKeys.end(), key), publishedKeys.end());
    if(published.empty() && publishedSeparator){ removeSeparator("Published"); publishedSeparator = false; }
    parameterGroupChanged.notify(this);
}

bool scAnalogBox::isConnected(const std::string& key) const {
    auto it = published.find(key);
    return it != published.end() && it->second && (it->second->hasInConnection() || it->second->hasOutConnections());
}

void scAnalogBox::publishMenu(const std::string& key) {
    if(!ImGui::BeginPopupContextItem(("##abPublish" + key).c_str())) return;
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

void scAnalogBox::publishedUnderline(const std::string& key) const {
    if(!published.count(key)) return;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y - 1.0f), ImVec2(b.x, b.y - 1.0f), ImGui::GetColorU32(ImGuiCol_Text));
}

std::vector<float> scAnalogBox::panPositions() const {
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



void scAnalogBox::sectionHeader(const char* title, float w, ofParameter<bool>* on, ImU32 accent) {
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

void scAnalogBox::slider(ofParameter<float>& p, const char* label, float w, const char* fmt, bool log) {
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
void scAnalogBox::sliderVec(ofParameter<std::vector<float>>& p, const char* label, float w, const char* fmt, bool log) {
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

void scAnalogBox::sliderInt(ofParameter<int>& p, const char* label, float w) {
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

void scAnalogBox::combo(ofParameter<int>& p, const char* label, float w, const std::vector<const char*>& items) {
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

void scAnalogBox::toggle(ofParameter<bool>& p, const char* label) {
    bool v = p.get();
    if(ImGui::Checkbox(label, &v)) p.set(v);
    publishedUnderline(p.getEscapedName());
    publishMenu(p.getEscapedName());
}





// ════════════════════════════════════════════════════════════════════════════
// Panning: where each voice sits on the speakers
// ════════════════════════════════════════════════════════════════════════════

void scAnalogBox::drawPanTab(float w) {
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

bool scAnalogBox::snapExcluded(const std::string& key) const {
    return key.rfind("Snapshot", 0) == 0 || key.rfind("Morph", 0) == 0 || key.rfind("Ext_", 0) == 0
        || key == spreadLayout.getEscapedName();
}

// a connection drives it: a recall leaves it alone
bool scAnalogBox::drivenByConnection(const std::string& key) const {
    auto it = published.find(key);
    return it != published.end() && it->second && it->second->hasInConnection();
}

ofJson scAnalogBox::captureSnapshot() const {
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

void scAnalogBox::applyState(const ofJson& a, const ofJson& b, float t) {
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
    ofJson ca = a.contains("controls") ? a["controls"] : none;
    ofJson cb = b.contains("controls") ? b["controls"] : none;
    // snapshots from before the extra filters and the frequency shifter: those at their defaults
    if(defaultSnap.contains("controls")){
        auto isNew = [](std::string k){
            std::replace(k.begin(), k.end(), '_', ' ');
            for(const char* p : {"F2 ", "F3 ", "F4 ", "Freq Shift"}) if(k.rfind(p, 0) == 0) return true;
            return k == "Filters" || k == "Filter Routing" || k == "Filter Level";
        };
        for(auto it = defaultSnap["controls"].begin(); it != defaultSnap["controls"].end(); ++it){
            if(!isNew(it.key())) continue;
            if(!ca.contains(it.key())) ca[it.key()] = it.value();
            if(!cb.contains(it.key())) cb[it.key()] = it.value();
        }
    }
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
    try { if(a.contains("matrix")) ma = scModulation::fitMatrix(a["matrix"].get<std::vector<float>>(), NUM_SOURCES, NUM_TARGETS); } catch(...) {}
    try { if(b.contains("matrix")) mb = scModulation::fitMatrix(b["matrix"].get<std::vector<float>>(), NUM_SOURCES, NUM_TARGETS); } catch(...) {}
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

void scAnalogBox::storeSnapshot(int slot, const std::string& name) {
    if(slot < 0 || slot >= NUM_SNAPS) return;
    const std::string keep = name.empty() ? snapName(slot) : name;
    snapSlots[slot] = captureSnapshot();
    snapSlots[slot]["name"] = keep.empty() ? "Snapshot " + ofToString(slot + 1) : keep;
    snapUsed[slot] = true;
    activeSnap = slot;
    saveSnapshotLibrary();
}

void scAnalogBox::recallSnapshot(int slot) {
    if(slot < 0 || slot >= NUM_SNAPS || !snapUsed[slot]) return;
    morphRun = false;
    applyState(snapSlots[slot], snapSlots[slot], 1.0f);
    activeSnap = slot;
    snapRecalling = true;
    snapshotP.set(slot + 1);      // shows / outputs it, without a second recall
    snapRecalling = false;
}

void scAnalogBox::startGlide(int slot) {
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

void scAnalogBox::applyAB() {
    const int a = morphAP.get() - 1, b = morphBP.get() - 1;
    if(a < 0 || b < 0 || a >= NUM_SNAPS || b >= NUM_SNAPS || !snapUsed[a] || !snapUsed[b]) return;
    morphRun = false;              // the slider takes over from a glide
    applyState(snapSlots[a], snapSlots[b], morphP.get());
}

std::string scAnalogBox::snapName(int k) const {
    if(k < 0 || k >= NUM_SNAPS || !snapUsed[k] || !snapSlots[k].contains("name")) return "";
    try { return snapSlots[k]["name"].get<std::string>(); } catch(...) { return ""; }
}

// ── Library (shared files) and banks ─────────────────────────────────────────
std::string scAnalogBox::snapDir() const {
    const std::string dir = ofToDataPath("nodeSnapshots/AnalogBox", true);
    try { std::filesystem::create_directories(std::filesystem::path(dir) / "banks"); } catch(...) {}
    return dir;
}

std::filesystem::file_time_type scAnalogBox::snapSetTime() const {
    std::filesystem::file_time_type t{};
    try {
        for(auto& e : std::filesystem::directory_iterator(snapDir()))
            if(e.is_regular_file() && e.path().filename().string().rfind("snapshot_", 0) == 0)
                t = std::max(t, std::filesystem::last_write_time(e.path()));
    } catch(...) {}
    return t;
}

void scAnalogBox::saveSnapshotLibrary() {
    const std::filesystem::path dir = snapDir();
    for(int k = 0; k < NUM_SNAPS; k++){
        const auto f = dir / ("snapshot_" + ofToString(k) + ".json");
        try {
            if(snapUsed[k]) ofSavePrettyJson(f.string(), snapSlots[k]);
            else if(std::filesystem::exists(f)) std::filesystem::remove(f);
        } catch(...) {}
    }
    // a marker: the library exists (empty slots stay empty, no factory refill)
    try { ofSaveJson((dir / "library.json").string(), ofJson{{"analogBoxSnapshots", 1}}); } catch(...) {}
    snapLibTime = snapSetTime();
}

void scAnalogBox::loadSnapshotLibrary() {
    const std::filesystem::path dir = snapDir();
    const auto bank = dir / "banks" / "Factory.json";
    {
        // the factory collection, as a bank anyone can reload (rewritten every
        // start, so it always holds this build's sounds)
        ofJson j;
        j["analogBoxSnapshotBank"] = 1;
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

void scAnalogBox::saveSnapshotBank(const std::string& path) {
    ofJson j;
    j["analogBoxSnapshotBank"] = 1;
    j["slots"] = ofJson::array();
    for(int k = 0; k < NUM_SNAPS; k++) j["slots"].push_back(snapUsed[k] ? snapSlots[k] : ofJson(nullptr));
    std::string p = path;
    if(std::filesystem::path(p).extension().string() != ".json") p += ".json";
    snapMsg = ofSavePrettyJson(p, j) ? "Saved " + std::filesystem::path(p).stem().string() : "Could not save " + p;
}

bool scAnalogBox::loadSnapshotBank(const std::string& path) {
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

void scAnalogBox::loadFactory() {
    snapUsed.fill(false);
    auto f = factorySnapshots();
    for(int k = 0; k < std::min(NUM_SNAPS, (int)f.size()); k++){ snapSlots[k] = f[k]; snapUsed[k] = true; }
    activeSnap = -1;
    snapMsg = "Loaded the factory snapshots";
    saveSnapshotLibrary();
}


// ── The bar under the header: a slot menu (recall), store, glide, A/B morph,
//    banks. Every one of them publishable. ─────────────────────────────────────
void scAnalogBox::drawSnapshotBar(float w) {
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
    if(ImGui::BeginCombo("##abSnap", prev.c_str(), ImGuiComboFlags_HeightLarge)){
        for(int k = 0; k < NUM_SNAPS; k++){
            if(!snapUsed[k]) continue;
            if(ImGui::Selectable(label(k).c_str(), k == activeSnap)){
                if(morphGlideP.get() || ImGui::GetIO().KeyAlt) startGlide(k); else recallSnapshot(k);
            }
            // right-click: replace with the current sound, or delete
            if(ImGui::BeginPopupContextItem(("##abSnapItem" + ofToString(k)).c_str())){
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
        ImGui::OpenPopup("##abSnapStore");
    }
    if(ImGui::BeginPopup("##abSnapStore")){
        ImGui::TextDisabled("Store the current sound");
        ImGui::SetNextItemWidth(200 * z);
        if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if(ImGui::InputTextWithHint("##abSnapName", "name", snapNameBuf, sizeof(snapNameBuf),
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
    if(ImGui::Button("Snapshots")) ImGui::OpenPopup("##abSnapMenu");
    if(ImGui::BeginPopup("##abSnapMenu")){
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
            const bool enter = ImGui::InputText("##abBankName", snapBankName, sizeof(snapBankName),
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
        if(ImGui::SliderFloat("##abMorphT", &v, 0.0f, 64.0f, beats ? "%.2f beats" : "%.2f s", ImGuiSliderFlags_Logarithmic))
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
    ImGui::TextUnformatted("A"); ImGui::SameLine(); slotCombo("##abMorphA", morphAP);
    ImGui::SameLine();
    ImGui::TextUnformatted("B"); ImGui::SameLine(); slotCombo("##abMorphB", morphBP);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(80.0f * z, w - ImGui::GetCursorPosX()));
    float mv = morphP.get();
    if(ImGui::SliderFloat("##abMorph", &mv, 0.0f, 1.0f, "Morph %.3f")) morphP.set(mv);
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
        ImGui::SetTooltip("Blends snapshot A (0) into B (1). Publish it (right-click) to drive it from a connection.");
    publishedUnderline(morphP.getEscapedName());
    publishMenu(morphP.getEscapedName());
}


// ════════════════════════════════════════════════════════════════════════════
// Window
// ════════════════════════════════════════════════════════════════════════════

void scAnalogBox::draw(ofEventArgs&) {
    if(showWindow.get()) drawWindow();
}

void scAnalogBox::drawWindow() {
    const float z = zoom();
    ImGui::SetNextWindowSize(ImVec2(1200 * z, 800 * z), ImGuiCond_FirstUseEver);
    bool open = showWindow.get();
    const std::string title = "AnalogBox " + ofToString(getNumIdentifier());
    if(!ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)){
        ImGui::End();
        if(!open) showWindow = false;
        return;
    }
    if(!open){ showWindow = false; ImGui::End(); return; }
    const float w = ImGui::GetContentRegionAvail().x;

    // ── Header: voices, channels, master, gate mode, voices' levels ─────────
    {
        labelW = 54.0f;
        ImGui::BeginGroup(); sliderInt(voices, "Voices", 170 * z); ImGui::EndGroup();
        ImGui::SameLine(0, 6 * z);
        toggle(extraVoices, allocating() ? ("+" + ofToString(n() - nIn()) + "##xv").c_str() : "Extra##xv");
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Extra Voices: gated, the synth runs up to twice as many voices (16 at most)\n"
                              "and each new note takes a free one, so a note never cuts a release short (no clicks).\n"
                              "Voices stays the number of Gate / Pitch / Velocity inputs. Now: %d voices.", n());
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
    }
    drawSnapshotBar(w);
    ImGui::Spacing();

    if(ImGui::BeginTabBar("##abTabs")){
        auto tab = [&](const char* name, std::function<void(float)> body){
            if(!ImGui::BeginTabItem(name)) return;
            ImGui::BeginChild((std::string("##abTab") + name).c_str(), ImVec2(0, 0), false);
            ImGui::Spacing();
            const float tw = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
            labelW = 70.0f;
            body(tw);
            ImGui::EndChild();
            ImGui::EndTabItem();
        };
        tab("Oscillators", [this](float tw){ drawOscTab(tw); });
        tab("Filter", [this](float tw){ drawFilterTab(tw); });
        tab("Envelopes", [this](float tw){
            if(!vcaOn()) ImGui::TextColored(ImColor(pal::envMod), "No envelope on Amp: the voices drone. Route an envelope to Amp (Routing) to shape them.");
            modulation.drawEnvelopes(tw);
        });
        tab("LFOs", [this](float tw){ modulation.drawLfos(tw); });
        tab("Routing", [this](float tw){ modulation.drawRouting(tw); });
        tab("Panning", [this](float tw){ drawPanTab(tw); });
        tab("Effects", [this](float tw){ drawFxTab(tw); });
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ── Oscillators: the cards (4 per row), then the mixer's other sources and
//    the pitch ────────────────────────────────────────────────────────────────
void scAnalogBox::drawOscCard(int k, float w) {
    const float z = zoom();
    Osc& O = osc[k];
    ImGui::PushID(k);
    ImGui::BeginGroup();
    const std::string title = "Osc " + ofToString(k + 1);
    sectionHeader(title.c_str(), w, nullptr, pal::osc[k]);
    {   // its waveform, as MorphOsc reads it
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float h = 64.0f * z;
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
        canvas(dl, a, b);
        dl->AddLine(ImVec2(a.x, (a.y + b.y) * 0.5f), ImVec2(b.x, (a.y + b.y) * 0.5f), pal::grid);
        std::vector<ImVec2> pts;
        const int N = 160;
        for(int i = 0; i <= N; i++){
            const double ph = std::fmod(2.0 * i / N, 1.0);   // two cycles
            const float y = morphShape(i == N ? 0.999999 : ph, O.skew.get(), O.round.get(), O.width.get(), O.centered.get());
            pts.push_back(ImVec2(a.x + w * i / N, (a.y + b.y) * 0.5f - y * h * 0.42f));
        }
        dl->AddPolyline(pts.data(), (int)pts.size(), pal::osc[k], 0, 1.8f * z);
        if(O.sync.get() && k > 0) dl->AddText(ImVec2(b.x - 40 * z, a.y + 2 * z), pal::envMod, "SYNC");
        ImGui::Dummy(ImVec2(w, h));
    }
    sliderVec(O.level, "Level", w, "%.2f");
    {   // octave / semitones / cents on one row
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Tune");
        ImGui::SameLine();
        ImGui::SetCursorPosX(x0 + labelW * z);
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        const float cw = (w - labelW * z - 2 * sp) / 3.0f;
        ImGui::SetNextItemWidth(cw);
        int o = O.oct.get();
        if(ImGui::SliderInt("##oct", &o, -4, 4, "%+d oct")) O.oct = o;
        publishedUnderline(O.oct.getEscapedName()); publishMenu(O.oct.getEscapedName());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(cw);
        float s = O.semi.get();
        if(ImGui::SliderFloat("##semi", &s, -24, 24, "%+.0f st")) O.semi = ImGui::GetIO().KeyShift ? s : std::round(s);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Semitones (Shift: in between)");
        publishedUnderline(O.semi.getEscapedName()); publishMenu(O.semi.getEscapedName());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(cw);
        float f = O.fine.get();
        if(ImGui::SliderFloat("##fine", &f, -100, 100, "%+.0f ct")) O.fine = f;
        publishedUnderline(O.fine.getEscapedName()); publishMenu(O.fine.getEscapedName());
    }
    slider(O.ratio, "Ratio", w, "x%.3f", true);
    slider(O.skew, "Skew", w, "%+.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("-1 / +1: saw (down / up), 0: symmetric");
    slider(O.round, "Round", w, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("0 triangle (saw with Skew), 0.5 sine, 1 square");
    slider(O.width, "Width", w, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Below 0.5 the shape narrows into a pulse (PWM), above it widens");
    slider(O.noise, "Noise", w, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Noise added to this oscillator (into its FM and ring too)");
    toggle(O.centered, "Centered");
    if(k > 0){ ImGui::SameLine(); toggle(O.sync, "Sync to Osc 1"); }
    {   // FM: from which oscillator, how much
        std::vector<const char*> from = {"FM off"};
        static const char* names[MAX_OSCS] = {"from Osc 1", "from Osc 2", "from Osc 3", "from Osc 4", "from Osc 5", "from Osc 6"};
        for(int j = 0; j < oscs(); j++) from.push_back(names[j]);
        if(O.fmFrom.get() > oscs()) O.fmFrom = 0;
        combo(O.fmFrom, "FM", w, from);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Phase modulation by another oscillator (itself: feedback FM)");
        if(O.fmFrom.get() > 0) sliderVec(O.fm, "FM Index", w, "%.2f");
    }
    ImGui::EndGroup();
    ImGui::PopID();
}

void scAnalogBox::drawOscTab(float w) {
    const float z = zoom();
    const float cw = colW(w, 4);
    // the oscillators, 4 per row
    for(int k = 0; k < oscs(); k++){
        if(k % 4) ImGui::SameLine(0, gapW());
        drawOscCard(k, cw);
    }
    // the add / remove card
    if(oscs() % 4) ImGui::SameLine(0, gapW());
    ImGui::BeginGroup();
    sectionHeader("Oscillators", cw);
    ImGui::TextColored(ImColor(pal::textDim), "%d in use (up to %d)", oscs(), MAX_OSCS);
    if(oscs() < MAX_OSCS && ImGui::Button("+  Add oscillator", ImVec2(cw, 0))){
        const int k = oscs();
        // a new one: a copy of the one before, an octave down (a sub)
        osc[k].oct = std::max(-4, osc[k - 1].oct.get() - 1);
        oscCount = oscs() + 1;
    }
    if(oscs() > 1 && ImGui::Button(("Remove Osc " + ofToString(oscs())).c_str(), ImVec2(cw, 0))){
        for(int j = 0; j < MAX_OSCS; j++) if(osc[j].fmFrom.get() == oscs()) osc[j].fmFrom = 0;
        oscCount = oscs() - 1;
    }
    publishMenu(oscCount.getEscapedName());
    ImGui::EndGroup();
    ImGui::Spacing();
    ImGui::Spacing();
    // noise, ring, pitch
    ImGui::BeginGroup();
    sectionHeader("Noise", cw, nullptr, IM_COL32(220, 220, 225, 255));
    sliderVec(noiseLevel, "Level", cw, "%.2f");
    slider(noiseColor, "Color", cw, "%+.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("-1 dark .. 0 white .. +1 bright");
    nextCol();
    sectionHeader("Ring 1 x 2", cw, nullptr, IM_COL32(232, 146, 104, 255));
    if(oscs() < 2) ImGui::TextColored(ImColor(pal::textDim), "Add a second oscillator.");
    else sliderVec(ringLevel, "Level", cw, "%.2f");
    nextCol();
    sectionHeader("Pitch", cw);
    labelW = 62.0f;
    slider(glide, "Glide", cw, "%.3f s", true);
    sliderVec(transpose, "Transpose", cw, "%+.2f st");
    sliderVec(detune, "Detune", cw, "%.2f st");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("A random offset per voice, up to this many semitones");
    slider(velSens, "Vel Sens", cw, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("How much Velocity sets the level (0: not at all)");
    slider(drift, "Drift", cw, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Each oscillator wanders a little, as an analog one");
    nextCol();
    sectionHeader("Vibrato", cw);
    slider(vibRate, "Rate", cw, "%.2f Hz");
    slider(vibRand, "Rate Rnd", cw, "%.2f");
    slider(vibAmt, "Depth", cw, "%.2f st");
    labelW = 70.0f;
    ImGui::EndGroup();
}

// ── Filters: a card each (added at will), their routing, the response ──────
void scAnalogBox::drawFilterCard(int k, float cw) {
    const FilterRef f = filterRef(k);
    const bool comb = f.mode->get() == kComb;
    ImGui::BeginGroup();
    ImGui::PushID(k);
    sectionHeader(("Filter " + ofToString(k + 1)).c_str(), cw, f.on, pal::osc[k % 6]);
    const int modeWas = f.mode->get();
    combo(*f.mode, "Mode", cw, kFilterModes);
    if(f.mode->get() == kComb && modeWas != kComb){
        // a comb plays the note: tracked, at the note itself
        *f.keyTrack = 1.0f;
        *f.cutoff = std::vector<float>{60.0f};
        if(first(f.res->get(), 0.0f) < 0.6f) *f.res = std::vector<float>{0.9f};
    }
    combo(*f.type, "Type", cw, comb ? kCombTypes : kFilterTypes);
    if(comb && ImGui::IsItemHovered())
        ImGui::SetTooltip("Feedback +: rings at the pitch (its harmonics)\nFeedback -: an octave down, odd harmonics (hollow)\n"
                          "Feedforward: notches between / on the harmonics (phasey)");
    const float note = first(f.cutoff->get(), 84.0f);
    const float fc = 440.0f * std::pow(2.0f, (note - 69.0f) / 12.0f);
    std::string fmt;
    if(comb && f.keyTrack->get() > 0.99f) fmt = (note >= 60.0f ? "+" : "") + ofToString(note - 60.0f, 1) + " st from the note";
    else fmt = ofToString(note, 1) + "  (" + (fc >= 1000 ? ofToString(fc / 1000.0f, 2) + " kHz)" : ofToString(fc, 0) + " Hz)");
    sliderVec(*f.cutoff, comb ? "Pitch" : "Cutoff", cw, fmt.c_str());
    if(comb && ImGui::IsItemHovered()) ImGui::SetTooltip("With Key Track 1: 60 is the note played (72 an octave up)");
    sliderVec(*f.res, comb ? "Feedback" : "Resonance", cw, "%.2f");
    slider(*f.keyTrack, "Key Track", cw, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("1: follows the note (one octave per octave)");
    slider(*f.drive, "Drive", cw, "%.1f dB");
    slider(*f.sat, "Saturation", cw, "%.2f");
    slider(*f.feedback, comb ? "Damping" : "Feedback", cw, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip(comb ? "Darkens each repeat (a low-pass in the comb's loop)"
                                                      : "The output back into the input, through a saturator");
    slider(*f.level, "Level", cw, "%.2f");
    if(k == 0){
        slider(hpf, "HPF", cw, "%.0f Hz", true);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("A high-pass before the filters (10 Hz: off)");
    }
    if(k > 0 && k == filters() - 1 && ImGui::Button(("Remove Filter " + ofToString(k + 1)).c_str(), ImVec2(cw, 0))) filterCount = filters() - 1;
    ImGui::PopID();
    ImGui::EndGroup();
}

void scAnalogBox::drawFilterTab(float w) {
    const float z = zoom();
    const float cw = colW(w, 4);
    for(int k = 0; k < filters(); k++){
        if(k) ImGui::SameLine(0, gapW());
        drawFilterCard(k, cw);
    }
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::BeginGroup();
    sectionHeader("Filters", cw);
    {
        int r = filterRouting.get();
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Routing");
        publishedUnderline(filterRouting.getEscapedName());
        ImGui::SameLine();
        ImGui::SetCursorPosX(x0 + labelW * z);
        ImGui::SetNextItemWidth(cw - labelW * z);
        if(ImGui::Combo("##fRouting", &r, "Series\0Parallel\0")) filterRouting = r;
        publishMenu(filterRouting.getEscapedName());
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Series: each filter into the next (1 -> 2 -> ...)\nParallel: all from the oscillators, summed (Level: each one's share)");
    }
    ImGui::TextColored(ImColor(pal::textDim), "%d in use (up to %d)", filters(), MAX_FILTERS);
    if(filters() < MAX_FILTERS && ImGui::Button("+  Add filter", ImVec2(cw, 0))){
        auto& f = moreF[filters() - 1];
        // a new one: on, a gentle band pass up high (or a comb, from its Mode)
        f.on = true;
        filterCount = filters() + 1;
    }
    publishMenu(filterCount.getEscapedName());
    for(int pass = 0; pass < 2; pass++){   // Env 2 / LFO 1 -> Cutoff 1, straight from the matrix
        const int source = pass == 0 ? SRC_ENV + 1 : 0;
        const bool has = pass == 0 ? modulation.envCount >= 2 : modulation.lfoCount >= 1;
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(pass == 0 ? "Env 2" : "LFO 1");
        ImGui::SameLine();
        ImGui::SetCursorPosX(x0 + labelW * z);
        ImGui::SetNextItemWidth(cw - labelW * z);
        float v = matrixCell(source, TCut);
        if(!has) ImGui::TextColored(ImColor(pal::textDim), pass == 0 ? "add a second envelope" : "add an LFO");
        else if(ImGui::SliderFloat(pass == 0 ? "##envCut" : "##lfoCut", &v, -1.0f, 1.0f, "%+.2f -> Cutoff 1")) setMatrixCell(source, TCut, v);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("The Routing matrix's cell: %+.0f semitones at its peak (F2.. Cutoff: their own columns)", v * 48);
    }
    nextCol();
    const float rw = cw * 3 + gapW() * 2;
    sectionHeader("Response", rw);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float h = 170.0f * z;
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + rw, a.y + h);
        canvas(dl, a, b);
        for(float f : {100.0f, 1000.0f, 10000.0f}){
            const float x = a.x + rw * std::log(f / 20.0f) / std::log(1000.0f);
            dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), pal::grid);
        }
        const float zeroY = a.y + h * 0.3f;
        dl->AddLine(ImVec2(a.x, zeroY), ImVec2(b.x, zeroY), pal::gridStrong);
        constexpr int N = 240;
        auto freqAt = [](int i){ return 20.0f * std::pow(1000.0f, i / (float)N); };
        auto plot = [&](const std::function<float(float)>& db, ImU32 col, float thick){
            std::vector<ImVec2> pts;
            for(int i = 0; i <= N; i++) pts.push_back(ImVec2(a.x + rw * i / N, ofClamp(zeroY - db(freqAt(i)) * h / 72.0f, a.y + 1, b.y - 1)));
            dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, thick);
        };
        // a filter as set (combs at a C3 note when tracked)
        auto H = [&](int k, float f, float cutNote, float r){
            const FilterRef fr = filterRef(k);
            if(!fr.on->get()) return std::complex<float>(1.0f, 0.0f) * fr.level->get();
            const float hz = 440.0f * std::pow(2.0f, (cutNote - 69.0f) / 12.0f);
            return filterH(fr.mode->get(), fr.type->get(), f, ofClamp(hz, 10.0f, 20000.0f), r) * fr.level->get();
        };
        auto setNote = [&](int k){ return first(filterRef(k).cutoff->get(), 84.0f); };
        auto setRes = [&](int k){ return first(filterRef(k).res->get(), 0.25f); };
        // each voice's first filter as it is now (key tracking, envelopes, LFOs...)
        const int nv = n();
        if(filterOn.get() && fMode.get() != kComb && (int)visValues.size() >= VIS_CUT + 2 * W){
            for(int v = 0; v < nv; v++){
                const float vn = visValues[VIS_CUT + v], vr = ofClamp(visValues[VIS_CUT + W + v], 0.0f, 1.0f);
                ImColor col = ImColor::HSV(std::fmod(v * 0.61803f, 1.0f), 0.5f, 0.98f);
                col.Value.w = 0.4f;
                plot([&](float f){ return 20.0f * std::log10(std::max(1e-6f, std::abs(H(0, f, vn, vr)))); }, (ImU32)col, 1.0f * z);
                const float hz = 440.0f * std::pow(2.0f, (vn - 69.0f) / 12.0f);
                const float x = a.x + rw * std::log(ofClamp(hz, 20.0f, 20000.0f) / 20.0f) / std::log(1000.0f);
                dl->AddLine(ImVec2(x, b.y - 6 * z), ImVec2(x, b.y), (ImU32)col, 2.0f * z);
            }
        }
        // each filter alone (faint), then all of them together
        const int nf = filters();
        if(nf > 1) for(int k = 0; k < nf; k++){
            const ImU32 c = (pal::osc[k % 6] & 0x00FFFFFF) | 0x60000000;
            plot([&](float f){ return 20.0f * std::log10(std::max(1e-6f, std::abs(H(k, f, setNote(k), setRes(k))))); }, c, 1.2f * z);
        }
        const bool series = filterRouting.get() == 0;
        plot([&](float f){
            std::complex<float> t = series ? std::complex<float>(1.0f, 0.0f) : std::complex<float>(0.0f, 0.0f);
            for(int k = 0; k < nf; k++){ const auto h = H(k, f, setNote(k), setRes(k)); t = series ? t * h : t + h; }
            return 20.0f * std::log10(std::max(1e-6f, std::abs(t)));
        }, (pal::accent & 0x00FFFFFF) | 0xC0000000, 2.2f * z);
        std::string t = nf == 1 ? std::string(kFilterModes[std::clamp(fMode.get(), 0, kComb)]) + "  " +
                                  (fMode.get() == kComb ? kCombTypes : kFilterTypes)[std::clamp(fType.get(), 0, 3)]
                                : ofToString(nf) + " filters in " + (series ? "series" : "parallel");
        dl->AddText(ImVec2(a.x + 4 * z, b.y - ImGui::GetTextLineHeight() - 2 * z), pal::textDim, t.c_str());
        ImGui::Dummy(ImVec2(rw, h));
    }
    ImGui::EndGroup();
    ImGui::Spacing();
    ImGui::TextColored(ImColor(pal::textDim), "More movement from Routing (F2..F4 Cutoff / Reso columns), or right-click a control: Modulate with.");
}

// ── Effects: two rows of columns ─────────────────────────────────────────────
void scAnalogBox::drawFxTab(float w) {
    const float z = zoom();
    const float cw = colW(w, 5);
    auto disabledIf = [](bool off){ if(off) ImGui::BeginDisabled(); };
    auto endDisabled = [](bool off){ if(off) ImGui::EndDisabled(); };
    ImGui::BeginGroup();
    sectionHeader("Sine Shaper", cw, &shaperOn);
    disabledIf(!shaperOn.get());
    slider(shaperDrive, "Drive", cw, "%.2f");
    slider(shaperMix, "Mix", cw, "%.2f");
    {   // its transfer curve
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float h = std::min(cw * 0.6f, 100.0f * z);
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + cw, a.y + h);
        canvas(dl, a, b);
        const float dr = shaperDrive.get(), mix = shaperMix.get();
        const float dry = std::cos(mix * HALF_PI), wet = std::sin(mix * HALF_PI);
        std::vector<ImVec2> pts;
        for(int i = 0; i <= 100; i++){
            const float x = -1.0f + 2.0f * i / 100.0f;
            const float y = ofClamp(dry * x + wet * std::sin(x * dr), -1.2f, 1.2f);
            pts.push_back(ImVec2(a.x + cw * i / 100.0f, (a.y + b.y) * 0.5f - y * h * 0.42f));
        }
        dl->AddPolyline(pts.data(), (int)pts.size(), pal::envMod, 0, 2.0f * z);
        ImGui::Dummy(ImVec2(cw, h));
    }
    endDisabled(!shaperOn.get());
    nextCol();
    sectionHeader("Saturation", cw, &satOn);
    disabledIf(!satOn.get());
    slider(satDrive, "Drive", cw, "%.1f dB");
    slider(satBias, "Bias", cw, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Even harmonics (warmth)");
    slider(satTone, "Tone", cw, "%.0f Hz", true);
    slider(satOutput, "Output", cw, "%+.1f dB");
    slider(satMix, "Mix", cw, "%.2f");
    endDisabled(!satOn.get());
    nextCol();
    sectionHeader("Ring Modulator", cw, &ringFxOn);
    disabledIf(!ringFxOn.get());
    slider(ringFreq, "Freq", cw, "%.1f Hz", true);
    slider(ringSpread, "Spread", cw, "%.2f st");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Each channel's carrier a little apart (semitones across the speakers)");
    slider(ringMix, "Mix", cw, "%.2f");
    endDisabled(!ringFxOn.get());
    nextCol();
    sectionHeader("Freq Shifter", cw, &shiftOn);
    disabledIf(!shiftOn.get());
    slider(shiftHz, "Shift", cw, "%+.1f Hz");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Every partial moves by this many Hz (not a ratio: inharmonic, metallic)");
    slider(shiftSpread, "Spread", cw, "%.1f Hz");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Each channel's shift a little apart (Hz across the speakers): width, beating");
    toggle(shiftBoth, "Both sidebands");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Up and down at once (a ring modulator without its carrier)");
    slider(shiftFeed, "Feedback", cw, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("The shifted sound shifted again, each repeat further: barber-pole spirals");
    slider(shiftDelay, "Delay", cw, "%.3f s");
    slider(shiftTone, "Tone", cw, "%.0f Hz", true);
    slider(shiftMix, "Mix", cw, "%.2f");
    endDisabled(!shiftOn.get());
    nextCol();
    sectionHeader("Chorus", cw, &chorusOn);
    disabledIf(!chorusOn.get());
    slider(chorusRate, "Rate", cw, "%.2f Hz", true);
    slider(chorusDepth, "Depth", cw, "%.2f ms");
    slider(chorusDelay, "Delay", cw, "%.1f ms");
    slider(chorusSpread, "Width", cw, "%.2f");
    slider(chorusMix, "Mix", cw, "%.2f");
    endDisabled(!chorusOn.get());
    ImGui::EndGroup();
    ImGui::Spacing();

    ImGui::BeginGroup();
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
    disabledIf(!eqOn.get());
    if(eqEditor.draw(cw, 150.0f * z, scEQEditor::defaultColors(), eqOn.get(), nullptr, 0, "##abEq")){
        for(int b = 0; b < 5; b++){
            eqFreq[b] = eqEditor.bands[b].freqHz;
            eqGain[b] = eqEditor.bands[b].gainDb;
            eqShape[b] = eqEditor.bands[b].shape;
        }
    }
    endDisabled(!eqOn.get());
    nextCol();
    sectionHeader("Echo", cw, &echoOn);
    disabledIf(!echoOn.get());
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
    combo(echoFilter, "Filter", cw, {"LowPass", "HighPass", "BandPass", "PeakEQ"});
    slider(echoPingPong, "Ping-pong", cw, "%.2f");
    slider(echoMix, "Mix", cw, "%.2f");
    endDisabled(!echoOn.get());
    nextCol();
    sectionHeader("Reverb", cw, &rvbOn);
    disabledIf(!rvbOn.get());
    slider(rvbMix, "Mix", cw, "%.2f");
    slider(rvbDecay, "Decay", cw, "%.2f s", true);
    slider(rvbSize, "Size", cw, "%.1f");
    slider(rvbPredelay, "Predelay", cw, "%.0f ms");
    slider(rvbLate, "Late", cw, "%.2f");
    slider(rvbSpread, "Spread", cw, "%+.2f");
    endDisabled(!rvbOn.get());
    nextCol();
    sectionHeader("Reverb Tone", cw);
    disabledIf(!rvbOn.get());
    slider(rvbLowpass, "Lowpass", cw, "%.0f Hz", true);
    slider(rvbHiDamp, "Hi Damp", cw, "%.1f dB");
    slider(rvbLoDamp, "Lo Damp", cw, "%.1f dB");
    slider(rvbModRate, "Mod Rate", cw, "%.3f Hz", true);
    slider(rvbSpin, "Spin", cw, "%.2f");
    slider(rvbWander, "Wander", cw, "%.2f");
    endDisabled(!rvbOn.get());
    ImGui::EndGroup();
    ImGui::TextColored(ImColor(pal::textDim), "Voices -> Shaper -> Saturation -> Ring -> Freq Shifter -> Chorus -> EQ -> Echo -> Reverb, at the speaker count. "
                       "A stage switched off lets its tail ring out, then stops using CPU.");
}

// ── The factory collection: each one the defaults plus its own settings
//    (control names as in the window), its routes [source, target, amount]
//    (Env 1 -> Amp is each one's VCA; sources: LFO 0-7, Env 8-11, Velocity 12, Random 13; targets: Pitch 0,
//    Slave Pitch 1, Width 2, Skew 3, Round 4, FM 5, Noise 6, Cutoff 7, Reso 8,
//    Drive 9, Filter Fb 10, Amp 11, Pan 12, F2 Cutoff 13, F2 Reso 14, F3 Cutoff 15,
//    F3 Reso 16, F4 Cutoff 17, F4 Reso 18) and counts ──
std::vector<ofJson> scAnalogBox::factorySnapshots() const {
    static const char* src = R"JSON([
 {"name": "Init Saw", "lfos": 1, "envs": 2, "routes": [[9, 7, 0.4], [8, 11, 1.0]], "set": {}},
 {"name": "Fat Bass", "lfos": 1, "envs": 2, "routes": [[9, 7, 0.45], [12, 7, 0.15], [8, 11, 1.0]], "set": {"Oscillators": 3, "Osc1 Skew": 1, "Osc1 Round": 0, "Osc2 Skew": 1, "Osc2 Round": 0, "Osc2 Semi": 0, "Osc2 Fine": 9, "Osc3 Octave": -1, "Osc3 Round": 1, "Osc3 Level": 0.5, "Cutoff": 52, "Resonance": 0.35, "Filter Mode": 3, "Filter Drive": 9, "Filter Saturation": 0.5, "Key Track": 0.4, "Env2 Attack": 0.002, "Env2 Decay": 0.25, "Env2 Sustain": 0.1, "Env2 Release": 0.2, "Env1 Attack": 0.002, "Env1 Decay": 0.3, "Env1 Sustain": 0.8, "Env1 Release": 0.15, "Glide": 0.04}},
 {"name": "Acid Line", "lfos": 1, "envs": 2, "routes": [[9, 7, 0.6], [12, 8, 0.2], [8, 11, 1.0]], "set": {"Oscillators": 1, "Osc1 Skew": 1, "Osc1 Round": 0, "Osc1 Level": 0.7, "Cutoff": 48, "Resonance": 0.82, "Filter Mode": 3, "Filter Saturation": 0.7, "Filter Drive": 6, "Key Track": 0.3, "Env2 Attack": 0.001, "Env2 Decay": 0.18, "Env2 Sustain": 0.0, "Env2 Release": 0.1, "Env1 Attack": 0.001, "Env1 Decay": 0.25, "Env1 Sustain": 0.6, "Env1 Release": 0.08, "Glide": 0.06, "Echo On": true, "Echo Beats": 0.75, "Echo Mix": 0.2}},
 {"name": "Sync Lead", "lfos": 1, "envs": 3, "routes": [[9, 7, 0.3], [10, 1, 0.75], [0, 0, 0.01], [8, 11, 1.0]], "set": {"Oscillators": 2, "Osc1 Skew": 1, "Osc1 Round": 0, "Osc1 Level": 0.25, "Osc2 Skew": 1, "Osc2 Round": 0, "Osc2 Semi": 12, "Osc2 Fine": 0, "Osc2 Sync": true, "Osc2 Level": 0.6, "Cutoff": 96, "Resonance": 0.2, "Filter Mode": 1, "Env3 Attack": 0.001, "Env3 Decay": 0.6, "Env3 Sustain": 0.2, "Env3 Release": 0.4, "Vib Amount": 0.15, "Vib Rate": 5.5, "Glide": 0.05, "Echo On": true, "Echo Beats": 0.5, "Echo Mix": 0.25, "Reverb On": true, "Reverb Mix": 0.2}},
 {"name": "PWM Strings", "lfos": 2, "envs": 2, "routes": [[0, 2, 0.6], [1, 2, -0.4], [9, 7, 0.15], [8, 11, 1.0]], "set": {"Oscillators": 2, "Osc1 Round": 1, "Osc1 Skew": 0, "Osc1 Width": 0.35, "Osc2 Round": 1, "Osc2 Skew": 0, "Osc2 Width": 0.35, "Osc2 Semi": 0, "Osc2 Fine": -7, "Osc1 Level": 0.45, "Osc2 Level": 0.45, "LFO1 Sync": false, "LFO1 Hz": 0.35, "LFO1 Spread": 1, "LFO2 Sync": false, "LFO2 Hz": 0.21, "Cutoff": 88, "Resonance": 0.1, "Filter Mode": 1, "HPF": 120, "Env1 Attack": 0.6, "Env1 Decay": 1.0, "Env1 Sustain": 0.85, "Env1 Release": 1.6, "Detune": 0.12, "Drift": 0.3, "Chorus On": true, "Chorus Mix": 0.6, "Reverb On": true, "Reverb Mix": 0.35, "Reverb Decay": 5}},
 {"name": "Brass", "lfos": 1, "envs": 2, "routes": [[9, 7, 0.45], [12, 7, 0.2], [8, 11, 1.0]], "set": {"Oscillators": 2, "Osc1 Skew": 1, "Osc1 Round": 0, "Osc2 Skew": 1, "Osc2 Round": 0, "Osc2 Semi": 0, "Osc2 Fine": 5, "Cutoff": 60, "Resonance": 0.15, "Filter Mode": 1, "Env2 Attack": 0.08, "Env2 Decay": 0.5, "Env2 Sustain": 0.5, "Env2 Release": 0.4, "Env1 Attack": 0.05, "Env1 Decay": 0.4, "Env1 Sustain": 0.85, "Env1 Release": 0.3, "Vib Amount": 0.08, "Drift": 0.25, "Reverb On": true, "Reverb Mix": 0.2}},
 {"name": "FM Bell", "lfos": 1, "envs": 2, "routes": [[9, 5, 0.6], [8, 11, 1.0]], "set": {"Oscillators": 2, "Osc1 Skew": 0, "Osc1 Round": 0.5, "Osc1 Level": 0.0, "Osc1 Ratio": 3.5, "Osc2 Skew": 0, "Osc2 Round": 0.5, "Osc2 Semi": 0, "Osc2 Fine": 0, "Osc2 Level": 0.7, "Osc2 FM From": 1, "Osc2 FM": 0.4, "Filter On": false, "Env2 Attack": 0.001, "Env2 Decay": 1.2, "Env2 Sustain": 0.0, "Env2 Release": 1.5, "Env1 Attack": 0.001, "Env1 Decay": 2.5, "Env1 Sustain": 0.0, "Env1 Release": 2.0, "Reverb On": true, "Reverb Mix": 0.35, "Reverb Decay": 6, "Echo On": true, "Echo Mix": 0.15}},
 {"name": "Wobble", "lfos": 1, "envs": 2, "routes": [[0, 7, 0.55], [0, 9, 0.3], [8, 11, 1.0]], "set": {"Oscillators": 3, "Osc1 Skew": 1, "Osc1 Round": 0, "Osc2 Round": 1, "Osc2 Skew": 0, "Osc2 Semi": 0, "Osc2 Fine": 0, "Osc3 Octave": -1, "Osc3 Round": 0.5, "Osc3 Skew": 0, "Osc3 Level": 0.5, "LFO1 Sync": true, "LFO1 Beats": 0.5, "LFO1 Round": 0.5, "Cutoff": 50, "Resonance": 0.55, "Filter Mode": 3, "Filter Drive": 12, "Filter Saturation": 0.6, "Env1 Attack": 0.005, "Env1 Sustain": 1, "Env1 Release": 0.2, "Saturation On": true, "Saturation Drive": 8}},
 {"name": "Noise Sweep", "lfos": 1, "envs": 2, "routes": [[9, 7, 0.7], [8, 11, 1.0]], "set": {"Oscillators": 1, "Osc1 Level": 0.0, "Noise": 0.8, "Noise Color": 0.2, "Cutoff": 30, "Resonance": 0.75, "Filter Mode": 0, "Filter Type": 1, "Env2 Attack": 2.5, "Env2 Decay": 3.0, "Env2 Sustain": 0.3, "Env2 Release": 3.0, "Env1 Attack": 1.5, "Env1 Sustain": 1, "Env1 Release": 3.0, "Reverb On": true, "Reverb Mix": 0.5, "Reverb Decay": 8}},
 {"name": "Pluck", "lfos": 1, "envs": 2, "routes": [[9, 7, 0.55], [12, 7, 0.25], [8, 11, 1.0]], "set": {"Oscillators": 2, "Osc1 Skew": 0.6, "Osc1 Round": 0.2, "Osc2 Round": 1, "Osc2 Skew": 0, "Osc2 Width": 0.2, "Osc2 Semi": 12, "Osc2 Fine": 3, "Osc2 Level": 0.3, "Cutoff": 46, "Resonance": 0.3, "Filter Mode": 2, "Key Track": 0.7, "Env2 Attack": 0.001, "Env2 Decay": 0.22, "Env2 Sustain": 0.0, "Env2 Release": 0.2, "Env1 Attack": 0.001, "Env1 Decay": 0.6, "Env1 Sustain": 0.0, "Env1 Release": 0.4, "Echo On": true, "Echo Beats": 0.375, "Echo Mix": 0.3, "Echo Ping-pong": 0.8, "Reverb On": true, "Reverb Mix": 0.25}},
 {"name": "Ring Drone", "lfos": 2, "envs": 1, "routes": [[0, 1, 0.08], [1, 7, 0.25], [8, 11, 1.0]], "set": {"Oscillators": 2, "Osc1 Round": 0.5, "Osc1 Skew": 0, "Osc2 Round": 0.3, "Osc2 Skew": 0.4, "Osc2 Semi": 7, "Osc2 Fine": 0, "Ring": 0.6, "Osc1 Level": 0.2, "Osc2 Level": 0.2, "LFO1 Sync": false, "LFO1 Hz": 0.05, "LFO1 Noise": true, "LFO1 Noise Scale": 3, "LFO2 Sync": false, "LFO2 Hz": 0.11, "LFO2 Spread": 1, "Cutoff": 80, "Resonance": 0.4, "Filter Mode": 1, "Filter Feedback": 0.3, "Env1 Attack": 2, "Env1 Sustain": 1, "Env1 Release": 4, "Drift": 0.6, "Chorus On": true, "Reverb On": true, "Reverb Mix": 0.45, "Reverb Decay": 10}},
 {"name": "Screaming Feedback", "lfos": 1, "envs": 2, "routes": [[9, 7, 0.3], [0, 10, 0.3], [8, 11, 1.0]], "set": {"Oscillators": 2, "Osc1 Skew": 1, "Osc1 Round": 0, "Osc2 Round": 1, "Osc2 Skew": 0, "Osc2 Width": 0.15, "Osc2 Semi": 0, "Osc2 Fine": 12, "Cutoff": 70, "Resonance": 0.9, "Filter Mode": 2, "Filter Drive": 18, "Filter Saturation": 0.8, "Filter Feedback": 0.55, "LFO1 Sync": true, "LFO1 Beats": 2, "Shaper On": true, "Shaper Drive": 1.5, "Shaper Mix": 0.4, "Echo On": true, "Echo Mix": 0.2}},
 {"name": "Comb Pluck", "lfos": 1, "envs": 2, "routes": [[9, 6, 1.0], [8, 11, 1.0]], "set": {"Oscillators": 1, "Osc1 Level": 0.0, "Noise": 0.001, "Noise Color": 0.2, "Filter Mode": 4, "Filter Type": 0, "Cutoff": 60, "Key Track": 1, "Resonance": 0.97, "Filter Feedback": 0.3, "Filters": 2, "F2 Mode": 1, "F2 Type": 0, "F2 Cutoff": 100, "F2 Resonance": 0.1, "F2 Key Track": 0.5, "Env2 Attack": 0.001, "Env2 Decay": 0.03, "Env2 Sustain": 0.0, "Env2 Release": 0.03, "Env1 Attack": 0.001, "Env1 Decay": 2.0, "Env1 Sustain": 0.0, "Env1 Release": 1.5, "Reverb On": true, "Reverb Mix": 0.25}},
 {"name": "Hollow Comb Drone", "lfos": 2, "envs": 1, "routes": [[0, 7, 0.08], [1, 13, 0.1], [8, 11, 1.0]], "set": {"Oscillators": 2, "Osc1 Skew": 1, "Osc1 Round": 0, "Osc2 Skew": 1, "Osc2 Round": 0, "Osc2 Semi": 0, "Osc2 Fine": 7, "Filter Mode": 4, "Filter Type": 1, "Cutoff": 72, "Key Track": 1, "Resonance": 0.85, "Filter Feedback": 0.5, "Filters": 2, "F2 Mode": 3, "F2 Type": 0, "F2 Cutoff": 90, "F2 Resonance": 0.3, "Filter Routing": 0, "LFO1 Sync": false, "LFO1 Hz": 0.07, "LFO2 Sync": false, "LFO2 Hz": 0.11, "Env1 Attack": 1.5, "Env1 Sustain": 1, "Env1 Release": 3, "Drift": 0.4, "Freq Shift On": true, "Freq Shift": 0.3, "Freq Shift Spread": 0.6, "Freq Shift Mix": 0.5, "Reverb On": true, "Reverb Mix": 0.4, "Reverb Decay": 8}},
 {"name": "Parallel Formants", "lfos": 1, "envs": 1, "routes": [[0, 7, 0.06], [0, 13, -0.06], [8, 11, 1.0]], "set": {"Oscillators": 2, "Osc1 Skew": 1, "Osc1 Round": 0, "Osc2 Skew": 1, "Osc2 Round": 0, "Osc2 Semi": 0, "Osc2 Fine": -6, "Filters": 3, "Filter Routing": 1, "Filter Mode": 0, "Filter Type": 1, "Cutoff": 74, "Resonance": 0.75, "Key Track": 0, "F2 Mode": 0, "F2 Type": 1, "F2 Cutoff": 86, "F2 Resonance": 0.75, "F2 Key Track": 0, "F2 Level": 0.8, "F3 Mode": 0, "F3 Type": 1, "F3 Cutoff": 98, "F3 Resonance": 0.7, "F3 Key Track": 0, "F3 Level": 0.5, "LFO1 Sync": false, "LFO1 Hz": 0.2, "Env1 Attack": 0.15, "Env1 Sustain": 1, "Env1 Release": 0.6, "Vib Amount": 0.12, "Chorus On": true, "Reverb On": true, "Reverb Mix": 0.3}},
 {"name": "Shifted Bells", "lfos": 1, "envs": 1, "routes": [[8, 11, 1.0]], "set": {"Oscillators": 2, "Osc1 Skew": 0, "Osc1 Round": 0.5, "Osc2 Skew": 0, "Osc2 Round": 0.5, "Osc2 Semi": 19, "Osc2 Fine": 0, "Osc2 Level": 0.3, "Filter On": false, "Env1 Attack": 0.001, "Env1 Decay": 2.2, "Env1 Sustain": 0.0, "Env1 Release": 1.8, "Freq Shift On": true, "Freq Shift": 137, "Freq Shift Spread": 4, "Freq Shift Feedback": 0.5, "Freq Shift Delay": 0.18, "Freq Shift Tone": 7000, "Freq Shift Mix": 0.6, "Reverb On": true, "Reverb Mix": 0.3, "Reverb Decay": 5}},
 {"name": "Sub Pad", "lfos": 2, "envs": 2, "routes": [[0, 4, 0.4], [1, 3, 0.3], [9, 7, 0.2], [8, 11, 1.0]], "set": {"Oscillators": 3, "Osc1 Round": 0.2, "Osc1 Skew": 0.3, "Osc2 Round": 0.6, "Osc2 Skew": -0.3, "Osc2 Semi": 0, "Osc2 Fine": -8, "Osc3 Octave": -1, "Osc3 Round": 0.5, "Osc3 Skew": 0, "Osc3 Level": 0.45, "LFO1 Sync": false, "LFO1 Hz": 0.13, "LFO1 Spread": 1, "LFO2 Sync": false, "LFO2 Hz": 0.07, "LFO2 Spread": 1, "Cutoff": 74, "Resonance": 0.2, "Filter Mode": 3, "Env1 Attack": 1.2, "Env1 Decay": 1.5, "Env1 Sustain": 0.9, "Env1 Release": 2.5, "Detune": 0.1, "Drift": 0.4, "Chorus On": true, "Chorus Mix": 0.5, "Reverb On": true, "Reverb Mix": 0.4, "Reverb Decay": 6}}
])JSON";
    std::vector<ofJson> out;
    ofJson list;
    try { list = ofJson::parse(src); } catch(const std::exception& e) { ofLogError("scAnalogBox") << "factory snapshots: " << e.what(); return out; }
    std::map<std::string, std::string> keyOf;
    for(const auto& c : controls) keyOf[c.name] = c.key;
    for(const auto& f : list){
        ofJson s = defaultSnap;
        auto& c = s["controls"];
        for(auto it = f["set"].begin(); it != f["set"].end(); ++it){
            auto k = keyOf.find(it.key());
            if(k == keyOf.end() || !c.contains(k->second)){ ofLogWarning("scAnalogBox") << "factory: no control " << it.key(); continue; }
            const ofJson& d = c[k->second];
            const ofJson& v = it.value();
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
        s["envs"] = f.value("envs", 1);
        s["exts"] = defaultSnap.value("exts", 0);
        s["name"] = f["name"];
        out.push_back(s);
    }
    return out;
}
