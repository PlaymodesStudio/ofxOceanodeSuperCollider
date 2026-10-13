//
//  scAdditiveBox.cpp
//  ofxOceanodeSuperCollider
//

#include "scAdditiveBox.h"
#include "serverManager.h"
#include "ofxSCServer.h"
#include "ofxOceanodeShared.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {
const std::vector<std::string> filterNames = {"Off", "Low Pass", "Band Pass", "High Pass", "Notch", "Comb", "Drawn"};
const std::vector<std::string> filter2Names = {"Off", "Vowel", "Drawn EQ"};
const std::vector<std::string> waveNames = {"Flat", "Saw", "Square", "Triangle", "Sine", "Pulse", "Organ", "Hollow", "Random", "Drawn"};
const std::vector<std::string> panModeNames = {"Random", "Spectrum", "Alternate"};
const std::vector<std::string> sweepModeNames = {"Partials", "Octaves"};
const std::vector<std::string> sweepShapeNames = {"Lobes", "Triangle", "Saw Up", "Saw Down", "Pulse", "Gauss", "Steps", "Drawn"};
const std::vector<std::string> lfoShapeNames = {"Sine", "Triangle", "Saw", "Square", "S&H"};
const std::vector<std::string> echoFilterNames = {"LowPass", "HighPass", "BandPass", "PeakEQ"};
const char* modSourceNames[3] = {"Env2", "LFO1", "LFO2"};
const char* modDestNames[7] = {"Cutoff", "Morph", "Pitch", "Level", "Sweep", "Pan", "Noise"};
const char* modDestHelp[7] = {"x 48 semitones", "0..1", "x 12 semitones", "x level", "0..1", "x half a turn", "x noise"};
const char* fxDefBase[3] = {"graphiceq", "Echo", "SpaceMaster"};
const char* eqBandNames[5] = {"Low", "LoMid", "Mid", "HiMid", "High"};

// Vowel formants (male voice): F1..F3 in Hz, levels in dB, bandwidths in Hz
struct Formants { float f[3]; float db[3]; float bw[3]; };
const Formants vowels[5] = {
    {{800, 1150, 2900}, {0, -6, -32}, {80, 90, 120}},    // A
    {{400, 1600, 2700}, {0, -24, -30}, {60, 80, 120}},   // E
    {{350, 1700, 2700}, {0, -20, -30}, {50, 100, 120}},  // I
    {{450, 800, 2830}, {0, -11, -22}, {70, 80, 100}},    // O
    {{325, 700, 2530}, {0, -16, -35}, {50, 60, 170}},    // U
};

// Folder holding "<name>1.scsyndef" under the Synthdefs folder ("" if none)
std::string findSynthdefFolder(const std::string& name){
    static std::map<std::string, std::string> cache;
    auto it = cache.find(name);
    if(it != cache.end()) return it->second;
    std::string found;
    std::error_code ec;
    const fs::path root = ofToDataPath("Supercollider/Synthdefs", true);
    for(fs::recursive_directory_iterator e(root, fs::directory_options::skip_permission_denied, ec), end; !ec && e != end; e.increment(ec)){
        if(e->is_regular_file(ec) && e->path().filename().string() == name + "1.scsyndef"){
            found = e->path().parent_path().string();
            break;
        }
    }
    if(found.empty()) ofLogWarning("scAdditiveBox") << "FX: no " << name << "1.scsyndef under " << root;
    cache[name] = found;
    return found;
}
constexpr float twoPi = 6.28318530718f;
constexpr int displayStride = 2 * scAdditiveBox::DISPLAY_MAX;   // display bus channels per voice
constexpr float curveSpan = 128.0f;                             // filter 1 curve: -128..+128 semitones

// Measured on a 44.1 / 48 kHz scsynth: % CPU per partial, and per partial and
// output (PanAz). A rough guide: watch the server's own CPU.
constexpr float cpuPerPartial = 0.013f, cpuPerPartialOutput = 0.0005f;

uint32_t hash32(uint32_t x){
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

float midiToHz(float m){ return 440.0f * std::pow(2.0f, (m - 69.0f) / 12.0f); }
float hzToMidi(float f){ return 69.0f + 12.0f * std::log2(std::max(f, 1e-3f) / 440.0f); }
float dbToGain(float db){ return std::pow(10.0f, db / 20.0f); }

ImU32 voiceColor(int voice, float alpha = 1.0f, float value = 1.0f){
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(std::fmod(0.58f + voice * 0.137f, 1.0f), 0.62f, value, r, g, b);
    return ImGui::GetColorU32(ImVec4(r, g, b, alpha));
}

template<typename T>
T valueFor(const std::vector<T>& v, int i, T fallback){
    return v.empty() ? fallback : v[i % v.size()];
}

// Linear interpolation in a table of points evenly spread over 0..1
float tableAt(const std::vector<float>& t, float x){
    if(t.empty()) return 0;
    const float p = std::clamp(x, 0.0f, 1.0f) * (t.size() - 1);
    const int i = std::min(int(p), int(t.size()) - 2 < 0 ? 0 : int(t.size()) - 2);
    if(t.size() == 1) return t[0];
    const float f = p - i;
    return t[i] + (t[i + 1] - t[i]) * f;
}

// SC's Env curve (number) between a and b, t in 0..1
float envSegment(float a, float b, float t, float curve){
    if(std::abs(curve) < 1e-3f) return a + (b - a) * t;
    return a + (b - a) * (1.0f - std::exp(curve * t)) / (1.0f - std::exp(curve));
}

// ADSR level at time t (gate held for `hold` s after the attack + decay)
float adsr(float t, float a, float d, float s, float r, float hold, float curve){
    if(t < a) return envSegment(0, 1, a > 0 ? t / a : 1, curve);
    t -= a;
    if(t < d) return envSegment(1, s, d > 0 ? t / d : 1, curve);
    t -= d;
    if(t < hold) return s;
    t -= hold;
    if(t < r) return envSegment(s, 0, r > 0 ? t / r : 1, curve);
    return 0;
}

bool audioBusIsLive(ofxSCServer* server, ofxSCBus* bus){
    if(!server || !bus || bus->index < 0 || bus->index >= (int)server->audioBusses.size()) return false;
    return server->audioBusses[bus->index] == bus;
}

bool controlBusIsLive(ofxSCServer* server, ofxSCBus* bus){
    if(!server || !bus || bus->index < 0 || bus->index >= (int)server->controlBusses.size()) return false;
    return server->controlBusses[bus->index] == bus;
}

bool bufferIsLive(ofxSCServer* server, ofxSCBuffer* buffer){
    if(!server || !buffer || buffer->index < 0 || buffer->index >= (int)server->buffers.size()) return false;
    return server->buffers[buffer->index] == buffer;
}

// Text <-> value for the typed-value popup ("0.2" or "0.1, 0.5, 0.9")
std::string toText(float v){ return ofToString(v); }
std::string toText(int v){ return ofToString(v); }
std::string toText(bool v){ return v ? "1" : "0"; }
template<typename T>
std::string toText(const std::vector<T>& v){
    std::string s;
    for(size_t i = 0; i < v.size(); i++) s += (i ? ", " : "") + ofToString(v[i]);
    return s;
}
void fromText(const std::string& s, float& v){ v = ofToFloat(s); }
void fromText(const std::string& s, int& v){ v = ofToInt(s); }
void fromText(const std::string& s, bool& v){ v = ofToBool(s) || ofToInt(s) != 0; }
template<typename T>
void fromText(const std::string& s, std::vector<T>& v){
    v.clear();
    for(auto& item : ofSplitString(s, ",", true, true)){ T x; fromText(item, x); v.push_back(x); }
}
}

// ════════════════════════════════════════════════════════════════════════════
// Lifetime
// ════════════════════════════════════════════════════════════════════════════

scAdditiveBox::scAdditiveBox(std::vector<serverManager*> outputServers)
    : scNode("AdditiveBox"), servers(outputServers) {
    description = "Polyphonic additive synth: up to 256 partials per voice from two morphing spectra, each partial "
                  "with its own envelope, filter gain, sweep, mask, vibrato, noise, blur and place among the outputs "
                  "(Voices and Outputs are independent), unison / harmonizer, a modulation envelope and two LFOs, "
                  "EQ / echo / reverb. Show opens the spectrum and every control; right-click a control there to "
                  "publish it to the node.";
}

scAdditiveBox::~scAdditiveBox(){
    for(auto& [server, st] : states) freeSynths(server);
    for(auto& [server, st] : states) releaseBuses(server);
    states.clear();
}

void scAdditiveBox::setup(){
    // ── Node ─────────────────────────────────────────────────────────────────
    addParameter(showWindow.set("Show", false));
    addParameter(numVoices.set("Voices", 1, 1, MAX_VOICES));
    addParameter(numOutputs.set("Outputs", 2, 1, MAX_OUTPUTS));
    scNode::addOutput("Out");

    // ── Window (any of these can be published to the node) ──────────────────
    pitch.set("Pitch", {48.0f}, {0.0f}, {127.0f});
    gate.set("Gate", {1.0f}, {0.0f}, {1.0f});
    levels.set("Levels", {0.5f}, {0.0f}, {1.0f});
    glide.set("Glide", {0.0f}, {0.0f}, {5.0f});
    trig.set("Trig", {0}, {0}, {1});
    volume.set("Volume", 1.0f, 0.0f, 2.0f);
    transpose.set("Transpose", 0.0f, -48.0f, 48.0f);

    waveA.set("Wave A", {WaveFlat}, {0}, {NumWaves - 1});
    waveB.set("Wave B", {WaveSaw}, {0}, {NumWaves - 1});
    morph.set("Morph", {0.0f}, {0.0f}, {1.0f});
    {
        std::vector<float> a(TABLE_SIZE), b(TABLE_SIZE);
        for(int i = 0; i < TABLE_SIZE; i++){
            const int k = i + 1;
            a[i] = 1.0f / k;
            b[i] = (k % 2) ? 1.0f / k : 0.0f;
        }
        harmonicsA.set("Harmonics A", a, {0.0f}, {1.0f});
        harmonicsB.set("Harmonics B", b, {0.0f}, {1.0f});
    }
    harmBlur.set("Harm Blur", {0.0f}, {0.0f}, {1.0f});

    partials.set("Partials", 32, 1, MAX_PARTIALS);
    tilt.set("Tilt", {-4.5f}, {-24.0f}, {12.0f});
    stretch.set("Stretch", {1.0f}, {0.5f}, {2.0f});
    inharm.set("Inharm", {0.0f}, {0.0f}, {0.02f});
    evenOdd.set("Even Odd", {0.0f}, {-1.0f}, {1.0f});
    detune.set("Detune", {0.0f}, {0.0f}, {100.0f});
    shift.set("Shift", {0.0f}, {-1000.0f}, {1000.0f});

    filterMode.set("Filter", {FilterOff}, {0}, {NumFilterModes - 1});
    cutoff.set("Cutoff", {100.0f}, {0.0f}, {140.0f});
    keytrack.set("Keytrack", {0.0f}, {0.0f}, {1.0f});
    slope.set("Slope", {24.0f}, {0.0f}, {96.0f});
    resonance.set("Res", {0.0f}, {0.0f}, {1.0f});
    safeBass.set("Safe Bass", {0.0f}, {0.0f}, {1.0f});
    {
        // A soft low pass with a bump at the cutoff, to start drawing from
        std::vector<float> s(SHAPE_POINTS);
        for(int i = 0; i < SHAPE_POINTS; i++){
            const float st = -48.0f + 96.0f * i / (SHAPE_POINTS - 1);
            s[i] = st <= 0 ? 6.0f * std::exp(-(st / 6.0f) * (st / 6.0f)) : std::max(-48.0f, -st);
        }
        filterShape.set("Filter Shape", s, {-48.0f}, {12.0f});
        std::vector<float> e(SHAPE_POINTS, 0.0f);
        drawnEq.set("EQ Shape", e, {-48.0f}, {12.0f});
    }
    filter2Mode.set("Filter 2", {Filter2Off}, {0}, {NumFilter2Modes - 1});
    vowel.set("Vowel", {0.0f}, {0.0f}, {4.0f});
    formantShift.set("Formant Shift", {0.0f}, {-24.0f}, {24.0f});
    formantWidth.set("Formant Width", {2.0f}, {0.25f}, {8.0f});
    filter2Mix.set("Filter 2 Mix", {1.0f}, {0.0f}, {1.0f});

    sweepAmt.set("Sweep", {0.0f}, {0.0f}, {1.0f});
    sweepWaves.set("Sweep Waves", {2.0f}, {0.25f}, {32.0f});
    sweepRate.set("Sweep Rate", {0.2f}, {-10.0f}, {10.0f});
    sweepSharp.set("Sweep Sharp", {0.0f}, {0.0f}, {1.0f});
    sweepMode.set("Sweep Mode", {0}, {0}, {1});
    sweepPhase.set("Sweep Phase", {0.0f}, {0.0f}, {1.0f});
    sweepShape.set("Sweep Shape", {SweepLobes}, {0}, {NumSweepShapes - 1});
    sweepSkew.set("Sweep Skew", {0.0f}, {-1.0f}, {1.0f});
    sweepLow.set("Sweep Low", {0.0f}, {0.0f}, {1.0f});
    sweepHigh.set("Sweep High", {1.0f}, {0.0f}, {1.0f});
    {
        std::vector<float> shape(SWEEP_POINTS);
        for(int i = 0; i < SWEEP_POINTS; i++) shape[i] = 0.5f + 0.5f * std::cos(twoPi * (float(i) / SWEEP_POINTS - 0.5f));
        sweepDrawn.set("Sweep Drawn", shape, {0.0f}, {1.0f});
    }
    sweepInvert.set("Sweep Invert", false);
    sweepRetrig.set("Sweep Retrig", false);
    sweepVoiceOffset.set("Sweep Voice Offset", 0.0f, 0.0f, 1.0f);

    unison.set("Unison", 1, 1, MAX_UNISON);
    unisonDetune.set("Unison Detune", 15.0f, 0.0f, 100.0f);
    unisonSpread.set("Unison Spread", 0.5f, 0.0f, 1.0f);
    unisonInterval.set("Unison Interval", 0.0f, -24.0f, 24.0f);
    unisonBlend.set("Unison Blend", 1.0f, 0.0f, 1.0f);

    density.set("Density", {1.0f}, {0.0f}, {1.0f});
    randAmp.set("Rand Amp", {0.0f}, {0.0f}, {1.0f});
    maskGlide.set("Mask Glide", {0.05f}, {0.0f}, {5.0f});
    seed.set("Seed", 0, 0, 9999);
    reroll.set("Reroll", true);

    attack.set("Attack", {0.01f}, {0.0f}, {20.0f});
    decay.set("Decay", {0.5f}, {0.0f}, {20.0f});
    sustain.set("Sustain", {1.0f}, {0.0f}, {1.0f});
    release.set("Release", {2.0f}, {0.0f}, {60.0f});
    curve.set("Curve", {-4.0f}, {-8.0f}, {8.0f});
    envRand.set("Env Rand", {0.0f}, {0.0f}, {1.0f});
    envTilt.set("Env Tilt", {0.0f}, {-1.0f}, {1.0f});

    vibrato.set("Vibrato", {0.0f}, {0.0f}, {2.0f});
    vibRate.set("Vib Rate", {5.0f}, {0.0f}, {20.0f});
    partialVib.set("Partial Vib", {0.0f}, {0.0f}, {1.0f});
    partialVibRate.set("Partial Vib Rate", {1.0f}, {0.0f}, {20.0f});
    shimmer.set("Shimmer", {0.0f}, {0.0f}, {1.0f});
    shimmerRate.set("Shimmer Rate", {4.0f}, {0.0f}, {30.0f});
    noise.set("Noise", {0.0f}, {0.0f}, {1.0f});
    noiseRate.set("Noise Rate", {200.0f}, {1.0f}, {700.0f});
    noiseTilt.set("Noise Tilt", {0.0f}, {-1.0f}, {1.0f});
    timeBlur.set("Time Blur", {0.0f}, {0.0f}, {10.0f});

    pan.set("Pan", {0.5f}, {0.0f}, {1.0f});
    spread.set("Spread", {1.0f}, {0.0f}, {1.0f});
    panMode.set("Pan Mode", {0}, {0}, {NumPanModes - 1});
    drift.set("Drift", {0.0f}, {0.0f}, {0.5f});
    driftRate.set("Drift Rate", {0.1f}, {0.0f}, {5.0f});
    width.set("Width", {2.0f}, {1.0f}, {8.0f});

    modAttack.set("Env2 Attack", 0.01f, 0.0f, 20.0f);
    modDecay.set("Env2 Decay", 0.5f, 0.0f, 20.0f);
    modSustain.set("Env2 Sustain", 0.0f, 0.0f, 1.0f);
    modRelease.set("Env2 Release", 0.5f, 0.0f, 30.0f);
    modCurve.set("Env2 Curve", -4.0f, -8.0f, 8.0f);
    for(int l = 0; l < 2; l++){
        const std::string n = "LFO" + ofToString(l + 1);
        lfoShape[l].set(n + " Shape", 0, 0, int(lfoShapeNames.size()) - 1);
        lfoRate[l].set(n + " Rate", 1.0f, 0.01f, 40.0f);
        lfoRetrig[l].set(n + " Retrig", false);
    }
    for(int s = 0; s < MOD_SOURCES; s++)
        for(int d = 0; d < MOD_DESTS; d++)
            modAmount[s][d].set(std::string(modSourceNames[s]) + " " + modDestNames[d], 0.0f, -1.0f, 1.0f);

    fxOn[FX_EQ].set("EQ On", false);
    fxOn[FX_ECHO].set("Echo On", false);
    fxOn[FX_REVERB].set("Reverb On", false);
    {
        const float freqs[5] = {80.0f, 250.0f, 1000.0f, 4000.0f, 12000.0f};
        for(int b = 0; b < 5; b++){
            const bool shelf = scEQEditor::isShelf(b);
            eqGain[b].set(std::string("EQ ") + eqBandNames[b], 0.0f, -24.0f, 24.0f);
            eqFreq[b].set(std::string("EQ ") + eqBandNames[b] + " Freq", freqs[b], 20.0f, 20000.0f);
            eqShape[b].set(std::string("EQ ") + eqBandNames[b] + (shelf ? " Slope" : " Q"), 1.0f, 0.1f, shelf ? 4.0f : 20.0f);
        }
    }
    eqMix.set("EQ Mix", 1.0f, 0.0f, 1.0f);
    echoDelay.set("Echo Delay", 0.2f, 0.01f, 2.0f);
    echoFeed.set("Echo Feed", 0.5f, 0.0f, 1.0f);
    echoCutoff.set("Echo Cutoff", 60.0f, 12.0f, 120.0f);   // MIDI note, as the Echo SynthDef
    echoMix.set("Echo Mix", 0.35f, 0.0f, 1.0f);
    echoFilter.set("Echo Filter", 0, 0, 3);
    echoResonance.set("Echo Resonance", 0.2f, 0.0f, 1.0f);
    revSize.set("Rev Size", 30.0f, 0.0f, 60.0f);
    revDecay.set("Rev Decay", 4.0f, 0.05f, 60.0f);
    revPredelay.set("Rev Predelay", 20.0f, 0.0f, 1000.0f);   // ms
    revLowpass.set("Rev Lowpass", 10000.0f, 20.0f, 20000.0f);
    revMix.set("Rev Mix", 0.33f, 0.0f, 1.0f);
    revPosition.set("Rev Position", 0.8f, 0.0f, 1.0f);       // early -> late
    revSpread.set("Rev Spread", 0.15f, -1.0f, 1.0f);

    initializePublishActions();
    publishByKey(pitch.getEscapedName());
    publishByKey(gate.getEscapedName());
    publishByKey(levels.getEscapedName());

    rolls.assign(MAX_VOICES, 0);
    lastTrig.assign(MAX_VOICES, 0);
    gateOpen.assign(MAX_VOICES, false);
    for(int v = 0; v < MAX_VOICES; v++) gateOpen[v] = valueFor(gate.get(), v, 0.0f) > 0;

    // ── Listeners ────────────────────────────────────────────────────────────
    auto structure = [this](){
        if(loadingBankPreset) return;
        // New voices start with the gate they are given (no edge, no reroll)
        for(int v = 0; v < MAX_VOICES; v++) gateOpen[v] = valueFor(gate.get(), v, 0.0f) > 0;
        for(auto& [server, st] : states) syncSlots(server);
        markDirty();
    };
    nodeListeners.push(numVoices.newListener([structure](int&){ structure(); }));
    nodeListeners.push(unison.newListener([structure](int&){ structure(); }));
    nodeListeners.push(numOutputs.newListener([this](int& o){
        if(loadingBankPreset) return;
        if(o != builtOutputs) rebuildDefs();
    }));
    nodeListeners.push(partials.newListener([this](int&){
        if(!loadingBankPreset && partialCap() != builtCap) rebuildDefs();
        markDirty();
    }));
    nodeListeners.push(volume.newListener([this](float& v){
        for(auto& [server, st] : states) if(st.out) st.out->set("volume", v);
    }));
    nodeListeners.push(gate.newListener([this](std::vector<float>& v){ onGate(v); }));
    nodeListeners.push(trig.newListener([this](std::vector<int>& v){ onTrig(v); }));
    nodeListeners.push(seed.newListener([this](int&){ markDirty(); }));
    nodeListeners.push(reroll.newListener([this](bool&){ markDirty(); }));
    for(auto* p : {&sweepInvert, &sweepRetrig})
        nodeListeners.push(p->newListener([this](bool&){ markDirty(); }));
    for(auto* p : {&transpose, &sweepVoiceOffset, &unisonDetune, &unisonSpread, &unisonInterval, &unisonBlend,
                   &modAttack, &modDecay, &modSustain, &modRelease, &modCurve, &lfoRate[0], &lfoRate[1]})
        nodeListeners.push(p->newListener([this](float&){ markDirty(); }));
    for(auto& p : lfoShape) nodeListeners.push(p.newListener([this](int&){ markDirty(); }));
    for(auto& p : lfoRetrig) nodeListeners.push(p.newListener([this](bool&){ markDirty(); }));
    for(int s = 0; s < MOD_SOURCES; s++)
        for(int d = 0; d < MOD_DESTS; d++)
            nodeListeners.push(modAmount[s][d].newListener([this](float&){ markDirty(); }));
    for(auto* p : {&pitch, &levels, &glide, &morph, &harmonicsA, &harmonicsB, &harmBlur,
                   &tilt, &stretch, &inharm, &evenOdd, &detune, &shift,
                   &cutoff, &keytrack, &slope, &resonance, &safeBass, &filterShape,
                   &vowel, &formantShift, &formantWidth, &filter2Mix, &drawnEq,
                   &sweepAmt, &sweepWaves, &sweepRate, &sweepSharp, &sweepPhase,
                   &sweepSkew, &sweepLow, &sweepHigh, &sweepDrawn,
                   &density, &randAmp, &maskGlide,
                   &attack, &decay, &sustain, &release, &curve, &envRand, &envTilt,
                   &vibrato, &vibRate, &partialVib, &partialVibRate, &shimmer, &shimmerRate,
                   &noise, &noiseRate, &noiseTilt, &timeBlur,
                   &pan, &spread, &drift, &driftRate, &width}){
        nodeListeners.push(p->newListener([this](std::vector<float>&){ markDirty(); }));
    }
    for(auto* p : {&waveA, &waveB, &filterMode, &filter2Mode, &sweepMode, &sweepShape, &panMode}){
        nodeListeners.push(p->newListener([this](std::vector<int>&){ markDirty(); }));
    }

    for(int st = 0; st < FX_COUNT; st++)
        nodeListeners.push(fxOn[st].newListener([this](bool&){ rebuildFxAll(); }));
    for(int b = 0; b < 5; b++)
        for(auto* p : {&eqGain[b], &eqFreq[b], &eqShape[b]})
            nodeListeners.push(p->newListener([this](float&){ eqCurveDirty = true; sendFxParamsAll(FX_EQ); }));
    nodeListeners.push(eqMix.newListener([this](float&){ sendFxParamsAll(FX_EQ); }));
    for(auto* p : {&echoDelay, &echoFeed, &echoCutoff, &echoMix, &echoResonance})
        nodeListeners.push(p->newListener([this](float&){ sendFxParamsAll(FX_ECHO); }));
    nodeListeners.push(echoFilter.newListener([this](int&){ sendFxParamsAll(FX_ECHO); }));
    for(auto* p : {&revSize, &revDecay, &revPredelay, &revLowpass, &revMix, &revPosition, &revSpread})
        nodeListeners.push(p->newListener([this](float&){ sendFxParamsAll(FX_REVERB); }));
    requireFxSynthdefs();

    builtCap = partialCap();
    builtOutputs = numOutputsClamped();
}

