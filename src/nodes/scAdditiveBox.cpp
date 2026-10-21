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
const std::vector<std::string> panModeNames = {"Random", "Spectrum", "Alternate", "Channels"};
const std::vector<std::string> layoutNames = {"Ring", "Line"};
const std::vector<std::string> sweepModeNames = {"Partials", "Octaves"};
const std::vector<std::string> sweepShapeNames = {"Lobes", "Triangle", "Saw Up", "Saw Down", "Pulse", "Gauss", "Steps", "Drawn"};
const std::vector<std::string> echoFilterNames = {"LowPass", "HighPass", "BandPass", "PeakEQ"};
// a fixed draw per (salt, partial): the LFO lanes' random phases
float hash01(int salt, int k){
    uint32_t h = uint32_t(salt) * 2654435761u ^ uint32_t(k) * 2246822519u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
    return (h & 0xFFFFFF) / float(0x1000000);
}
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
// display bus channels per voice: D levels, D positions (raw streams), then
// P.Level's factor and the pan offset (the tail's; applied here)
constexpr int displayScalars = 2 * scAdditiveBox::DISPLAY_MAX;
constexpr int displayStride = displayScalars + 2;
constexpr float curveSpan = 128.0f;                             // filter 1 curve: -128..+128 semitones

// Measured on a 44.1 / 48 kHz scsynth: % CPU per partial, and per partial and
// output (PanAz). A rough guide: watch the server's own CPU.
// rough CPU (% per partial): a voice's source, each part in use, each core (+ per output)
constexpr float cpuSrc = 0.004f, cpuPart = 0.0012f, cpuCore = 0.006f, cpuPerPartialOutput = 0.0005f;

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
        timeline.setup(this, [this](const std::string& k){ publishByKey(k); }, [this](const std::string& k){ unpublishByKey(k); },
                       [this](){ return publishedKeys; });
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

    speakerLayout.set("Speaker Layout", 0, 0, 1);

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

    snaps.setupParameters();   // Snapshot, Morph... (scSnapshots, shared by the synths)
    initializePublishActions();

    // ── The shared modulation (after the publish actions: it adds its own) ──
    {
        scModulation::Host h;
        h.node = this;
        h.regF = [this](ofParameter<float>& p, std::function<void()> f){ regMod(p, f); };
        h.regI = [this](ofParameter<int>& p, std::function<void()> f){ regMod(p, f); };
        h.regB = [this](ofParameter<bool>& p, std::function<void()> f){ regMod(p, f); };
        h.regV = [this](ofParameter<std::vector<float>>& p, std::function<void()> f){ regMod(p, f); };
        h.control = [this](const std::string& k) -> ofAbstractParameter* {
            if(k == trig.getEscapedName() || k == gate.getEscapedName()) return nullptr;   // events, not values
            const auto* a = findPublishAction(k);
            return a ? a->param : nullptr;
        };
        h.publishMenu = [this](const std::string& k){ controlMenu(k); };
        h.publishedUnderline = [](const std::string&){};          // controlMenu underlines the control
        h.unpublish = [this](const std::string& k){ unpublishByKey(k); };
        h.publishedKeys = [this]() -> const std::vector<std::string>& { return publishedKeys; };
        h.voices = [this](){ return numVoicesClamped(); };
        mod.partialLfos = true;
        // LFOs to the per-partial targets run in the voices (lanes), not as routes
        mod.skipRoute = [](int s, int t){ return s < scModulation::MAX_LFOS && t >= FIRST_PARTIAL_TARGET; };
        mod.changed = [this](){ modDirty = true; };   // only the modulation's data, not every partial
        // an LFO in a lane: its value on each partial now (voice 1), as the voices compute it
        mod.partialBars = [this](int li) -> std::vector<float> {
            const auto lanes = activeLanes();
            int l = -1;
            for(int k = 0; k < (int)lanes.size(); k++) if(lanes[k].lfo == li) l = k;
            if(l < 0 || l >= (int)laneTables.size() || laneTables[l].size() < 2) return {};
            const auto& tab = laneTables[l];
            const int count = std::clamp(partials.get(), 1, partialCap());
            if(mod.lfoIsNoise(li)){   // the voices' 2D noise: rising counter, a seed per partial (voice 1)
                const auto& curve = laneTables[MAX_LANES + l];
                const float t = (float)ofGetElapsedTimef() * mod.lfoRate(li, 0) + mod.lfoInitPhase(li);
                std::vector<float> out(count);
                for(int i = 0; i < count; i++){
                    const float nz = scModulation::noise2(t, i * mod.lfoNoiseScale(li), mod.lfoNoiseSeed(li));
                    if(curve.size() < 2){ out[i] = nz; continue; }
                    const float q = nz * (curve.size() - 1);
                    const int q0 = std::min((int)q, (int)curve.size() - 2);
                    out[i] = curve[q0] + (curve[q0 + 1] - curve[q0]) * (q - q0);
                }
                return out;
            }
            const float run = (float)ofGetElapsedTimef() * mod.lfoRate(li, 0);
            const float base = mod.lfoPhase(li, 0), spr = mod.lfoPartialSpread(li), rnd = mod.lfoPartialRandom(li);
            std::vector<float> out(count);
            for(int i = 0; i < count; i++){
                float x = run + base + spr * float(i) / count + rnd * hash01(31 + li, i + 1);
                x -= std::floor(x);
                const float p = x * (tab.size() - 1);
                const int i0 = std::min((int)p, (int)tab.size() - 2);
                out[i] = tab[i0] + (tab[i0 + 1] - tab[i0]) * (p - i0);
            }
            return out;
        };
        // the synth reads these, in this order (additiveBox.scd)
        mod.setup(h, {{"Cutoff", 48, "st"}, {"Morph", 1, "", true}, {"Pitch", 12, "st"}, {"Level", 1, "x"}, {"Sweep", 1, ""},
                      {"Pan", 0.5f, "turn"}, {"Noise", 1, "x"}, {"P.Level", 1, "x"}, {"P.Pan", 0.5f, "turn"},
                      {"P.Pitch", 12, "st"}}, true, true, true);   // velocity: the gates' levels
        mod.targetOfKey = {{cutoff.getEscapedName(), 0}, {morph.getEscapedName(), 1}, {pitch.getEscapedName(), 2},
                           {levels.getEscapedName(), 3}, {sweepAmt.getEscapedName(), 4}, {pan.getEscapedName(), 5},
                           {noise.getEscapedName(), 6}};
    }
    {
        scSnapshots::Host h;
        h.folder = "AdditiveBox";
        h.title = "AdditiveBox";
        h.legacyPresetFolder = "Supercollider/AdditiveBox/Presets";
        // ... as a bank (tools/presets_to_bank.py): the bar lists slots and banks only
        h.defaultBank = "Demo and Astres.json";
        h.capture = [this](){ return captureSound(); };
        h.apply = [this](const ofJson& a, const ofJson& b, float t, bool sw){ applySound(a, b, t, sw); };
        h.bpm = [this](){ return currentBpm; };
        h.paramMenu = [this](const std::string& k){ controlMenu(k); };
        snaps.setup(h);
    }
    noteInputs.enable(this, [this](){ for(auto& [server, st] : states) if(st.created) return true; return false; });
    publishByKey(pitch.getEscapedName());
    publishByKey(gate.getEscapedName());
    publishByKey(levels.getEscapedName());

    rolls.assign(MAX_VOICES, 0);
    {
        static std::mt19937 rng((unsigned)ofGetElapsedTimeMicros() ^ 0xADD17u);
        freshSeed = (uint32_t)rng() | 0x10000u;
    }
    lastTrig.assign(MAX_VOICES, 0);
    gateOpen.assign(MAX_VOICES, false);
    for(int v = 0; v < MAX_VOICES; v++) gateOpen[v] = valueFor(gate.get(), v, 0.0f) > 0;

    // ── Listeners ────────────────────────────────────────────────────────────
    auto structure = [this](){
        if(loadingBankPreset) return;
        // New voices start with the gate they are given (no edge, no reroll)
        for(int v = 0; v < MAX_VOICES; v++) gateOpen[v] = valueFor(gate.get(), v, 0.0f) > 0;
        for(auto& [server, st] : states) syncVoices(server);
        mod.voicesChanged();                                                       // the LFOs' voice phases
        for(auto& [server, st] : states) if(st.modCreated) mod.rebuild(server);   // N voices wide
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
    // The timeline's frame set of a value it already sent timetagged: nothing to do.
    nodeListeners.push(gate.newListener([this](std::vector<float>& v){
        if(scScheduling::isBackendSendSuppressed()) return;
        onGate(v);
    }));
    // A scheduled pitch is sent at its instant: the voices recomputed now.
    nodeListeners.push(pitch.newListener([this](std::vector<float>&){
        if(scScheduling::isBackendSendSuppressed()) return;
        markDirty();
        if(noteInputs.isSending()) flushDirty();
    }));
    nodeListeners.push(trig.newListener([this](std::vector<int>& v){ onTrig(v); }));
    nodeListeners.push(seed.newListener([this](int&){ markDirty(); }));
    nodeListeners.push(reroll.newListener([this](bool&){ markDirty(); }));
    for(auto* p : {&sweepInvert, &sweepRetrig})
        nodeListeners.push(p->newListener([this](bool&){ markDirty(); }));
    for(auto* p : {&transpose, &sweepVoiceOffset, &unisonDetune, &unisonSpread, &unisonInterval, &unisonBlend})
        nodeListeners.push(p->newListener([this](float&){ markDirty(); }));
    nodeListeners.push(speakerLayout.newListener([this](int&){ markDirty(); }));
    for(auto* p : {&levels, &glide, &morph, &harmonicsA, &harmonicsB, &harmBlur,
                   &tilt, &stretch, &inharm, &evenOdd, &detune, &shift,
                   &cutoff, &keytrack, &slope, &resonance, &safeBass, &filterShape,
                   &vowel, &formantShift, &formantWidth, &filter2Mix, &drawnEq,
                   &sweepAmt, &sweepWaves, &sweepRate, &sweepSharp, &sweepPhase,
                   &sweepSkew, &sweepLow, &sweepHigh, &sweepDrawn,
                   &density, &randAmp, &maskGlide,
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
    timeline.update();
    snaps.update();
    {   // the snapshot names, to a published Snapshot / Morph A / B dropdown
        const auto names = snaps.slotNames();
        for(const auto& key : snaps.slotKeys()) { auto it = publishedHandles.find(key); if(it != publishedHandles.end()) scDropdowns::setNames(it->second, names); }
    }
    const float now = ofGetElapsedTimef();
    mod.update();   // (showing: set below, from the last frame's drawing)

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
    if(!dirty && modDirty && !voiceData.empty()){
        // a modulation change (an LFO control, the tempo, the routing): the
        // lanes and the VCA flag of each voice, the parts on / off, sent
        modDirty = false;
        computeLaneTables();
        for(int v = 0; v < (int)voiceData.size(); v++) laneData(v, voiceData[v]);
        for(auto& [server, st] : states){
            if(!st.created) continue;
            uploadCurves(server, false);
            for(int v = 0; v < (int)st.voices.size(); v++) syncVoice(server, v, false);
        }
    }
    if(dirty) flushDirty();

    // Display bus of the first server, only while the window is open
    // only while the window was really drawn (not hidden behind a tab or
    // collapsed), 30 times a second
    const bool drawn = windowDrawn;
    windowDrawn = false;
    mod.showing = drawn;
    if(!drawn) liveValid = false;
    else if(now >= liveReadAt){
        liveReadAt = now + 1.0f / 30.0f;
        for(auto& [server, st] : states){
            if(!st.vis || !controlBusIsLive(server, st.vis) || st.voices.empty()) continue;
            liveData = st.vis->readValues;
            liveValid = liveData.size() >= size_t(numVoicesClamped() * displayStride);
            const int shownChannels = 2 * displaySlots();
            for(int v = 0; v < numVoicesClamped(); v++){
                server->requestControlValues(st.vis->index + v * displayStride, shownChannels);
                server->requestControlValues(st.vis->index + v * displayStride + displayScalars, 2);
            }
            break;
        }
    }
}

void scAdditiveBox::flushDirty(){
    auto inEffect = noteInputs.inEffectNow();   // not a scheduled send: the notes as they stand now
    dirty = false;
    modDirty = false;
    computeAll();
    // every voice: parts switched on / off where needed, what changed sent
    for(auto& [server, st] : states){
        if(!st.created) continue;
        uploadCurves(server, false);
        for(int v = 0; v < (int)st.voices.size(); v++) syncVoice(server, v, false);
    }
}

#ifdef OFXOCEANODE_HAS_TIMELINE_INSTRUMENT
bool scAdditiveBox::getTimelineInstrument(ofxOceanodeTimelineInstrument& out){
    if(!gateIn.handle && !pitchIn.handle) return false;
    out.gate = gateIn.handle.get();
    out.pitch = pitchIn.handle.get();
    out.voices = numVoicesClamped();
    return true;
}
#endif

void scAdditiveBox::draw(ofEventArgs&){
    if(showWindow) drawWindow();
    keyboardPiano.finishFrame();
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
// partial (so a preset always sounds the same until a gate rerolls it).
// Seed 0: no seed, draws of this node's own (new ones each time it is made)
float scAdditiveBox::random01(int voice, int salt, int partial) const {
    const int roll = voice < (int)rolls.size() ? rolls[voice] : 0;
    const uint32_t s = seed.get() != 0 ? uint32_t(seed.get()) : freshSeed;
    uint32_t h = hash32(s * 0x9E3779B1U ^ hash32(uint32_t(voice) * 0x85EBCA77U + uint32_t(roll) * 0xC2B2AE3DU));
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

    std::vector<float> fr(P), po(P), pa(P, 0.0f), pbs(P, 0.0f), vf(P), vd(P),
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
        if(pMode == PanChannels){
            // partial k on output (k - 1) % O, round the ring or along the line
            const int O = numOutputsClamped(), c = i % O;
            pos[i] = O == 1 ? 0.5f : O == 2 ? float(c) : speakerLayout.get() == 1 ? float(c) / (O - 1) : float(c) / O;
        }
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


    const float blend = unisonBlend.get();
    const float uGain = (u == 0 ? 1.0f : blend) / std::sqrt(1.0f + blend * blend * (U - 1));

    d.pitch = p;
    d.peak = peak;
    d.view = std::move(view);
    d.arrays = {{"fr", fr}, {"po", po}, {"pa", pa}, {"pd", pd},
                {"vf", vf}, {"vd", vd}, {"nr", nr}, {"nd", nd}, {"shr", shr}, {"sph", sph}, {"sw", sw}, {"bl", bl},
                {"pb", pos}, {"drr", drr}};
    d.scalars = {
        {"pitch", p},
        {"glide", valueFor(glide.get(), v, 0.0f)},
        {"amp", valueFor(levels.get(), v, 0.5f) * uGain},
        {"shift", sh},
        {"maskglide", valueFor(maskGlide.get(), v, 0.05f)},
        {"morph", mo},
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
        {"line", float(speakerLayout.get())},
        {"voice", float(v)},
    };
    laneData(v, d);
    return d;
}

// The modulation's share of a voice's data: the LFO lanes (rate, each
// partial's phase, depths, hold, S&H) and whether an envelope is its VCA.
// A modulation change refreshes only this (not the whole voice).
void scAdditiveBox::laneData(int v, VoiceData& d) const {
    const int P = partialCap();
    const int count = std::clamp(partials.get(), 1, P);
    // LFO lanes: the LFO's rate for this voice, each partial's phase (the
    // voice's phase + Partial Spread cycles across the partials + a random
    // part), the depths (bipolar, scaled to the targets' ranges)
    const auto lanes = activeLanes();
    for(int l = 0; l < MAX_LANES; l++){
        const std::string L = "l" + ofToString(l);
        std::vector<float> ph(P, 0.0f), amt(3, 0.0f);
        float rate = 1.0f;
        if(l < (int)lanes.size()){
            const int li = lanes[l].lfo;
            rate = mod.lfoRate(li, v);
            const float base = mod.lfoPhase(li, v), spr = mod.lfoPartialSpread(li), rnd = mod.lfoPartialRandom(li);
            for(int i = 0; i < P; i++){
                const float x = base + spr * float(i) / std::max(1, count) + rnd * hash01(31 + li, i + 1);
                ph[i] = x - std::floor(x);
            }
            for(int j = 0; j < 3; j++) amt[j] = lanes[l].amt[j];
        }
        d.arrays[L + "ph"] = ph;
        d.arrays[L + "amt"] = amt;
        d.scalars[L + "rate"] = rate;
        d.scalars[L + "reset"] = float(mod.resets());
        d.scalars[L + "sh"] = l < (int)lanes.size() ? mod.lfoSampleHold(lanes[l].lfo) : 0.0f;
        // Noise: the 2D noise per partial (the LaneN part): the partials' seeds
        // (index x Scale) and this LFO's and this voice's own field
        const bool nz = l < (int)lanes.size() && mod.lfoIsNoise(lanes[l].lfo);
        d.scalars[L + "noise"] = nz ? 1.0f : 0.0f;
        d.scalars[L + "nscale"] = nz ? mod.lfoNoiseScale(lanes[l].lfo) : 1.0f;
        d.scalars[L + "init"] = nz ? mod.lfoInitPhase(lanes[l].lfo) : 0.0f;
        d.scalars[L + "seed"] = nz ? mod.lfoNoiseSeed(lanes[l].lfo) + v * 3.71f : 0.0f;
        d.scalars[L + "shg"] = l < (int)lanes.size() ? mod.lfoShGlide(lanes[l].lfo) : 0.0f;
    }
    d.scalars["vca"] = levelEnvelope() ? 1.0f : 0.0f;
}

bool scAdditiveBox::lanesHaveNoise() const {
    for(const auto& lane : activeLanes()) if(mod.lfoIsNoise(lane.lfo)) return true;
    return false;
}

void scAdditiveBox::computeLaneTables(){
    const auto lanes = activeLanes();
    laneTables.assign(2 * MAX_LANES, {});
    for(int l = 0; l < (int)lanes.size(); l++){
        laneTables[l] = mod.lfoTable(lanes[l].lfo, LANE_SIZE);
        laneTables[MAX_LANES + l] = mod.lfoTransfer(lanes[l].lfo, LANE_SIZE);   // the noise's curve
    }
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
    computeLaneTables();
    dataGen++;
}

// The LFOs routed to per-partial targets (P.Level, P.Pan, P.Pitch), each
// with its depths in the targets' units; the first MAX_LANES of them
std::vector<scAdditiveBox::Lane> scAdditiveBox::activeLanes() const {
    std::vector<Lane> lanes;
    const auto& targets = mod.targets();
    if((int)mod.matrix.size() < scModulation::NUM_SOURCES * NUM_TARGETS) return lanes;
    for(int li = 0; li < mod.lfoCount && (int)lanes.size() < MAX_LANES; li++){
        Lane lane;
        bool any = false;
        for(int j = 0; j < 3; j++){
            const int t = FIRST_PARTIAL_TARGET + j;
            const float a = mod.matrix[li * NUM_TARGETS + t];
            lane.amt[j] = a * targets[t].range;
            any = any || std::abs(a) > 1e-4f;
        }
        if(!any) continue;
        lane.lfo = li;
        lanes.push_back(lane);
    }
    return lanes;
}

template<typename T>
void scAdditiveBox::regMod(ofParameter<T>& p, std::function<void()> send){
    addPublishAction(p);
    nodeListeners.push(p.newListener([send](T&){ send(); }));
}

// The modulation synths' groups, before the first voice; scModulation creates
// its buses and synths there
void scAdditiveBox::createMod(ofxSCServer* server){
    auto& st = states[server];
    if(st.modCreated || !st.created) return;
    st.modGroup = new ofxSCGroup(server);
    st.modGroup->create(0, st.root->nodeID);   // the head: before the voices
    st.modSources = new ofxSCGroup(server);
    st.modSources->create(1, st.modGroup->nodeID);
    st.modRoutes = new ofxSCGroup(server);
    st.modRoutes->create(1, st.modGroup->nodeID);
    st.modCreated = true;
    mod.create(server, st.modSources->nodeID, st.modRoutes->nodeID, st.gateBus ? st.gateBus->index : 0);
}

void scAdditiveBox::freeMod(ofxSCServer* server, bool sendFree){
    auto it = states.find(server);
    if(it == states.end()) return;
    auto& st = it->second;
    mod.free(server, false);                       // the group's /n_free takes its synths
    if(sendFree && st.modCreated && st.modGroup) st.modGroup->free();
    delete st.modSources; delete st.modRoutes; delete st.modGroup;
    st.modSources = st.modRoutes = st.modGroup = nullptr;
    st.modCreated = false;
}

// ════════════════════════════════════════════════════════════════════════════
// Synths
// ════════════════════════════════════════════════════════════════════════════

std::string scAdditiveBox::coreDefName() const {
    return "AdditiveBoxCore" + ofToString(chunkSize()) + "_" + ofToString(numOutputsClamped());
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
    // the voices' gates (copy 0 of each voice), then a junk half for the unison copies
    if(st.gateBus && !audioBusIsLive(server, st.gateBus)){ delete st.gateBus; st.gateBus = nullptr; }
    if(!st.gateBus) st.gateBus = new ofxSCBus(RATE_AUDIO, 2 * scModulation::W, server);
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
    for(auto*& buffer : st.laneBufs){
        if(buffer && bufferIsLive(server, buffer)) continue;
        delete buffer;
        buffer = new ofxSCBuffer(LANE_SIZE, 1, server);
        buffer->alloc();
        allocated = true;
    }
    if(allocated){
        for(auto& c : st.sentCurves) c.clear();
        for(auto& c : st.sentSweeps) c.clear();
        for(auto& c : st.sentLanes) c.clear();
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
    for(auto*& b : st.laneBufs){ if(b){ if(bufferIsLive(server, b)) b->free(); delete b; b = nullptr; } }
    if(st.gateBus){ if(audioBusIsLive(server, st.gateBus)) st.gateBus->free(); delete st.gateBus; st.gateBus = nullptr; }
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
    for(int l = 0; l < 2 * MAX_LANES && l < (int)laneTables.size(); l++){
        if(laneTables[l].size() != LANE_SIZE || !st.laneBufs[l] || !bufferIsLive(server, st.laneBufs[l])) continue;
        if(!force && st.sentLanes[l] == laneTables[l]) continue;
        st.sentLanes[l] = laneTables[l];
        ofxOscMessage m;
        m.setAddress("/b_setn");
        m.addIntArg(st.laneBufs[l]->index);
        m.addIntArg(0);
        m.addIntArg(LANE_SIZE);
        for(float x : laneTables[l]) m.addFloatArg(x);
        server->sendMsg(m);
    }
}

// ── Which synth of a voice takes which control ───────────────────────────────
namespace {
enum Role : unsigned { RSrc = 1, RMotion = 2, RLane = 4, RFilt = 8, RSweep = 16, RShim = 32, RBlur = 64,
                       RDrift = 128, RCore = 256, RTail = 512, RAll = 1023 };
const unsigned kPartRole[scAdditiveBox::NumParts] = {RMotion, RLane, RFilt, RSweep, RShim, RBlur, RDrift};
const char* kPartDef[scAdditiveBox::NumParts] = {"AdditiveBoxMotion", "AdditiveBoxLanes", "AdditiveBoxFilt",
                                                  "AdditiveBoxSweep", "AdditiveBoxShim", "AdditiveBoxBlur",
                                                  "AdditiveBoxDrift"};
unsigned roleOf(const std::string& n){
    static const std::map<std::string, unsigned> roles = {
        {"pa", RSrc}, {"pd", RSrc}, {"morph", RSrc}, {"maskglide", RSrc}, {"fr", RSrc}, {"pb", RSrc}, {"vca", RCore},
        {"gatebus", RSrc}, {"rndbus", RSrc}, {"velbus", RSrc}, {"pitchoff", RFilt | RCore},
        {"gate", RSrc | RSweep | RCore | RTail},
        {"vf", RMotion}, {"vd", RMotion}, {"nr", RMotion}, {"nd", RMotion},
        {"fbuf", RFilt}, {"po", RFilt}, {"cutoff", RFilt}, {"keytrack", RFilt}, {"safe", RFilt},
        {"pitch", RFilt | RCore}, {"glide", RFilt | RCore},
        {"sbuf", RSweep}, {"swamt", RSweep}, {"swrate", RSweep}, {"swphase", RSweep}, {"swretrig", RSweep},
        {"sph", RSweep}, {"sw", RSweep},
        {"shimmer", RShim}, {"shr", RShim}, {"bl", RBlur}, {"drift", RDrift}, {"drr", RDrift},
        {"vibdepth", RSrc}, {"vibrate", RSrc}, {"shift", RCore}, {"amp", RCore}, {"width", RCore},
        {"line", RCore}, {"panoff", RCore}, {"out", RCore}, {"silbus", RCore},
        {"vis", RTail}, {"pauser", RTail}, {"grp", RTail},
        {"np", RLane | RCore}, {"first", RLane | RCore},
        {"modbus", RAll}, {"voice", RAll}, {"bus", RAll}};
    if(n.size() >= 2 && n[0] == 'l' && (n[1] == '0' || n[1] == '1')) return RLane;   // l0buf, l1rate...
    auto it = roles.find(n);
    return it == roles.end() ? 0u : it->second;
}
}

float scAdditiveBox::unisonSide(int u) const {
    const int U = unisonClamped();
    if(u <= 0 || U <= 1) return 0.0f;
    const float steps = std::ceil((U - 1) / 2.0f);
    return ((u % 2) ? 1.0f : -1.0f) * std::ceil(u / 2.0f) / steps;
}

// What a unison copy's core has of its own: its pitch (detune, interval), its
// level (blend), its pan offset. The rest is its voice's (the streams too).
std::map<std::string, float> scAdditiveBox::coreScalars(int v, int u) const {
    const int U = unisonClamped();
    const float side = unisonSide(u);
    const float blend = unisonBlend.get();
    const float uGain = (u == 0 ? 1.0f : blend) / std::sqrt(1.0f + blend * blend * (U - 1));
    return {
        {"pitch", valueFor(pitch.get(), v, 48.0f) + transpose.get() + side * unisonDetune.get() / 200.0f + u * unisonInterval.get()},
        {"amp", valueFor(levels.get(), v, 0.5f) * uGain},
        {"panoff", side * unisonSpread.get() * 0.5f},
    };
}

// A part runs only where it changes something
bool scAdditiveBox::partWanted(int part, int v) const {
    switch(part){
        case PartMotion: return valueFor(partialVib.get(), v, 0.0f) > 0.0f || valueFor(noise.get(), v, 0.0f) > 0.0f;
        case PartLane:   return !activeLanes().empty();
        case PartFilt:   return valueFor(filterMode.get(), v, 0) != FilterOff;
        case PartSweep: {
            if(valueFor(sweepAmt.get(), v, 0.0f) > 0.0f) return true;
            for(int s = 0; s < scModulation::NUM_SOURCES; s++)   // something routed to Sweep
                if(mod.sourceUsed(s) && std::abs(mod.matrix[s * NUM_TARGETS + 4]) > 1e-4f) return true;
            return false;
        }
        case PartShim:   return valueFor(shimmer.get(), v, 0.0f) > 0.0f;
        case PartBlur:   return valueFor(timeBlur.get(), v, 0.0f) > 0.0f;
        case PartDrift:  return valueFor(drift.get(), v, 0.0f) > 0.0f;
        default: return false;
    }
}

// The values of one synth of a voice: its role's arrays and scalars (and the
// per-server ones in `extra`), only what changed since last time
void scAdditiveBox::sendTo(VoiceSC& vs, ofxSCSynth* s, unsigned role, const VoiceData& data,
                           const std::map<std::string, float>& extra, bool force){
    if(!s) return;
    auto& sa = vs.sentA[s];
    auto& ss = vs.sentS[s];
    for(const auto& [name, values] : data.arrays){
        if(!(roleOf(name) & role)) continue;
        auto& last = sa[name];
        if(!force && last == values) continue;
        last = values;
        s->set(name, values);
    }
    auto scalar = [&](const std::string& name, float value){
        if(!(roleOf(name) & role) || skipScalars.count(name)) return;
        auto it = ss.find(name);
        if(!force && it != ss.end() && it->second == value) return;
        ss[name] = value;
        s->set(name, value);
    };
    for(const auto& [name, value] : data.scalars) scalar(name, value);
    for(const auto& [name, value] : extra) scalar(name, value);
}

void scAdditiveBox::sendVoice(ofxSCServer* server, int v, const VoiceData& data, bool force){
    auto& st = states[server];
    if(v >= (int)st.voices.size()) return;
    auto& vs = st.voices[v];
    // per server: buses and buffers
    std::map<std::string, float> extra;
    extra["bus"] = vs.bus ? float(vs.bus->index) : 0.0f;
    extra["voice"] = float(v);
    extra["modbus"] = st.modCreated ? float(mod.targetBus(server, 0)) : -1.0f;
    if(st.gateBus) extra["gatebus"] = float(st.gateBus->index);
    extra["rndbus"] = float(st.modCreated ? mod.sourceBus(server, scModulation::SRC_RND) : (st.visJunk ? st.visJunk->index : 0));
    const bool curveReady = !st.curvesPending && v < MAX_VOICES && st.curves[v] && bufferIsLive(server, st.curves[v]);
    extra["fbuf"] = curveReady ? float(st.curves[v]->index) : -1.0f;
    const bool sweepReady = !st.curvesPending && v < MAX_VOICES && st.sweeps[v] && bufferIsLive(server, st.sweeps[v]);
    extra["sbuf"] = sweepReady ? float(st.sweeps[v]->index) : -1.0f;
    for(int l = 0; l < MAX_LANES; l++){
        const bool ready = !st.curvesPending && st.laneBufs[l] && bufferIsLive(server, st.laneBufs[l]);
        extra["l" + ofToString(l) + "buf"] = ready ? float(st.laneBufs[l]->index) : -1.0f;
        auto* cb = st.laneBufs[MAX_LANES + l];
        extra["l" + ofToString(l) + "cbuf"] = !st.curvesPending && cb && bufferIsLive(server, cb) ? float(cb->index) : -1.0f;
    }
    // the gate is a level (a gate rising to 0.2 plays at 0.2); a cable: mapped
    const int gb = gateIn.bus(server), pb = pitchIn.bus(server);
    const float rawPitch = valueFor(pitch.get(), v, 48.0f);
    // (from the value, not gateOpen: scheduled notes move gateOpen ahead of now)
    extra["gate"] = std::max(0.0f, valueFor(gate.get(), v, 0.0f));
    extra["velbus"] = float(st.modCreated ? mod.sourceBus(server, scModulation::SRC_VEL) + v : (st.visJunk ? st.visJunk->index : 0));
    extra["pitchoff"] = pb >= 0 ? transpose.get() : 0.0f;
    skipScalars.clear();
    if(gb >= 0) skipScalars.insert("gate");
    if(pb >= 0) skipScalars.insert("pitch");
    auto mapTo = [&](ofxSCSynth* s, const std::string& name, int bus){
        if(!s || bus < 0) return;
        auto& ss = vs.sentS[s];
        const std::string k = "@" + name;
        auto it = ss.find(k);
        if(!force && it != ss.end() && it->second == (float)bus) return;
        ss[k] = (float)bus;
        ss.erase(name);
        s->mapan(name, bus, 1);
    };
    sendTo(vs, vs.src, RSrc, data, extra, force);
    mapTo(vs.src, "gate", gb >= 0 ? gb + v : -1);
    for(int k = 0; k < NumParts; k++){
        sendTo(vs, vs.parts[k], kPartRole[k], data, extra, force);
        if(kPartRole[k] & RSweep) mapTo(vs.parts[k], "gate", gb >= 0 ? gb + v : -1);
        if(kPartRole[k] & RFilt) mapTo(vs.parts[k], "pitch", pb >= 0 ? pb + v : -1);
    }
    // the chunks: where they sit in the streams, and their share of each phase
    const int P = partialCap(), C = chunkSize();
    const int LC = vs.lanes.empty() ? laneChunkSize() : P / (int)vs.lanes.size();   // the chunks running
    const float junk = st.visJunk ? float(st.visJunk->index) : 0.0f;
    extra["np"] = float(P);
    VoiceData scalarsOnly;
    scalarsOnly.scalars = data.scalars;
    sendTo(vs, vs.laneClock, RLane, scalarsOnly, extra, force);
    for(int c = 0; c < (int)vs.lanes.size(); c++){
        VoiceData d;
        d.scalars = data.scalars;
        for(const auto& [name, values] : data.arrays){
            if(!(roleOf(name) & RLane)) continue;
            const bool perPartial = name.size() == 4 && name.compare(2, 2, "ph") == 0 && (int)values.size() >= (c + 1) * LC;
            d.arrays[name] = perPartial ? std::vector<float>(values.begin() + c * LC, values.begin() + (c + 1) * LC) : values;
        }
        auto e = extra;
        e["first"] = float(c * LC);
        sendTo(vs, vs.lanes[c], RLane, d, e, force);
    }
    for(int u = 0; u < (int)vs.cores.size(); u++){
        auto e = extra;
        for(const auto& [name, value] : coreScalars(v, u)) e[name] = value;
        if(pb >= 0) e["pitchoff"] = e["pitch"] - rawPitch;   // transpose, unison detune / interval
        e["out"] = st.mix ? float(st.mix->index) : 0.0f;
        // copy 0's chunks say when the voice is silent (the tail pauses it)
        e["silbus"] = u == 0 && vs.bus ? float(vs.bus->index + 3 * P) : junk;
        // the copy's own pitch / amp win over the voice's
        VoiceData d;
        d.scalars = data.scalars;
        for(const auto& [name, value] : coreScalars(v, u)) d.scalars[name] = value;
        for(int c = 0; c < (int)vs.cores[u].size(); c++){
            e["first"] = float(c * C);
            sendTo(vs, vs.cores[u][c], RCore, d, e, force);
            mapTo(vs.cores[u][c], "gate", gb >= 0 ? gb + v : -1);
            mapTo(vs.cores[u][c], "pitch", pb >= 0 ? pb + v : -1);
        }
    }
    if(vs.tail){
        auto e = extra;
        e["vis"] = st.vis ? float(st.vis->index + v * displayStride) : junk;
        e["pauser"] = gb < 0 ? 1.0f : 0.0f;   // a gate cable: no pausing (nothing here would resume it)
        e["grp"] = vs.grp ? float(vs.grp->nodeID) : 0.0f;
        sendTo(vs, vs.tail, RTail, scalarsOnly, e, force);
        mapTo(vs.tail, "gate", gb >= 0 ? gb + v : -1);
    }
    skipScalars.clear();
}

// A cable came or went: every voice resent whole (mapped / set again), resumed
void scAdditiveBox::resendAllVoices(){
    for(auto& [server, st] : states){
        for(int v = 0; v < (int)st.voices.size() && v < (int)voiceData.size(); v++){
            auto& vs = st.voices[v];
            for(auto& [synth, ss] : vs.sentS){ ss.erase("@gate"); ss.erase("@pitch"); }
            sendVoice(server, v, voiceData[v], true);
            if(getActive() && vs.grp && vs.grp->isCreated()) vs.grp->run(true);
        }
    }
}

void scAdditiveBox::setInputBus(ofxSCServer* server, scNode* node, int bus){
    if(gateIn.take(server, node, bus) | pitchIn.take(server, node, bus)) resendAllVoices();
}
void scAdditiveBox::resetInputBusses(ofxSCServer* server, int){
    const bool had = gateIn.bus(server) >= 0 || pitchIn.bus(server) >= 0;
    gateIn.reset(server); pitchIn.reset(server);
    if(had) resendAllVoices();
}

void scAdditiveBox::freeVoice(ofxSCServer* server, VoiceSC& vs, bool sendFree){
    if(sendFree && vs.grp && vs.grp->isCreated()) vs.grp->free();   // its synths with it
    delete vs.src;
    for(auto*& p : vs.parts){ delete p; p = nullptr; }
    delete vs.laneClock;
    for(auto* l : vs.lanes) delete l;
    for(auto& copy : vs.cores) for(auto* c : copy) delete c;
    delete vs.tail;
    delete vs.grp;
    if(vs.bus){ if(controlBusIsLive(server, vs.bus)) vs.bus->free(); delete vs.bus; }
    vs = VoiceSC();
}

// One voice: its group and stream bus, the source, the parts it needs (in
// order; the lanes as a clock and chunks), one core per unison copy and chunk,
// the tail; new synths get every value before they start
void scAdditiveBox::syncVoice(ofxSCServer* server, int v, bool force){
    auto& st = states[server];
    if(!st.created || v >= (int)st.voices.size()) return;
    auto& vs = st.voices[v];
    const int P = partialCap(), C = chunkSize(), chunks = P / C;
    if(!vs.grp){
        vs.grp = new ofxSCGroup(server);
        vs.grp->create(1, st.voicesGroup->nodeID);   // voices in order (a closed one pauses itself once silent)
    }
    if(vs.bus && (!controlBusIsLive(server, vs.bus) || vs.bus->channels != 3 * P + STREAM_EXTRA)){
        if(controlBusIsLive(server, vs.bus)) vs.bus->free();
        delete vs.bus;
        vs.bus = nullptr;
    }
    if(!vs.bus) vs.bus = new ofxSCBus(RATE_CONTROL, 3 * P + STREAM_EXTRA, server);
    if((int)voiceData.size() <= v) computeAll();
    const VoiceData& data = voiceData[v];
    std::vector<ofxSCSynth*> fresh;
    auto make = [&](ofxSCSynth*& s, const std::string& defName){
        s = new ofxSCSynth(defName, server);
        fresh.push_back(s);
    };
    auto drop = [&](ofxSCSynth*& s){
        if(!s) return;
        // freed only where it changes nothing (its control at 0): no jump
        s->free();
        vs.sentA.erase(s); vs.sentS.erase(s);
        delete s;
        s = nullptr;
    };
    if(!vs.src) make(vs.src, "AdditiveBoxSrc" + ofToString(P));
    for(int k = 0; k < NumParts; k++){
        if(k == PartLane) continue;
        const bool want = partWanted(k, v);
        auto*& p = vs.parts[k];
        if(want && !p) make(p, kPartDef[k] + ofToString(P));
        else if(!want && p) drop(p);
    }
    // the lanes: a clock and the chunks; with a noise LFO the LaneN variant
    // (another def: all of them again, as one synth was before)
    {
        const bool want = partWanted(PartLane, v);
        const int LC = laneChunkSize();
        const std::string defName = (lanesHaveNoise() ? "AdditiveBoxLanesN" : "AdditiveBoxLanes") + ofToString(LC);
        if(!want || (!vs.lanes.empty() && vs.lanes[0]->getName() != defName) || ((int)vs.lanes.size() != P / LC && !vs.lanes.empty())){
            drop(vs.laneClock);
            for(auto*& l : vs.lanes) drop(l);
            vs.lanes.clear();
        }
        if(want && vs.lanes.empty()){
            make(vs.laneClock, "AdditiveBoxLaneClock");
            vs.lanes.resize(P / LC, nullptr);
            for(auto*& l : vs.lanes) make(l, defName);
        }
    }
    const int U = unisonClamped();
    while((int)vs.cores.size() > U){
        for(auto*& c : vs.cores.back()) drop(c);
        vs.cores.pop_back();
    }
    while((int)vs.cores.size() < U){
        vs.cores.emplace_back(chunks, nullptr);
        for(auto*& c : vs.cores.back()) make(c, coreDefName());
    }
    if(!vs.tail) make(vs.tail, "AdditiveBoxTail" + ofToString(P));
    // values (the new synths: all of them, baked into /s_new)
    for(auto* f : fresh){ vs.sentA.erase(f); vs.sentS.erase(f); }
    sendVoice(server, v, data, force);
    // start the new ones in their place: source first, the parts in order (the
    // lanes at theirs), the cores, the tail last
    std::vector<ofxSCSynth*> order;
    order.push_back(vs.src);
    for(int k = 0; k < NumParts; k++){
        if(k == PartLane){
            if(vs.laneClock) order.push_back(vs.laneClock);
            for(auto* l : vs.lanes) order.push_back(l);
        }else if(vs.parts[k]) order.push_back(vs.parts[k]);
    }
    for(auto& copy : vs.cores) for(auto* c : copy) order.push_back(c);
    order.push_back(vs.tail);
    for(size_t i = 0; i < order.size(); i++){
        auto* s = order[i];
        if(s->isCreated() || std::find(fresh.begin(), fresh.end(), s) == fresh.end()) continue;
        // before the next one that runs already, else at the tail of the voice
        ofxSCSynth* next = nullptr;
        for(size_t k = i + 1; k < order.size() && !next; k++)
            if(std::find(fresh.begin(), fresh.end(), order[k]) == fresh.end()) next = order[k];
        if(next) s->createAndRun(2, next->nodeID, true);
        else s->createAndRun(1, vs.grp->nodeID, true);
    }
}

// Voices / unison changed, or a part switched: the voices to match
void scAdditiveBox::syncVoices(ofxSCServer* server){
    auto& st = states[server];
    if(!st.created) return;
    const int V = numVoicesClamped();
    while((int)st.voices.size() > V){ freeVoice(server, st.voices.back(), true); st.voices.pop_back(); }
    ensureBuses(server);   // the display bus follows the voice count
    computeAll();
    st.voices.resize(V);
    for(int v = 0; v < V; v++) syncVoice(server, v, false);
    uploadCurves(server, false);
}

void scAdditiveBox::sendAll(ofxSCServer* server, bool force){
    auto& st = states[server];
    computeAll();
    for(int v = 0; v < (int)st.voices.size() && v < (int)voiceData.size(); v++) sendVoice(server, v, voiceData[v], force);
    uploadCurves(server, force);
    if(st.out){
        st.out->set("in", st.outIn >= 0 ? st.outIn : (st.mix ? st.mix->index : 0));
        st.out->set("volume", volume.get());
        if(st.outBus >= 0) st.out->set("out", st.outBus);
    }
}

void scAdditiveBox::freeSynths(ofxSCServer* server, bool sendFree){
    auto it = states.find(server);
    if(it == states.end()) return;
    auto& st = it->second;
    const bool groupFree = sendFree && st.root && st.created;
    if(groupFree) st.root->free();   // everything in it
    for(auto& vs : st.voices) freeVoice(server, vs, false);
    st.voices.clear();
    for(auto*& f : st.fx) if(f){ delete f; f = nullptr; }
    st.fxPlan.clear();
    delete st.out; st.out = nullptr;
    freeMod(server, false);
    delete st.voicesGroup; st.voicesGroup = nullptr;
    delete st.root; st.root = nullptr;
    st.created = false;
}

// A new partial cap or output count: the voices again (their parts and
// streams depend on the cap), the output synth and FX for the outputs
void scAdditiveBox::rebuildDefs(){
    const bool outputsChanged = numOutputsClamped() != builtOutputs;
    builtCap = partialCap();
    builtOutputs = numOutputsClamped();
    computeAll();
    for(auto& [server, st] : states){
        if(!st.created) continue;
        ensureBuses(server);
        if(outputsChanged && st.out){
            auto* fresh = new ofxSCSynth(outDefName(), server);
            fresh->set("in", st.mix->index);
            fresh->set("volume", volume.get());
            if(st.outBus >= 0) fresh->set("out", st.outBus);
            fresh->createAndRun(4, st.out->nodeID, true);   // the old one was sent (st.created), confirmed or not
            delete st.out;   // no /n_free: action 4 replaces the node atomically
            st.out = fresh;
            rebuildFx(server);   // the FX synths have one def per output count too
        }
        for(auto& vs : st.voices) freeVoice(server, vs, true);
        syncVoices(server);
    }
}

// After a preset: voice / unison count, partial cap and output count as they
// are now
void scAdditiveBox::applyStructure(){
    for(int v = 0; v < MAX_VOICES; v++) gateOpen[v] = valueFor(gate.get(), v, 0.0f) > 0;
    if(partialCap() != builtCap || numOutputsClamped() != builtOutputs) rebuildDefs();
    for(auto& [server, st] : states) syncVoices(server);
    markDirty();
}

void scAdditiveBox::onGate(const std::vector<float>& values){
    const int V = numVoicesClamped();
    for(int v = 0; v < V; v++){
        const bool open = valueFor(values, v, 0.0f) > 0;
        if(open == gateOpen[v]) continue;
        gateOpen[v] = open;
        if(open && reroll){
            rolls[v]++;   // new random draws, sent before the gate opens
            if((int)voiceData.size() > v){ voiceData[v] = computeVoice(v, 0); dataGen++; }
        }
        for(auto& [server, st] : states){
            if(v >= (int)st.voices.size() || (int)voiceData.size() <= v) continue;
            sendVoice(server, v, voiceData[v], false);   // also the gate
            if(open && getActive() && st.voices[v].grp && st.voices[v].grp->isCreated()) st.voices[v].grp->run(true);
        }
    }
}

void scAdditiveBox::onTrig(const std::vector<int>& values){
    // Rising edges only; `trig` is a trigger control, so a 1 fires once
    const int V = numVoicesClamped();
    for(int v = 0; v < V; v++){
        const int t = valueFor(values, v, 0);
        const bool rise = t > 0 && lastTrig[v] <= 0;
        lastTrig[v] = t;
        if(!rise) continue;
        if(reroll){
            rolls[v]++;
            if((int)voiceData.size() > v){ voiceData[v] = computeVoice(v, 0); dataGen++; }
        }
        for(auto& [server, st] : states){
            if(v >= (int)st.voices.size()) continue;
            auto& vs = st.voices[v];
            if((int)voiceData.size() > v) sendVoice(server, v, voiceData[v], false);
            if(getActive() && vs.grp && vs.grp->isCreated()) vs.grp->run(true);
            if(vs.src) vs.src->set("trig", 1.0f);
            if(vs.parts[PartSweep]) vs.parts[PartSweep]->set("trig", 1.0f);
        }
    }
}

// Only the objects: setOutputBus() comes next, and every value set before
// createSynth() is baked into /s_new
void scAdditiveBox::buildSynth(ofxSCServer* server){
    if(server == nullptr) return;
    freeSynths(server);
    auto& st = states[server];
    ensureBuses(server);
    builtCap = partialCap();
    builtOutputs = numOutputsClamped();
    st.root = new ofxSCGroup(server);
    st.out = new ofxSCSynth(outDefName(), server);
    st.out->set("in", st.mix->index);
    st.out->set("volume", volume.get());
    rebuildFx(server);   // wrappers only: created in createSynth()
}

// The node's group at the head of the default group (the graph moves it):
// [modulation, voices, FX..., output]
void scAdditiveBox::createSynth(ofxSCServer* server){
    auto it = states.find(server);
    if(it == states.end() || !it->second.out || !it->second.root) return;
    auto& st = it->second;
    st.root->create(0, 1);
    st.voicesGroup = new ofxSCGroup(server);
    st.voicesGroup->create(1, st.root->nodeID);
    st.created = true;
    if(!st.out->isCreated()) st.out->createAndRun(1, st.root->nodeID, true);
    for(int stage : st.fxPlan)
        if(st.fx[stage] && !st.fx[stage]->isCreated()) st.fx[stage]->createAndRun(2, st.out->nodeID, true);   // before out, in order
    createMod(server);          // at the head
    syncVoices(server);         // every voice, with the modulation buses
    if(!getActive()) st.root->run(false);
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
    if(it == states.end() || !it->second.created) return;
    auto& st = it->second;
    if(st.out && st.outBus >= 0) st.out->set("out", st.outBus);
    st.root->moveBefore(nodeID);
}

int scAdditiveBox::getLastSynthID(ofxSCServer* server){
    auto it = states.find(server);
    return (it != states.end() && it->second.created && it->second.root) ? it->second.root->nodeID : -1;
}

void scAdditiveBox::activate(){
    // Voices whose gate is off pause themselves again once silent
    for(auto& [server, st] : states){
        if(!st.created) continue;
        st.root->run(true);
        for(auto& vs : st.voices) if(vs.grp && vs.grp->isCreated()) vs.grp->run(true);
    }
}

void scAdditiveBox::deactivate(){
    for(auto& [server, st] : states) if(st.created) st.root->run(false);
}

void scAdditiveBox::resendParametersForNRT(){
    for(auto& [server, st] : states){
        sendAll(server, true);
        for(int stage : st.fxPlan) sendFxParams(stage, st.fx[stage]);
    }
    mod.resend();
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
        // addBefore out, once the node exists (isCreated() waits for /n_go: a stage
        // switched on right after creation would never start)
        if(st.created) synth->createAndRun(2, st.out->nodeID, true);
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
    action.param = p;
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
    for(auto* p : {&snaps.snapshotP, &snaps.morphAP, &snaps.morphBP, &snaps.morphCurveP, &snaps.morphSwitchP}) addPublishAction(*p);
    for(auto* p : {&snaps.morphP, &snaps.morphTimeP}) addPublishAction(*p);
    for(auto* p : {&snaps.morphBeatsP, &snaps.morphGlideP}) addPublishAction(*p);
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
    addPublishAction(speakerLayout, layoutNames);
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
    // Gate and Pitch: audio cables too
    if(key == gate.getEscapedName()){
        gateIn.attach(publishedHandles[key], availableInputs, [this](){ refreshOutputs(); });
        noteInputs.add(publishedHandles[key], gate);
    }
    if(key == pitch.getEscapedName()){
        pitchIn.attach(publishedHandles[key], availableInputs, [this](){ refreshOutputs(); });
        noteInputs.add(publishedHandles[key], pitch);
    }
    parameterGroupChanged.notify(this);
    return true;
}

bool scAdditiveBox::unpublishByKey(const std::string& key){
    auto it = publishedHandles.find(key);
    if(it == publishedHandles.end() || hasPublishedConnection(key)) return false;
    if(key == gate.getEscapedName()){ gateIn.detach(availableInputs); noteInputs.remove(gate); }
    if(key == pitch.getEscapedName()){ pitchIn.detach(availableInputs); noteInputs.remove(pitch); }
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
    timeline.mark(key);
    if(isPublished(key) && !timeline.isHidden(key)){
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
    } else if(timeline.isHidden(key)){
        if(ImGui::MenuItem("Publish to Node GUI")) timeline.reveal(key);
    } else if(isPublished(key)){
        ImGui::TextDisabled("Published in node");
        if(hasPublishedConnection(key)) ImGui::TextDisabled("Connected: disconnect to unpublish");
        else if(ImGui::MenuItem("Unpublish from Node")) timeline.unpublish(key);
    } else if(ImGui::MenuItem("Publish to Node GUI")){
        publishByKey(key);
    }
    timeline.menuItems(key, title);
    mod.modulateMenu(key);   // a modulation target: route sources to it
    ImGui::PopID();
}

// ════════════════════════════════════════════════════════════════════════════
// Presets
// ════════════════════════════════════════════════════════════════════════════

void scAdditiveBox::presetSave(ofJson& json){
    mod.saveLfoOutputs(json);
    json["publishedEditorParameters"] = publishedKeys;
    timeline.save(json, "timelineHiddenEditorParameters");
    json["viewHeight"] = viewHeight;
    json["bankPreset"] = snaps.currentPreset;
    // Window-only controls; published ones are saved by Oceanode as usual
    for(const auto& action : publishActions)
        if(action.save && !isPublished(action.key)) action.save(json);
    mod.patchBases(json);   // a modulated control: its own value
    mod.save(json, "additiveBoxMod");
}

void scAdditiveBox::loadBeforeConnections(ofJson& json){
    mod.loadLfoOutputs(json);
    if(!json.contains("publishedEditorParameters") || !json["publishedEditorParameters"].is_array()) return;
    try { syncPublished(json["publishedEditorParameters"].get<std::vector<std::string>>()); } catch(...) {}
    timeline.load(json, "timelineHiddenEditorParameters");
}

void scAdditiveBox::presetRecallBeforeSettingParameters(ofJson& json){
    loadBeforeConnections(json);
}

void scAdditiveBox::presetRecallAfterSettingParameters(ofJson& json){
    if(json.contains("bankPreset") && json["bankPreset"].is_string()) snaps.currentPreset = json["bankPreset"].get<std::string>();
    if(json.contains("viewHeight") && json["viewHeight"].is_number())
        viewHeight = ofClamp(json["viewHeight"].get<float>(), 140.0f, 2000.0f);
    for(const auto& action : publishActions){
        if(isPublished(action.key) || !json.contains(action.key) || !action.load) continue;
        action.load(json[action.key]);
    }
    mod.load(json, "additiveBoxMod");
    migrateOldModulation(json);
    migrateOldEnvelope(json);
    mod.afterLoad();
}

// An envelope routed to Level: it is the voices' VCA (the core's \vca)
bool scAdditiveBox::levelEnvelope() const {
    for(int e = 0; e < mod.envCount; e++)
        if(std::abs(mod.matrix[(scModulation::SRC_ENV + e) * NUM_TARGETS + 3]) > 1e-4f) return true;
    return false;
}

// Presets from before the built-in per-partial envelope was removed: its
// ADSR becomes an envelope routed to Level (the VCA), so they sound as before
void scAdditiveBox::migrateOldEnvelope(const ofJson& json){
    if(!json.contains("Attack") && !json.contains("Release")) return;   // not such a preset
    if(levelEnvelope()) return;                                          // it has one already
    if(mod.envCount >= scModulation::MAX_ENVS) return;
    auto first = [&](const char* key, float fallback){
        if(!json.contains(key)) return fallback;
        const auto& v = json[key];
        if(v.is_number()) return v.get<float>();
        if(v.is_array() && !v.empty() && v[0].is_number()) return v[0].get<float>();
        return fallback;
    };
    // the new envelope first, the others after it
    const int e = mod.envCount;
    mod.envCount++;
    const std::string E = "Env" + ofToString(e + 1) + "_";
    auto set = [&](const std::string& key, const ofJson& value){
        if(const auto* a = findPublishAction(key)) if(a->load) a->load(value);
    };
    set(E + "Attack", std::max(0.001f, first("Attack", 0.01f)));
    set(E + "Decay", std::max(0.001f, first("Decay", 0.5f)));
    set(E + "Sustain", ofClamp(first("Sustain", 1.0f), 0.0f, 1.0f));
    set(E + "Release", std::max(0.001f, first("Release", 2.0f)));
    for(const char* c : {"_Attack_Curve", "_Decay_Curve", "_Release_Curve"})
        set("Env" + ofToString(e + 1) + c, first("Curve", -4.0f));
    mod.matrix[(scModulation::SRC_ENV + e) * NUM_TARGETS + 3] = 1.0f;
}

// Presets from before the shared modulation: Env2 and LFO 1 / 2 with their
// amounts become Env 1, LFO 1 / 2 and the matrix (the same ranges)
void scAdditiveBox::migrateOldModulation(const ofJson& json){
    if(json.contains("additiveBoxModMatrix")) return;
    static const char* dests[7] = {"Cutoff", "Morph", "Pitch", "Level", "Sweep", "Pan", "Noise"};
    static const char* sources[3] = {"Env2", "LFO1", "LFO2"};
    float amount[3][7] = {};
    bool any[3] = {false, false, false};
    for(int s = 0; s < 3; s++)
        for(int d = 0; d < 7; d++){
            const std::string key = std::string(sources[s]) + "_" + dests[d];
            if(!json.contains(key) || !json[key].is_number()) continue;
            amount[s][d] = json[key].get<float>();
            any[s] = any[s] || std::abs(amount[s][d]) > 1e-4f;
        }
    if(!any[0] && !any[1] && !any[2]) return;
    auto set = [&](const std::string& key, const ofJson& value){
        if(const auto* a = findPublishAction(key)) if(a->load) a->load(value);
    };
    auto old = [&](const std::string& key, float fallback){
        return json.contains(key) && json[key].is_number() ? json[key].get<float>() : fallback;
    };
    std::fill(mod.matrix.begin(), mod.matrix.end(), 0.0f);
    if(any[0]){
        mod.envCount = std::max(mod.envCount, 1);
        set("Env1_Attack", std::max(0.001f, old("Env2_Attack", 0.01f)));
        set("Env1_Decay", std::max(0.001f, old("Env2_Decay", 0.5f)));
        set("Env1_Sustain", old("Env2_Sustain", 0.0f));
        set("Env1_Release", std::max(0.001f, old("Env2_Release", 0.5f)));
        for(const char* c : {"Env1_Attack_Curve", "Env1_Decay_Curve", "Env1_Release_Curve"}) set(c, old("Env2_Curve", -4.0f));
        for(int d = 0; d < 7; d++) mod.matrix[scModulation::SRC_ENV * NUM_TARGETS + d] = amount[0][d];
    }
    for(int l = 0; l < 2; l++){
        if(!any[l + 1]) continue;
        mod.lfoCount = std::max(mod.lfoCount, l + 1);
        const std::string L = "LFO" + ofToString(l + 1) + "_";
        set(L + "Sync", false);
        set(L + "Hz", std::vector<float>{old(L + "Rate", 1.0f)});
        // 0 sine, 1 triangle, 2 saw, 3 square, 4 S&H
        const int shape = (int)old(L + "Shape", 0);
        set(L + "Round", std::vector<float>{shape == 3 ? 1.0f : (shape == 1 || shape == 2) ? 0.0f : 0.5f});
        set(L + "Skew", std::vector<float>{shape == 2 ? 1.0f : 0.0f});
        set(L + "Noise", shape == 4);
        for(int d = 0; d < 7; d++) mod.matrix[l * NUM_TARGETS + d] = amount[l + 1][d];
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Preset bank
// ════════════════════════════════════════════════════════════════════════════

// Played, not part of a sound: a preset leaves them as they are
bool scAdditiveBox::notInPresets(const std::string& key) const {
    return key == gate.getEscapedName() || key == trig.getEscapedName() || key == transpose.getEscapedName()
        || scSnapshots::isOwnKey(key);
}

// The sound (a snapshot / a named preset): Voices, every control a preset holds, the modulation
ofJson scAdditiveBox::captureSound() const {
    ofJson json;
    json["Voices"] = numVoices.get();
    for(const auto& action : publishActions)
        if(action.save && !notInPresets(action.key)) action.save(json);
    mod.patchBases(json);
    mod.save(json, "additiveBoxMod");
    return json;
}

// Between two sounds: numbers glide, the rest (voices, partials, modes, the
// routing) switches at the switch point; the end is a whole load
void scAdditiveBox::applySound(const ofJson& a, const ofJson& b, float t, bool sw){
    if(t >= 1.0f && sw){ loadSound(b); return; }
    scSnapshots::blendActions(publishActions, a, b, t, sw, [this](const std::string& k){ return notInPresets(k); });
    const ofJson& src = sw ? b : a;
    if(src.contains("Voices") && src["Voices"].is_number_integer() && src["Voices"].get<int>() != numVoices.get())
        numVoices = src["Voices"].get<int>();
    if(&src != modSource && src.contains("additiveBoxModMatrix")){
        modSource = &src;
        mod.load(src, "additiveBoxMod");
        mod.afterLoad();
    }
}

void scAdditiveBox::loadSound(const ofJson& json){
    modSource = nullptr;
    // Every control back to its default, then the preset's values; the synths
    // change once afterwards (a voice count, partial cap or output change each
    // replacing every synth on its own made a burst of large /s_new on the
    // server: audio dropouts). Outputs, Gate, Trig and Transpose are not loaded.
    loadingBankPreset = true;
    const int voicesBefore = numVoicesClamped();
    for(const auto& action : publishActions)
        if(action.reset && !notInPresets(action.key)) action.reset();
    std::fill(mod.matrix.begin(), mod.matrix.end(), 0.0f);
    mod.lfoCount = mod.envCount = mod.extCount = 0;
    numVoices = json.contains("Voices") && json["Voices"].is_number_integer() ? json["Voices"].get<int>() : 1;
    for(const auto& action : publishActions){
        if(!action.load || notInPresets(action.key) || !json.contains(action.key)) continue;
        action.load(json[action.key]);
    }
    mod.load(json, "additiveBoxMod");
    migrateOldModulation(json);
    migrateOldEnvelope(json);
    // A new voice count: the modulation N voices wide before it is sent (the
    // structure listener waits for the load, and scModulation sends nothing
    // while built for another count: no Level route, a silent VCA)
    if(numVoicesClamped() != voicesBefore){
        mod.voicesChanged();
        for(auto& [server, st] : states) if(st.modCreated) mod.rebuild(server);
    }
    mod.afterLoad();
    loadingBankPreset = false;
    applyStructure();
}

// ════════════════════════════════════════════════════════════════════════════
// Window
// ════════════════════════════════════════════════════════════════════════════

int scAdditiveBox::displaySlots() const { return std::min(partialCap(), int(DISPLAY_MAX)); }

int scAdditiveBox::displayPartial(int slot) const { return slot * (partialCap() / displaySlots()); }

float scAdditiveBox::liveLevel(int voice, int slot) const {
    if(!liveValid) return -1;
    const size_t i = size_t(voice) * displayStride + slot, k = size_t(voice) * displayStride + displayScalars;
    if(k >= liveData.size() || !std::isfinite(liveData[i]) || !std::isfinite(liveData[k])) return -1;
    return std::max(0.0f, liveData[i] * liveData[k]);   // x P.Level's factor, as the cores
}

// A slot's position as the cores pan it: + the pan offset, folded into 0..1 on
// 2 outputs, and with Line on more (NAN if no data)
float scAdditiveBox::livePos(int voice, int slot) const {
    if(!liveValid) return NAN;
    const size_t i = size_t(voice) * displayStride + displaySlots() + slot, k = size_t(voice) * displayStride + displayScalars + 1;
    if(k >= liveData.size() || !std::isfinite(liveData[i]) || !std::isfinite(liveData[k])) return NAN;
    float p = liveData[i] + liveData[k];
    const int O = numOutputsClamped();
    if(O == 2 || (O > 2 && speakerLayout.get() > 0)){
        p -= 2.0f * std::floor(p * 0.5f);   // SuperCollider's fold(0, 1)
        if(p > 1.0f) p = 2.0f - p;
    }
    return p;
}

void scAdditiveBox::drawWindow(){
    const std::string title = getWindowTitle();
    bool open = showWindow.get();
    ImGui::SetNextWindowSize(ImVec2(1180.0f, 900.0f), ImGuiCond_FirstUseEver);
    if(ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)){
        if(open) keyboardPiano.draw(pitch, gate, numVoicesClamped());
        windowDrawn = true;
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

        // voices x partials
        (void)spacing;
        drawMatrix(ImVec2(w, stripHeight));

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
            auto modTab = [&](const char* name, std::function<void(float)> body){
                tab(name, [this, body](){
                    ImGui::Spacing();
                    body(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize);
                });
            };
            modTab("Envelopes", [this](float w){ mod.drawEnvelopes(w); });
            modTab("LFOs", [this](float w){
                mod.drawLfos(w);
                ImGui::Spacing();
                // which LFOs run per partial
                int routed = 0;
                for(int li = 0; li < mod.lfoCount; li++)
                    for(int t = FIRST_PARTIAL_TARGET; t < NUM_TARGETS; t++)
                        if(std::abs(mod.matrix[li * NUM_TARGETS + t]) > 1e-4f){ routed++; break; }
                ImGui::TextDisabled("An LFO routed to P.Level, P.Pan or P.Pitch runs per partial inside each voice: each partial its own "
                                    "phase (Partials: Spread, Rand). Every other target: one value per voice.");
                if(routed > MAX_LANES)
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Only the first %d LFOs routed to per-partial targets run (%d are)",
                                       MAX_LANES, routed);
            });
            modTab("Routing", [this](float w){ mod.drawRouting(w); });
            tab("FX", [this](){ drawFxPanel(); });
            ImGui::EndTabBar();
        }
        ImGui::PopID();
    }
    ImGui::End();
    if(open != showWindow.get()) showWindow = open;
}

void scAdditiveBox::drawHeader(){
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
    float cpu = 0;
    int partsOn = 0;
    for(int v = 0; v < V; v++){
        int parts = 0;
        for(int k = 0; k < NumParts; k++) parts += partWanted(k, v) ? 1 : 0;
        partsOn = std::max(partsOn, parts);
        cpu += cap * (cpuSrc + parts * cpuPart + U * (cpuCore + cpuPerPartialOutput * O));
    }
    ImGui::TextDisabled("%d x %d x %d partials -> %d out, %d parts on | ~%.0f%% CPU, every gate open",
                        V, U, cap, O, partsOn, cpu);
    if(ImGui::IsItemHovered()){
        ImGui::SetTooltip("Each voice: a source, the parts in use (Partial Vib / Noise, partial LFOs, Filter 1,\n"
                          "Sweep, Shimmer, Time Blur, Drift: each runs only while it changes something), and\n"
                          "a sine bank per unison copy. The smallest of 8 / 16 / 32 / 64 / 128 / 256 partials\n"
                          ">= Partials runs (the extra ones are silent but still cost). A voice whose gate is\n"
                          "off pauses once silent and costs nothing. Rough estimate: watch the server's CPU.");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if(ImGui::IsItemHovered()){
        ImGui::SetTooltip("Every per-voice control takes one value per voice (a vector), or one value for all.\n"
                          "Right-click a control to publish it to the node or type a vector.\n"
                          "Spectrum: drag to move filter 1's cutoff (x) and resonance (y), wheel: slope,\n"
                          "double-click: filter 1 on / off.");
    }
    // snapshots, named presets, morph (scSnapshots)
    snaps.drawBar(std::max(1.0f, ImGui::GetContentRegionAvail().x));
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

    // The static parts (every partial's level at rest, the filter curves):
    // computed again only when the voices' data or the view's size change
    const int V = std::min(numVoicesClamped(), (int)voiceData.size());
    const int slots = displaySlots();
    const int P = partialCap();
    const int N = std::max(32, int(size.x / 3));
    if(specCache.gen != dataGen || specCache.w != size.x || specCache.h != size.y){
        specCache.gen = dataGen; specCache.w = size.x; specCache.h = size.y;
        specCache.stat.assign(V, {});
        for(int v = 0; v < V; v++){
            const auto& d = voiceData[v];
            if(d.ratio.empty() || d.view.empty()) continue;
            const float f0 = midiToHz(d.pitch);
            const float sh = d.scalars.count("shift") ? d.scalars.at("shift") : 0.0f;
            const float peak = std::max(1e-9f, d.peak);
            for(int i = 0; i < P && i < (int)d.ratio.size(); i++){
                if(d.view[i] <= 0) continue;
                const float db = 20.0f * std::log10(d.view[i] / peak);
                if(db < dbMin) continue;
                specCache.stat[v].push_back(ImVec2(xOf(std::abs(f0 * d.ratio[i] + sh)) - o.x, yOf(db) - o.y));
            }
        }
        specCache.f2.clear();
        specCache.f1.clear();
        if(valueFor(filter2Mode.get(), 0, 0) != Filter2Off)
            for(int i = 0; i < N; i++){
                const float x = o.x + size.x * i / (N - 1);
                specCache.f2.push_back(ImVec2(x - o.x, yOf(20.0f * std::log10(std::max(filter2Gain(0, fOf(x)), 1e-6f))) - o.y));
            }
        if(mode0 != FilterOff){
            const float cut = voiceCutoff(0);
            for(int i = 0; i < N; i++){
                const float x = o.x + size.x * i / (N - 1);
                specCache.f1.push_back(ImVec2(x - o.x, yOf(20.0f * std::log10(std::max(filter1Gain(0, hzToMidi(fOf(x)) - cut), 1e-6f))) - o.y));
            }
        }
    }
    // Partials, back voices first: a glow, the line, a spark at the top
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
        if(live && v < (int)specCache.stat.size()){
            const ImU32 dim = voiceColor(v, 0.13f);
            for(const auto& q : specCache.stat[v])
                dl->AddLine(ImVec2(o.x + q.x, y0), ImVec2(o.x + q.x, o.y + q.y), dim, 1.0f);
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

    // Filter 2 of voice 1 (cyan), filter 1 (warm, at its unmodulated cutoff): cached curves
    std::vector<ImVec2> pts;
    auto place = [&](const std::vector<ImVec2>& c){
        pts.resize(c.size());
        for(size_t i = 0; i < c.size(); i++) pts[i] = ImVec2(o.x + c[i].x, o.y + c[i].y);
    };
    if(!specCache.f2.empty()){
        place(specCache.f2);
        dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(120, 230, 255, 160), 0, 1.4f);
    }
    if(mode0 != FilterOff && !specCache.f1.empty()){
        const float cut = voiceCutoff(0);
        place(specCache.f1);
        dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(255, 255, 255, 40), 0, 6.0f);
        dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(255, 240, 200, 210), 0, 1.6f);
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
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("0: no seed (draws of this node's own, new each time it is made)\nElse: the same draws every time");
    checkbox(reroll);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("New random draws (mask, levels, times, pans...) on every gate");
    ImGui::SameLine();
    if(ImGui::Button("Draw", ImVec2(-FLT_MIN, 0))){
        for(auto& r : rolls) r++;
        markDirty();
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("New random draws for every voice now");

    ImGui::TableNextRow();

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
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Random: partials scattered at random\nSpectrum: low to high partials across the spread\nAlternate: odd on one side, even on the other\nChannels: partial 1 on output 1, 2 on 2... (round the outputs again)");
    sliderVector(drift, "%.2f");
    sliderVector(driftRate, "%.2f Hz", true);
    sliderVector(width, "%.2f");
    if(numOutputsClamped() > 2){
        comboInt(speakerLayout, layoutNames);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Ring: the outputs round a circle (position 0 and 1 meet)\nLine: from the first output to the last");
    }
    drawSpeakers(ImVec2(ImGui::GetContentRegionAvail().x, 110.0f * ofxOceanodeShared::getZoomLevel()));

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

// ════════════════════════════════════════════════════════════════════════════
// Speakers: every voice's partials where they sound now (the display bus:
// position and level of up to 64 partials per voice), over the outputs —
// a ring (Outputs >= 3, Ring), or a line (stereo, or Line)
// ════════════════════════════════════════════════════════════════════════════

void scAdditiveBox::drawSpeakers(ImVec2 size){
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##abspeakers", size);
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), IM_COL32(10, 12, 20, 255), 6.0f);
    const int O = numOutputsClamped();
    const bool ring = O >= 3 && speakerLayout.get() == 0;
    const ImVec2 c(o.x + size.x * 0.5f, o.y + size.y * 0.5f);
    const float r = std::min(size.x, size.y) * 0.5f - 12.0f;
    const float x0 = o.x + 14.0f, x1 = o.x + size.x - 14.0f;
    // a position (0..1, as the synth reads it) on the drawing
    auto at = [&](float p, float lane){
        if(ring){
            const float a = (p - std::floor(p)) * twoPi - twoPi * 0.25f;
            const float rr = r * (0.55f + 0.4f * lane);
            return ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr);
        }
        float q = O == 2 ? p - 2.0f * std::floor(p * 0.5f) : p;   // stereo folds, a line clips
        if(O == 2 && q > 1.0f) q = 2.0f - q;
        q = ofClamp(q, 0.0f, 1.0f);
        return ImVec2(x0 + (x1 - x0) * q, o.y + 14.0f + (size.y - 28.0f) * lane);
    };
    // the outputs
    if(ring) dl->AddCircle(c, r * 0.95f, IM_COL32(255, 255, 255, 25), 64);
    else dl->AddLine(ImVec2(x0, c.y), ImVec2(x1, c.y), IM_COL32(255, 255, 255, 25));
    for(int k = 0; k < O; k++){
        const float p = O == 1 ? 0.5f : ring ? float(k) / O : float(k) / (O - 1);
        const ImVec2 s = ring ? at(p, 1.1f) : ImVec2(x0 + (x1 - x0) * (O == 1 ? 0.5f : float(k) / (O - 1)), o.y + size.y - 8.0f);
        dl->AddRectFilled(ImVec2(s.x - 4, s.y - 4), ImVec2(s.x + 4, s.y + 4), IM_COL32(200, 200, 210, 160), 1.5f);
    }
    // the partials: live when the voices run, else where they are placed
    const int V = std::min(numVoicesClamped(), (int)voiceData.size());
    const int slots = displaySlots();
    for(int v = 0; v < V; v++){
        const float peak = std::max(1e-9f, voiceData[v].peak);
        const auto it = voiceData[v].arrays.find("pb");
        for(int sl = 0; sl < slots; sl++){
            const int i = displayPartial(sl);
            if(i >= partials.get()) break;
            float p, level;
            const float lp = livePos(v, sl);
            if(std::isfinite(lp)){
                p = lp;
                level = std::max(0.0f, liveLevel(v, sl));
            }else{
                if(it == voiceData[v].arrays.end() || i >= (int)it->second.size()) continue;
                p = it->second[i];
                level = i < (int)voiceData[v].view.size() ? voiceData[v].view[i] : 0.0f;
            }
            const float t = std::sqrt(std::clamp(level / peak, 0.0f, 1.0f));
            if(t < 0.02f) continue;
            const float lane = slots > 1 ? float(sl) / (slots - 1) : 0.5f;   // low partials inside / on top
            dl->AddCircleFilled(at(p, lane), 1.5f + 2.5f * t, voiceColor(v, 0.25f + 0.75f * t));
        }
    }
    dl->AddText(ImVec2(o.x + 8, o.y + 4), IM_COL32(255, 255, 255, 110), "SPEAKERS");
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip("Each voice's partials where they sound now (size: level; low partials %s).\n"
                          "Modulate Pan (the voice) or P.Pan (each partial, with a per-partial LFO).",
                          ring ? "inside" : "on top");
}