// ════════════════════════════════════════════════════════════════════════════
// Frame
// ════════════════════════════════════════════════════════════════════════════

void scAdditiveBox::update(ofEventArgs&){
    const float now = ofGetElapsedTimef();

    // An FX stage switched on before the server had loaded its SynthDefs
    if(fxPending){
        bool ready = true;
        for(auto& [server, st] : states) if(st.out && !fxStageReady(server)) ready = false;
        if(ready) rebuildFxAll();
    }
    // Filter curve buffers allocated: write them, then every voice reads its own
    for(auto& [server, st] : states){
        if(!st.curvesPending) continue;
        if(server->isNRTSyncPending() && now - st.curvesAllocTime < 5.0f) continue;
        st.curvesPending = false;
        if(curveData.size() < size_t(numVoicesClamped())) computeAll();
        uploadCurves(server, true);
        dirty = true;   // the fbuf of every voice
    }
    if(dirty){
        dirty = false;
        computeAll();
        const int V = numVoicesClamped(), U = unisonClamped();
        std::vector<VoiceData> slotData(V * U);
        for(int v = 0; v < V; v++)
            for(int u = 0; u < U; u++) slotData[v * U + u] = u == 0 ? voiceData[v] : computeVoice(v, u);
        for(auto& [server, st] : states){
            uploadCurves(server, false);
            for(int s = 0; s < (int)st.slots.size() && s < V * U; s++) sendSlot(server, s, slotData[s], false);
        }
    }

    // Display bus of the first server, only while the window is open
    liveValid = false;
    if(showWindow){
        for(auto& [server, st] : states){
            if(!st.vis || !controlBusIsLive(server, st.vis) || st.slots.empty()) continue;
            liveData = st.vis->readValues;
            liveValid = liveData.size() >= size_t(numVoicesClamped() * displayStride);
            st.vis->requestValues();
            break;
        }
    }
}

void scAdditiveBox::draw(ofEventArgs&){
    if(showWindow) drawWindow();
}

// ════════════════════════════════════════════════════════════════════════════
// Partials
// ════════════════════════════════════════════════════════════════════════════

int scAdditiveBox::numVoicesClamped() const { return std::clamp(numVoices.get(), 1, MAX_VOICES); }
int scAdditiveBox::numOutputsClamped() const { return std::clamp(numOutputs.get(), 1, MAX_OUTPUTS); }
int scAdditiveBox::unisonClamped() const { return std::clamp(unison.get(), 1, MAX_UNISON); }

int scAdditiveBox::partialCap() const {
    const int n = std::clamp(partials.get(), 1, MAX_PARTIALS);
    for(int cap : CAPS) if(cap >= n) return cap;
    return MAX_PARTIALS;
}

float scAdditiveBox::sampleRate() const {
    float sr = 48000.0f;
    for(auto* sm : servers) if(sm && sm->getServer()){ sr = float(serverManager::getSampleRateForServer(sm->getServer())); break; }
    return sr < 8000.0f ? 48000.0f : sr;
}

// Uniform 0..1, the same for the same seed, voice, draw generation, salt and
// partial (so a preset always sounds the same until a gate rerolls it)
float scAdditiveBox::random01(int voice, int salt, int partial) const {
    const int roll = voice < (int)rolls.size() ? rolls[voice] : 0;
    uint32_t h = hash32(uint32_t(seed.get()) * 0x9E3779B1U ^ hash32(uint32_t(voice) * 0x85EBCA77U + uint32_t(roll) * 0xC2B2AE3DU));
    h = hash32(h ^ (uint32_t(salt) * 0x27D4EB2FU) ^ hash32(uint32_t(partial) + 0x165667B1U));
    return float(h >> 8) * (1.0f / 16777216.0f);
}

// Level of harmonic k (1-based) in a waveform, before tilt
float scAdditiveBox::waveLevel(int voice, int wave, const std::vector<float>& table, int k) const {
    const bool odd = k % 2;
    switch(std::clamp(wave, 0, NumWaves - 1)){
        case WaveFlat:     return 1.0f;
        case WaveSaw:      return 1.0f / k;
        case WaveSquare:   return odd ? 1.0f / k : 0.0f;
        case WaveTriangle: return odd ? 1.0f / (float(k) * k) : 0.0f;
        case WaveSine:     return k == 1 ? 1.0f : 0.0f;
        case WavePulse:    return std::abs(std::sin(float(M_PI) * k * 0.25f)) / k * 1.4142f;   // 25% duty
        case WaveOrgan: {
            // drawbars 16' 8' 5 1/3' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1': harmonics 1 2 3 4 6 8 10 12 16
            static const std::map<int, float> bars = {{1, 1.0f}, {2, 0.8f}, {3, 0.7f}, {4, 0.6f}, {6, 0.5f},
                                                      {8, 0.45f}, {10, 0.3f}, {12, 0.25f}, {16, 0.2f}};
            auto it = bars.find(k);
            return it == bars.end() ? 0.0f : it->second;
        }
        case WaveHollow:   return k == 1 ? 1.0f : (odd ? 0.12f / k : 1.0f / k);
        case WaveRandom:   return random01(voice, 11, k) / std::sqrt(float(k));
        case WaveDrawn: {
            if(table.empty()) return 0.0f;
            if(k <= (int)table.size()) return std::max(0.0f, table[k - 1]);
            return std::max(0.0f, table.back()) * float(table.size()) / k;
        }
        default:           return 1.0f;
    }
}

// Filter 1 at `d` semitones from the cutoff (linear gain)
float scAdditiveBox::filter1Gain(int voice, float d) const {
    const int mode = std::clamp(valueFor(filterMode.get(), voice, 0), 0, NumFilterModes - 1);
    if(mode == FilterOff) return 1.0f;
    const float o = d / 12.0f;                                   // octaves from the cutoff
    const float s = valueFor(slope.get(), voice, 24.0f);
    const float res = valueFor(resonance.get(), voice, 0.0f);
    const float bump = 1.0f + res * 4.0f * std::exp(-(o * 6.0f) * (o * 6.0f));
    switch(mode){
        case FilterLP:    return dbToGain(-s * std::max(o, 0.0f)) * bump;
        case FilterBP:    return dbToGain(-s * std::abs(o)) * bump;
        case FilterHP:    return dbToGain(-s * std::max(-o, 0.0f)) * bump;
        case FilterNotch: return 1.0f - std::exp(-(o * s / 6.0f) * (o * s / 6.0f));
        case FilterComb: {
            // Teeth every 96 / Slope semitones, one on the cutoff; Res = depth
            const float spacing = 96.0f / std::max(1.0f, s);
            const float depth = 0.3f + 0.7f * res;
            return 1.0f - depth * (0.5f - 0.5f * std::cos(twoPi * d / spacing));
        }
        case FilterDrawn: {
            const auto& shape = filterShape.get();
            return dbToGain(tableAt(shape, (d + 48.0f) / 96.0f));
        }
        default: return 1.0f;
    }
}

// Filter 2 (formants or a drawn EQ) at an absolute frequency (linear gain)
float scAdditiveBox::filter2Gain(int voice, float hz) const {
    const int mode = std::clamp(valueFor(filter2Mode.get(), voice, 0), 0, NumFilter2Modes - 1);
    if(mode == Filter2Off) return 1.0f;
    const float mix = valueFor(filter2Mix.get(), voice, 1.0f);
    float g = 1.0f;
    if(mode == Filter2Vowel){
        const float vp = std::clamp(valueFor(vowel.get(), voice, 0.0f), 0.0f, 4.0f);
        const int a = std::min(int(vp), 3);
        const float t = vp - a;
        const float shiftRatio = std::pow(2.0f, valueFor(formantShift.get(), voice, 0.0f) / 12.0f);
        const float w = valueFor(formantWidth.get(), voice, 2.0f);
        g = 0.01f;   // -40 dB between the formants
        for(int f = 0; f < 3; f++){
            const float freq = (vowels[a].f[f] + (vowels[a + 1].f[f] - vowels[a].f[f]) * t) * shiftRatio;
            const float db = vowels[a].db[f] + (vowels[a + 1].db[f] - vowels[a].db[f]) * t;
            const float bw = (vowels[a].bw[f] + (vowels[a + 1].bw[f] - vowels[a].bw[f]) * t) * w * shiftRatio;
            const float x = (hz - freq) / std::max(1.0f, bw);
            g += dbToGain(db) * std::exp(-0.5f * x * x);
        }
        g = std::min(g, 1.5f);
    } else {
        const float x = std::log(std::max(hz, 20.0f) / 20.0f) / std::log(1000.0f);
        g = dbToGain(tableAt(drawnEq.get(), x));
    }
    return 1.0f - mix + mix * g;
}

std::vector<float> scAdditiveBox::filter1Curve(int voice) const {
    std::vector<float> c(CURVE_SIZE);
    for(int i = 0; i < CURVE_SIZE; i++){
        const float d = -curveSpan + 2.0f * curveSpan * i / (CURVE_SIZE - 1);
        c[i] = filter1Gain(voice, d);
    }
    return c;
}

float scAdditiveBox::voiceCutoff(int voice) const {
    const float p = valueFor(pitch.get(), voice, 48.0f) + transpose.get();
    return valueFor(cutoff.get(), voice, 100.0f) + valueFor(keytrack.get(), voice, 0.0f) * (p - 60.0f);
}

// Copy u of voice v: the arrays and scalars of its synth. Copies 1.. of a
// unison are detuned around copy 0 (alternately up / down), transposed by u x
// Unison Interval, spread around the voice's position, at Unison Blend.
scAdditiveBox::VoiceData scAdditiveBox::computeVoice(int v, int u) const {
    VoiceData d;
    const int P = partialCap();
    const int count = std::clamp(partials.get(), 1, P);
    const int U = unisonClamped();
    float side = 0;   // -1..1: where copy u sits in the unison
    if(u > 0 && U > 1){
        const float steps = std::ceil((U - 1) / 2.0f);
        side = ((u % 2) ? 1.0f : -1.0f) * std::ceil(u / 2.0f) / steps;
    }
    const float p = valueFor(pitch.get(), v, 48.0f) + transpose.get()
                  + side * unisonDetune.get() / 200.0f + u * unisonInterval.get();
    const float f0 = midiToHz(p);
    const float st = valueFor(stretch.get(), v, 1.0f);
    const float B = valueFor(inharm.get(), v, 0.0f);
    const float det = valueFor(detune.get(), v, 0.0f);
    const float sh = valueFor(shift.get(), v, 0.0f);
    const float tl = valueFor(tilt.get(), v, -4.5f);
    const float eo = valueFor(evenOdd.get(), v, 0.0f);
    const int wA = valueFor(waveA.get(), v, 0), wB = valueFor(waveB.get(), v, 1);
    const float mo = std::clamp(valueFor(morph.get(), v, 0.0f), 0.0f, 1.0f);
    const float hb = valueFor(harmBlur.get(), v, 0.0f);
    const float dens = valueFor(density.get(), v, 1.0f);
    const float rAmp = valueFor(randAmp.get(), v, 0.0f);
    const float atk = valueFor(attack.get(), v, 0.01f), dec = valueFor(decay.get(), v, 0.5f), rel = valueFor(release.get(), v, 2.0f);
    const float eRand = valueFor(envRand.get(), v, 0.0f), eTilt = valueFor(envTilt.get(), v, 0.0f);
    const float vRate = valueFor(vibRate.get(), v, 5.0f), pVib = valueFor(partialVib.get(), v, 0.0f);
    const float pvRate = valueFor(partialVibRate.get(), v, 1.0f);
    const float shRate = valueFor(shimmerRate.get(), v, 4.0f);
    const float nAmt = valueFor(noise.get(), v, 0.0f), nRate = valueFor(noiseRate.get(), v, 200.0f), nTilt = valueFor(noiseTilt.get(), v, 0.0f);
    const float tBlur = valueFor(timeBlur.get(), v, 0.0f);
    const float waves = valueFor(sweepWaves.get(), v, 2.0f);
    const int swMode = valueFor(sweepMode.get(), v, 0);
    const float pn = valueFor(pan.get(), v, 0.5f) + side * unisonSpread.get() * 0.5f;
    const float spr = valueFor(spread.get(), v, 1.0f);
    const int pMode = std::clamp(valueFor(panMode.get(), v, 0), 0, NumPanModes - 1);
    const float dRate = valueFor(driftRate.get(), v, 0.1f);
    const float sr = sampleRate();
    const auto& tableA = harmonicsA.get();
    const auto& tableB = harmonicsB.get();

    std::vector<float> fr(P), po(P), pa(P, 0.0f), pbs(P, 0.0f), ta(P), td(P), tr(P), vf(P), vd(P),
                       nr(P), nd(P), shr(P), sph(P), sw(P), bl(P), pos(P), drr(P);
    d.ratio.resize(P);
    std::vector<float> baseA(P, 0.0f), baseB(P, 0.0f);
    double sumA = 0, sumB = 0;
    for(int i = 0; i < P; i++){
        const int k = i + 1;
        const float ratio = std::pow(float(k), st) * std::sqrt(1.0f + B * k * k);
        d.ratio[i] = ratio;
        fr[i] = ratio * std::pow(2.0f, (random01(v, 0, k) * 2 - 1) * det / 1200.0f);
        po[i] = 12.0f * std::log2(fr[i]);
        if(k <= count){
            const float t = dbToGain(std::log2(ratio) * tl);
            baseA[i] = waveLevel(v, wA, tableA, k) * t;
            baseB[i] = waveLevel(v, wB, tableB, k) * t;
            sumA += double(baseA[i]) * baseA[i];
            sumB += double(baseB[i]) * baseB[i];
        }
    }
    // Same loudness for any partial count, waveform or tilt; filters, masks
    // and the envelopes then change the level as they would on a real spectrum
    const float normA = 0.25f / float(std::sqrt(std::max(sumA, 1e-8)));
    const float normB = 0.25f / float(std::sqrt(std::max(sumB, 1e-8)));
    const float logCount = std::max(1.0f, std::log2(float(count)));
    float peak = 1e-9f;
    for(int i = 0; i < P; i++){
        const int k = i + 1;
        const float f = f0 * fr[i] + sh;
        const float af = std::abs(f);
        const float eoGain = (k % 2) ? (1.0f - std::max(eo, 0.0f)) : (1.0f + std::min(eo, 0.0f));
        const float mask = random01(v, 4, k) < dens ? 1.0f : 0.0f;
        const float ra = 1.0f - rAmp * random01(v, 5, k);
        const float nyq = std::clamp((0.47f * sr - af) / (0.03f * sr), 0.0f, 1.0f);
        const float common = eoGain * filter2Gain(v, std::max(af, 1.0f)) * mask * ra * nyq;
        pa[i] = baseA[i] * normA * common;
        pbs[i] = baseB[i] * normB * common;
        peak = std::max(peak, std::max(baseA[i] * normA, baseB[i] * normB));

        const float tf = (1.0f - eRand * random01(v, 7, k)) * std::pow(d.ratio[i], -eTilt);
        ta[i] = atk * (1.0f - eRand * random01(v, 8, k));
        td[i] = dec * tf;
        tr[i] = rel * tf;
        vf[i] = pvRate * (0.5f + random01(v, 1, k));   // 0.5 .. 1.5 x Partial Vib Rate
        vd[i] = std::pow(2.0f, pVib * random01(v, 3, k) / 12.0f) - 1.0f;
        shr[i] = shRate * (0.5f + random01(v, 6, k));
        // Noise: up to +-6% frequency jitter; Noise Tilt leans it to the low (-) or high (+) partials
        const float x = count > 1 ? std::min(1.0f, float(i) / (count - 1)) : 0.0f;
        nr[i] = nRate * (0.5f + random01(v, 12, k));
        nd[i] = nAmt * 0.06f * std::max(0.0f, 1.0f + nTilt * (2.0f * x - 1.0f));
        bl[i] = tBlur * (0.5f + random01(v, 13, k));

        // Sweep: each partial's place in the shape's cycle (table points;
        // phase 0: the shape's middle on the fundamental) and depth share
        const float sx = swMode == 1 ? std::log2(d.ratio[i]) / logCount : float(i) / count;
        const float cyc = -sx * waves;
        sph[i] = (cyc - std::floor(cyc)) * (SWEEP_SIZE - 1);
        sw[i] = sweepRangeWeight(v, sx);

        float order = 0;
        switch(pMode){
            case PanRandom:    order = random01(v, 9, k) * 2 - 1; break;
            case PanSpectrum:  order = (count > 1 ? std::min(1.0f, float(i) / (count - 1)) : 0.5f) * 2 - 1; break;
            case PanAlternate: order = (k % 2) ? -1.0f : 1.0f; break;
            default: break;
        }
        pos[i] = pn + spr * 0.5f * order;
        drr[i] = dRate * (0.5f + random01(v, 10, k));
    }
    // Harmonic blur: each partial's level spread over its neighbours
    if(hb > 0.001f){
        const float sigma = 0.3f + hb * 5.0f;
        const int radius = int(std::ceil(sigma * 2.5f));
        std::vector<float> w(radius + 1);
        for(int j = 0; j <= radius; j++) w[j] = std::exp(-0.5f * (j / sigma) * (j / sigma));
        auto blur = [&](std::vector<float>& a){
            std::vector<float> out(P, 0.0f);
            for(int i = 0; i < count; i++){
                float s = 0, ws = 0;
                for(int j = -radius; j <= radius; j++){
                    const int n = i + j;
                    if(n < 0 || n >= count) continue;
                    s += a[n] * w[std::abs(j)];
                    ws += w[std::abs(j)];
                }
                out[i] = ws > 0 ? s / ws : 0;
            }
            a = std::move(out);
        };
        blur(pa);
        blur(pbs);
    }
    std::vector<float> pd(P), view(P);
    const float cut = voiceCutoff(v);
    for(int i = 0; i < P; i++){
        pd[i] = pbs[i] - pa[i];
        view[i] = (pa[i] + pd[i] * mo) * filter1Gain(v, po[i] + p - cut);
    }

    std::vector<float> mm(MOD_SOURCES * MOD_DESTS);
    for(int s = 0; s < MOD_SOURCES; s++)
        for(int dd = 0; dd < MOD_DESTS; dd++) mm[s * MOD_DESTS + dd] = modAmount[s][dd].get();

    const float blend = unisonBlend.get();
    const float uGain = (u == 0 ? 1.0f : blend) / std::sqrt(1.0f + blend * blend * (U - 1));

    d.pitch = p;
    d.peak = peak;
    d.view = std::move(view);
    d.arrays = {{"fr", fr}, {"po", po}, {"pa", pa}, {"pd", pd}, {"ta", ta}, {"td", td}, {"tr", tr},
                {"vf", vf}, {"vd", vd}, {"nr", nr}, {"nd", nd}, {"shr", shr}, {"sph", sph}, {"sw", sw}, {"bl", bl},
                {"pb", pos}, {"drr", drr}, {"mm", mm}};
    d.scalars = {
        {"pitch", p},
        {"glide", valueFor(glide.get(), v, 0.0f)},
        {"amp", valueFor(levels.get(), v, 0.5f) * uGain},
        {"shift", sh},
        {"maskglide", valueFor(maskGlide.get(), v, 0.05f)},
        {"morph", mo},
        {"sus", valueFor(sustain.get(), v, 1.0f)},
        {"curve", valueFor(curve.get(), v, -4.0f)},
        {"vibdepth", valueFor(vibrato.get(), v, 0.0f)},
        {"vibrate", vRate},
        {"shimmer", valueFor(shimmer.get(), v, 0.0f)},
        {"swamt", valueFor(sweepAmt.get(), v, 0.0f)},
        {"swrate", valueFor(sweepRate.get(), v, 0.2f)},
        {"swphase", valueFor(sweepPhase.get(), v, 0.0f) + v * sweepVoiceOffset.get()},
        {"swretrig", sweepRetrig ? 1.0f : 0.0f},
        {"drift", valueFor(drift.get(), v, 0.0f)},
        {"width", valueFor(width.get(), v, 2.0f)},
        {"cutoff", valueFor(cutoff.get(), v, 100.0f)},
        {"keytrack", valueFor(keytrack.get(), v, 0.0f)},
        {"safe", valueFor(safeBass.get(), v, 0.0f)},
        {"matk", modAttack.get()}, {"mdec", modDecay.get()}, {"msus", modSustain.get()},
        {"mrel", modRelease.get()}, {"mcurve", modCurve.get()},
        {"l1shape", float(lfoShape[0].get())}, {"l1rate", lfoRate[0].get()}, {"l1retrig", lfoRetrig[0] ? 1.0f : 0.0f},
        {"l2shape", float(lfoShape[1].get())}, {"l2rate", lfoRate[1].get()}, {"l2retrig", lfoRetrig[1] ? 1.0f : 0.0f},
    };
    return d;
}

// The sweep's shape over one cycle, x = 0..1. Computed on u = x + 0.5, so the
// shape's middle (the peak of Lobes, Triangle, Pulse, Gauss) sits at x = 0:
// with Lobes and Skew 0 this is the raised-cosine lobe the sweep always had.
float scAdditiveBox::sweepShapeAt(int v, float x) const {
    float u = x + 0.5f;
    u -= std::floor(u);
    // Skew: the middle moves to k (0.05 .. 0.95), each half stretched to fit
    const float k = 0.5f + std::clamp(valueFor(sweepSkew.get(), v, 0.0f), -1.0f, 1.0f) * 0.45f;
    u = u < k ? 0.5f * u / k : 0.5f + 0.5f * (u - k) / (1.0f - k);
    const float sharp = std::clamp(valueFor(sweepSharp.get(), v, 0.0f), 0.0f, 1.0f);
    const float tri = 1.0f - std::abs(2.0f * u - 1.0f);
    float y = 0;
    switch(std::clamp(valueFor(sweepShape.get(), v, 0), 0, NumSweepShapes - 1)){
        case SweepLobes: {
            // The part of a cosine above `floor`, scaled to 0..1 (Sharp 0: a
            // plain raised cosine, 1: narrow peaks)
            const float floor = std::min(sharp, 0.95f) * 2.0f - 1.0f;
            y = std::max(0.0f, (std::cos(twoPi * (u - 0.5f)) - floor) / (1.0f - floor));
            break;
        }
        case SweepTriangle: y = std::pow(tri, 1.0f + sharp * 7.0f); break;
        case SweepSawUp:    y = std::pow(u, 1.0f + sharp * 7.0f); break;
        case SweepSawDown:  y = std::pow(1.0f - u, 1.0f + sharp * 7.0f); break;
        case SweepPulse: {
            // Sharp narrows the pulse; soft edges, so no clicks
            const float half = (1.0f - sharp * 0.95f) * 0.5f, edge = 0.02f;
            const float dist = std::abs(u - 0.5f);
            y = std::clamp((half + edge - dist) / (2.0f * edge), 0.0f, 1.0f);
            break;
        }
        case SweepGauss: {
            const float sigma = 0.02f + 0.25f * (1.0f - sharp);
            y = std::exp(-0.5f * ((u - 0.5f) / sigma) * ((u - 0.5f) / sigma));
            break;
        }
        case SweepSteps: {
            const int n = 2 + int(std::round(sharp * 14.0f));
            y = std::min(1.0f, std::floor(tri * n) / (n - 1));
            break;
        }
        case SweepDrawn: {
            // The drawn points around one closed cycle
            const auto& pts = sweepDrawn.get();
            if(pts.empty()) break;
            const int n = int(pts.size());
            const float f = u * n;
            const int a = int(f) % n, b = (a + 1) % n;
            y = pts[a] + (pts[b] - pts[a]) * (f - std::floor(f));
            break;
        }
        default: break;
    }
    y = std::clamp(y, 0.0f, 1.0f);
    return sweepInvert ? 1.0f - y : y;
}

std::vector<float> scAdditiveBox::sweepTable(int v) const {
    std::vector<float> t(SWEEP_SIZE);
    for(int i = 0; i < SWEEP_SIZE - 1; i++) t[i] = sweepShapeAt(v, float(i) / (SWEEP_SIZE - 1));
    t[SWEEP_SIZE - 1] = t[0];   // the reading interpolates past the last point
    return t;
}

// Share of the sweep depth at x (0..1 along the partials / octaves): 1 inside
// Low..High, fading to 0 outside over a short edge (0 and 1 are open ends)
float scAdditiveBox::sweepRangeWeight(int v, float x) const {
    const float lo = std::clamp(valueFor(sweepLow.get(), v, 0.0f), 0.0f, 1.0f);
    const float hi = std::clamp(valueFor(sweepHigh.get(), v, 1.0f), 0.0f, 1.0f);
    constexpr float edge = 0.06f;
    auto smooth = [](float a, float b, float t){
        const float k = std::clamp((t - a) / (b - a), 0.0f, 1.0f);
        return k * k * (3.0f - 2.0f * k);
    };
    const float in = lo <= 0.0f ? 1.0f : smooth(lo - edge, lo + edge, x);
    const float out = hi >= 1.0f ? 1.0f : 1.0f - smooth(hi - edge, hi + edge, x);
    return in * out;
}

void scAdditiveBox::computeAll(){
    const int V = numVoicesClamped();
    voiceData.resize(V);
    curveData.resize(V);
    sweepData.resize(V);
    for(int v = 0; v < V; v++){
        voiceData[v] = computeVoice(v, 0);
        curveData[v] = filter1Curve(v);
        sweepData[v] = sweepTable(v);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Synths
// ════════════════════════════════════════════════════════════════════════════

std::string scAdditiveBox::voiceDefName() const {
    return "AdditiveBoxV" + ofToString(partialCap()) + "_" + ofToString(numOutputsClamped());
}

std::string scAdditiveBox::outDefName() const {
    return "AdditiveBoxOut" + ofToString(numOutputsClamped());
}

// Buses are allocated client-side only (no server message): kept across graph
// recomputes, replaced when stale (server reboot) or when the voice count
// changes the display bus size. The filter curve buffers are allocated once
// per server (one per possible voice) and written when /b_alloc is done.
void scAdditiveBox::ensureBuses(ofxSCServer* server){
    auto& st = states[server];
    if(st.mix && !audioBusIsLive(server, st.mix)){ delete st.mix; st.mix = nullptr; }
    if(!st.mix) st.mix = new ofxSCBus(RATE_AUDIO, MAX_OUTPUTS, server);
    const int visChannels = numVoicesClamped() * displayStride;
    if(st.vis && !controlBusIsLive(server, st.vis)){ delete st.vis; st.vis = nullptr; }
    if(st.vis && st.vis->channels != visChannels){ st.vis->free(); delete st.vis; st.vis = nullptr; }
    if(!st.vis) st.vis = new ofxSCBus(RATE_CONTROL, visChannels, server);
    if(st.visJunk && !controlBusIsLive(server, st.visJunk)){ delete st.visJunk; st.visJunk = nullptr; }
    if(!st.visJunk) st.visJunk = new ofxSCBus(RATE_CONTROL, displayStride, server);
    bool allocated = false;
    for(int v = 0; v < MAX_VOICES; v++){
        for(auto* slot : {&st.curves[v], &st.sweeps[v]}){
            auto*& buffer = *slot;
            if(buffer && bufferIsLive(server, buffer)) continue;
            delete buffer;
            buffer = new ofxSCBuffer(slot == &st.curves[v] ? CURVE_SIZE : SWEEP_SIZE, 1, server);
            buffer->alloc();
            allocated = true;
        }
    }
    if(allocated){
        for(auto& c : st.sentCurves) c.clear();
        for(auto& c : st.sentSweeps) c.clear();
        st.curvesPending = true;
        st.curvesAllocTime = ofGetElapsedTimef();
        server->requestNRTSync();
    }
}

void scAdditiveBox::releaseBuses(ofxSCServer* server){
    auto it = states.find(server);
    if(it == states.end()) return;
    auto& st = it->second;
    if(st.mix){ if(audioBusIsLive(server, st.mix)) st.mix->free(); delete st.mix; st.mix = nullptr; }
    if(st.vis){ if(controlBusIsLive(server, st.vis)) st.vis->free(); delete st.vis; st.vis = nullptr; }
    if(st.visJunk){ if(controlBusIsLive(server, st.visJunk)) st.visJunk->free(); delete st.visJunk; st.visJunk = nullptr; }
    for(auto*& b : st.curves){ if(b){ if(bufferIsLive(server, b)) b->free(); delete b; b = nullptr; } }
    for(auto*& b : st.sweeps){ if(b){ if(bufferIsLive(server, b)) b->free(); delete b; b = nullptr; } }
    for(auto* b : st.fxBuses) if(b){ if(audioBusIsLive(server, b)) b->free(); delete b; }
    st.fxBuses.clear();
}

// Each voice's filter 1 curve and sweep shape, written with /b_setn when they changed
void scAdditiveBox::uploadCurves(ofxSCServer* server, bool force){
    auto& st = states[server];
    if(st.curvesPending) return;
    for(int v = 0; v < (int)curveData.size() && v < MAX_VOICES; v++){
        if(!st.curves[v] || !bufferIsLive(server, st.curves[v])) continue;
        if(!force && st.sentCurves[v] == curveData[v]) continue;
        st.sentCurves[v] = curveData[v];
        ofxOscMessage m;
        m.setAddress("/b_setn");
        m.addIntArg(st.curves[v]->index);
        m.addIntArg(0);
        m.addIntArg(CURVE_SIZE);
        for(float x : curveData[v]) m.addFloatArg(x);
        server->sendMsg(m);
    }
    for(int v = 0; v < (int)sweepData.size() && v < MAX_VOICES; v++){
        if(!st.sweeps[v] || !bufferIsLive(server, st.sweeps[v])) continue;
        if(!force && st.sentSweeps[v] == sweepData[v]) continue;
        st.sentSweeps[v] = sweepData[v];
        ofxOscMessage m;
        m.setAddress("/b_setn");
        m.addIntArg(st.sweeps[v]->index);
        m.addIntArg(0);
        m.addIntArg(SWEEP_SIZE);
        for(float x : sweepData[v]) m.addFloatArg(x);
        server->sendMsg(m);
    }
}

ofxSCSynth* scAdditiveBox::newSlotSynth(ofxSCServer* server){
    auto& st = states[server];
    auto* synth = new ofxSCSynth(voiceDefName(), server);
    synth->set("out", st.mix->index);
    return synth;
}

// New voices go before the FX (they must write the mix bus before the
// first stage reads it) — or before the output synth
ofxSCSynth* scAdditiveBox::chainHead(ServerState& st) const {
    for(int stage : st.fxPlan) if(st.fx[stage]) return st.fx[stage];
    return st.out;
}

void scAdditiveBox::sendSlot(ofxSCServer* server, int s, const VoiceData& data, bool force){
    auto& st = states[server];
    if(s >= (int)st.slots.size() || !st.slots[s]) return;
    if(st.sentArrays.size() < st.slots.size()){
        st.sentArrays.resize(st.slots.size());
        st.sentScalars.resize(st.slots.size());
    }
    auto* synth = st.slots[s];
    for(const auto& [name, values] : data.arrays){
        auto& last = st.sentArrays[s][name];
        if(!force && last == values) continue;
        last = values;
        synth->set(name, values);
    }
    auto setScalar = [&](const std::string& name, float value){
        auto it = st.sentScalars[s].find(name);
        if(!force && it != st.sentScalars[s].end() && it->second == value) return;
        st.sentScalars[s][name] = value;
        synth->set(name, value);
    };
    for(const auto& [name, value] : data.scalars) setScalar(name, value);
    // Per server: display bus (copy 0 only), curve and sweep buffers, gate of the voice
    const int U = unisonClamped(), v = s / U, u = s % U;
    if(u == 0 && st.vis) setScalar("vis", float(st.vis->index + v * displayStride));
    else if(st.visJunk) setScalar("vis", float(st.visJunk->index));
    const bool curveReady = !st.curvesPending && v < MAX_VOICES && st.curves[v] && bufferIsLive(server, st.curves[v]);
    setScalar("fbuf", curveReady ? float(st.curves[v]->index) : -1.0f);
    const bool sweepReady = !st.curvesPending && v < MAX_VOICES && st.sweeps[v] && bufferIsLive(server, st.sweeps[v]);
    setScalar("sbuf", sweepReady ? float(st.sweeps[v]->index) : -1.0f);
    setScalar("gate", v < (int)gateOpen.size() && gateOpen[v] ? 1.0f : 0.0f);
}

void scAdditiveBox::sendAll(ofxSCServer* server, bool force){
    auto& st = states[server];
    computeAll();
    const int V = numVoicesClamped(), U = unisonClamped();
    for(int s = 0; s < (int)st.slots.size() && s < V * U; s++){
        const int v = s / U, u = s % U;
        sendSlot(server, s, u == 0 ? voiceData[v] : computeVoice(v, u), force);
    }
    uploadCurves(server, force);
    if(st.out){
        st.out->set("in", st.outIn >= 0 ? st.outIn : (st.mix ? st.mix->index : 0));
        st.out->set("volume", volume.get());
        if(st.outBus >= 0) st.out->set("out", st.outBus);
    }
}

void scAdditiveBox::freeSynths(ofxSCServer* server){
    auto it = states.find(server);
    if(it == states.end()) return;
    auto& st = it->second;
    for(auto* v : st.slots) if(v){ v->free(); delete v; }
    st.slots.clear();
    st.sentArrays.clear();
    st.sentScalars.clear();
    for(auto*& f : st.fx) if(f){ f->free(); delete f; f = nullptr; }
    st.fxPlan.clear();
    if(st.out){ st.out->free(); delete st.out; st.out = nullptr; }
}

// Voices x Unison changed while the synths run: extra slots freed (the last
// first), new ones added before the FX; every slot then gets its (possibly
// new) voice's values — all slots run the same SynthDef, so a slot can take
// over any voice
void scAdditiveBox::syncSlots(ofxSCServer* server){
    auto& st = states[server];
    if(!st.out) return;
    const int V = numVoicesClamped(), U = unisonClamped(), want = V * U;
    while((int)st.slots.size() > want){
        if(st.slots.back()){ st.slots.back()->free(); delete st.slots.back(); }
        st.slots.pop_back();
    }
    st.sentArrays.resize(st.slots.size());
    st.sentScalars.resize(st.slots.size());
    ensureBuses(server);   // the display bus follows the voice count
    computeAll();
    std::vector<VoiceData> slotData(want);
    for(int s = 0; s < want; s++) slotData[s] = s % U == 0 ? voiceData[s / U] : computeVoice(s / U, s % U);
    const int existing = int(st.slots.size());
    while((int)st.slots.size() < want){
        st.slots.push_back(newSlotSynth(server));
        st.sentArrays.emplace_back();
        st.sentScalars.emplace_back();
    }
    for(int s = 0; s < want; s++) sendSlot(server, s, slotData[s], s >= existing);
    uploadCurves(server, false);
    if(st.out->isCreated()){
        ofxSCSynth* head = chainHead(st);
        for(int s = existing; s < want; s++) st.slots[s]->createAndRun(2, head->nodeID, getActive());
    }
}

// A new partial cap or output count: every synth replaced in place
// (/s_new action 4), all values baked into the new node
void scAdditiveBox::rebuildDefs(){
    const bool outputsChanged = numOutputsClamped() != builtOutputs;
    builtCap = partialCap();
    builtOutputs = numOutputsClamped();
    computeAll();
    const int U = unisonClamped();
    for(auto& [server, st] : states){
        if(!st.out) continue;
        ensureBuses(server);
        if(outputsChanged){
            auto* fresh = new ofxSCSynth(outDefName(), server);
            fresh->set("in", st.mix->index);
            fresh->set("volume", volume.get());
            if(st.outBus >= 0) fresh->set("out", st.outBus);
            if(st.out->isCreated()) fresh->createAndRun(4, st.out->nodeID, getActive());
            delete st.out;   // no /n_free: action 4 replaces the node atomically
            st.out = fresh;
            rebuildFx(server);   // the FX synths have one def per output count too
        }
        st.sentArrays.assign(st.slots.size(), {});
        st.sentScalars.assign(st.slots.size(), {});
        for(int s = 0; s < (int)st.slots.size(); s++){
            auto* old = st.slots[s];
            auto* fresh = newSlotSynth(server);
            st.slots[s] = fresh;
            const int v = s / U, u = s % U;
            sendSlot(server, s, u == 0 ? voiceData[v] : computeVoice(v, u), true);
            if(old && old->isCreated()) fresh->createAndRun(4, old->nodeID, getActive());
            else if(st.out->isCreated()) fresh->createAndRun(2, chainHead(st)->nodeID, getActive());
            delete old;
        }
    }
}

// After a preset: voice / unison count, partial cap and output count as they
// are now, with as few new synths as possible: extra slots freed first, every
// remaining one replaced once if a def changed, then the missing ones added
void scAdditiveBox::applyStructure(){
    const int want = numVoicesClamped() * unisonClamped();
    for(int v = 0; v < MAX_VOICES; v++) gateOpen[v] = valueFor(gate.get(), v, 0.0f) > 0;
    for(auto& [server, st] : states){
        while((int)st.slots.size() > want){
            if(st.slots.back()){ st.slots.back()->free(); delete st.slots.back(); }
            st.slots.pop_back();
        }
        st.sentArrays.resize(st.slots.size());
        st.sentScalars.resize(st.slots.size());
    }
    if(partialCap() != builtCap || numOutputsClamped() != builtOutputs) rebuildDefs();
    for(auto& [server, st] : states) syncSlots(server);
    markDirty();
}

void scAdditiveBox::onGate(const std::vector<float>& values){
    const int V = numVoicesClamped(), U = unisonClamped();
    for(int v = 0; v < V; v++){
        const bool open = valueFor(values, v, 0.0f) > 0;
        if(open == gateOpen[v]) continue;
        gateOpen[v] = open;
        std::vector<VoiceData> copies;
        if(open && reroll){
            // New random draws, sent before the gate opens
            rolls[v]++;
            for(int u = 0; u < U; u++) copies.push_back(computeVoice(v, u));
            if((int)voiceData.size() > v) voiceData[v] = copies[0];
        }
        for(auto& [server, st] : states){
            for(int u = 0; u < U; u++){
                const int s = v * U + u;
                if(s >= (int)st.slots.size() || !st.slots[s]) continue;
                if(!copies.empty()) sendSlot(server, s, copies[u], false);   // also sends the gate
                else {
                    st.slots[s]->set("gate", open ? 1.0f : 0.0f);
                    if(s < (int)st.sentScalars.size()) st.sentScalars[s]["gate"] = open ? 1.0f : 0.0f;
                }
                if(open && getActive() && st.slots[s]->isCreated()) st.slots[s]->run(true);
            }
        }
    }
}

void scAdditiveBox::onTrig(const std::vector<int>& values){
    // Rising edges only; `trig` is a trigger control, so a 1 fires once
    const int V = numVoicesClamped(), U = unisonClamped();
    for(int v = 0; v < V; v++){
        const int t = valueFor(values, v, 0);
        const bool rise = t > 0 && lastTrig[v] <= 0;
        lastTrig[v] = t;
        if(!rise) continue;
        std::vector<VoiceData> copies;
        if(reroll){
            rolls[v]++;
            for(int u = 0; u < U; u++) copies.push_back(computeVoice(v, u));
            if((int)voiceData.size() > v) voiceData[v] = copies[0];
        }
        for(auto& [server, st] : states){
            for(int u = 0; u < U; u++){
                const int s = v * U + u;
                if(s >= (int)st.slots.size() || !st.slots[s]) continue;
                if(!copies.empty()) sendSlot(server, s, copies[u], false);
                if(getActive() && st.slots[s]->isCreated()) st.slots[s]->run(true);
                st.slots[s]->set("trig", 1.0f);
            }
        }
    }
}

void scAdditiveBox::buildSynth(ofxSCServer* server){
    if(server == nullptr) return;
    freeSynths(server);
    auto& st = states[server];
    ensureBuses(server);
    // Only the wrappers: setOutputBus() comes next, and every value set
    // before createSynth() is baked into /s_new
    builtCap = partialCap();
    builtOutputs = numOutputsClamped();
    st.out = new ofxSCSynth(outDefName(), server);
    const int n = numVoicesClamped() * unisonClamped();
    for(int s = 0; s < n; s++) st.slots.push_back(newSlotSynth(server));
    st.sentArrays.assign(n, {});
    st.sentScalars.assign(n, {});
    rebuildFx(server);   // wrappers only: created in createSynth()
    sendAll(server, true);
}

void scAdditiveBox::createSynth(ofxSCServer* server){
    auto it = states.find(server);
    if(it == states.end() || !it->second.out) return;
    auto& st = it->second;
    if(!st.out->isCreated()) st.out->createAndRun(0, 1, getActive());
    for(auto* v : st.slots)
        if(v && !v->isCreated()) v->createAndRun(2, st.out->nodeID, getActive());   // voices before out, in order
    for(int stage : st.fxPlan)
        if(st.fx[stage] && !st.fx[stage]->isCreated()) st.fx[stage]->createAndRun(2, st.out->nodeID, getActive());   // then the FX
}

void scAdditiveBox::free(ofxSCServer* server){
    freeSynths(server);
    auto it = states.find(server);
    if(it != states.end()) it->second.outBus = -1;
}

void scAdditiveBox::setOutputBus(ofxSCServer* server, int index, int bus){
    if(index != 0) return;
    auto& st = states[server];
    st.outBus = bus;
    if(st.out) st.out->set("out", bus);
}

int scAdditiveBox::getOutputBusIndex(ofxSCServer* server, int index){
    auto it = states.find(server);
    return (index == 0 && it != states.end()) ? it->second.outBus : -1;
}

void scAdditiveBox::moveSynthBefore(ofxSCServer* server, int nodeID){
    auto it = states.find(server);
    if(it == states.end() || !it->second.out) return;
    auto& st = it->second;
    if(st.outBus >= 0) st.out->set("out", st.outBus);
    st.out->moveBefore(nodeID);
    int target = st.out->nodeID;
    for(auto f = st.fxPlan.rbegin(); f != st.fxPlan.rend(); ++f){
        if(!st.fx[*f]) continue;
        st.fx[*f]->moveBefore(target);
        target = st.fx[*f]->nodeID;
    }
    for(auto v = st.slots.rbegin(); v != st.slots.rend(); ++v){
        if(!*v) continue;
        (*v)->moveBefore(target);
        target = (*v)->nodeID;
    }
}

// The earliest of this node's synths in SC's order: the first voice
int scAdditiveBox::getLastSynthID(ofxSCServer* server){
    auto it = states.find(server);
    if(it == states.end()) return -1;
    for(auto* v : it->second.slots) if(v) return v->nodeID;
    return it->second.out ? it->second.out->nodeID : -1;
}

void scAdditiveBox::activate(){
    // Voices whose gate is off pause themselves again once silent
    for(auto& [server, st] : states){
        if(st.out && st.out->isCreated()) st.out->run(true);
        for(auto* f : st.fx) if(f && f->isCreated()) f->run(true);
        for(auto* v : st.slots) if(v && v->isCreated()) v->run(true);
    }
}

void scAdditiveBox::deactivate(){
    for(auto& [server, st] : states){
        if(st.out && st.out->isCreated()) st.out->run(false);
        for(auto* f : st.fx) if(f && f->isCreated()) f->run(false);
        for(auto* v : st.slots) if(v && v->isCreated()) v->run(false);
    }
}

void scAdditiveBox::resendParametersForNRT(){
    for(auto& [server, st] : states){
        sendAll(server, true);
        for(int stage : st.fxPlan) sendFxParams(stage, st.fx[stage]);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// FX
// ════════════════════════════════════════════════════════════════════════════

std::string scAdditiveBox::fxDefName(int stage) const {
    return std::string(fxDefBase[std::clamp(stage, 0, 2)]) + ofToString(numOutputsClamped());
}

// The FX SynthDefs live outside Defaults: serverManager loads their folders
// on request (idempotent), and the stage waits until scsynth has them
void scAdditiveBox::requireFxSynthdefs(){
    for(const char* base : fxDefBase){
        const std::string folder = findSynthdefFolder(base);
        if(folder.empty()) continue;
        for(auto* sm : servers) if(sm) sm->requireSynthdefFolder(folder);
    }
}

bool scAdditiveBox::fxStageReady(ofxSCServer* server) const {
    if(!server) return false;
    if(server->isNRTCapturing()) return true;
    for(auto* sm : servers) if(sm && sm->getServer() == server) return !sm->areRequiredSynthdefsLoading();
    return true;
}

void scAdditiveBox::freeFxSynths(ofxSCServer* server){
    auto& st = states[server];
    for(auto*& f : st.fx) if(f){ f->free(); delete f; f = nullptr; }
    st.fxPlan.clear();
}

// Re-plan the chain: new FX synths before the output synth (created now if
// the output synth runs, else by createSynth()), the output synth reading
// the end of the chain
void scAdditiveBox::rebuildFx(ofxSCServer* server){
    auto it = states.find(server);
    if(it == states.end() || !it->second.out || !it->second.mix) return;
    auto& st = it->second;
    freeFxSynths(server);
    fxPending = false;
    const bool ready = fxStageReady(server);
    for(int stage = 0; stage < FX_COUNT; stage++){
        if(!fxOn[stage]) continue;
        if(ready) st.fxPlan.push_back(stage);
        else fxPending = true;
    }
    for(auto*& b : st.fxBuses) if(b && !audioBusIsLive(server, b)){ delete b; b = nullptr; }
    st.fxBuses.erase(std::remove(st.fxBuses.begin(), st.fxBuses.end(), nullptr), st.fxBuses.end());
    while(st.fxBuses.size() < st.fxPlan.size()) st.fxBuses.push_back(new ofxSCBus(RATE_AUDIO, MAX_OUTPUTS, server));

    int in = st.mix->index;
    for(size_t i = 0; i < st.fxPlan.size(); i++){
        const int stage = st.fxPlan[i];
        auto* synth = new ofxSCSynth(fxDefName(stage), server);
        synth->set("in", in);
        synth->set("out", st.fxBuses[i]->index);
        sendFxParams(stage, synth);
        if(st.out->isCreated()) synth->createAndRun(2, st.out->nodeID, getActive());   // addBefore out
        st.fx[stage] = synth;
        in = st.fxBuses[i]->index;
    }
    st.outIn = in;
    st.out->set("in", in);
}

void scAdditiveBox::rebuildFxAll(){
    for(auto& [server, st] : states) rebuildFx(server);
}

void scAdditiveBox::sendFxParams(int stage, ofxSCSynth* s){
    if(!s) return;
    const int n = numOutputsClamped();
    auto vec = [n](float v){ return std::vector<float>(n, v); };
    switch(stage){
        case FX_EQ:
            // graphiceqN: bNgain dB, bNfreq Hz, shelves bNslope (= 1/S), peaks bNrq = 1/Q
            for(int b = 0; b < 5; b++){
                const std::string k = "b" + ofToString(b + 1);
                s->set(k + "gain", vec(eqGain[b].get()));
                s->set(k + "freq", vec(eqFreq[b].get()));
                if(scEQEditor::isShelf(b)) s->set(k + "slope", vec(1.0f / scEQEditor::safeShelfSlope(eqShape[b].get(), eqGain[b].get())));
                else s->set(k + "rq", vec(1.0f / std::max(0.01f, eqShape[b].get())));
            }
            s->set("mix", eqMix.get());
            break;
        case FX_ECHO:
            s->set("delay", vec(echoDelay.get()));
            s->set("feed", vec(echoFeed.get()));
            s->set("cutoff", vec(echoCutoff.get()));
            s->set("mix", vec(echoMix.get()));
            s->set("filtertype", vec(float(std::clamp(echoFilter.get(), 0, 3))));
            s->set("resonance", vec(echoResonance.get()));
            break;
        case FX_REVERB:
            s->set("size", vec(revSize.get()));
            s->set("decay", vec(revDecay.get()));
            s->set("predelay", vec(revPredelay.get()));
            s->set("lowpass", vec(revLowpass.get()));
            s->set("mix", vec(revMix.get()));
            s->set("position", vec(revPosition.get()));
            s->set("spread", vec(revSpread.get()));
            break;
        default: break;
    }
}

void scAdditiveBox::sendFxParamsAll(int stage){
    for(auto& [server, st] : states) if(st.fx[stage]) sendFxParams(stage, st.fx[stage]);
}

// ════════════════════════════════════════════════════════════════════════════
// Publishing
// ════════════════════════════════════════════════════════════════════════════

template<typename T>
void scAdditiveBox::addPublishAction(ofParameter<T>& parameter, std::vector<std::string> dropdown){
    auto* p = &parameter;
    const std::string key = parameter.getEscapedName();
    PublishAction action;
    action.key = key;
    action.isVector = std::is_same<T, std::vector<float>>::value || std::is_same<T, std::vector<int>>::value;
    action.dropdown = dropdown;
    action.publish = [this, p, dropdown]() -> std::shared_ptr<ofxOceanodeAbstractParameter> {
        auto handle = addParameter(*p);
        if(!dropdown.empty()) handle->setDropdownOptions(dropdown);
        return handle;
    };
    if(p != (void*)&trig){   // a saved trigger would fire on load
        action.save = [key, p](ofJson& json){ json[key] = p->get(); };
        action.load = [p](const ofJson& value){ try { p->set(value.get<T>()); } catch(...) {} };
        const T initial = p->get();
        action.reset = [p, initial](){ p->set(initial); };
    }
    action.valueText = [p](){ return toText(p->get()); };
    action.applyText = [p](const std::string& text){ T v; fromText(text, v); p->set(v); };
    publishActions.push_back(std::move(action));
}

void scAdditiveBox::initializePublishActions(){
    publishActions.clear();
    addPublishAction(pitch);
    addPublishAction(transpose);
    addPublishAction(gate);
    addPublishAction(levels);
    addPublishAction(glide);
    addPublishAction(trig);
    addPublishAction(volume);
    addPublishAction(waveA, waveNames);
    addPublishAction(waveB, waveNames);
    addPublishAction(morph);
    addPublishAction(harmonicsA);
    addPublishAction(harmonicsB);
    addPublishAction(harmBlur);
    addPublishAction(partials);
    addPublishAction(tilt);
    addPublishAction(stretch);
    addPublishAction(inharm);
    addPublishAction(evenOdd);
    addPublishAction(detune);
    addPublishAction(shift);
    addPublishAction(filterMode, filterNames);
    addPublishAction(cutoff);
    addPublishAction(keytrack);
    addPublishAction(slope);
    addPublishAction(resonance);
    addPublishAction(safeBass);
    addPublishAction(filterShape);
    addPublishAction(filter2Mode, filter2Names);
    addPublishAction(vowel);
    addPublishAction(formantShift);
    addPublishAction(formantWidth);
    addPublishAction(filter2Mix);
    addPublishAction(drawnEq);
    addPublishAction(sweepAmt);
    addPublishAction(sweepWaves);
    addPublishAction(sweepRate);
    addPublishAction(sweepSharp);
    addPublishAction(sweepMode, sweepModeNames);
    addPublishAction(sweepPhase);
    addPublishAction(sweepShape, sweepShapeNames);
    addPublishAction(sweepSkew);
    addPublishAction(sweepInvert);
    addPublishAction(sweepLow);
    addPublishAction(sweepHigh);
    addPublishAction(sweepVoiceOffset);
    addPublishAction(sweepRetrig);
    addPublishAction(sweepDrawn);
    addPublishAction(unison);
    addPublishAction(unisonDetune);
    addPublishAction(unisonSpread);
    addPublishAction(unisonInterval);
    addPublishAction(unisonBlend);
    addPublishAction(density);
    addPublishAction(randAmp);
    addPublishAction(maskGlide);
    addPublishAction(seed);
    addPublishAction(reroll);
    addPublishAction(attack);
    addPublishAction(decay);
    addPublishAction(sustain);
    addPublishAction(release);
    addPublishAction(curve);
    addPublishAction(envRand);
    addPublishAction(envTilt);
    addPublishAction(vibrato);
    addPublishAction(vibRate);
    addPublishAction(partialVib);
    addPublishAction(partialVibRate);
    addPublishAction(shimmer);
    addPublishAction(shimmerRate);
    addPublishAction(noise);
    addPublishAction(noiseRate);
    addPublishAction(noiseTilt);
    addPublishAction(timeBlur);
    addPublishAction(pan);
    addPublishAction(spread);
    addPublishAction(panMode, panModeNames);
    addPublishAction(drift);
    addPublishAction(driftRate);
    addPublishAction(width);
    for(auto* p : {&modAttack, &modDecay, &modSustain, &modRelease, &modCurve}) addPublishAction(*p);
    for(int l = 0; l < 2; l++){
        addPublishAction(lfoShape[l], lfoShapeNames);
        addPublishAction(lfoRate[l]);
        addPublishAction(lfoRetrig[l]);
    }
    for(int s = 0; s < MOD_SOURCES; s++)
        for(int d = 0; d < MOD_DESTS; d++) addPublishAction(modAmount[s][d]);
    for(auto& p : fxOn) addPublishAction(p);
    for(int b = 0; b < 5; b++){ addPublishAction(eqGain[b]); addPublishAction(eqFreq[b]); addPublishAction(eqShape[b]); }
    addPublishAction(eqMix);
    addPublishAction(echoDelay);
    addPublishAction(echoFeed);
    addPublishAction(echoCutoff);
    addPublishAction(echoFilter, echoFilterNames);
    addPublishAction(echoResonance);
    addPublishAction(echoMix);
    for(auto* p : {&revSize, &revDecay, &revPredelay, &revLowpass, &revPosition, &revSpread, &revMix}) addPublishAction(*p);
}

const scAdditiveBox::PublishAction* scAdditiveBox::findPublishAction(const std::string& key) const {
    for(const auto& action : publishActions) if(action.key == key) return &action;
    return nullptr;
}

bool scAdditiveBox::isPublished(const std::string& key) const {
    return publishedHandles.count(key) != 0;
}

bool scAdditiveBox::hasPublishedConnection(const std::string& key) const {
    auto it = publishedHandles.find(key);
    return it != publishedHandles.end() && it->second
        && (it->second->hasInConnection() || it->second->hasOutConnections());
}

bool scAdditiveBox::publishByKey(const std::string& key){
    const PublishAction* action = findPublishAction(key);
    if(!action || !action->publish || isPublished(key)) return false;
    if(!publishedSeparatorAdded){
        addSeparator("Published", ofColor(200));
        publishedSeparatorAdded = true;
    }
    publishedHandles[key] = action->publish();
    publishedKeys.push_back(key);
    parameterGroupChanged.notify(this);
    return true;
}

bool scAdditiveBox::unpublishByKey(const std::string& key){
    auto it = publishedHandles.find(key);
    if(it == publishedHandles.end() || hasPublishedConnection(key)) return false;
    removeParameter(it->second->getEscapedName());
    publishedHandles.erase(it);
    publishedKeys.erase(std::remove(publishedKeys.begin(), publishedKeys.end(), key), publishedKeys.end());
    if(publishedHandles.empty() && publishedSeparatorAdded){
        removeSeparator("Published");
        publishedSeparatorAdded = false;
    }
    parameterGroupChanged.notify(this);
    return true;
}

void scAdditiveBox::syncPublished(const std::vector<std::string>& keys){
    const auto current = publishedKeys;
    for(const auto& key : current)
        if(std::find(keys.begin(), keys.end(), key) == keys.end() && !hasPublishedConnection(key))
            unpublishByKey(key);
    for(const auto& key : keys) if(!isPublished(key)) publishByKey(key);
}

void scAdditiveBox::controlMenu(const std::string& key){
    if(isPublished(key)){
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y - 1.0f), ImVec2(b.x, b.y - 1.0f),
                                            ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
    }
    // Same menu as GrainBox: a typed value, then Publish / Unpublish
    if(!ImGui::BeginPopupContextItem(("##PublishToNode_" + key).c_str())) return;
    controlMenuItems(key);
    ImGui::EndPopup();
}

void scAdditiveBox::controlMenuItems(const std::string& key, const char* title){
    ImGui::PushID(key.c_str());   // several controls can share one menu (EQ bands)
    const PublishAction* action = findPublishAction(key);
    if(action && action->valueText && action->applyText){
        static std::map<std::string, std::array<char, 2048>> buffers;   // one per control
        auto& buffer = buffers[key];
        const bool appearing = ImGui::IsWindowAppearing();
        if(appearing) std::snprintf(buffer.data(), buffer.size(), "%s", action->valueText().c_str());
        ImGui::TextDisabled("%s", title ? title : (action->isVector ? "Value (one for all voices, or a,b,c...)" : "Value"));
        ImGui::SetNextItemWidth(180.0f * ofxOceanodeShared::getZoomLevel());
        if(appearing && !title) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##abTypedValue", buffer.data(), buffer.size(),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if(enter || ImGui::IsItemDeactivatedAfterEdit()){
            action->applyText(std::string(buffer.data()));
            if(enter) ImGui::CloseCurrentPopup();
        }
        ImGui::Separator();
    }
    if(action == nullptr){
        const bool onNode = key == numVoices.getEscapedName() || key == numOutputs.getEscapedName();
        ImGui::TextDisabled("%s", onNode ? "Already in node" : "Not publishable");
    } else if(isPublished(key)){
        ImGui::TextDisabled("Published in node");
        if(hasPublishedConnection(key)) ImGui::TextDisabled("Connected: disconnect to unpublish");
        else if(ImGui::MenuItem("Unpublish from Node")) unpublishByKey(key);
    } else if(ImGui::MenuItem("Publish to Node")){
        publishByKey(key);
    }
    ImGui::PopID();
}

// ════════════════════════════════════════════════════════════════════════════
// Presets
// ════════════════════════════════════════════════════════════════════════════

void scAdditiveBox::presetSave(ofJson& json){
    json["publishedEditorParameters"] = publishedKeys;
    json["viewHeight"] = viewHeight;
    json["bankPreset"] = currentPreset;
    // Window-only controls; published ones are saved by Oceanode as usual
    for(const auto& action : publishActions)
        if(action.save && !isPublished(action.key)) action.save(json);
}

void scAdditiveBox::loadBeforeConnections(ofJson& json){
    if(!json.contains("publishedEditorParameters") || !json["publishedEditorParameters"].is_array()) return;
    try { syncPublished(json["publishedEditorParameters"].get<std::vector<std::string>>()); } catch(...) {}
}

void scAdditiveBox::presetRecallBeforeSettingParameters(ofJson& json){
    loadBeforeConnections(json);
}

void scAdditiveBox::presetRecallAfterSettingParameters(ofJson& json){
    if(json.contains("bankPreset") && json["bankPreset"].is_string()) currentPreset = json["bankPreset"].get<std::string>();
    if(json.contains("viewHeight") && json["viewHeight"].is_number())
        viewHeight = ofClamp(json["viewHeight"].get<float>(), 140.0f, 2000.0f);
    for(const auto& action : publishActions){
        if(isPublished(action.key) || !json.contains(action.key) || !action.load) continue;
        action.load(json[action.key]);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Preset bank
// ════════════════════════════════════════════════════════════════════════════

std::string scAdditiveBox::presetDir() const {
    return ofToDataPath("Supercollider/AdditiveBox/Presets", true);
}

// Played, not part of a sound: a preset leaves them as they are
bool scAdditiveBox::notInPresets(const std::string& key) const {
    return key == gate.getEscapedName() || key == trig.getEscapedName() || key == transpose.getEscapedName();
}

void scAdditiveBox::scanPresets(){
    presetsScanned = true;
    bankPresets.clear();
    std::error_code ec;
    for(const auto& entry : fs::directory_iterator(presetDir(), ec)){
        if(entry.is_regular_file(ec) && entry.path().extension() == ".json")
            bankPresets.push_back(entry.path().stem().string());
    }
    std::sort(bankPresets.begin(), bankPresets.end());
}

void scAdditiveBox::loadBankPreset(const std::string& name){
    std::ifstream file(presetDir() + "/" + name + ".json");
    if(!file) return;
    ofJson json;
    try { file >> json; } catch(...) { ofLogWarning("scAdditiveBox") << "Cannot read preset " << name; return; }
    // Every control back to its default, then the preset's values; the synths
    // change once afterwards (a voice count, partial cap or output change each
    // replacing every synth on its own made a burst of large /s_new on the
    // server: audio dropouts). Outputs, Gate, Trig and Transpose are not loaded.
    loadingBankPreset = true;
    for(const auto& action : publishActions)
        if(action.reset && !notInPresets(action.key)) action.reset();
    numVoices = json.contains("Voices") && json["Voices"].is_number_integer() ? json["Voices"].get<int>() : 1;
    for(const auto& action : publishActions){
        if(!action.load || notInPresets(action.key) || !json.contains(action.key)) continue;
        action.load(json[action.key]);
    }
    loadingBankPreset = false;
    applyStructure();
    currentPreset = name;
}

bool scAdditiveBox::saveBankPreset(const std::string& rawName){
    std::string name;
    for(char c : rawName) name += (std::isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_') ? c : '_';
    name = ofTrim(name);
    if(name.empty()) return false;
    ofJson json;
    json["Voices"] = numVoices.get();
    for(const auto& action : publishActions)
        if(action.save && !notInPresets(action.key)) action.save(json);
    std::error_code ec;
    fs::create_directories(presetDir(), ec);
    std::ofstream file(presetDir() + "/" + name + ".json");
    if(!file) return false;
    file << json.dump(4);
    currentPreset = name;
    scanPresets();
    return true;
}

void scAdditiveBox::drawPresetBar(){
    if(!presetsScanned) scanPresets();
    ImGui::SetNextItemWidth(260.0f);
    const std::string preview = currentPreset.empty() ? "Preset..." : currentPreset;
    if(ImGui::BeginCombo("##abpreset", preview.c_str(), ImGuiComboFlags_HeightLarge)){
        if(ImGui::IsWindowAppearing()) scanPresets();
        for(const auto& name : bankPresets)
            if(ImGui::Selectable(name.c_str(), name == currentPreset)) loadBankPreset(name);
        if(bankPresets.empty()) ImGui::TextDisabled("No presets in %s", presetDir().c_str());
        ImGui::EndCombo();
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Sound presets (Gate, Transpose and Outputs are left as they are)");
    ImGui::SameLine();
    if(ImGui::ArrowButton("##abprevpreset", ImGuiDir_Left) && !bankPresets.empty()){
        auto it = std::find(bankPresets.begin(), bankPresets.end(), currentPreset);
        const int i = it == bankPresets.end() ? 0 : int(it - bankPresets.begin()) - 1;
        loadBankPreset(bankPresets[(i + bankPresets.size()) % bankPresets.size()]);
    }
    ImGui::SameLine(0, 2);
    if(ImGui::ArrowButton("##abnextpreset", ImGuiDir_Right) && !bankPresets.empty()){
        auto it = std::find(bankPresets.begin(), bankPresets.end(), currentPreset);
        const int i = it == bankPresets.end() ? 0 : int(it - bankPresets.begin()) + 1;
        loadBankPreset(bankPresets[i % bankPresets.size()]);
    }
    ImGui::SameLine();
    static char name[128] = "";
    if(ImGui::Button("Save...")){
        std::snprintf(name, sizeof(name), "%s", currentPreset.c_str());
        ImGui::OpenPopup("##absavepreset");
    }
    if(ImGui::BeginPopup("##absavepreset")){
        ImGui::TextDisabled("Preset name (same name overwrites)");
        ImGui::SetNextItemWidth(240.0f);
        if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##abpresetname", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if((ImGui::Button("Save") || enter) && saveBankPreset(name)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Window
// ════════════════════════════════════════════════════════════════════════════

int scAdditiveBox::displaySlots() const { return std::min(partialCap(), int(DISPLAY_MAX)); }

int scAdditiveBox::displayPartial(int slot) const { return slot * (partialCap() / displaySlots()); }

float scAdditiveBox::liveLevel(int voice, int slot) const {
    if(!liveValid) return -1;
    const size_t i = size_t(voice) * displayStride + slot;
    return i < liveData.size() && std::isfinite(liveData[i]) ? std::max(0.0f, liveData[i]) : -1;
}

void scAdditiveBox::drawWindow(){
    const std::string title = (canvasID == "Canvas" ? "" : canvasID + "/") + "AdditiveBox " + ofToString(getNumIdentifier());
    bool open = showWindow.get();
    ImGui::SetNextWindowSize(ImVec2(1180.0f, 900.0f), ImGuiCond_FirstUseEver);
    if(ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)){
        ImGui::PushID(this);
        if((int)voiceData.size() != numVoicesClamped()) dirty = true;
        drawHeader();

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float w = std::max(1.0f, avail.x);
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        constexpr float handleHeight = 8.0f, stripHeight = 120.0f, controlsHeight = 300.0f;
        viewHeight = ofClamp(viewHeight, 140.0f, std::max(140.0f, avail.y - handleHeight - stripHeight - controlsHeight));

        drawSpectrum(ImVec2(w, viewHeight));

        // Handle: drag to resize the views
        const ImVec2 handle = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##abresize", ImVec2(w, handleHeight));
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if(hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if(ImGui::IsItemActive()) viewHeight += ImGui::GetIO().MouseDelta.y;
        const float midY = handle.y + handleHeight * 0.5f, midX = handle.x + w * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(midX - 24, midY), ImVec2(midX + 24, midY),
            ImGui::GetColorU32(hot ? ImGuiCol_Text : ImGuiCol_TextDisabled), 2.0f);

        // Envelopes | voices x partials
        const float envWidth = std::max(1.0f, w * 0.34f);
        drawEnvelopes(ImVec2(envWidth, stripHeight));
        ImGui::SameLine();
        drawMatrix(ImVec2(std::max(1.0f, w - envWidth - spacing), stripHeight));

        ImGui::Spacing();
        if(ImGui::BeginTabBar("##abtabs")){
            auto tab = [&](const char* name, std::function<void()> body){
                if(!ImGui::BeginTabItem(name)) return;
                if(ImGui::BeginChild((std::string("##abtab") + name).c_str(), ImVec2(0, 0), false)) body();
                ImGui::EndChild();
                ImGui::EndTabItem();
            };
            tab("Sound", [this](){ drawSoundTab(); });
            tab("Source", [this](){ drawSourceTab(); });
            tab("Filters", [this](){ drawFiltersTab(); });
            tab("Sweep", [this](){ drawSweepTab(); });
            tab("Modulation", [this](){ drawModTab(); });
            tab("FX", [this](){ drawFxPanel(); });
            ImGui::EndTabBar();
        }
        ImGui::PopID();
    }
    ImGui::End();
    if(open != showWindow.get()) showWindow = open;
}

void scAdditiveBox::drawHeader(){
    drawPresetBar();
    ImGui::SetNextItemWidth(130.0f);
    sliderInt(numVoices);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    sliderInt(numOutputs);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170.0f);
    sliderInt(partials);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    sliderInt(unison);
    ImGui::SameLine();
    const int V = numVoicesClamped(), U = unisonClamped(), O = numOutputsClamped(), cap = partialCap();
    const float cpu = V * U * cap * (cpuPerPartial + cpuPerPartialOutput * O);
    ImGui::TextDisabled("%d x %d x %d partials -> %d out | ~%.0f%% CPU, every gate open", V, U, cap, O, cpu);
    if(ImGui::IsItemHovered()){
        ImGui::SetTooltip("Voices x Unison synths, each running the smallest of 8 / 16 / 32 / 64 / 128 / 256\n"
                          "partials >= Partials (the extra ones are silent but still cost). A voice whose\n"
                          "gate is off pauses itself once silent and costs nothing.\n"
                          "Rough estimate: watch the server's CPU.");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if(ImGui::IsItemHovered()){
        ImGui::SetTooltip("Every per-voice control takes one value per voice (a vector), or one value for all.\n"
                          "Right-click a control to publish it to the node or type a vector.\n"
                          "Spectrum: drag to move filter 1's cutoff (x) and resonance (y), wheel: slope,\n"
                          "double-click: filter 1 on / off.");
    }
}

void scAdditiveBox::drawSpectrum(ImVec2 size){
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##spectrum", size);
    const bool hovered = ImGui::IsItemHovered();

    constexpr float fMin = 20.0f, fMax = 20000.0f, dbMin = -66.0f, dbMax = 12.0f;
    const float logSpan = std::log(fMax / fMin);
    auto xOf = [&](float f){ return o.x + size.x * std::log(std::max(f, fMin) / fMin) / logSpan; };
    auto fOf = [&](float x){ return fMin * std::exp((x - o.x) / size.x * logSpan); };
    auto yOf = [&](float db){ return o.y + size.y * (1.0f - (std::clamp(db, dbMin, dbMax) - dbMin) / (dbMax - dbMin)); };
    const float y0 = yOf(dbMin);

    // Interaction: filter 1 of every voice
    const int mode0 = std::clamp(valueFor(filterMode.get(), 0, 0), 0, NumFilterModes - 1);
    const float pitch0 = valueFor(pitch.get(), 0, 48.0f) + transpose.get();
    const float key0 = valueFor(keytrack.get(), 0, 0.0f);
    if(ImGui::IsItemActive() && mode0 != FilterOff){
        const ImVec2 m = ImGui::GetIO().MousePos;
        const float cutMidi = hzToMidi(fOf(ofClamp(m.x, o.x, o.x + size.x))) - key0 * (pitch0 - 60.0f);
        cutoff = std::vector<float>{ofClamp(cutMidi, 0.0f, 140.0f)};
        if(ImGui::GetIO().MouseDelta.y != 0){
            const float r = ofClamp(valueFor(resonance.get(), 0, 0.0f) - ImGui::GetIO().MouseDelta.y / size.y * 1.5f, 0.0f, 1.0f);
            resonance = std::vector<float>{r};
        }
    }
    if(hovered && ImGui::IsMouseDoubleClicked(0)) filterMode = std::vector<int>{mode0 == FilterOff ? int(FilterLP) : int(FilterOff)};
    if(hovered && ImGui::GetIO().MouseWheel != 0 && mode0 != FilterOff){
        slope = std::vector<float>{ofClamp(valueFor(slope.get(), 0, 24.0f) + ImGui::GetIO().MouseWheel * 3.0f, 0.0f, 96.0f)};
    }

    // Background, octave and level grid
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), IM_COL32(10, 12, 20, 255), 6.0f);
    dl->PushClipRect(o, ImVec2(o.x + size.x, o.y + size.y), true);
    for(int oct = 0; oct <= 10; oct++){
        const float f = midiToHz(12.0f * (oct + 1));   // C0 .. C10
        if(f < fMin || f > fMax) continue;
        const float x = xOf(f);
        dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + size.y), IM_COL32(255, 255, 255, 14));
        dl->AddText(ImVec2(x + 3, o.y + size.y - 15), IM_COL32(255, 255, 255, 60), ("C" + ofToString(oct)).c_str());
    }
    for(float db = 0; db >= dbMin; db -= 12.0f){
        const float y = yOf(db);
        dl->AddLine(ImVec2(o.x, y), ImVec2(o.x + size.x, y), IM_COL32(255, 255, 255, db == 0 ? 28 : 10));
        dl->AddText(ImVec2(o.x + 4, y + 1), IM_COL32(255, 255, 255, 50), (ofToString(int(db)) + " dB").c_str());
    }
    const float sr = sampleRate();
    if(sr * 0.5f < fMax){
        const float x = xOf(sr * 0.5f);
        dl->AddRectFilled(ImVec2(x, o.y), ImVec2(o.x + size.x, o.y + size.y), IM_COL32(0, 0, 0, 120));
    }

    // Partials, back voices first: a glow, the line, a spark at the top
    const int V = std::min(numVoicesClamped(), (int)voiceData.size());
    const int slots = displaySlots();
    const int P = partialCap();
    for(int v = V - 1; v >= 0; v--){
        const auto& d = voiceData[v];
        if(d.ratio.empty() || d.view.empty()) continue;
        const float f0 = midiToHz(d.pitch);
        const float sh = d.scalars.count("shift") ? d.scalars.at("shift") : 0.0f;
        const float peak = std::max(1e-9f, d.peak);
        const auto& view = d.view;
        const bool live = liveValid;
        const int n = live ? slots : P;
        // Every partial's static level (filter 1 at rest), dim, under the
        // live ones (the display bus carries at most 64 partials per voice)
        if(live){
            for(int i = 0; i < P && i < (int)d.ratio.size(); i++){
                if(view[i] <= 0) continue;
                const float db = 20.0f * std::log10(view[i] / peak);
                if(db < dbMin) continue;
                const float x = xOf(std::abs(f0 * d.ratio[i] + sh));
                dl->AddLine(ImVec2(x, y0), ImVec2(x, yOf(db)), voiceColor(v, 0.13f), 1.0f);
            }
        }
        for(int s = 0; s < n; s++){
            const int i = live ? displayPartial(s) : s;
            if(i >= (int)d.ratio.size()) break;
            float level = live ? liveLevel(v, s) : view[i];
            if(level <= 0) continue;
            const float db = 20.0f * std::log10(level / peak);
            if(db < dbMin) continue;
            const float x = xOf(std::abs(f0 * d.ratio[i] + sh));
            const float y = yOf(db);
            const float t = (db - dbMin) / (0 - dbMin);
            dl->AddLine(ImVec2(x, y0), ImVec2(x, y), voiceColor(v, 0.10f + 0.10f * t), 5.0f);
            dl->AddLine(ImVec2(x, y0), ImVec2(x, y), voiceColor(v, 0.35f + 0.6f * t), 1.4f);
            dl->AddCircleFilled(ImVec2(x, y), 1.5f + 1.5f * t, voiceColor(v, 0.9f, 1.0f));
        }
    }

    // Filter 2 of voice 1 (cyan), filter 1 (warm, at its unmodulated cutoff)
    const int N = std::max(32, int(size.x / 3));
    std::vector<ImVec2> pts(N);
    if(valueFor(filter2Mode.get(), 0, 0) != Filter2Off){
        for(int i = 0; i < N; i++){
            const float x = o.x + size.x * i / (N - 1);
            pts[i] = ImVec2(x, yOf(20.0f * std::log10(std::max(filter2Gain(0, fOf(x)), 1e-6f))));
        }
        dl->AddPolyline(pts.data(), N, IM_COL32(120, 230, 255, 160), 0, 1.4f);
    }
    if(mode0 != FilterOff){
        const float cut = voiceCutoff(0);
        for(int i = 0; i < N; i++){
            const float x = o.x + size.x * i / (N - 1);
            pts[i] = ImVec2(x, yOf(20.0f * std::log10(std::max(filter1Gain(0, hzToMidi(fOf(x)) - cut), 1e-6f))));
        }
        dl->AddPolyline(pts.data(), N, IM_COL32(255, 255, 255, 40), 0, 6.0f);
        dl->AddPolyline(pts.data(), N, IM_COL32(255, 240, 200, 210), 0, 1.6f);
        dl->AddCircle(ImVec2(xOf(midiToHz(cut)), yOf(20.0f * std::log10(std::max(filter1Gain(0, 0), 1e-6f)))), 6.0f,
                      IM_COL32(255, 240, 200, 220), 0, 1.5f);
    }
    dl->PopClipRect();

    std::string info = "SPECTRUM";
    if(mode0 == FilterOff) info += "   double-click: filter 1 on";
    else info += "   " + filterNames[mode0] + "  " + ofToString(midiToHz(voiceCutoff(0)), 0) + " Hz";
    if(!liveValid) info += "   (static: no server data)";
    dl->AddText(ImVec2(o.x + size.x - ImGui::CalcTextSize(info.c_str()).x - 8, o.y + 6), IM_COL32(255, 255, 255, 120), info.c_str());
}

void scAdditiveBox::drawEnvelopes(ImVec2 size){
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##envelopes", size);
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), IM_COL32(10, 12, 20, 255), 6.0f);
    if(voiceData.empty() || !voiceData[0].arrays.count("ta")) return;
    const auto& d = voiceData[0];
    const auto& ta = d.arrays.at("ta");
    const auto& td = d.arrays.at("td");
    const auto& tr = d.arrays.at("tr");
    const float sus = valueFor(sustain.get(), 0, 1.0f);
    const float crv = valueFor(curve.get(), 0, -4.0f);
    const int count = std::min(std::max(partials.get(), 1), (int)ta.size());
    float maxAD = 0, maxR = 0;
    for(int i = 0; i < count; i++){ maxAD = std::max(maxAD, ta[i] + td[i]); maxR = std::max(maxR, tr[i]); }
    maxAD = std::max(maxAD, modAttack.get() + modDecay.get());
    maxR = std::max(maxR, modRelease.get());
    const float hold = std::max(0.05f, (maxAD + maxR) * 0.25f);
    const float total = std::max(0.01f, maxAD + hold + maxR);

    const float pad = 8.0f;
    const ImVec2 a(o.x + pad, o.y + 20), b(o.x + size.x - pad, o.y + size.y - pad);
    const int N = std::max(24, int(size.x / 4));
    std::vector<ImVec2> pts(N);
    auto trace = [&](float at, float dt, float s, float rt, float c){
        for(int n = 0; n < N; n++){
            const float t = total * n / (N - 1);
            // Release starts when the longest attack + decay is over
            float lvl;
            if(t < maxAD + hold){
                lvl = adsr(t, at, dt, s, 0, 1e9f, c);
            } else {
                const float atRelease = adsr(maxAD + hold, at, dt, s, 0, 1e9f, c);
                const float u = t - maxAD - hold;
                lvl = u < rt ? envSegment(atRelease, 0, rt > 0 ? u / rt : 1, c) : 0;
            }
            pts[n] = ImVec2(a.x + (b.x - a.x) * t / total, b.y - (b.y - a.y) * lvl);
        }
    };
    const int shown = std::min(count, 24);
    for(int j = shown - 1; j >= 0; j--){
        const int i = shown > 1 ? j * (count - 1) / (shown - 1) : 0;
        const float frac = shown > 1 ? float(j) / (shown - 1) : 0.0f;
        trace(ta[i], td[i], sus, tr[i], crv);
        float r, g, bl;
        ImGui::ColorConvertHSVtoRGB(0.08f + 0.5f * frac, 0.65f, 1.0f, r, g, bl);
        dl->AddPolyline(pts.data(), N, ImGui::GetColorU32(ImVec4(r, g, bl, 0.75f - 0.4f * frac)), 0, 1.2f);
    }
    // The modulation envelope, dashed white, when it modulates something
    bool envUsed = false;
    for(int dd = 0; dd < MOD_DESTS; dd++) if(modAmount[0][dd].get() != 0) envUsed = true;
    if(envUsed){
        trace(modAttack.get(), modDecay.get(), modSustain.get(), modRelease.get(), modCurve.get());
        for(int n = 0; n + 1 < N; n += 2) dl->AddLine(pts[n], pts[n + 1], IM_COL32(255, 255, 255, 170), 1.2f);
    }
    const float gx = a.x + (b.x - a.x) * (maxAD + hold) / total;
    dl->AddLine(ImVec2(gx, a.y), ImVec2(gx, b.y), IM_COL32(255, 255, 255, 30));
    dl->AddText(ImVec2(o.x + 8, o.y + 4), IM_COL32(255, 255, 255, 110),
                envUsed ? "ENVELOPES  (warm: low partials, cool: high, dashed: Env 2)" : "ENVELOPES  (warm: low partials, cool: high)");
    // Gate of each voice
    const int V = numVoicesClamped();
    for(int v = 0; v < V; v++){
        const ImVec2 p(o.x + size.x - 10 - (V - 1 - v) * 11.0f, o.y + 10);
        if(gateOpen[v]) dl->AddCircleFilled(p, 4.0f, voiceColor(v));
        else dl->AddCircle(p, 4.0f, voiceColor(v, 0.5f));
    }
}

void scAdditiveBox::drawMatrix(ImVec2 size){
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##matrix", size);
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), IM_COL32(10, 12, 20, 255), 6.0f);
    const int V = std::min(numVoicesClamped(), (int)voiceData.size());
    if(V == 0) return;
    const int slots = displaySlots();
    const float top = o.y + 20, left = o.x + 8, right = o.x + size.x - 8, bottom = o.y + size.y - 6;
    const float cw = (right - left) / slots, ch = (bottom - top) / V;
    for(int v = 0; v < V; v++){
        const auto& d = voiceData[v];
        if(d.view.empty()) continue;
        const float peak = std::max(1e-9f, d.peak);
        for(int s = 0; s < slots; s++){
            const int i = displayPartial(s);
            if(i >= (int)d.view.size()) break;
            const float level = liveValid ? liveLevel(v, s) : d.view[i];
            const float t = std::sqrt(std::clamp(level / peak, 0.0f, 1.0f));
            const ImVec2 p0(left + s * cw + 0.5f, top + v * ch + 0.5f);
            const ImVec2 p1(left + (s + 1) * cw - 0.5f, top + (v + 1) * ch - 0.5f);
            dl->AddRectFilled(p0, p1, i < partials.get() ? voiceColor(v, 0.06f + 0.94f * t, 0.25f + 0.75f * t) : IM_COL32(255, 255, 255, 6));
        }
    }
    dl->AddText(ImVec2(o.x + 8, o.y + 4), IM_COL32(255, 255, 255, 110), "VOICES x PARTIALS");
    if(ImGui::IsItemHovered()){
        const ImVec2 m = ImGui::GetIO().MousePos;
        const int s = int((m.x - left) / cw), v = int((m.y - top) / ch);
        if(s >= 0 && s < slots && v >= 0 && v < V && !voiceData[v].ratio.empty()){
            const int i = displayPartial(s);
            const auto& d = voiceData[v];
            const float f = std::abs(midiToHz(d.pitch) * d.ratio[i] + (d.scalars.count("shift") ? d.scalars.at("shift") : 0.0f));
            ImGui::SetTooltip("voice %d  partial %d  %.1f Hz", v + 1, i + 1, f);
        }
    }
}

// A row of points / bars set by dragging across them (harmonic tables, drawn
// filter curves). Right-click: type the values, publish.
bool scAdditiveBox::drawPointEditor(const char* id, ofParameter<std::vector<float>>& p, float lo, float hi,
                                    ImVec2 size, bool bars, const char* title, int points){
    const int n = points > 0 ? points : (bars ? TABLE_SIZE : SHAPE_POINTS);
    auto values = p.get();
    values.resize(n, values.empty() ? lo : values.back());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    bool changed = false;
    auto indexAt = [&](float x){
        const float t = std::clamp((x - o.x) / size.x, 0.0f, 0.9999f);
        return bars ? int(t * n) : int(std::round(t * (n - 1)));
    };
    if(ImGui::IsItemActive()){
        const ImVec2 m = ImGui::GetIO().MousePos;
        const int idx = indexAt(m.x);
        const float val = lo + (hi - lo) * std::clamp(1.0f - (m.y - o.y) / size.y, 0.0f, 1.0f);
        int prev = ImGui::IsItemActivated() || !dragPoint.count(id) ? idx : dragPoint[id];
        // A fast drag skips points: fill the ones in between
        for(int i = std::min(prev, idx); i <= std::max(prev, idx); i++){
            if(values[i] != val){ values[i] = val; changed = true; }
        }
        dragPoint[id] = idx;
    }
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), IM_COL32(10, 12, 20, 255), 4.0f);
    auto yOf = [&](float v){ return o.y + size.y * (1.0f - (std::clamp(v, lo, hi) - lo) / (hi - lo)); };
    const float base = lo < 0 && hi > 0 ? yOf(0) : o.y + size.y;
    if(lo < 0 && hi > 0) dl->AddLine(ImVec2(o.x, base), ImVec2(o.x + size.x, base), IM_COL32(255, 255, 255, 40));
    if(bars){
        const float bw = size.x / n;
        for(int i = 0; i < n; i++){
            const float x0 = o.x + i * bw + 1, x1 = o.x + (i + 1) * bw - 1;
            dl->AddRectFilled(ImVec2(x0, std::min(base, yOf(values[i]))), ImVec2(x1, std::max(base, yOf(values[i]))),
                              IM_COL32(110, 190, 255, 200));
        }
    } else {
        std::vector<ImVec2> pts(n);
        for(int i = 0; i < n; i++) pts[i] = ImVec2(o.x + size.x * i / (n - 1), yOf(values[i]));
        dl->AddPolyline(pts.data(), n, IM_COL32(255, 220, 150, 220), 0, 1.6f);
        for(const auto& pt : pts) dl->AddCircleFilled(pt, 2.0f, IM_COL32(255, 220, 150, 220));
    }
    dl->AddText(ImVec2(o.x + 6, o.y + 3), IM_COL32(255, 255, 255, 110), title);
    if(changed) p = values;
    controlMenu(p.getEscapedName());
    return changed;
}

void scAdditiveBox::group(const char* title){
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", title);
    ImGui::Separator();
}

bool scAdditiveBox::sliderVector(ofParameter<std::vector<float>>& p, const char* format, bool logarithmic){
    auto values = p.get();
    float v = values.empty() ? 0.0f : values[0];
    const float lo = p.getMin().empty() ? 0.0f : p.getMin()[0];
    const float hi = p.getMax().empty() ? 1.0f : p.getMax()[0];
    std::string label = p.getName() + (values.size() > 1 ? " [" + ofToString(values.size()) + "]" : "") + "  " + format;
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool changed = ImGui::SliderFloat(("##" + p.getEscapedName()).c_str(), &v, lo, hi, label.c_str(),
                                            logarithmic ? ImGuiSliderFlags_Logarithmic : 0);
    if(changed) p = std::vector<float>{v};
    if(ImGui::IsItemHovered() && values.size() > 1) ImGui::SetTooltip("%s", toText(values).c_str());
    controlMenu(p.getEscapedName());
    return changed;
}

bool scAdditiveBox::comboVector(ofParameter<std::vector<int>>& p, const std::vector<std::string>& options){
    const auto values = p.get();
    int v = std::clamp(values.empty() ? 0 : values[0], 0, int(options.size()) - 1);
    const std::string preview = p.getName() + ": " + options[v] + (values.size() > 1 ? " ..." : "");
    bool changed = false;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if(ImGui::BeginCombo(("##" + p.getEscapedName()).c_str(), preview.c_str())){
        for(int i = 0; i < (int)options.size(); i++){
            if(ImGui::Selectable(options[i].c_str(), i == v)){ p = std::vector<int>{i}; changed = true; }
        }
        ImGui::EndCombo();
    }
    controlMenu(p.getEscapedName());
    return changed;
}

bool scAdditiveBox::comboInt(ofParameter<int>& p, const std::vector<std::string>& options){
    int v = std::clamp(p.get(), 0, int(options.size()) - 1);
    const std::string preview = p.getName() + ": " + options[v];
    bool changed = false;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if(ImGui::BeginCombo(("##" + p.getEscapedName()).c_str(), preview.c_str())){
        for(int i = 0; i < (int)options.size(); i++){
            if(ImGui::Selectable(options[i].c_str(), i == v)){ p = i; changed = true; }
        }
        ImGui::EndCombo();
    }
    controlMenu(p.getEscapedName());
    return changed;
}

bool scAdditiveBox::sliderInt(ofParameter<int>& p){
    int v = p.get();
    const std::string label = p.getName() + "  %d";
    const bool changed = ImGui::SliderInt(("##" + p.getEscapedName()).c_str(), &v, p.getMin(), p.getMax(), label.c_str());
    if(changed) p = v;
    controlMenu(p.getEscapedName());
    return changed;
}

bool scAdditiveBox::sliderFloat(ofParameter<float>& p, const char* format, bool logarithmic){
    float v = p.get();
    const std::string label = p.getName() + "  " + format;
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool changed = ImGui::SliderFloat(("##" + p.getEscapedName()).c_str(), &v, p.getMin(), p.getMax(), label.c_str(),
                                            logarithmic ? ImGuiSliderFlags_Logarithmic : 0);
    if(changed) p = v;
    controlMenu(p.getEscapedName());
    return changed;
}

bool scAdditiveBox::checkbox(ofParameter<bool>& p){
    bool v = p.get();
    const bool changed = ImGui::Checkbox(p.getName().c_str(), &v);
    if(changed) p = v;
    controlMenu(p.getEscapedName());
    return changed;
}

void scAdditiveBox::drawSoundTab(){
    if(!ImGui::BeginTable("##absound", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) return;

    group("VOICE");
    sliderVector(pitch, "%.2f");
    sliderFloat(transpose, "%+.2f st");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Added to every voice's Pitch (not stored in presets)");
    sliderVector(levels);
    sliderVector(gate, "%.0f");
    sliderVector(glide, "%.2f s", true);
    sliderFloat(volume, "%.2f");
    if(ImGui::Button("Retrig", ImVec2(-FLT_MIN, 0))){
        trig = std::vector<int>(numVoicesClamped(), 1);
        trig = std::vector<int>(numVoicesClamped(), 0);
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Re-attack every voice (publish Trig to patch it)");
    controlMenu(trig.getEscapedName());

    group("HARMONICS");
    sliderVector(tilt, "%.1f dB/oct");
    sliderVector(stretch, "%.3f");
    sliderVector(inharm, "%.4f", true);
    sliderVector(evenOdd, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("-1: odd partials only  0: all  1: even partials only");
    sliderVector(detune, "%.1f cents");
    sliderVector(shift, "%.0f Hz");

    group("UNISON");
    ImGui::SetNextItemWidth(-FLT_MIN);
    sliderInt(unison);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Copies of every voice (each one more synth)");
    sliderFloat(unisonDetune, "%.1f cents");
    sliderFloat(unisonSpread, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("How far the copies sit from the voice's position");
    sliderFloat(unisonInterval, "%+.2f st");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Harmonizer: copy n is transposed by n x this (12: octaves, 7: fifths)");
    sliderFloat(unisonBlend, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Level of the copies against the voice");

    group("SCATTER");
    sliderVector(density, "%.2f");
    sliderVector(randAmp, "%.2f");
    sliderVector(maskGlide, "%.2f s", true);
    ImGui::SetNextItemWidth(-FLT_MIN);
    sliderInt(seed);
    checkbox(reroll);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("New random draws (mask, levels, times, pans...) on every gate");
    ImGui::SameLine();
    if(ImGui::Button("Draw", ImVec2(-FLT_MIN, 0))){
        for(auto& r : rolls) r++;
        markDirty();
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("New random draws for every voice now");

    ImGui::TableNextRow();

    group("ENVELOPE");
    sliderVector(attack, "%.3f s", true);
    sliderVector(decay, "%.3f s", true);
    sliderVector(sustain, "%.2f");
    sliderVector(release, "%.2f s", true);
    sliderVector(curve, "%.1f");
    sliderVector(envRand, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Shortens each partial's times at random (0: all equal)");
    sliderVector(envTilt, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("+: high partials decay faster (plucks)  -: they ring longer");

    group("MOTION");
    sliderVector(vibrato, "%.2f st");
    sliderVector(vibRate, "%.2f Hz");
    sliderVector(partialVib, "%.3f st");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Each partial wobbles on its own: random depth up to this");
    sliderVector(partialVibRate, "%.2f Hz", true);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Rate of each partial's wobble: 0.5 .. 1.5 x this, at random");
    sliderVector(shimmer, "%.2f");
    sliderVector(shimmerRate, "%.1f Hz");
    sliderVector(noise, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Turns each partial into a narrow band of noise (frequency jitter)");
    sliderVector(noiseRate, "%.0f Hz", true);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Speed of the jitter: low = wobbly, high = breathy");
    sliderVector(noiseTilt, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("-: noise on the low partials  +: on the high ones (air)");
    sliderVector(timeBlur, "%.2f s", true);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Smears every partial's level changes over time (washes)");

    group("SPACE");
    sliderVector(pan, "%.2f");
    sliderVector(spread, "%.2f");
    comboVector(panMode, panModeNames);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Random: partials scattered at random\nSpectrum: low to high partials across the spread\nAlternate: odd on one side, even on the other");
    sliderVector(drift, "%.2f");
    sliderVector(driftRate, "%.2f Hz", true);
    sliderVector(width, "%.2f");

    ImGui::EndTable();
}

void scAdditiveBox::drawSourceTab(){
    if(!ImGui::BeginTable("##absource", 3, ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupColumn("controls", ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch, 1.0f);

    group("SPECTRA A / B");
    comboVector(waveA, waveNames);
    comboVector(waveB, waveNames);
    sliderVector(morph, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("0: spectrum A  1: spectrum B (modulate it in the Modulation tab)");
    sliderVector(harmBlur, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Spreads each partial's level over its neighbours");
    ImGui::TextDisabled("Tilt, stretch and even / odd\nare in Sound > Harmonics.");

    // Drawn tables: copy a waveform into them, then draw
    auto table = [&](const char* name, ofParameter<std::vector<float>>& p, ofParameter<std::vector<int>>& wave){
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s  (used when the wave is Drawn)", name);
        ImGui::SameLine();
        if(ImGui::SmallButton((std::string("From wave##") + name).c_str())){
            const int w = valueFor(wave.get(), 0, 0);
            std::vector<float> t(TABLE_SIZE);
            float peak = 1e-9f;
            for(int i = 0; i < TABLE_SIZE; i++){ t[i] = w == WaveDrawn ? p.get()[i % p.get().size()] : waveLevel(0, w, p.get(), i + 1); peak = std::max(peak, t[i]); }
            for(float& x : t) x /= peak;
            p = t;
            wave = std::vector<int>{WaveDrawn};
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Fill the table with the current wave and switch to Drawn");
        drawPointEditor((std::string("##ab") + name).c_str(), p, 0.0f, 1.0f,
                        ImVec2(ImGui::GetContentRegionAvail().x, 150.0f), true, "harmonics 1 .. 64");
    };
    table("Harmonics A", harmonicsA, waveA);
    table("Harmonics B", harmonicsB, waveB);
    ImGui::EndTable();
}

// ── Sweep tab: controls | one cycle of the shape + the sweep map ──
void scAdditiveBox::drawSweepTab(){
    if(!ImGui::BeginTable("##absweep", 2, ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupColumn("controls", ImGuiTableColumnFlags_WidthStretch, 0.75f);
    ImGui::TableSetupColumn("views", ImGuiTableColumnFlags_WidthStretch, 2.0f);

    group("SWEEP  (a shape moving across the partials)");
    sliderVector(sweepAmt, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Depth (0: no sweep); modulate it in the Modulation tab");
    comboVector(sweepShape, sweepShapeNames);
    sliderVector(sweepSharp, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Width of the shape: 0 wide .. 1 narrow\n(Pulse: duty, Steps: number of steps)");
    sliderVector(sweepSkew, "%+.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Moves the shape's middle earlier (-) or later (+) in the cycle");
    checkbox(sweepInvert);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Peaks become notches");
    ImGui::Spacing();
    sliderVector(sweepWaves, "%.2f", true);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("How many cycles of the shape fit across the partials");
    sliderVector(sweepRate, "%+.3f Hz");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Cycles per second (negative: the other way; 0: still, scan it with Phase)");
    sliderVector(sweepPhase, "%.3f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Offset in the cycle: with Rate 0, patch it to scan by hand");
    comboVector(sweepMode, sweepModeNames);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Partials: cycles evenly spaced in partial number (a comb)\nOctaves: evenly spaced in pitch (a phaser)");
    sliderFloat(sweepVoiceOffset, "%.3f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Voice n starts n x this later in the cycle");
    checkbox(sweepRetrig);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Restart the sweep on every gate / trig");
    ImGui::Spacing();
    sliderVector(sweepLow, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Range: only the partials between Low and High are swept\n(drag the lines on the map)");
    sliderVector(sweepHigh, "%.2f");

    ImGui::TableNextColumn();
    const float w = ImGui::GetContentRegionAvail().x;
    const bool drawn = valueFor(sweepShape.get(), 0, 0) == SweepDrawn;
    if(drawn){
        drawPointEditor("##absweepdrawn", sweepDrawn, 0.0f, 1.0f, ImVec2(w * 0.5f - 4.0f, 110.0f), false,
                        "Drawn: one cycle (drag)", SWEEP_POINTS);
        ImGui::SameLine();
        drawSweepShape(ImVec2(w * 0.5f - 4.0f, 110.0f));
    } else {
        drawSweepShape(ImVec2(w, 110.0f));
    }
    drawSweepMap(ImVec2(w, 230.0f));
    ImGui::EndTable();
}

// Two cycles of voice 1's shape (Skew and Invert applied), with where the
// fundamental is now
void scAdditiveBox::drawSweepShape(ImVec2 size){
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##absweepshape", size);
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), IM_COL32(10, 12, 20, 255), 4.0f);
    const float pad = 6.0f, top = o.y + 20.0f, bottom = o.y + size.y - pad;
    const int N = std::max(32, int(size.x / 2));
    std::vector<ImVec2> pts(N);
    for(int i = 0; i < N; i++){
        const float x = 2.0f * i / (N - 1);
        pts[i] = ImVec2(o.x + pad + (size.x - 2 * pad) * i / (N - 1), bottom - (bottom - top) * sweepShapeAt(0, x - std::floor(x)));
    }
    dl->AddLine(ImVec2(o.x + size.x * 0.5f, top), ImVec2(o.x + size.x * 0.5f, bottom), IM_COL32(255, 255, 255, 25));
    dl->AddPolyline(pts.data(), N, IM_COL32(255, 200, 120, 230), 0, 1.8f);
    // The phase now (approximate: the server's clock is not shared)
    const float rate = valueFor(sweepRate.get(), 0, 0.2f);
    float ph = valueFor(sweepPhase.get(), 0, 0.0f) + rate * ofGetElapsedTimef();
    ph -= std::floor(ph);
    const float px = o.x + pad + (size.x - 2 * pad) * ph * 0.5f;
    dl->AddLine(ImVec2(px, top), ImVec2(px, bottom), IM_COL32(255, 255, 255, 90));
    dl->AddText(ImVec2(o.x + 6, o.y + 3), IM_COL32(255, 255, 255, 110),
                ("SHAPE  " + sweepShapeNames[std::clamp(valueFor(sweepShape.get(), 0, 0), 0, NumSweepShapes - 1)] + " (two cycles)").c_str());
}

// Voice 1's sweep over time: x = partials (or octaves, per Mode), y = the
// next seconds (top: now), brightness = the gain the sweep gives each
// partial. The lines are the range: drag them.
void scAdditiveBox::drawSweepMap(ImVec2 size){
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##absweepmap", size);
    const float pad = 6.0f, left = o.x + pad, right = o.x + size.x - pad, top = o.y + 20.0f, bottom = o.y + size.y - pad;
    auto xOf = [&](float t){ return left + (right - left) * t; };

    // Drag: the nearer of Low / High follows the mouse
    if(ImGui::IsItemActive()){
        const float t = ofClamp((ImGui::GetIO().MousePos.x - left) / (right - left), 0.0f, 1.0f);
        const float lo = valueFor(sweepLow.get(), 0, 0.0f), hi = valueFor(sweepHigh.get(), 0, 1.0f);
        if(ImGui::IsItemActivated()) dragPoint["##absweepmap"] = std::abs(t - lo) < std::abs(t - hi) ? 0 : 1;
        if(dragPoint["##absweepmap"] == 0) sweepLow = std::vector<float>{std::min(t, hi)};
        else sweepHigh = std::vector<float>{std::max(t, lo)};
    }

    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), IM_COL32(10, 12, 20, 255), 4.0f);
    if(voiceData.empty() || !voiceData[0].arrays.count("sph")) return;
    const auto& d = voiceData[0];
    const auto& sph = d.arrays.at("sph");
    const auto& sw = d.arrays.at("sw");
    const int count = std::min(std::max(partials.get(), 1), (int)sph.size());
    const bool octaves = valueFor(sweepMode.get(), 0, 0) == 1;
    const float logCount = std::max(1.0f, std::log2(float(count)));
    const float amt = std::clamp(valueFor(sweepAmt.get(), 0, 0.0f), 0.0f, 1.0f);
    const float rate = valueFor(sweepRate.get(), 0, 0.2f);
    const float phase = valueFor(sweepPhase.get(), 0, 0.0f);
    // Two cycles tall (1 .. 16 s)
    const float span = std::abs(rate) > 1e-4f ? ofClamp(2.0f / std::abs(rate), 1.0f, 16.0f) : 4.0f;
    const float now = ofGetElapsedTimef();
    const int rows = 48;
    const float rh = (bottom - top) / rows;
    auto xOfPartial = [&](int i){
        return octaves ? std::log2(std::max(1.0f, d.ratio[i])) / logCount : float(i) / count;
    };
    for(int r = 0; r < rows; r++){
        const float t = now + span * r / (rows - 1);
        float cyc = phase + rate * t;
        for(int i = 0; i < count; i++){
            const float x0 = xOfPartial(i);
            const float x1 = i + 1 < count ? xOfPartial(i + 1) : 1.0f;
            float x = sph[i] / (SWEEP_SIZE - 1) + cyc;
            x -= std::floor(x);
            const float aw = amt * sw[i];
            const float g = 1.0f - aw * (1.0f - sweepShapeAt(0, x));
            dl->AddRectFilled(ImVec2(xOf(x0), top + r * rh), ImVec2(std::max(xOf(x0) + 1.0f, xOf(x1)), top + (r + 1) * rh),
                              voiceColor(0, 0.15f + 0.85f * g, 0.15f + 0.85f * g));
        }
    }
    // The gain across the partials now, as a line over the map
    std::vector<ImVec2> pts;
    for(int i = 0; i < count; i++){
        float x = sph[i] / (SWEEP_SIZE - 1) + phase + rate * now;
        x -= std::floor(x);
        const float g = 1.0f - amt * sw[i] * (1.0f - sweepShapeAt(0, x));
        pts.push_back(ImVec2(xOf(xOfPartial(i)), top + 3.0f + (1.0f - g) * 30.0f));
    }
    if(pts.size() > 1) dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(255, 255, 255, 200), 0, 1.5f);
    // Range
    for(float t : {valueFor(sweepLow.get(), 0, 0.0f), valueFor(sweepHigh.get(), 0, 1.0f)}){
        const float x = xOf(t);
        dl->AddLine(ImVec2(x, top), ImVec2(x, bottom), IM_COL32(255, 220, 120, 200), 2.0f);
    }
    char info[160];
    std::snprintf(info, sizeof(info), "SWEEP MAP  voice 1, %s across, next %.1f s down  (drag: range)",
                  octaves ? "octaves" : "partials", span);
    dl->AddText(ImVec2(o.x + 6, o.y + 3), IM_COL32(255, 255, 255, 110), info);
    if(amt <= 0) dl->AddText(ImVec2(o.x + size.x * 0.5f - 60, o.y + size.y * 0.5f), IM_COL32(255, 255, 255, 140), "Sweep amount is 0");
}

void scAdditiveBox::drawFiltersTab(){
    if(!ImGui::BeginTable("##abfilters", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) return;

    group("FILTER 1  (per partial, around a moving cutoff)");
    comboVector(filterMode, filterNames);
    sliderVector(cutoff, "%.1f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("MIDI note (%.0f Hz); modulate it in the Modulation tab", midiToHz(valueFor(cutoff.get(), 0, 100.0f)));
    sliderVector(keytrack, "%.2f");
    sliderVector(slope, "%.0f dB/oct");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Comb: teeth every 96 / Slope semitones");
    sliderVector(resonance, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("A bump at the cutoff (Comb: the depth of the teeth)");
    sliderVector(safeBass, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Keeps the fundamental out of filter 1");
    drawPointEditor("##abfiltershape", filterShape, -48.0f, 12.0f, ImVec2(ImGui::GetContentRegionAvail().x, 120.0f), false,
                    "Drawn: -48 .. +48 semitones around the cutoff, -48 .. +12 dB");

    group("FILTER 2  (fixed: vowel formants or a drawn EQ)");
    comboVector(filter2Mode, filter2Names);
    sliderVector(vowel, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("0 A  1 E  2 I  3 O  4 U (in between: a blend)");
    sliderVector(formantShift, "%+.1f st");
    sliderVector(formantWidth, "%.2f", true);
    sliderVector(filter2Mix, "%.2f");
    drawPointEditor("##abeqshape", drawnEq, -48.0f, 12.0f, ImVec2(ImGui::GetContentRegionAvail().x, 120.0f), false,
                    "Drawn EQ: 20 Hz .. 20 kHz, -48 .. +12 dB");

    ImGui::EndTable();
}

void scAdditiveBox::drawModTab(){
    if(ImGui::BeginTable("##abmodsources", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)){
        group("ENV 2  (gated with each voice)");
        sliderFloat(modAttack, "%.3f s", true);
        sliderFloat(modDecay, "%.3f s", true);
        sliderFloat(modSustain, "%.2f");
        sliderFloat(modRelease, "%.3f s", true);
        sliderFloat(modCurve, "%.1f");
        for(int l = 0; l < 2; l++){
            group(l == 0 ? "LFO 1" : "LFO 2");
            comboInt(lfoShape[l], lfoShapeNames);
            sliderFloat(lfoRate[l], "%.3f Hz", true);
            checkbox(lfoRetrig[l]);
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Restart on every gate / trig (each voice its own phase)");
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("MATRIX  (drag a cell; right-click: type, publish)");
    if(!ImGui::BeginTable("##abmatrix", MOD_DESTS + 1, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) return;
    ImGui::TableNextColumn();
    for(int d = 0; d < MOD_DESTS; d++){
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(modDestNames[d]);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", modDestHelp[d]);
    }
    for(int s = 0; s < MOD_SOURCES; s++){
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(modSourceNames[s]);
        for(int d = 0; d < MOD_DESTS; d++){
            ImGui::TableNextColumn();
            auto& p = modAmount[s][d];
            float v = p.get();
            const bool tint = v != 0;   // green: positive, red: negative
            ImGui::SetNextItemWidth(-FLT_MIN);
            if(tint) ImGui::PushStyleColor(ImGuiCol_FrameBg, v > 0 ? IM_COL32(60, 110, 70, 255) : IM_COL32(120, 60, 60, 255));
            if(ImGui::DragFloat(("##" + p.getEscapedName()).c_str(), &v, 0.005f, -1.0f, 1.0f, "%+.2f")) p = ofClamp(v, -1.0f, 1.0f);
            if(tint) ImGui::PopStyleColor();
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s -> %s (%s); double-click: 0", modSourceNames[s], modDestNames[d], modDestHelp[d]);
            if(ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) p = 0.0f;
            controlMenu(p.getEscapedName());
        }
    }
    ImGui::EndTable();
}

// FX panel: EQ (curve editor, half the width) | Echo | Reverb
void scAdditiveBox::drawFxPanel(){
    ImGui::TextDisabled("FX   (EQ -> Echo -> Reverb, after every voice)");
    ImGui::Separator();
    if(!ImGui::BeginTable("##abfx", 3, ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupColumn("eq", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("echo", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("reverb", ImGuiTableColumnFlags_WidthStretch, 1.0f);

    auto header = [&](int stage, const char* title){
        bool on = fxOn[stage].get();
        if(ImGui::Checkbox(title, &on)) fxOn[stage] = on;
        controlMenu(fxOn[stage].getEscapedName());
        if(on && fxPending){
            bool planned = false;
            for(auto& [server, st] : states)
                if(std::find(st.fxPlan.begin(), st.fxPlan.end(), stage) != st.fxPlan.end()) planned = true;
            if(!planned){ ImGui::SameLine(); ImGui::TextDisabled("loading SynthDefs..."); }
        }
    };

    ImGui::TableNextColumn();
    header(FX_EQ, "EQ");
    ImGui::SameLine();
    if(!fxOn[FX_EQ]) ImGui::BeginDisabled();
    sliderFloat(eqMix, "%.2f");
    if(!fxOn[FX_EQ]) ImGui::EndDisabled();
    drawEqEditor(ImGui::GetContentRegionAvail().x - 6.0f, 150.0f * ofxOceanodeShared::getZoomLevel());

    ImGui::TableNextColumn();
    header(FX_ECHO, "Echo");
    if(!fxOn[FX_ECHO]) ImGui::BeginDisabled();
    sliderFloat(echoDelay, "%.3f s", true);
    sliderFloat(echoFeed, "%.2f");
    sliderFloat(echoCutoff, "%.0f (note)");
    comboInt(echoFilter, echoFilterNames);
    sliderFloat(echoResonance, "%.2f");
    sliderFloat(echoMix, "%.2f");
    if(!fxOn[FX_ECHO]) ImGui::EndDisabled();

    ImGui::TableNextColumn();
    header(FX_REVERB, "Reverb (SpaceMaster)");
    if(!fxOn[FX_REVERB]) ImGui::BeginDisabled();
    sliderFloat(revSize, "%.1f");
    sliderFloat(revDecay, "%.2f s", true);
    sliderFloat(revPredelay, "%.0f ms");
    sliderFloat(revLowpass, "%.0f Hz", true);
    sliderFloat(revPosition, "%.2f");
    sliderFloat(revSpread, "%+.2f");
    sliderFloat(revMix, "%.2f");
    if(!fxOn[FX_REVERB]) ImGui::EndDisabled();

    ImGui::EndTable();
}

// scGraphicEQ's curve (scEQEditor) with draggable band handles: drag
// frequency / gain, wheel Q / slope, double-click 0 dB, right-click a handle
// for its values and Publish
void scAdditiveBox::drawEqEditor(float w, float h){
    for(int b = 0; b < 5; b++) eqEditor.bands[b] = {eqFreq[b].get(), eqGain[b].get(), eqShape[b].get()};
    eqEditor.dbRange = 24.0f;
    eqEditor.gainLimit = eqGain[0].getMax();
    eqEditor.qMin = 0.1f; eqEditor.qMax = 20.0f;
    eqEditor.slopeMin = 0.1f; eqEditor.slopeMax = 4.0f;
    if(eqCurveDirty){
        eqEditor.recompute(sampleRate());
        eqCurveDirty = false;
    }
    eqEditor.bandMenu = [this](int b){
        ImGui::TextUnformatted(scEQEditor::bandName(b));
        ImGui::Separator();
        controlMenuItems(eqFreq[b].getEscapedName(), "Freq (Hz)");
        controlMenuItems(eqGain[b].getEscapedName(), "Gain (dB)");
        controlMenuItems(eqShape[b].getEscapedName(), scEQEditor::isShelf(b) ? "Slope" : "Q");
    };
    const bool off = !fxOn[FX_EQ].get();
    if(off) ImGui::BeginDisabled();
    if(eqEditor.draw(std::max(120.0f, w), std::max(60.0f, h), scEQEditor::defaultColors(), !off, nullptr, 0, "##abEqCurve")){
        for(int b = 0; b < 5; b++){
            eqFreq[b] = eqEditor.bands[b].freqHz;
            eqGain[b] = eqEditor.bands[b].gainDb;
            eqShape[b] = eqEditor.bands[b].shape;
        }
    }
    if(off) ImGui::EndDisabled();
}
