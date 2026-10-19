//
//  scPulsarBox.h
//  ofxOceanodeSuperCollider
//
//  PulsarBox: pulsar synthesis (Curtis Roads), with the nuPG table library
//  (Marcin Pietruszewski, MIT).
//
//  • Every voice emits pulses at its Fundamental (below ~20 Hz a rhythm, above
//    a pitch). Each pulse holds 1..4 formant layers: a pulsaret table read at
//    the layer's formant (Hz, or a ratio of the fundamental) under an envelope
//    table lasting Cycles periods of the formant. The formant sets where the
//    spectrum peaks, the fundamental the harmonics under that peak, Cycles the
//    peak's width (fewer cycles: a wider, brighter peak).
//  • Masking removes pulses: Probability, Burst / Rest and a Sieve of up to 32
//    steps. Every pulse can land on another output (Voice, Cycle, Random,
//    Layer), with per-pulse jitter of rate, formant, level and output.
//  • Synths: one PulsarBox_<voices>_<outputs> (synthdefs/Defaults/PulsarBox,
//    stock GrainBuf). The 8 tables (4 layers x pulsaret / envelope) are held
//    here as 2048 points and written into the server's buffers with /b_setn.
//  • GUI: the node shows Show / Voices / Outputs and what is published
//    (Fundamental and Levels by default). The window shows what is heard: the
//    masking pattern, the pulse train and one pulse drawn from the very tables
//    and values sent, and the pulsar's spectrum (formant peaks over the
//    harmonics). Right-click a control there to publish it to the node.
//  • Modulation: the shared LFOs / envelopes / routing (scModulation), in a
//    group before the synth; targets per voice: Pitch, Formant, Cycles, Level,
//    Probability, Pan (any other control: right-click it, Modulate with). An
//    envelope routed to Level replaces the ASR (a VCA, on the Gate).
//  • Preset bank: data/Supercollider/PulsarBox/Presets.
//

#pragma once

#include "scNode.h"
#include "scTimelineMenu.h"
#include "ofxSCSynth.h"
#include "ofxSCBuffer.h"
#include "ofxSCBus.h"
#include "ofxSCGroup.h"
#include "ofxSCServer.h"
#include "scModulation.h"
#include "scAudioParam.h"
#include "scScheduledInputs.h"
#include "scEnvelopeCurve.h"
#include "scSnapshots.h"
#include "ofxOceanodeShared.h"
#include "imgui.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <vector>

class serverManager;

namespace pulsarbox {

constexpr int TABLE_N = 2048;

inline const std::vector<std::string>& pulsaretNames(){
    static const std::vector<std::string> n = {"Sine", "Sine x2", "Sine x3", "Saw", "Square", "Triangle",
                                               "Pulse 25%", "Sinc", "Spike", "Noise"};
    return n;
}
inline const std::vector<std::string>& envelopeNames(){
    static const std::vector<std::string> n = {"Hann", "Gauss", "Expodec", "Rexpodec", "Rect", "Trapezoid",
                                               "Tukey", "Blackman", "Linear Decay"};
    return n;
}
inline const std::vector<std::string>& formantModeNames(){ static const std::vector<std::string> n = {"Hz", "x Fund"}; return n; }
inline const std::vector<std::string>& panModeNames(){ static const std::vector<std::string> n = {"Voice", "Cycle", "Random", "Layer"}; return n; }

inline bool isBuiltin(const std::string& name, bool envelope){
    const auto& names = envelope ? envelopeNames() : pulsaretNames();
    return std::find(names.begin(), names.end(), name) != names.end();
}

// A built-in table, n points over one cycle (pulsaret) or the whole pulse (envelope)
inline std::vector<float> builtin(const std::string& name, bool envelope, int n = TABLE_N){
    constexpr float pi = 3.14159265358979f;
    std::vector<float> t(n);
    uint32_t seed = 22222;
    for(int i = 0; i < n; i++){
        const float x = float(i) / n;                    // pulsaret: one period, 0..1 excluded
        const float e = float(i) / (n - 1);              // envelope: 0..1 included
        float y = 0;
        if(!envelope){
            if(name == "Sine x2") y = std::sin(2 * pi * 2 * x);
            else if(name == "Sine x3") y = std::sin(2 * pi * 3 * x);
            else if(name == "Saw") y = 1.0f - 2.0f * x;
            else if(name == "Square") y = x < 0.5f ? 1.0f : -1.0f;
            else if(name == "Triangle") y = x < 0.25f ? 4 * x : x < 0.75f ? 2 - 4 * x : 4 * x - 4;
            else if(name == "Pulse 25%") y = x < 0.25f ? 1.0f : -1.0f / 3.0f;
            else if(name == "Sinc"){ const float u = (x - 0.5f) * 16 * pi; y = std::abs(u) < 1e-6f ? 1.0f : std::sin(u) / u; }
            else if(name == "Spike"){ const float u = (x - 0.5f) * 40; y = std::exp(-u * u); }
            else if(name == "Noise"){ seed = seed * 1664525u + 1013904223u; y = (seed >> 8) / float(1 << 24) * 2 - 1; }
            else y = std::sin(2 * pi * x);
        } else {
            if(name == "Gauss"){ const float u = (e - 0.5f) * 6; y = std::exp(-0.5f * u * u); }
            else if(name == "Expodec") y = std::exp(-6 * e) * std::min(1.0f, e * 200);
            else if(name == "Rexpodec") y = std::exp(-6 * (1 - e)) * std::min(1.0f, (1 - e) * 200);
            else if(name == "Rect") y = 1;
            else if(name == "Trapezoid") y = std::min({1.0f, e * 5, (1 - e) * 5});
            else if(name == "Tukey") y = e < 0.25f ? 0.5f - 0.5f * std::cos(4 * pi * e) : e > 0.75f ? 0.5f - 0.5f * std::cos(4 * pi * (1 - e)) : 1;
            else if(name == "Blackman") y = 0.42f - 0.5f * std::cos(2 * pi * e) + 0.08f * std::cos(4 * pi * e);
            else if(name == "Linear Decay") y = (1 - e) * std::min(1.0f, e * 200);
            else y = 0.5f - 0.5f * std::cos(2 * pi * e);
        }
        t[i] = y;
    }
    return t;
}

inline uint16_t rd16(const unsigned char* p){ return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const unsigned char* p){ return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

// First channel of a WAV (8/16/24/32-bit PCM or 32-bit float), empty if unreadable
inline std::vector<float> readWav(const std::string& path){
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if(!file) return {};
    const size_t size = size_t(file.tellg());
    if(size < 44 || size > 64 * 1024 * 1024) return {};
    std::vector<unsigned char> b(size);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(b.data()), std::streamsize(size));
    if(!file || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return {};
    uint16_t format = 0, channels = 0, bits = 0;
    size_t data = 0, dataSize = 0;
    for(size_t i = 12; i + 8 <= size;){
        const size_t len = rd32(b.data() + i + 4), body = i + 8;
        if(!std::memcmp(b.data() + i, "fmt ", 4) && len >= 16 && body + 16 <= size){
            format = rd16(b.data() + body);
            channels = rd16(b.data() + body + 2);
            bits = rd16(b.data() + body + 14);
            if(format == 0xFFFE && len >= 26 && body + 26 <= size) format = rd16(b.data() + body + 24);
        }
        if(!std::memcmp(b.data() + i, "data", 4)){ data = body; dataSize = std::min(len, size - body); break; }
        i = body + len + (len & 1);
    }
    const int width = bits / 8;
    if(!data || channels < 1 || width < 1) return {};
    if(!((format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (format == 3 && bits == 32))) return {};
    const size_t frames = dataSize / (size_t(width) * channels);
    std::vector<float> out(frames);
    for(size_t f = 0; f < frames; f++){
        const unsigned char* p = b.data() + data + f * width * channels;
        float x;
        if(format == 3) std::memcpy(&x, p, 4);
        else if(bits == 8) x = (int(p[0]) - 128) / 128.0f;
        else if(bits == 16) x = int16_t(rd16(p)) / 32768.0f;
        else if(bits == 24){ int32_t v = int32_t(p[0] | (p[1] << 8) | (p[2] << 16)); if(v & 0x800000) v |= ~0xffffff; x = v / 8388608.0f; }
        else x = int32_t(rd32(p)) / 2147483648.0f;
        out[f] = std::isfinite(x) ? x : 0.0f;
    }
    return out;
}

inline std::vector<float> resample(const std::vector<float>& v, int n){
    std::vector<float> out(n, 0.0f);
    if(v.empty()) return out;
    for(int i = 0; i < n; i++){
        const float p = float(i) * (v.size() - 1) / std::max(1, n - 1);
        const size_t a = size_t(p), c = std::min(a + 1, v.size() - 1);
        out[i] = v[a] + (v[c] - v[a]) * (p - a);
    }
    return out;
}

inline void normalize(std::vector<float>& v){
    float peak = 0;
    for(float x : v) peak = std::max(peak, std::abs(x));
    if(peak > 1e-6f) for(float& x : v) x /= peak;
}

// Linear interpolation: a pulsaret wraps (x = cycles), an envelope holds (x = 0..1)
inline float wrapRead(const std::vector<float>& t, double x){
    const double p = (x - std::floor(x)) * t.size();
    const size_t a = size_t(p) % t.size(), c = (a + 1) % t.size();
    return t[a] + (t[c] - t[a]) * float(p - std::floor(p));
}
inline float clampRead(const std::vector<float>& t, double x){
    const double p = std::clamp(x, 0.0, 1.0) * (t.size() - 1);
    const size_t a = size_t(p), c = std::min(a + 1, t.size() - 1);
    return t[a] + (t[c] - t[a]) * float(p - a);
}

inline std::string noteName(float hz){
    if(hz < 16.0f) return "";
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int m = int(std::round(69.0f + 12.0f * std::log2(hz / 440.0f)));
    return std::string(names[((m % 12) + 12) % 12]) + ofToString(m / 12 - 1);
}

inline std::string timeText(float s){
    if(s >= 1.0f) return ofToString(s, 2) + " s";
    if(s >= 0.01f) return ofToString(s * 1000.0f, 1) + " ms";
    return ofToString(s * 1000.0f, 2) + " ms";
}

// Text <-> value for the typed-value popup ("0.2" or "0.1, 0.5, 0.9")
inline std::string toText(float v){ return ofToString(v); }
inline std::string toText(int v){ return ofToString(v); }
inline std::string toText(bool v){ return v ? "1" : "0"; }
inline std::string toText(const std::string& v){ return v; }
template<typename T>
std::string toText(const std::vector<T>& v){
    std::string s;
    for(size_t i = 0; i < v.size(); i++) s += (i ? ", " : "") + ofToString(v[i]);
    return s;
}
inline void fromText(const std::string& s, float& v){ v = ofToFloat(s); }
inline void fromText(const std::string& s, int& v){ v = ofToInt(s); }
inline void fromText(const std::string& s, bool& v){ v = ofToBool(s) || ofToInt(s) != 0; }
inline void fromText(const std::string& s, std::string& v){ v = s; }
template<typename T>
void fromText(const std::string& s, std::vector<T>& v){
    v.clear();
    for(auto& item : ofSplitString(s, ",", true, true)){ T x; fromText(item, x); v.push_back(x); }
}

template<typename T>
T valueFor(const std::vector<T>& v, int i, T fallback){ return v.empty() ? fallback : v[i % v.size()]; }

inline bool bufferIsLive(ofxSCServer* server, ofxSCBuffer* buffer){
    if(!server || !buffer || buffer->index < 0 || buffer->index >= (int)server->buffers.size()) return false;
    return server->buffers[buffer->index] == buffer;
}

inline bool audioBusIsLive(ofxSCServer* server, ofxSCBus* bus){
    if(!server || !bus || bus->index < 0 || bus->index >= (int)server->audioBusses.size()) return false;
    return server->audioBusses[bus->index] == bus;
}

inline ImU32 layerColor(int g, float alpha = 1.0f){
    static const float c[4][3] = {{0.33f, 0.86f, 0.82f}, {0.95f, 0.64f, 0.34f}, {0.70f, 0.55f, 0.96f}, {0.55f, 0.88f, 0.42f}};
    return ImGui::GetColorU32(ImVec4(c[g][0], c[g][1], c[g][2], alpha));
}

} // namespace pulsarbox

class scPulsarBox : public scNode {
public:
    // its window (SC Mixer Pro's "Go to origin synth window" opens and focuses it)
    std::string getWindowTitle() override { return (canvasID == "Canvas" ? "" : canvasID + "/") + "PulsarBox " + ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return &showWindow; }
private:
public:
    static constexpr int MAX_VOICES = 8;     // PulsarBox_<v>_<o>: v = 1..8
    static constexpr int MAX_OUTPUTS = 16;   //                    o = 1..16
    static constexpr int LAYERS = 4;
    static constexpr int SIEVE_MAX = 32;
    static constexpr int TABLE_N = pulsarbox::TABLE_N;

    explicit scPulsarBox(std::vector<serverManager*>) : scNode("PulsarBox") {
        description = "Pulsar synthesis: every voice emits pulses at its Fundamental (a rhythm below ~20 Hz, a pitch "
                      "above), each holding 1..4 formant layers (a pulsaret table at the formant frequency under an "
                      "envelope Cycles long). Masking (probability, burst / rest, sieve), per-pulse output routing "
                      "and jitter; nuPG tables. Show opens the pulse train, the spectrum and every control; "
                      "right-click a control there to publish it to the node.";
    }

    ~scPulsarBox() override {
        for(auto& [server, st] : states){
            if(st.synth){ st.synth->free(); delete st.synth; }
            freeMod(server, st, true);
            if(st.gateBus){
                if(pulsarbox::audioBusIsLive(server, st.gateBus)) st.gateBus->free();
                delete st.gateBus;
                st.gateBus = nullptr;
            }
            for(auto*& b : st.bufs) if(b){ if(pulsarbox::bufferIsLive(server, b)) b->free(); delete b; b = nullptr; }
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Setup
    // ════════════════════════════════════════════════════════════════════════

    void setup() override {
        timeline.setup(this, [this](const std::string& k){ publishByKey(k); }, [this](const std::string& k){ unpublishByKey(k); },
                       [this](){ return publishedKeys; });
        addParameter(showWindow.set("Show", false));
        addParameter(numVoices.set("Voices", 1, 1, MAX_VOICES));
        addParameter(numOutputs.set("Outputs", 2, 1, MAX_OUTPUTS));
        scNode::addOutput("Out");
        // Gate (levels: a gate rising to 0.2 plays at 0.2) and Pitch (MIDI notes:
        // once used, they set the voices' fundamentals): Oceanode values or audio cables
        gateIn.attach(addParameter(gate.set("Gate", {1.0f}, {0.0f}, {1.0f})), availableInputs, [this](){ refreshOutputs(); });
        pitchIn.attach(addParameter(pitchP.set("Pitch", {57.0f}, {0.0f}, {127.0f})), availableInputs, [this](){ refreshOutputs(); });
        noteInputs.add(gateIn.handle, gate);
        noteInputs.add(pitchIn.handle, pitchP);
        noteInputs.enable(this, [this](){ for(auto& [server, st] : states) if(st.synth) return true; return false; });
        // The timeline's frame set of a value it already sent timetagged:
        // nothing to do. A scheduled one is sent at its instant, right away.
        listeners.push(pitchP.newListener([this](std::vector<float>&){
            if(scScheduling::isBackendSendSuppressed()) return;
            pitchTouched = true; viewDirty = true;
            if(noteInputs.isSending()) sendNotes(); else controlsDirty = true;
        }));
        listeners.push(gate.newListener([this](std::vector<float>& v){
            if(scScheduling::isBackendSendSuppressed()) return;
            viewDirty = true;
            if(!gateTouched) for(float x : v) if(x > 0.0f) { gateTouched = true; break; }   // Auto: now gated
            if(noteInputs.isSending()) sendNotes(); else controlsDirty = true;
        }));

        // Window (any of these can be published to the node)
        fundamental.set("Fundamental", {110.0f}, {0.1f}, {2000.0f});
        transpose.set("Transpose", 0.0f, -48.0f, 48.0f);
        glide.set("Glide", 0.0f, 0.0f, 5.0f);
        levels.set("Levels", {0.5f}, {0.0f}, {1.0f});
        // the Amp Envelope on each voice's gate (Gate Mode: whether it plays through it)
        gateMode.set("Gate Mode", 0, 0, 2);
        attack.set("Attack", 0.005f, 0.0f, 10.0f);
        decay.set("Decay", 0.3f, 0.001f, 10.0f);
        sustain.set("Sustain", 1.0f, 0.0f, 1.0f);
        release.set("Release", 0.3f, 0.0f, 10.0f);
        attackCurve.set("Attack Curve", -4.0f, -12.0f, 12.0f);
        decayCurve.set("Decay Curve", -4.0f, -12.0f, 12.0f);
        releaseCurve.set("Release Curve", -4.0f, -12.0f, 12.0f);
        volume.set("Volume", 1.0f, 0.0f, 2.0f);
        rateJitter.set("Rate Jitter", 0.0f, 0.0f, 1.0f);

        numLayers.set("Formants", 1, 1, LAYERS);
        formantShift.set("Formant Shift", 0.0f, -48.0f, 48.0f);
        cyclesScale.set("Cycles Scale", 1.0f, 0.25f, 4.0f);
        formantJitter.set("Formant Jitter", 0.0f, 0.0f, 24.0f);
        levelJitter.set("Level Jitter", 0.0f, 0.0f, 1.0f);
        const float hz[LAYERS] = {880, 1760, 3520, 7040}, ratio[LAYERS] = {8, 16, 24, 32}, lev[LAYERS] = {1, 0.7f, 0.5f, 0.35f};
        for(int g = 0; g < LAYERS; g++){
            const std::string s = " " + std::string(1, char('A' + g));
            formantMode[g].set("Formant Mode" + s, 0, 0, 1);
            formantHz[g].set("Formant" + s, hz[g], 20.0f, 16000.0f);
            formantRatio[g].set("Ratio" + s, ratio[g], 0.25f, 64.0f);
            cycles[g].set("Cycles" + s, 1.0f, 0.1f, 64.0f);
            layerLevel[g].set("Level" + s, lev[g], 0.0f, 1.0f);
            layerDelay[g].set("Delay" + s, 0.0f, 0.0f, 1.0f);
            layerPan[g].set("Pan" + s, 0.0f, -8.0f, 8.0f);
            pulsaret[g].set("Pulsaret" + s, "Sine");
            envelope[g].set("Envelope" + s, "Hann");
        }

        probability.set("Probability", 1.0f, 0.0f, 1.0f);
        burst.set("Burst", 1, 1, 64);
        rest.set("Rest", 0, 0, 64);
        sieve.set("Sieve", {1}, {0}, {1});

        panMode.set("Pan Mode", 0, 0, 3);
        rotate.set("Rotate", 0.0f, 0.0f, 16.0f);
        panJitter.set("Pan Jitter", 0.0f, 0.0f, 8.0f);

        snaps.setupParameters();   // Snapshot, Morph... (scSnapshots, shared by the synths)
        initializePublishActions();
        setupModulation();
        {
            scSnapshots::Host h;
            h.folder = "PulsarBox";
            h.title = "PulsarBox";
            h.presetFolder = "Supercollider/PulsarBox/Presets";   // the named presets it always had
            h.capture = [this](){ return captureSound(); };
            h.apply = [this](const ofJson& a, const ofJson& b, float t, bool sw){ applySound(a, b, t, sw); };
            h.bpm = [this](){ return currentBpm; };
            h.paramMenu = [this](const std::string& k){ controlMenu(k); };
            snaps.setup(h);
        }
        publishByKey(fundamental.getEscapedName());
        publishByKey(levels.getEscapedName());

        // Listeners
        auto controls = [this](){ controlsDirty = true; viewDirty = true; };
        listeners.push(numVoices.newListener([this](int&){ if(!loadingBankPreset) structureDirty = true; viewDirty = true; }));
        listeners.push(numOutputs.newListener([this](int&){ if(!loadingBankPreset) structureDirty = true; viewDirty = true; }));
        for(auto* p : {&fundamental, &levels})
            listeners.push(p->newListener([controls](std::vector<float>&){ controls(); }));
        for(auto* p : {&transpose, &glide, &attack, &decay, &sustain, &release, &attackCurve, &decayCurve, &releaseCurve,
                       &volume, &rateJitter, &formantShift, &cyclesScale,
                       &formantJitter, &levelJitter, &probability, &rotate, &panJitter})
            listeners.push(p->newListener([controls](float&){ controls(); }));
        for(auto* p : {&burst, &rest, &panMode, &gateMode})
            listeners.push(p->newListener([controls](int&){ controls(); }));
        // Formants: a synth with that many layers (the others are not built)
        listeners.push(numLayers.newListener([this, controls](int&){ if(!loadingBankPreset) structureDirty = true; controls(); }));
        listeners.push(sieve.newListener([controls](std::vector<int>&){ controls(); }));
        for(int g = 0; g < LAYERS; g++){
            listeners.push(formantMode[g].newListener([controls](int&){ controls(); }));
            for(auto* p : {&formantHz[g], &formantRatio[g], &cycles[g], &layerLevel[g], &layerDelay[g], &layerPan[g]})
                listeners.push(p->newListener([controls](float&){ controls(); }));
            for(auto* p : {&pulsaret[g], &envelope[g]})
                listeners.push(p->newListener([this](std::string&){ tablesDirty = true; viewDirty = true; }));
        }
        refreshTables();
    }

    // ════════════════════════════════════════════════════════════════════════
    // Frame
    // ════════════════════════════════════════════════════════════════════════

    void update(ofEventArgs&) override {
        timeline.update();
        snaps.update();
        const float now = ofGetElapsedTimef();
        if(pendingDialog){
            const int g = (pendingDialog - 1) / 2, kind = (pendingDialog - 1) % 2;
            pendingDialog = 0;
            auto result = ofSystemLoadDialog(kind ? "Envelope WAV (mono, one pulse)" : "Pulsaret WAV (mono, one cycle)",
                                             false, ofToDataPath("Supercollider/PulsarBox", true));
            if(result.bSuccess){
                const std::string chosen = result.getPath();
                (kind ? envelope[g] : pulsaret[g]) = relativeToData(chosen);
            }
        }
        if(structureDirty){ structureDirty = false; applyStructure(); }
        if(const int g = ampGated() ? 1 : 0; g != ampGatedSent){ ampGatedSent = g; controlsDirty = true; }
        if(tablesDirty){
            tablesDirty = false;
            refreshTables();
            for(auto& [server, st] : states) uploadTables(server, false);
        }
        // Table buffers allocated: write them once scsynth has them
        for(auto& [server, st] : states){
            if(!st.bufsPending) continue;
            if(server->isNRTSyncPending() && now - st.allocTime < 5.0f) continue;
            st.bufsPending = false;
            uploadTables(server, true);
        }
        if(controlsDirty){
            auto inEffect = noteInputs.inEffectNow();   // the notes as they stand now
            controlsDirty = false;
            for(auto& [server, st] : states){ sendAll(server, false); sendGate(server, st); }
        }
        if(modDirty){
            modDirty = false;
            for(auto& [server, st] : states) sendModArgs(server, st);
        }
        mod.showing = showWindow.get();
        mod.update();
        if(showWindow && viewDirty && now - lastViewTime > 0.04f){
            viewDirty = false;
            lastViewTime = now;
            computeView();
        }
    }

    void draw(ofEventArgs&) override { if(showWindow) drawWindow(); }

    // ════════════════════════════════════════════════════════════════════════
    // scNode
    // ════════════════════════════════════════════════════════════════════════

    void buildSynth(ofxSCServer* server) override {
        if(!server) return;
        auto& st = states[server];
        if(st.synth){ st.synth->free(); delete st.synth; st.synth = nullptr; }
        freeMod(server, st, true);
        ensureBuffers(server);
        st.synth = new ofxSCSynth(defName(), server);
        st.builtVoices = voices(); st.builtOutputs = outputs(); st.builtLayers = layers();
        st.sent.clear();
        sendAll(server, true);   // set before /s_new: baked into it
    }
    void createSynth(ofxSCServer* server) override {
        auto it = states.find(server);
        if(it == states.end() || !it->second.synth) return;
        if(!it->second.synth->isCreated()) it->second.synth->createAndRun(0, 1, getActive());
        createMod(server, it->second);   // the head: before the synth
    }
    void free(ofxSCServer* server) override {
        auto it = states.find(server);
        if(it == states.end()) return;
        if(it->second.synth){ it->second.synth->free(); delete it->second.synth; it->second.synth = nullptr; }
        freeMod(server, it->second, true);
        it->second.outBus = -1;
    }
    void setOutputBus(ofxSCServer* server, int index, int bus) override {
        if(index != 0) return;
        auto& st = states[server];
        st.outBus = bus;
        if(st.synth) st.synth->set("out", bus);
    }
    int getOutputBusIndex(ofxSCServer* server, int index) override {
        auto it = states.find(server);
        return index == 0 && it != states.end() ? it->second.outBus : -1;
    }
    void moveSynthBefore(ofxSCServer* server, int nodeID) override {
        auto it = states.find(server);
        if(it == states.end() || !it->second.synth) return;
        auto& st = it->second;
        // The modulation group writes the gate bus read by the main synth.
        // Move both parts as one ordered unit when another graph source is
        // added; moving only the synth can put it ahead of its gate writer.
        st.synth->moveBefore(nodeID);
        if(st.modGroup) st.modGroup->moveBefore(st.synth->nodeID);
    }
    int getLastSynthID(ofxSCServer* server) override {
        auto it = states.find(server);
        if(it == states.end() || !it->second.synth) return -1;
        // Upstream gate and pitch sources must run before ModGate as well.
        return it->second.modGroup ? it->second.modGroup->nodeID : it->second.synth->nodeID;
    }
    void setInputBus(ofxSCServer* server, scNode* node, int bus) override {
        auto& st = states[server];
        const bool g = gateIn.take(server, node, bus), p = pitchIn.take(server, node, bus);
        if(g) sendGate(server, st);
        if(p || g) sendModArgs(server, st);
    }
    void resetInputBusses(ofxSCServer* server, int) override {
        auto& st = states[server];
        gateIn.reset(server); pitchIn.reset(server);
        sendGate(server, st);
        sendModArgs(server, st);
    }
    void activate() override {
        for(auto& [s, st] : states){ if(st.synth) st.synth->run(true); if(st.modGroup) st.modGroup->run(true); }
    }
    void deactivate() override {
        for(auto& [s, st] : states){ if(st.synth) st.synth->run(false); if(st.modGroup) st.modGroup->run(false); }
    }
    void resendParametersForNRT() override {
        for(auto& [server, st] : states){ uploadTables(server, true); sendAll(server, true); sendGate(server, st); sendModArgs(server, st); }
        mod.resend();
    }
    void setBpm(float bpm) override { mod.setBpm(bpm); currentBpm = bpm; }

    // ════════════════════════════════════════════════════════════════════════
    // Presets
    // ════════════════════════════════════════════════════════════════════════

    void presetSave(ofJson& json) override {
        json["publishedEditorParameters"] = publishedKeys;
        timeline.save(json, "timelineHiddenEditorParameters");
        json["viewHeight"] = viewHeight;
        json["bankPreset"] = snaps.currentPreset;
        for(const auto& a : publishActions) if(a.save && !isPublished(a.key)) a.save(json);
        mod.patchBases(json);   // a modulated control: its own value
        mod.save(json, "pulsarBoxMod");
    }
    void loadBeforeConnections(ofJson& json) override {
        if(!json.contains("publishedEditorParameters") || !json["publishedEditorParameters"].is_array()) return;
        try { syncPublished(json["publishedEditorParameters"].get<std::vector<std::string>>()); } catch(...) {}
        timeline.load(json, "timelineHiddenEditorParameters");
    }
    void presetRecallBeforeSettingParameters(ofJson& json) override { loadBeforeConnections(json); }
#ifdef OFXOCEANODE_HAS_TIMELINE_INSTRUMENT
    // Gate (its level is the velocity) and Pitch, one note per voice
    bool getTimelineInstrument(ofxOceanodeTimelineInstrument& out) override {
        if(!gateIn.handle || !pitchIn.handle) return false;
        out.gate = gateIn.handle.get();
        out.pitch = pitchIn.handle.get();
        out.voices = voices();
        return true;
    }
#endif
    void presetRecallAfterSettingParameters(ofJson& json) override {
        if(json.contains("bankPreset") && json["bankPreset"].is_string()) snaps.currentPreset = json["bankPreset"].get<std::string>();
        if(json.contains("viewHeight") && json["viewHeight"].is_number())
            viewHeight = ofClamp(json["viewHeight"].get<float>(), 160.0f, 2000.0f);
        // Saved before Gate Mode: the gate always went through the envelope
        if(!json.contains(gateMode.getEscapedName())) gateMode = 1;
        for(const auto& a : publishActions){
            if(isPublished(a.key) || !json.contains(a.key) || !a.load) continue;
            a.load(json[a.key]);
        }
        gateTouched = false;   // Auto drones again until the Gate is used
        loadModulation(json);
    }

private:
    // ════════════════════════════════════════════════════════════════════════
    // Synth values
    // ════════════════════════════════════════════════════════════════════════

    struct ServerState {
        ofxSCSynth* synth = nullptr;
        int outBus = -1;
        int builtVoices = 0, builtOutputs = 0, builtLayers = 0;
        std::array<ofxSCBuffer*, 2 * LAYERS> bufs{};   // layer g: pulsaret 2g, envelope 2g + 1
        bool bufsPending = false;
        float allocTime = 0;
        std::array<std::vector<float>, 2 * LAYERS> sentTables;
        std::map<std::string, std::vector<float>> sent;
        // modulation: a group at the head (gates, LFOs and envelopes, then routes)
        ofxSCGroup* modGroup = nullptr;
        ofxSCGroup* modSources = nullptr;
        ofxSCGroup* modRoutes = nullptr;
        ofxSCSynth* gateS = nullptr;   // ModGate<voices>: the gates the envelopes follow
        ofxSCBus* gateBus = nullptr;   // audio, 16
        bool modCreated = false;
    };

    int voices() const { return std::clamp(numVoices.get(), 1, MAX_VOICES); }
    int outputs() const { return std::clamp(numOutputs.get(), 1, MAX_OUTPUTS); }
    int layers() const { return std::clamp(numLayers.get(), 1, LAYERS); }
    std::string defName() const {
        return "PulsarBox_" + ofToString(voices()) + "_" + ofToString(outputs()) + "_" + ofToString(layers());
    }
    // The Amp Envelope is the voices' VCA: Gated, or Auto once the Gate is used
    bool ampGated() const {
        if(gateMode.get() == 2) return false;
        if(gateMode.get() == 1) return true;
        return gateTouched || gateIn.connected() || (gateIn.handle && gateIn.handle->isTimelined());
    }

    float voiceFund(int v) const {
        if(pitchMode(nullptr) == 1)   // from Pitch (Oceanode values)
            return std::clamp(440.0f * std::pow(2.0f, (pulsarbox::valueFor(pitchP.get(), v, 57.0f) + transpose.get() - 69.0f) / 12.0f), 0.05f, 4000.0f);
        return std::clamp(pulsarbox::valueFor(fundamental.get(), v, 110.0f) * std::pow(2.0f, transpose.get() / 12.0f), 0.05f, 4000.0f);
    }
    float shiftFactor() const { return std::pow(2.0f, formantShift.get() / 12.0f); }
    float layerHz(int g, float fund) const {
        const float hz = formantMode[g].get() == 1 ? fund * formantRatio[g].get() : formantHz[g].get();
        return std::clamp(hz * shiftFactor(), 1.0f, 20000.0f);
    }
    float layerCycles(int g) const { return std::max(0.01f, cycles[g].get() * cyclesScale.get()); }
    int sieveLength() const { return std::clamp(int(sieve.get().size()), 1, SIEVE_MAX); }
    bool sieveStep(int j) const {
        const auto& s = sieve.get();
        return s.empty() || s[j % sieveLength()] != 0;
    }
    // Pulse j (0 = the first) survives the sieve and burst / rest
    bool pulseOn(int j) const {
        const int b = std::max(1, burst.get()), r = std::max(0, rest.get());
        return sieveStep(j) && (j % (b + r)) < b;
    }

    std::map<std::string, std::vector<float>> synthValues(const ServerState& st) const {
        std::map<std::string, std::vector<float>> c;
        const int V = voices(), L = layers();
        std::vector<float> fund(V), lev(V), gt(V);
        for(int v = 0; v < V; v++){
            fund[v] = voiceFund(v);
            lev[v] = pulsarbox::valueFor(levels.get(), v, 0.5f) * volume.get();
            gt[v] = pulsarbox::valueFor(gate.get(), v, 1.0f);
        }
        c["fund"] = fund; c["lev"] = lev; c["gate"] = gt;
        // the gates' levels from ModGate (Gate values / cable), the pitch cable
        c["gatebus"] = {st.modCreated && st.gateBus ? (float)st.gateBus->index : -1.0f};
        c["tr"] = {transpose.get()};
        c["glide"] = {glide.get()};
        c["atk"] = {attack.get()}; c["dec"] = {decay.get()}; c["sus"] = {sustain.get()}; c["rel"] = {release.get()};
        c["ca"] = {attackCurve.get()}; c["cd"] = {decayCurve.get()}; c["cr"] = {releaseCurve.get()};
        c["gated"] = {ampGated() ? 1.0f : 0.0f};
        std::vector<float> lmode(LAYERS), lhz(LAYERS), lrat(LAYERS), lcyc(LAYERS), lamp(LAYERS),
                           loff(LAYERS), lpan(LAYERS), pbuf(LAYERS), ebuf(LAYERS);
        for(int g = 0; g < LAYERS; g++){
            lmode[g] = float(formantMode[g].get());
            lhz[g] = std::clamp(formantHz[g].get() * shiftFactor(), 1.0f, 20000.0f);
            lrat[g] = formantRatio[g].get() * shiftFactor();
            lcyc[g] = layerCycles(g);
            lamp[g] = layerLevel[g].get();
            loff[g] = layerDelay[g].get();
            lpan[g] = layerPan[g].get();
            pbuf[g] = st.bufs[2 * g] ? float(st.bufs[2 * g]->index) : -1.0f;
            ebuf[g] = st.bufs[2 * g + 1] ? float(st.bufs[2 * g + 1]->index) : -1.0f;
        }
        c["lmode"] = lmode; c["lhz"] = lhz; c["lrat"] = lrat; c["lcyc"] = lcyc;
        c["lamp"] = lamp; c["loff"] = loff; c["lpan"] = lpan; c["pbuf"] = pbuf; c["ebuf"] = ebuf;
        c["rjit"] = {rateJitter.get()}; c["fjit"] = {formantJitter.get()};
        c["ajit"] = {levelJitter.get()}; c["pjit"] = {panJitter.get()};
        c["prob"] = {probability.get()};
        c["burst"] = {float(std::max(1, burst.get()))}; c["rest"] = {float(std::max(0, rest.get()))};
        std::vector<float> sv(SIEVE_MAX, 1.0f);
        for(int i = 0; i < SIEVE_MAX && i < (int)sieve.get().size(); i++) sv[i] = sieve.get()[i] ? 1.0f : 0.0f;
        c["sieve"] = sv; c["slen"] = {float(sieveLength())};
        c["pmode"] = {float(panMode.get())}; c["rot"] = {rotate.get()};
        return c;
    }

    void sendAll(ofxSCServer* server, bool force){
        auto it = states.find(server);
        if(it == states.end() || !it->second.synth) return;
        auto& st = it->second;
        for(const auto& [name, values] : synthValues(st)){
            auto s = st.sent.find(name);
            if(!force && s != st.sent.end() && s->second == values) continue;
            st.sent[name] = values;
            if(values.size() == 1) st.synth->set(name, double(values[0]));
            else st.synth->set(name, values);
        }
        if(force && st.outBus >= 0) st.synth->set("out", st.outBus);
    }

    // Voices / Outputs / Formants changed: the synth replaced in place (/s_new action 4)
    void applyStructure(){
        for(auto& [server, st] : states){
            if(!st.synth || (st.builtVoices == voices() && st.builtOutputs == outputs() && st.builtLayers == layers())) continue;
            auto* old = st.synth;
            st.synth = new ofxSCSynth(defName(), server);
            st.builtVoices = voices(); st.builtOutputs = outputs(); st.builtLayers = layers();
            st.sent.clear();
            sendAll(server, true);
            if(old->isCreated()) st.synth->createAndRun(4, old->nodeID, getActive());
            delete old;   // no /n_free: action 4 replaced the node
            if(st.modCreated){   // the modulation's synths at the new voice count
                if(st.gateS){ st.gateS->free(); delete st.gateS; st.gateS = nullptr; }
                createGate(server, st);
                mod.rebuild(server);
                sendModArgs(server, st);
            }
        }
        mod.voicesChanged();
    }

    // ════════════════════════════════════════════════════════════════════════
    // Modulation (the shared LFOs, envelopes and routing)
    // ════════════════════════════════════════════════════════════════════════

    enum { TGT_PITCH, TGT_FORMANT, TGT_CYCLES, TGT_LEVEL, TGT_PROB, TGT_PAN, NUM_TARGETS };

    template<typename T>
    void regMod(ofParameter<T>& p, std::function<void()> send){
        addPublishAction(p);
        listeners.push(p.newListener([send](T&){ send(); }));
    }
    void setupModulation(){
        scModulation::Host h;
        h.regF = [this](ofParameter<float>& p, std::function<void()> f){ regMod(p, f); };
        h.regI = [this](ofParameter<int>& p, std::function<void()> f){ regMod(p, f); };
        h.regB = [this](ofParameter<bool>& p, std::function<void()> f){ regMod(p, f); };
        h.regV = [this](ofParameter<std::vector<float>>& p, std::function<void()> f){ regMod(p, f); };
        h.control = [this](const std::string& k) -> ofAbstractParameter* {
            if(k == gate.getEscapedName()) return nullptr;   // events, not values
            const auto* a = findPublishAction(k);
            return a ? a->param : nullptr;
        };
        h.publishMenu = [this](const std::string& k){ controlMenu(k); };
        h.publishedUnderline = [](const std::string&){};      // controlMenu underlines the control
        h.unpublish = [this](const std::string& k){ unpublishByKey(k); };
        h.publishedKeys = [this]() -> const std::vector<std::string>& { return publishedKeys; };
        h.voices = [this](){ return voices(); };
        mod.maxVoices = MAX_VOICES;
        mod.changed = [this](){ modDirty = true; };
        mod.setup(h, {
            {"Pitch", 24.0f, "st"}, {"Formant", 24.0f, "st"}, {"Cycles", 2.0f, "oct"},
            {"Level", 1.0f, ""}, {"Probability", 1.0f, ""}, {"Pan", 4.0f, "ch"},
        }, true, true, true);   // velocity: the gates' levels (envelopes can track them)
        mod.targetOfKey[fundamental.getEscapedName()] = TGT_PITCH;
        mod.targetOfKey[transpose.getEscapedName()] = TGT_PITCH;
        mod.targetOfKey[formantShift.getEscapedName()] = TGT_FORMANT;
        mod.targetOfKey[cyclesScale.getEscapedName()] = TGT_CYCLES;
        mod.targetOfKey[levels.getEscapedName()] = TGT_LEVEL;
        mod.targetOfKey[probability.getEscapedName()] = TGT_PROB;
        mod.targetOfKey[rotate.getEscapedName()] = TGT_PAN;
    }
    void createGate(ofxSCServer* server, ServerState& st){
        if(!st.modSources) return;
        st.gateS = new ofxSCSynth("ModGate" + ofToString(voices()), server);
        sendGate(server, st);
        st.gateS->createAndRun(0, st.modSources->nodeID, true);   // the head of the sources
    }
    // A scheduled note (inside its timetag): only what a note changes.
    void sendNotes(){
        for(auto& [server, st] : states){
            if(!st.synth) continue;
            auto values = synthValues(st);
            for(const char* name : {"fund", "gate"}){
                const auto& v = values[name];
                st.sent[name] = v;
                if(v.size() == 1) st.synth->set(name, double(v[0])); else st.synth->set(name, v);
            }
            sendGate(server, st);
        }
    }
    void sendGate(ofxSCServer* server, ServerState& st){
        if(!st.gateS) return;
        const int gb = gateIn.bus(server);
        std::vector<float> g(voices(), 0.0f);   // a cable: only the cable (Gate's own 1 would hold every gate)
        if(gb < 0) for(int v = 0; v < voices(); v++) g[v] = pulsarbox::valueFor(gate.get(), v, 1.0f);
        st.gateS->set("gate", g);
        if(gb >= 0){ st.gateS->set("gate_sel", 1.0f); st.gateS->mapan("gate_ar", gb, voices()); }
        else st.gateS->set("gate_sel", 0.0f);
        st.gateS->set("out", st.gateBus ? st.gateBus->index : 0);
        st.gateS->set("rnd", mod.sourceBus(server, scModulation::SRC_RND));
        st.gateS->set("vel", mod.sourceBus(server, scModulation::SRC_VEL));
    }
    void createMod(ofxSCServer* server, ServerState& st){
        if(st.modCreated) return;
        if(!(st.gateBus && st.gateBus->index >= 0 && st.gateBus->index < (int)server->audioBusses.size()
             && server->audioBusses[st.gateBus->index] == st.gateBus)){
            delete st.gateBus;
            st.gateBus = new ofxSCBus(RATE_AUDIO, scModulation::W, server);
            if(!(st.gateBus->index >= 0 && st.gateBus->index < (int)server->audioBusses.size()
                 && server->audioBusses[st.gateBus->index] == st.gateBus)){ st.gateBus->free(); delete st.gateBus; st.gateBus = nullptr; }
        }
        st.modGroup = new ofxSCGroup(server);
        st.modGroup->create(0, 1);
        st.modSources = new ofxSCGroup(server);
        st.modSources->create(1, st.modGroup->nodeID);
        st.modRoutes = new ofxSCGroup(server);
        st.modRoutes->create(1, st.modGroup->nodeID);
        st.modCreated = true;
        mod.create(server, st.modSources->nodeID, st.modRoutes->nodeID, st.gateBus ? st.gateBus->index : 0);
        createGate(server, st);
        if(!getActive()) st.modGroup->run(false);
        sendModArgs(server, st);
    }
    void freeMod(ofxSCServer* server, ServerState& st, bool sendFree){
        if(st.modCreated) mod.free(server, false);   // the group's /n_free takes its synths
        if(sendFree && st.modCreated && st.modGroup) st.modGroup->free();
        delete st.gateS; st.gateS = nullptr;
        delete st.modSources; delete st.modRoutes; delete st.modGroup;
        st.modSources = st.modRoutes = st.modGroup = nullptr;
        st.modCreated = false;
    }
    // 0: Fundamental, 1: Pitch (values), 2: Pitch (a cable)
    int pitchMode(ofxSCServer* server) const {
        if(server && pitchIn.bus(server) >= 0) return 2;
        if(!server) for(const auto& [sv, st] : states) if(pitchIn.bus(sv) >= 0) return 2;
        return (pitchTouched || pitchIn.connected()) ? 1 : 0;
    }
    void sendModArgs(ofxSCServer* server, ServerState& st){
        if(!st.synth) return;
        const int pb = pitchIn.bus(server);
        st.synth->set("pitch_sel", pb >= 0 ? 1.0f : 0.0f);
        if(pb >= 0) st.synth->mapan("pitch_ar", pb, voices());
        st.synth->set("gatebus", st.modCreated && st.gateBus ? (float)st.gateBus->index : -1.0f);
        st.synth->set("mod", st.modCreated ? (float)mod.targetBus(server, 0) : -1.0f);
    }
    void loadModulation(const ofJson& json){
        if(json.contains("pulsarBoxModMatrix")){ mod.load(json, "pulsarBoxMod"); migrateAmpEnvelope(json); }
        else {   // presets from before it: nothing routed
            std::fill(mod.matrix.begin(), mod.matrix.end(), 0.0f);
            mod.lfoCount = mod.envCount = mod.extCount = 0;
            mod.loadParamsJson(ofJson::array());
        }
        mod.afterLoad();
        mod.refresh();
        modDirty = true;
    }

    // Saved before the Amp Envelope: an envelope routed to Level replaced the
    // ASR (it was the VCA); it becomes the Amp Envelope, played Gated
    void migrateAmpEnvelope(const ofJson& json){
        if(json.contains(decay.getEscapedName())) return;   // saved with the Amp Envelope
        bool found = false;
        for(int e = 0; e < scModulation::MAX_ENVS; e++){
            float& cell = mod.matrix[(scModulation::SRC_ENV + e) * NUM_TARGETS + TGT_LEVEL];
            if(std::abs(cell) < 1e-4f) continue;
            if(!found && e < mod.envCount){
                const auto E = mod.envValues(e);
                attack = E.a; decay = E.d; sustain = E.s; release = E.r;
                attackCurve = E.ca; decayCurve = E.cd; releaseCurve = E.cr;
                gateMode = 1;
                found = true;
            }
            cell = 0.0f;   // now a route to Level adds to it
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Tables
    // ════════════════════════════════════════════════════════════════════════

    std::string relativeToData(const std::string& path) const {
        const std::string data = ofToDataPath("", true);
        return path.rfind(data, 0) == 0 ? path.substr(data.size()) : path;
    }
    std::string resolvePath(const std::string& name) const {
        if(name.empty()) return "";
        return std::filesystem::path(name).is_absolute() ? name : ofToDataPath(name, true);
    }
    // 2048 points, peak 1 (the default table if the name is unknown or unreadable)
    const std::vector<float>& tableFor(const std::string& name, bool envelope){
        const std::string key = (envelope ? "E:" : "P:") + name;
        auto it = tableCache.find(key);
        if(it != tableCache.end()) return it->second;
        std::vector<float> t;
        if(pulsarbox::isBuiltin(name, envelope)) t = pulsarbox::builtin(name, envelope);
        else if(!name.empty()){
            auto raw = pulsarbox::readWav(resolvePath(name));
            if(raw.size() >= 2) t = pulsarbox::resample(raw, TABLE_N);
            else failedTables.insert(key);
        }
        if(t.empty()) t = pulsarbox::builtin(envelope ? "Hann" : "Sine", envelope);
        pulsarbox::normalize(t);
        return tableCache[key] = t;
    }
    bool tableFailed(const std::string& name, bool envelope) const {
        return failedTables.count((envelope ? "E:" : "P:") + name) != 0;
    }
    void refreshTables(){
        for(int g = 0; g < LAYERS; g++){
            tables[g][0] = tableFor(pulsaret[g].get(), false);
            tables[g][1] = tableFor(envelope[g].get(), true);
        }
    }

    // One buffer per table, allocated once per server; written when scsynth has them
    void ensureBuffers(ofxSCServer* server){
        auto& st = states[server];
        bool allocated = false;
        for(auto*& b : st.bufs){
            if(b && pulsarbox::bufferIsLive(server, b)) continue;
            delete b;
            b = new ofxSCBuffer(TABLE_N, 1, server);
            b->alloc();
            allocated = true;
        }
        if(allocated){
            for(auto& t : st.sentTables) t.clear();
            st.bufsPending = true;
            st.allocTime = ofGetElapsedTimef();
            server->requestNRTSync();
        }
    }
    void uploadTables(ofxSCServer* server, bool force){
        auto& st = states[server];
        if(st.bufsPending) return;
        for(int i = 0; i < 2 * LAYERS; i++){
            const auto& data = tables[i / 2][i % 2];
            auto* b = st.bufs[i];
            if(!b || !pulsarbox::bufferIsLive(server, b) || data.size() != TABLE_N) continue;
            if(!force && st.sentTables[i] == data) continue;
            st.sentTables[i] = data;
            constexpr int chunk = 1024;
            for(int start = 0; start < TABLE_N; start += chunk){
                ofxOscMessage m;
                m.setAddress("/b_setn");
                m.addIntArg(b->index);
                m.addIntArg(start);
                m.addIntArg(chunk);
                for(int k = 0; k < chunk; k++) m.addFloatArg(data[start + k]);
                server->sendMsg(m);
            }
        }
    }

    void scanLibrary(){
        libraryScanned = true;
        library.clear();
        std::error_code ec;
        for(const char* folder : {"Supercollider/PulsarBox/nuPG_TABLES", "Supercollider/PulsarBox/Tables"}){
            for(const auto& e : std::filesystem::recursive_directory_iterator(ofToDataPath(folder, true), ec)){
                if(!e.is_regular_file(ec)) continue;
                auto ext = ofToLower(e.path().extension().string());
                if(ext != ".wav" || e.path().stem().string().empty()) continue;
                library.push_back(relativeToData(e.path().string()));
            }
        }
        std::sort(library.begin(), library.end(), [](const std::string& a, const std::string& b){
            return ofToLower(std::filesystem::path(a).filename().string()) < ofToLower(std::filesystem::path(b).filename().string());
        });
    }

    // ════════════════════════════════════════════════════════════════════════
    // View: what the synth makes, computed from the same values and tables
    // ════════════════════════════════════════════════════════════════════════

    static constexpr int TRAIN_COLS = 480, PULSE_COLS = 300, SPEC_POINTS = 240;
    struct LayerView { bool on = false; float hz = 0, dur = 0, delay = 0, amp = 0, overlap = 0; };
    struct View {
        float fund = 110, period = 1.0f / 110, train = 0, pulse = 0;   // train / pulse: shown spans (s)
        std::array<LayerView, LAYERS> layer;
        std::array<std::vector<float>, LAYERS> trainLo, trainHi, ghostLo, ghostHi, pulseLo, pulseHi, pulseEnv;
        float trainPeak = 1, pulsePeak = 1;
        std::vector<float> specHz, specDb;   // the spectrum of one pulse
        std::vector<uint8_t> pattern;        // 1 on, 0 sieve, 2 rest
    } view;

    float grain(int g, const LayerView& l, double t) const {
        if(t < 0 || t >= l.dur) return 0.0f;
        return pulsarbox::wrapRead(tables[g][0], t * l.hz) * pulsarbox::clampRead(tables[g][1], t / l.dur);
    }

    void computeView(){
        auto& w = view;
        const int vv = std::clamp(viewVoice, 0, voices() - 1);
        w.fund = voiceFund(vv);
        w.period = 1.0f / w.fund;
        float maxEnd = 0;
        for(int g = 0; g < LAYERS; g++){
            auto& l = w.layer[g];
            l.on = g < layers();
            l.hz = layerHz(g, w.fund);
            l.dur = std::min(std::clamp(layerCycles(g) / l.hz, 2.0f / 48000.0f, 2.0f), 200.0f / w.fund);   // as the synth
            l.delay = std::min(layerDelay[g].get() * w.period, 2.0f);
            l.overlap = l.dur * w.fund;
            l.amp = layerLevel[g].get() / std::sqrt(std::max(1.0f, l.overlap));
            if(l.on) maxEnd = std::max(maxEnd, l.delay + l.dur);
        }
        // Pattern: one whole cycle of sieve x burst / rest (up to 64 pulses)
        const int b = std::max(1, burst.get()), r = std::max(0, rest.get());
        const int patternLength = std::min(64, std::lcm(sieveLength(), b + r));
        w.pattern.assign(patternLength, 0);
        for(int j = 0; j < patternLength; j++)
            w.pattern[j] = pulseOn(j) ? 1 : (sieveStep(j) ? 2 : 0);

        // Pulse train: enough periods to see the pulses and how they overlap
        const int periods = std::clamp(int(std::ceil((maxEnd + w.period) * w.fund)), 3, 12);
        w.train = periods * w.period;
        w.trainPeak = 1e-6f;
        constexpr int SUB = 6;
        for(int g = 0; g < LAYERS; g++){
            auto& l = w.layer[g];
            for(auto* v : {&w.trainLo[g], &w.trainHi[g], &w.ghostLo[g], &w.ghostHi[g]}) v->assign(TRAIN_COLS, 0.0f);
            if(!l.on) continue;
            for(int c = 0; c < TRAIN_COLS; c++){
                float lo = 0, hi = 0, glo = 0, ghi = 0;
                for(int s = 0; s < SUB; s++){
                    const double t = w.train * (c + s / double(SUB)) / TRAIN_COLS;
                    const int jLast = int(std::floor((t - l.delay) * w.fund));
                    const int jFirst = std::max(0, std::max(jLast - 47, int(std::floor((t - l.delay - l.dur) * w.fund)) + 1));
                    double on = 0, off = 0;
                    for(int j = jFirst; j <= jLast; j++){
                        const float x = grain(g, l, t - j * w.period - l.delay) * l.amp;
                        (pulseOn(j) ? on : off) += x;
                    }
                    lo = std::min(lo, float(on)); hi = std::max(hi, float(on));
                    glo = std::min(glo, float(off)); ghi = std::max(ghi, float(off));
                }
                w.trainLo[g][c] = lo; w.trainHi[g][c] = hi; w.ghostLo[g][c] = glo; w.ghostHi[g][c] = ghi;
                w.trainPeak = std::max({w.trainPeak, hi, -lo, ghi, -glo});
            }
        }

        // One pulse, zoomed: every layer from the pulse's start to its end
        w.pulse = std::max(maxEnd * 1.04f, 1e-4f);
        w.pulsePeak = 1e-6f;
        for(int g = 0; g < LAYERS; g++){
            auto& l = w.layer[g];
            for(auto* v : {&w.pulseLo[g], &w.pulseHi[g], &w.pulseEnv[g]}) v->assign(PULSE_COLS, 0.0f);
            if(!l.on) continue;
            for(int c = 0; c < PULSE_COLS; c++){
                float lo = 0, hi = 0;
                for(int s = 0; s < SUB; s++){
                    const double t = w.pulse * (c + s / double(SUB)) / PULSE_COLS - l.delay;
                    const float x = grain(g, l, t) * l.amp;
                    lo = std::min(lo, x); hi = std::max(hi, x);
                }
                const double t = w.pulse * (c + 0.5) / PULSE_COLS - l.delay;
                w.pulseLo[g][c] = lo; w.pulseHi[g][c] = hi;
                w.pulseEnv[g][c] = (t >= 0 && t < l.dur) ? pulsarbox::clampRead(tables[g][1], t / l.dur) * l.amp : 0.0f;
                w.pulsePeak = std::max({w.pulsePeak, hi, -lo, w.pulseEnv[g][c]});
            }
        }

        // Spectrum of one pulse (all layers, with their delays): a DFT at
        // log-spaced frequencies. A pulse train has this envelope sampled at
        // the harmonics of the fundamental.
        const double fs = std::min(48000.0, 16384.0 / std::max(1e-4f, maxEnd));
        const int n = std::max(8, int(std::ceil(maxEnd * fs)));
        std::vector<float> sig(n, 0.0f);
        for(int g = 0; g < LAYERS; g++){
            const auto& l = w.layer[g];
            if(!l.on) continue;
            for(int i = 0; i < n; i++) sig[i] += grain(g, l, i / fs - l.delay) * l.amp;
        }
        w.specHz.assign(SPEC_POINTS, 0.0f);
        w.specDb.assign(SPEC_POINTS, -120.0f);
        float top = -200;
        for(int k = 0; k < SPEC_POINTS; k++){
            const double f = 20.0 * std::pow(1000.0, k / double(SPEC_POINTS - 1));
            w.specHz[k] = float(f);
            if(f >= fs * 0.5) continue;
            const std::complex<double> step = std::polar(1.0, -2.0 * M_PI * f / fs);
            std::complex<double> z = 1.0, acc = 0.0;
            for(int i = 0; i < n; i++){
                acc += double(sig[i]) * z;
                z *= step;
                if((i & 1023) == 1023) z /= std::abs(z);
            }
            w.specDb[k] = float(20.0 * std::log10(std::abs(acc) + 1e-9));
            top = std::max(top, w.specDb[k]);
        }
        for(float& d : w.specDb) d -= top;
    }

    // ════════════════════════════════════════════════════════════════════════
    // Publishing window controls into the node (as in GrainBox / AdditiveBox)
    // ════════════════════════════════════════════════════════════════════════

    struct PublishAction {
        std::string key;
        ofAbstractParameter* param = nullptr;
        bool isVector = false;
        std::function<std::shared_ptr<ofxOceanodeAbstractParameter>()> publish;
        std::function<void(ofJson&)> save;
        std::function<void(const ofJson&)> load;
        std::function<void()> reset;
        std::function<std::string()> valueText;
        std::function<void(const std::string&)> applyText;
    };

    template<typename T>
    void addPublishAction(ofParameter<T>& parameter, std::vector<std::string> dropdown = {}){
        auto* p = &parameter;
        const std::string key = parameter.getEscapedName();
        PublishAction a;
        a.key = key;
        a.param = p;
        a.isVector = std::is_same<T, std::vector<float>>::value || std::is_same<T, std::vector<int>>::value;
        a.publish = [this, p, dropdown]() -> std::shared_ptr<ofxOceanodeAbstractParameter> {
            auto handle = addParameter(*p);
            if(!dropdown.empty()) handle->setDropdownOptions(dropdown);
            return handle;
        };
        a.save = [key, p](ofJson& json){ json[key] = p->get(); };
        a.load = [p](const ofJson& value){ try { p->set(value.get<T>()); } catch(...) {} };
        const T initial = p->get();
        a.reset = [p, initial](){ p->set(initial); };
        a.valueText = [p](){ return pulsarbox::toText(p->get()); };
        a.applyText = [p](const std::string& text){ T v; pulsarbox::fromText(text, v); p->set(v); };
        publishActions.push_back(std::move(a));
    }

    void initializePublishActions(){
        publishActions.clear();
        for(auto* p : {&snaps.snapshotP, &snaps.morphAP, &snaps.morphBP, &snaps.morphCurveP, &snaps.morphSwitchP}) addPublishAction(*p);
        for(auto* p : {&snaps.morphP, &snaps.morphTimeP}) addPublishAction(*p);
        for(auto* p : {&snaps.morphBeatsP, &snaps.morphGlideP}) addPublishAction(*p);
        addPublishAction(fundamental);
        addPublishAction(transpose);
        addPublishAction(glide);
        addPublishAction(levels);
        addPublishAction(gateMode);
        addPublishAction(attack);
        addPublishAction(decay);
        addPublishAction(sustain);
        addPublishAction(release);
        addPublishAction(attackCurve);
        addPublishAction(decayCurve);
        addPublishAction(releaseCurve);
        addPublishAction(volume);
        addPublishAction(rateJitter);
        addPublishAction(numLayers);
        addPublishAction(formantShift);
        addPublishAction(cyclesScale);
        addPublishAction(formantJitter);
        addPublishAction(levelJitter);
        for(int g = 0; g < LAYERS; g++){
            addPublishAction(formantMode[g], pulsarbox::formantModeNames());
            addPublishAction(formantHz[g]);
            addPublishAction(formantRatio[g]);
            addPublishAction(cycles[g]);
            addPublishAction(layerLevel[g]);
            addPublishAction(layerDelay[g]);
            addPublishAction(layerPan[g]);
            addPublishAction(pulsaret[g]);
            addPublishAction(envelope[g]);
        }
        addPublishAction(probability);
        addPublishAction(burst);
        addPublishAction(rest);
        addPublishAction(sieve);
        addPublishAction(panMode, pulsarbox::panModeNames());
        addPublishAction(rotate);
        addPublishAction(panJitter);
    }
    const PublishAction* findPublishAction(const std::string& key) const {
        for(const auto& a : publishActions) if(a.key == key) return &a;
        return nullptr;
    }
    bool isPublished(const std::string& key) const { return publishedHandles.count(key) != 0; }
    bool hasPublishedConnection(const std::string& key) const {
        auto it = publishedHandles.find(key);
        return it != publishedHandles.end() && it->second && (it->second->hasInConnection() || it->second->hasOutConnections());
    }
    bool publishByKey(const std::string& key){
        const PublishAction* a = findPublishAction(key);
        if(!a || !a->publish || isPublished(key)) return false;
        if(!publishedSeparatorAdded){ addSeparator("Published", ofColor(200)); publishedSeparatorAdded = true; }
        publishedHandles[key] = a->publish();
        publishedKeys.push_back(key);
        parameterGroupChanged.notify(this);
        return true;
    }
    bool unpublishByKey(const std::string& key){
        auto it = publishedHandles.find(key);
        if(it == publishedHandles.end() || hasPublishedConnection(key)) return false;
        removeParameter(it->second->getEscapedName());
        publishedHandles.erase(it);
        publishedKeys.erase(std::remove(publishedKeys.begin(), publishedKeys.end(), key), publishedKeys.end());
        if(publishedHandles.empty() && publishedSeparatorAdded){ removeSeparator("Published"); publishedSeparatorAdded = false; }
        parameterGroupChanged.notify(this);
        return true;
    }
    void syncPublished(const std::vector<std::string>& keys){
        const auto current = publishedKeys;
        for(const auto& key : current)
            if(std::find(keys.begin(), keys.end(), key) == keys.end() && !hasPublishedConnection(key)) unpublishByKey(key);
        for(const auto& key : keys) if(!isPublished(key)) publishByKey(key);
    }
    void controlMenu(const std::string& key){
        timeline.mark(key);
        if(isPublished(key) && !timeline.isHidden(key)){
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y - 1.0f), ImVec2(b.x, b.y - 1.0f), ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
        }
        if(!ImGui::BeginPopupContextItem(("##PublishToNode_" + key).c_str())) return;
        ImGui::PushID(key.c_str());
        const PublishAction* a = findPublishAction(key);
        if(a && a->valueText && a->applyText){
            static std::map<std::string, std::array<char, 2048>> buffers;
            auto& buffer = buffers[key];
            const bool appearing = ImGui::IsWindowAppearing();
            if(appearing) std::snprintf(buffer.data(), buffer.size(), "%s", a->valueText().c_str());
            ImGui::TextDisabled("%s", a->isVector ? "Value (one for all voices, or a,b,c...)" : "Value");
            ImGui::SetNextItemWidth(180.0f * ofxOceanodeShared::getZoomLevel());
            if(appearing) ImGui::SetKeyboardFocusHere();
            const bool enter = ImGui::InputText("##pbTypedValue", buffer.data(), buffer.size(),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if(enter || ImGui::IsItemDeactivatedAfterEdit()){
                a->applyText(std::string(buffer.data()));
                if(enter) ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
        }
        if(!a){
            const bool onNode = key == numVoices.getEscapedName() || key == numOutputs.getEscapedName();
            ImGui::TextDisabled("%s", onNode ? "Already in node" : "Not publishable");
        } else if(timeline.isHidden(key)){
            if(ImGui::MenuItem("Publish to Node GUI")) timeline.reveal(key);
        } else if(isPublished(key)){
            ImGui::TextDisabled("Published in node");
            if(hasPublishedConnection(key)) ImGui::TextDisabled("Connected: disconnect to unpublish");
            else if(ImGui::MenuItem("Unpublish from Node")) timeline.unpublish(key);
        } else if(ImGui::MenuItem("Publish to Node GUI")) publishByKey(key);
        timeline.menuItems(key);
        mod.modulateMenu(key);   // route sources to it
        ImGui::PopID();
        ImGui::EndPopup();
    }

    // ════════════════════════════════════════════════════════════════════════
    // Snapshots and named presets (data/Supercollider/PulsarBox/Presets): the sound as json
    // ════════════════════════════════════════════════════════════════════════

    // Played, not part of a sound: a preset leaves them as they are
    bool notInPresets(const std::string& key) const {
        return key == gate.getEscapedName() || key == transpose.getEscapedName() || scSnapshots::isOwnKey(key);
    }
    // The sound (a snapshot / a named preset): Voices, every control a preset holds, the modulation
    ofJson captureSound() const {
        ofJson json;
        json["Voices"] = numVoices.get();
        for(const auto& a : publishActions) if(a.save && !notInPresets(a.key)) a.save(json);
        mod.patchBases(json);
        mod.save(json, "pulsarBoxMod");
        return json;
    }
    // Every control back to its default, then the sound's values; the synth
    // is replaced once afterwards if the voice count changed
    void loadSound(const ofJson& json){
        loadingBankPreset = true;
        for(const auto& a : publishActions) if(a.reset && !notInPresets(a.key)) a.reset();
        numVoices = json.contains("Voices") && json["Voices"].is_number_integer() ? json["Voices"].get<int>() : 1;
        for(const auto& a : publishActions)
            if(a.load && !notInPresets(a.key) && json.contains(a.key)) a.load(json[a.key]);
        loadModulation(json);
        loadingBankPreset = false;
        structureDirty = true;
        modSource = nullptr;
    }
    // Between two sounds: numbers glide, the rest (voices, layers, modes,
    // tables, the routing) switches at the switch point
    void applySound(const ofJson& a, const ofJson& b, float t, bool sw){
        if(t >= 1.0f && sw){ loadSound(b); return; }
        scSnapshots::blendActions(publishActions, a, b, t, sw, [this](const std::string& k){ return notInPresets(k); });
        const ofJson& src = sw ? b : a;
        if(src.contains("Voices") && src["Voices"].is_number_integer() && src["Voices"].get<int>() != numVoices.get())
            numVoices = src["Voices"].get<int>();
        if(&src != modSource && src.contains("pulsarBoxModMatrix")){ modSource = &src; loadModulation(src); }
    }
    const ofJson* modSource = nullptr;   // the routing switched to (a blend's side)
    float currentBpm = 120.0f;

    // ════════════════════════════════════════════════════════════════════════
    // Window
    // ════════════════════════════════════════════════════════════════════════

    void drawWindow(){
        const std::string title = getWindowTitle();
        bool open = showWindow.get();
        ImGui::SetNextWindowSize(ImVec2(1180.0f, 860.0f), ImGuiCond_FirstUseEver);
        if(ImGui::Begin(title.c_str(), &open)){
            ImGui::PushID(this);
            if(view.trainLo[0].empty()) computeView();
            drawHeader();
            const float w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
            viewHeight = ofClamp(viewHeight, 160.0f, 900.0f);
            drawViews(w, viewHeight);
            // Handle: drag to resize the views
            const ImVec2 h = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##pbresize", ImVec2(w, 8.0f));
            const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
            if(hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if(ImGui::IsItemActive()) viewHeight += ImGui::GetIO().MouseDelta.y;
            ImGui::GetWindowDrawList()->AddLine(ImVec2(h.x + w / 2 - 24, h.y + 4), ImVec2(h.x + w / 2 + 24, h.y + 4),
                ImGui::GetColorU32(hot ? ImGuiCol_Text : ImGuiCol_TextDisabled), 2.0f);
            if(ImGui::BeginTabBar("##pbtabs")){
                // Few tabs, many columns: short sections stacked
                const float tw = std::max(1.0f, ImGui::GetContentRegionAvail().x);
                if(ImGui::BeginTabItem("Sound")){
                    drawPulseTab();
                    ImGui::Spacing();
                    drawFormantsTab();
                    ImGui::EndTabItem();
                }
                if(ImGui::BeginTabItem("Masking & Space")){
                    drawMaskingTab();
                    ImGui::Spacing();
                    drawSpaceTab();
                    ImGui::EndTabItem();
                }
                if(ImGui::BeginTabItem("Envelopes")){
                    mod.drawEnvelopes(tw, [this](float cw){ drawAmpCard(cw); });
                    ImGui::EndTabItem();
                }
                if(ImGui::BeginTabItem("Modulation")){
                    mod.drawLfos(tw);
                    ImGui::Spacing();
                    mod.drawRouting(tw);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            drawTableBrowser();
            ImGui::PopID();
        }
        ImGui::End();
        if(open != showWindow.get()) showWindow = open;
    }

    void drawHeader(){
        ImGui::TextUnformatted("PULSARBOX");
        ImGui::SameLine(0, 24);
        ImGui::TextDisabled("Formants");
        for(int n = 1; n <= LAYERS; n++){
            ImGui::SameLine(0, n == 1 ? 6 : 2);
            const bool on = n <= layers();
            ImGui::PushStyleColor(ImGuiCol_Button, on ? ImGui::ColorConvertU32ToFloat4(pulsarbox::layerColor(n - 1, 0.55f)) : ImVec4(0.16f, 0.18f, 0.22f, 1));
            if(ImGui::Button((ofToString(n) + "##pbl" + ofToString(n)).c_str(), ImVec2(26, 0))) numLayers = n;
            ImGui::PopStyleColor();
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("%d formant layer%s per pulse", n, n > 1 ? "s" : "");
        }
        ImGui::SameLine(); ImGui::InvisibleButton("##pbfspacer", ImVec2(1, 1)); controlMenu(numLayers.getEscapedName());
        ImGui::SameLine(0, 24);
        ImGui::SetNextItemWidth(110);
        int v = numVoices.get();
        if(ImGui::SliderInt("##pbvoices", &v, 1, MAX_VOICES, "Voices %d")) numVoices = v;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        int o = numOutputs.get();
        if(ImGui::SliderInt("##pboutputs", &o, 1, MAX_OUTPUTS, "Outputs %d")) numOutputs = o;
        if(voices() > 1){
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110);
            int vv = std::clamp(viewVoice, 0, voices() - 1) + 1;
            if(ImGui::SliderInt("##pbviewvoice", &vv, 1, voices(), "View voice %d")){ viewVoice = vv - 1; viewDirty = true; }
        }
        // snapshots, named presets, morph (scSnapshots)
        snaps.drawBar(std::max(1.0f, ImGui::GetContentRegionAvail().x));
    }

    // ── The views ──────────────────────────────────────────────────────────
    void drawViews(float w, float h){
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##pbviews", ImVec2(w, h));
        auto* dl = ImGui::GetWindowDrawList();
        const float gap = 6, laneH = 34;
        drawPatternLane(dl, o, ImVec2(w, laneH));
        const float y = o.y + laneH + gap, ph = h - laneH - gap;
        const float trainW = (w - 2 * gap) * 0.46f, pulseW = (w - 2 * gap) * 0.2f, specW = w - 2 * gap - trainW - pulseW;
        drawTrain(dl, ImVec2(o.x, y), ImVec2(trainW, ph));
        drawPulse(dl, ImVec2(o.x + trainW + gap, y), ImVec2(pulseW, ph));
        drawSpectrum(dl, ImVec2(o.x + trainW + pulseW + 2 * gap, y), ImVec2(specW, ph));
    }

    static void panel(ImDrawList* dl, ImVec2 o, ImVec2 s, const std::string& title, const std::string& sub){
        dl->AddRectFilled(o, ImVec2(o.x + s.x, o.y + s.y), IM_COL32(13, 16, 24, 255), 5.0f);
        dl->AddText(ImVec2(o.x + 8, o.y + 5), IM_COL32(225, 232, 240, 230), title.c_str());
        if(!sub.empty()) dl->AddText(ImVec2(o.x + 8, o.y + 21), IM_COL32(150, 162, 178, 200), sub.c_str());
    }

    void drawPatternLane(ImDrawList* dl, ImVec2 o, ImVec2 s){
        dl->AddRectFilled(o, ImVec2(o.x + s.x, o.y + s.y), IM_COL32(13, 16, 24, 255), 5.0f);
        const float labelW = 150;
        const std::string label = probability.get() < 1.0f ? "MASK  p " + ofToString(probability.get(), 2) : "MASK";
        dl->AddText(ImVec2(o.x + 8, o.y + 4), IM_COL32(225, 232, 240, 230), label.c_str());
        const int n = int(view.pattern.size());
        const std::string len = ofToString(n) + " pulse" + (n > 1 ? "s" : "") + " = " + pulsarbox::timeText(n * view.period);
        dl->AddText(ImVec2(o.x + 8, o.y + 18), IM_COL32(150, 162, 178, 200), len.c_str());
        if(n == 0) return;
        const float x0 = o.x + labelW, cw = (s.x - labelW - 6) / n;
        // Playhead (approximate: the synth's own count is not read back)
        const int playing = view.fund <= 40.0f ? int(std::fmod(ofGetElapsedTimef() * view.fund, float(n))) : -1;
        for(int j = 0; j < n; j++){
            const ImVec2 a(x0 + j * cw + 1, o.y + 5), b(x0 + (j + 1) * cw - 1, o.y + s.y - 5);
            if(view.pattern[j] == 1){
                dl->AddRectFilled(a, b, pulsarbox::layerColor(0, 0.25f + 0.65f * probability.get()), 2.0f);
            } else if(view.pattern[j] == 2){
                dl->AddRect(a, b, IM_COL32(110, 120, 135, 120), 2.0f);   // rest of a burst
            } else {
                dl->AddRectFilled(a, b, IM_COL32(32, 36, 46, 255), 2.0f);   // sieve off
            }
            if(j == playing) dl->AddRect(ImVec2(a.x - 1, a.y - 2), ImVec2(b.x + 1, b.y + 2), IM_COL32(255, 255, 255, 230), 2.0f, 0, 2.0f);
        }
        if(ImGui::IsMouseHoveringRect(o, ImVec2(o.x + s.x, o.y + s.y)))
            ImGui::SetTooltip("One cycle of the masking: filled = pulse plays (brightness = probability),\n"
                              "outline = Rest of a burst, dark = Sieve step off. The playhead is approximate.");
    }

    void drawTrain(ImDrawList* dl, ImVec2 o, ImVec2 s){
        const auto& w = view;
        const std::string note = pulsarbox::noteName(w.fund);
        panel(dl, o, s, "PULSE TRAIN", pulsarbox::timeText(w.period) + " period  ·  " + ofToString(w.fund, w.fund < 10 ? 2 : 1) + " Hz"
              + (note.empty() ? "  (rhythm)" : "  " + note));
        const float top = o.y + 40, bottom = o.y + s.y - 18, mid = (top + bottom) * 0.5f, half = (bottom - top) * 0.5f;
        // Period lines (pulse starts)
        const int periods = int(std::round(w.train / w.period));
        for(int j = 0; j <= periods; j++){
            const float x = o.x + s.x * (j * w.period / w.train);
            dl->AddLine(ImVec2(x, top - 4), ImVec2(x, bottom), IM_COL32(255, 255, 255, pulseOn(j) ? 45 : 18));
            if(j < periods && x + 4 < o.x + s.x)
                dl->AddText(ImVec2(x + 3, bottom + 2), IM_COL32(120, 130, 145, 160), ofToString(j + 1).c_str());
        }
        dl->AddLine(ImVec2(o.x, mid), ImVec2(o.x + s.x, mid), IM_COL32(255, 255, 255, 25));
        const float cw = s.x / TRAIN_COLS, scale = half * 0.92f / w.trainPeak;
        for(int g = 0; g < LAYERS; g++){
            if(!w.layer[g].on || w.trainLo[g].size() != TRAIN_COLS) continue;
            for(int c = 0; c < TRAIN_COLS; c++){
                const float x = o.x + (c + 0.5f) * cw;
                if(w.ghostHi[g][c] - w.ghostLo[g][c] > 1e-6f)
                    dl->AddLine(ImVec2(x, mid - w.ghostHi[g][c] * scale), ImVec2(x, mid - w.ghostLo[g][c] * scale + 1), IM_COL32(120, 125, 140, 55), std::max(1.0f, cw));
                if(w.trainHi[g][c] - w.trainLo[g][c] > 1e-6f)
                    dl->AddLine(ImVec2(x, mid - w.trainHi[g][c] * scale), ImVec2(x, mid - w.trainLo[g][c] * scale + 1), pulsarbox::layerColor(g, 0.75f), std::max(1.0f, cw));
            }
        }
        if(ImGui::IsMouseHoveringRect(o, ImVec2(o.x + s.x, o.y + s.y)))
            ImGui::SetTooltip("The pulses as the synth makes them, one colour per formant layer.\n"
                              "Grey: pulses the masking removes. Numbers: pulse count in the pattern.");
    }

    void drawPulse(ImDrawList* dl, ImVec2 o, ImVec2 s){
        const auto& w = view;
        panel(dl, o, s, "ONE PULSE", pulsarbox::timeText(w.pulse));
        const float top = o.y + 40, bottom = o.y + s.y - 8;
        const int L = layers();
        const float laneH = (bottom - top) / L;
        for(int g = 0; g < L; g++){
            const auto& l = w.layer[g];
            const float lt = top + g * laneH, mid = lt + laneH * 0.5f, half = laneH * 0.42f;
            const float scale = half / w.pulsePeak, cw = s.x / PULSE_COLS;
            dl->AddLine(ImVec2(o.x, mid), ImVec2(o.x + s.x, mid), IM_COL32(255, 255, 255, 18));
            if(w.pulseLo[g].size() == PULSE_COLS){
                for(int c = 0; c < PULSE_COLS; c++){
                    const float x = o.x + (c + 0.5f) * cw;
                    if(w.pulseHi[g][c] - w.pulseLo[g][c] > 1e-6f)
                        dl->AddLine(ImVec2(x, mid - w.pulseHi[g][c] * scale), ImVec2(x, mid - w.pulseLo[g][c] * scale + 1), pulsarbox::layerColor(g, 0.8f), std::max(1.0f, cw));
                }
                for(int c = 1; c < PULSE_COLS; c++){
                    const float x0 = o.x + (c - 0.5f) * cw, x1 = o.x + (c + 0.5f) * cw;
                    dl->AddLine(ImVec2(x0, mid - w.pulseEnv[g][c - 1] * scale), ImVec2(x1, mid - w.pulseEnv[g][c] * scale), IM_COL32(255, 255, 255, 90));
                }
            }
            const std::string info = std::string(1, char('A' + g)) + "  " + ofToString(l.hz, 0) + " Hz  " + pulsarbox::timeText(l.dur);
            dl->AddText(ImVec2(o.x + 6, lt + 2), pulsarbox::layerColor(g, 0.9f), info.c_str());
        }
        // Where the next pulse starts, if inside
        if(w.period < w.pulse){
            const float x = o.x + s.x * w.period / w.pulse;
            dl->AddLine(ImVec2(x, top - 4), ImVec2(x, bottom), IM_COL32(255, 230, 120, 150), 1.5f);
            dl->AddText(ImVec2(std::min(x + 3, o.x + s.x - 70), o.y + 22), IM_COL32(255, 230, 120, 200), "next pulse");
        }
        if(ImGui::IsMouseHoveringRect(o, ImVec2(o.x + s.x, o.y + s.y)))
            ImGui::SetTooltip("One pulse: each layer's pulsaret at its formant under its envelope (white),\n"
                              "starting after its Delay. Pulse length = Cycles / formant.");
    }

    void drawSpectrum(ImDrawList* dl, ImVec2 o, ImVec2 s){
        const auto& w = view;
        panel(dl, o, s, "SPECTRUM", w.fund >= 20.0f ? "harmonics of " + ofToString(w.fund, 1) + " Hz under the formant peaks"
                                                   : "below 20 Hz: a rhythm of this colour");
        const float left = o.x + 6, right = o.x + s.x - 6, top = o.y + 42, bottom = o.y + s.y - 18;
        constexpr float dbRange = 60.0f;
        auto xOf = [&](float hz){ return left + (right - left) * std::log(std::max(hz, 20.0f) / 20.0f) / std::log(1000.0f); };
        auto yOf = [&](float db){ return top + (bottom - top) * std::clamp(-db / dbRange, 0.0f, 1.0f); };
        for(float f : {50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f}){
            const float x = xOf(f);
            dl->AddLine(ImVec2(x, top), ImVec2(x, bottom), IM_COL32(255, 255, 255, 14));
            dl->AddText(ImVec2(x + 2, bottom + 2), IM_COL32(120, 130, 145, 150), f >= 1000 ? (ofToString(int(f / 1000)) + "k").c_str() : ofToString(int(f)).c_str());
        }
        for(float db : {-20.0f, -40.0f}) dl->AddLine(ImVec2(left, yOf(db)), ImVec2(right, yOf(db)), IM_COL32(255, 255, 255, 12));
        if(w.specDb.size() != SPEC_POINTS) return;
        auto dbAt = [&](float hz){
            const float k = std::log(std::max(hz, 20.0f) / 20.0f) / std::log(1000.0f) * (SPEC_POINTS - 1);
            const int a = std::clamp(int(k), 0, SPEC_POINTS - 1), b = std::min(a + 1, SPEC_POINTS - 1);
            return w.specDb[a] + (w.specDb[b] - w.specDb[a]) * (k - a);
        };
        const int harmonics = w.fund >= 20.0f ? int(20000.0f / w.fund) : 0;
        if(harmonics > 0 && harmonics <= 600){
            for(int k = 1; k <= harmonics; k++){
                const float hz = k * w.fund, x = xOf(hz);
                dl->AddLine(ImVec2(x, bottom), ImVec2(x, yOf(dbAt(hz))), IM_COL32(150, 215, 255, 190), 1.5f);
            }
        } else {
            for(int k = 0; k < SPEC_POINTS; k++){
                const float x = xOf(w.specHz[k]);
                dl->AddLine(ImVec2(x, bottom), ImVec2(x, yOf(w.specDb[k])), IM_COL32(150, 215, 255, 60), std::max(1.0f, (right - left) / SPEC_POINTS));
            }
        }
        std::vector<ImVec2> curve(SPEC_POINTS);
        for(int k = 0; k < SPEC_POINTS; k++) curve[k] = ImVec2(xOf(w.specHz[k]), yOf(w.specDb[k]));
        dl->AddPolyline(curve.data(), SPEC_POINTS, IM_COL32(255, 255, 255, 120), 0, 1.2f);
        for(int g = 0; g < LAYERS; g++){
            if(!w.layer[g].on) continue;
            const float x = xOf(w.layer[g].hz);
            dl->AddLine(ImVec2(x, top - 2), ImVec2(x, bottom), pulsarbox::layerColor(g, 0.6f), 1.0f);
            dl->AddText(ImVec2(x + 3, top - 2 + g * 13), pulsarbox::layerColor(g), std::string(1, char('A' + g)).c_str());
        }
        if(w.fund >= 20.0f){
            const float x = xOf(w.fund);
            dl->AddTriangleFilled(ImVec2(x - 4, bottom + 1), ImVec2(x + 4, bottom + 1), ImVec2(x, bottom - 6), IM_COL32(255, 230, 120, 220));
        }
        if(ImGui::IsMouseHoveringRect(o, ImVec2(o.x + s.x, o.y + s.y)))
            ImGui::SetTooltip("White: the spectrum of one pulse (0..-60 dB). Blue lines: the harmonics of the\n"
                              "fundamental (yellow) it shapes. Coloured lines: the formants. Fewer Cycles =\n"
                              "wider peaks; the pulsaret's own harmonics repeat the peak higher up.");
    }

    // ── Controls ───────────────────────────────────────────────────────────
    bool sliderFloat(ofParameter<float>& p, const char* format = "%.3f", bool logarithmic = false, const char* label = nullptr){
        float v = p.get();
        const std::string text = std::string(label ? label : p.getName().c_str()) + "  " + format;
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool changed = ImGui::SliderFloat(("##" + p.getEscapedName()).c_str(), &v, p.getMin(), p.getMax(), text.c_str(),
                                                logarithmic ? ImGuiSliderFlags_Logarithmic : 0);
        if(changed) p = v;
        controlMenu(p.getEscapedName());
        return changed;
    }
    bool sliderInt(ofParameter<int>& p, const char* label = nullptr){
        int v = p.get();
        const std::string text = std::string(label ? label : p.getName().c_str()) + "  %d";
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool changed = ImGui::SliderInt(("##" + p.getEscapedName()).c_str(), &v, p.getMin(), p.getMax(), text.c_str());
        if(changed) p = v;
        controlMenu(p.getEscapedName());
        return changed;
    }
    bool sliderVector(ofParameter<std::vector<float>>& p, const char* format = "%.3f", bool logarithmic = false){
        auto values = p.get();
        float v = values.empty() ? 0.0f : values[0];
        const float lo = p.getMin().empty() ? 0.0f : p.getMin()[0], hi = p.getMax().empty() ? 1.0f : p.getMax()[0];
        const std::string label = p.getName() + (values.size() > 1 ? " [" + ofToString(values.size()) + "]" : "") + "  " + format;
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool changed = ImGui::SliderFloat(("##" + p.getEscapedName()).c_str(), &v, lo, hi, label.c_str(),
                                                logarithmic ? ImGuiSliderFlags_Logarithmic : 0);
        if(changed) p = std::vector<float>{v};
        if(ImGui::IsItemHovered() && values.size() > 1) ImGui::SetTooltip("%s", pulsarbox::toText(values).c_str());
        controlMenu(p.getEscapedName());
        return changed;
    }
    bool combo(ofParameter<int>& p, const std::vector<std::string>& options){
        int v = std::clamp(p.get(), 0, int(options.size()) - 1);
        bool changed = false;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if(ImGui::BeginCombo(("##" + p.getEscapedName()).c_str(), (p.getName() + ": " + options[v]).c_str())){
            for(int i = 0; i < (int)options.size(); i++)
                if(ImGui::Selectable(options[i].c_str(), i == v)){ p = i; changed = true; }
            ImGui::EndCombo();
        }
        controlMenu(p.getEscapedName());
        return changed;
    }
    static void heading(const char* title){ ImGui::TextDisabled("%s", title); ImGui::Separator(); }

    // A table thumbnail; a click opens the browser for it
    void tableThumb(int g, int kind, ImVec2 size){
        const auto& t = tables[g][kind];
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::PushID(g * 2 + kind);
        const bool clicked = ImGui::InvisibleButton("##thumb", size);
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), hovered ? IM_COL32(28, 34, 46, 255) : IM_COL32(18, 22, 31, 255), 4.0f);
        const float mid = kind ? o.y + size.y - 4 : o.y + size.y * 0.5f, amp = kind ? size.y - 18 : size.y * 0.5f - 10;
        if(!kind) dl->AddLine(ImVec2(o.x, mid), ImVec2(o.x + size.x, mid), IM_COL32(255, 255, 255, 20));
        if(t.size() > 1){
            constexpr int n = 96;
            ImVec2 pts[n];
            for(int i = 0; i < n; i++){
                const float x = float(i) / (n - 1);
                pts[i] = ImVec2(o.x + 3 + x * (size.x - 6), mid - (kind ? pulsarbox::clampRead(t, x) : pulsarbox::wrapRead(t, x * 0.9999)) * amp);
            }
            dl->AddPolyline(pts, n, kind ? IM_COL32(235, 240, 245, 200) : pulsarbox::layerColor(g), 0, 1.5f);
        }
        const std::string& name = kind ? envelope[g].get() : pulsaret[g].get();
        std::string shown = name.empty() ? (kind ? "Hann" : "Sine") : std::filesystem::path(name).stem().string();
        if(tableFailed(name, kind == 1)) shown += " (missing)";
        dl->AddText(ImVec2(o.x + 5, o.y + 2), IM_COL32(200, 208, 218, 210), shown.c_str());
        if(hovered) ImGui::SetTooltip("%s: %s\nClick to choose another table", kind ? "Envelope" : "Pulsaret", shown.c_str());
        if(clicked){ browseLayer = g; browseKind = kind; browseOpen = true; }
        controlMenu((kind ? envelope[g] : pulsaret[g]).getEscapedName());
    }

    void drawFormantsTab(){
        const int L = layers();
        if(!ImGui::BeginTable("##pbformants", L + 1, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) return;
        for(int g = 0; g < L; g++){
            ImGui::TableNextColumn();
            ImGui::PushID(g);
            const auto& l = view.layer[g];
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pulsarbox::layerColor(g)));
            ImGui::Text("FORMANT %c", char('A' + g));
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", (ofToString(l.hz, 0) + " Hz · " + pulsarbox::timeText(l.dur)).c_str());
            const float cw = ImGui::GetContentRegionAvail().x;
            tableThumb(g, 0, ImVec2(cw, 52));
            tableThumb(g, 1, ImVec2(cw, 40));
            // Mode: Hz or a ratio of the fundamental
            for(int m = 0; m < 2; m++){
                if(m) ImGui::SameLine(0, 2);
                const bool on = formantMode[g].get() == m;
                if(on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(pulsarbox::layerColor(g, 0.45f)));
                if(ImGui::Button(pulsarbox::formantModeNames()[m].c_str(), ImVec2(cw * 0.5f - 1, 0))) formantMode[g] = m;
                if(on) ImGui::PopStyleColor();
                controlMenu(formantMode[g].getEscapedName());
                if(ImGui::IsItemHovered()) ImGui::SetTooltip(m ? "The formant follows the fundamental (a fixed timbre over pitches)"
                                                               : "A fixed formant in Hz (a resonance: vowels, body)");
            }
            if(formantMode[g].get() == 0) sliderFloat(formantHz[g], "%.0f Hz", true, "Formant");
            else sliderFloat(formantRatio[g], "x %.2f", true, "Ratio");
            sliderFloat(cycles[g], "%.2f", true, "Cycles");
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Formant periods under the envelope: fewer = shorter pulse, wider and brighter peak");
            sliderFloat(layerLevel[g], "%.2f", false, "Level");
            sliderFloat(layerDelay[g], "%.2f", false, "Delay");
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Start of this layer after the pulse, as a share of the period");
            sliderFloat(layerPan[g], "%.2f ch", false, "Pan");
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Output offset of this layer, in channels");
            if(l.overlap > 1.0f) ImGui::TextColored(ImVec4(1, 0.85f, 0.45f, 1), "overlaps %.1f pulses", l.overlap);
            else ImGui::TextDisabled("duty %.0f%%", l.overlap * 100.0f);
            ImGui::PopID();
        }
        ImGui::TableNextColumn();
        heading("ALL FORMANTS");
        sliderFloat(formantShift, "%.1f st");
        sliderFloat(cyclesScale, "x %.2f", true);
        sliderFloat(formantJitter, "%.1f st");
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Every pulse's formant moved at random by up to this many semitones");
        sliderFloat(levelJitter, "%.2f");
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Every pulse's level lowered at random by up to this share");
        ImGui::Spacing();
        ImGui::TextDisabled("Click a table to change it.");
        ImGui::TextDisabled("Formants 1..4 in the header (only those run).");
        ImGui::EndTable();
    }

    // The Envelopes tab's lead card: Gate Mode and the Amp Envelope
    void drawAmpCard(float cw){
        ImGui::BeginChild("##pbAmpCard", ImVec2(cw, 0), ImGuiChildFlags_AutoResizeY);
        heading("AMP ENVELOPE");
        combo(gateMode, {"Auto", "Gated", "Drone"});
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Auto: drones until the Gate is used (a value set, a cable, the timeline),\n"
                              "then each voice plays through the Amp Envelope\n"
                              "Gated: always through the Amp Envelope (Gate 0: silent)\n"
                              "Drone: the Gate opens nothing");
        scEnvCurve::ADSR e{std::max(0.001f, attack.get()), decay.get(), sustain.get(), std::max(0.001f, release.get()),
                           {attackCurve.get(), decayCurve.get(), releaseCurve.get()}};
        if(scEnvCurve::editADSR("##pbAmpEnv", ImVec2(cw, 90), e, pulsarbox::layerColor(0), IM_COL32(13, 16, 24, 255),
                                false, release.getMax())){
            attack = ofClamp(e.a, attack.getMin(), attack.getMax());
            decay = ofClamp(e.d, decay.getMin(), decay.getMax());
            sustain = e.s;
            release = ofClamp(e.r, release.getMin(), release.getMax());
            attackCurve = e.curve[0]; decayCurve = e.curve[1]; releaseCurve = e.curve[2];
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Drag the points; Alt+drag a segment to bend it");
        sliderFloat(attack, "%.3f s", true);
        sliderFloat(decay, "%.3f s", true);
        sliderFloat(sustain, "%.2f");
        sliderFloat(release, "%.3f s", true);
        sliderVector(gate, "%.2f");
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
        ImGui::TextDisabled(ampGated() ? "Each voice plays through it; the Gate's level is its velocity."
                                       : "Droning: the voices play continuously.");
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
    }

    void drawPulseTab(){
        if(!ImGui::BeginTable("##pbpulse", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) return;
        ImGui::TableNextColumn();
        heading("FUNDAMENTAL");
        sliderVector(fundamental, "%.2f Hz", true);
        const float f = voiceFund(std::clamp(viewVoice, 0, voices() - 1));
        const std::string note = pulsarbox::noteName(f);
        ImGui::TextDisabled("%s", (f < 20.0f ? "rhythm: " + ofToString(f * 60.0f, 0) + " pulses / min"
                                             : "pitch: " + note).c_str());
        sliderFloat(transpose, "%.2f st");
        sliderFloat(glide, "%.2f s");
        sliderFloat(rateJitter, "%.3f oct");
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("The rate wanders at random by up to this many octaves (0.02: alive, 1: chaos)");
        ImGui::TableNextColumn();
        heading("VOICE");
        sliderVector(levels, "%.2f");
        sliderVector(gate, "%.2f");
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Each voice's gate: its Amp Envelope (Envelopes tab); its level is the velocity");
        sliderFloat(volume, "%.2f");
        ImGui::TableNextColumn();
        heading("HOW IT WORKS");
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Each voice fires pulses at its Fundamental. A pulse is the pulsaret table played at the "
                            "formant frequency for Cycles periods, shaped by the envelope. Below ~20 Hz you hear a "
                            "rhythm; above, a pitch whose timbre has a peak at every formant. Fundamental and Levels "
                            "take one value per voice (vectors).");
        ImGui::PopTextWrapPos();
        ImGui::EndTable();
    }

    void drawMaskingTab(){
        if(!ImGui::BeginTable("##pbmask", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) return;
        ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableNextColumn();
        heading("STOCHASTIC / BURST");
        sliderFloat(probability, "%.2f");
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Chance that each pulse plays");
        sliderInt(burst);
        sliderInt(rest);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Burst pulses play, then Rest pulses are silent, over and over");
        ImGui::TableNextColumn();
        heading("SIEVE");
        drawSieveEditor();
        ImGui::EndTable();
    }

    void drawSieveEditor(){
        auto s = sieve.get();
        if(s.empty()) s = {1};
        const int n = std::min(int(s.size()), SIEVE_MAX);
        s.resize(n);
        bool changed = false;
        const float w = ImGui::GetContentRegionAvail().x;
        const float cell = std::min(34.0f, (w - 4) / std::max(16, n));
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##sieve", ImVec2(cell * n, 30));
        controlMenu(sieve.getEscapedName());
        auto* dl = ImGui::GetWindowDrawList();
        if(ImGui::IsItemActivated()){
            const int i = std::clamp(int((ImGui::GetIO().MousePos.x - o.x) / cell), 0, n - 1);
            sievePaint = s[i] ? 0 : 1;
        }
        if(ImGui::IsItemActive()){
            const int i = std::clamp(int((ImGui::GetIO().MousePos.x - o.x) / cell), 0, n - 1);
            if(s[i] != sievePaint){ s[i] = sievePaint; changed = true; }
        }
        for(int i = 0; i < n; i++){
            const ImVec2 a(o.x + i * cell + 1, o.y), b(o.x + (i + 1) * cell - 1, o.y + 30);
            dl->AddRectFilled(a, b, s[i] ? pulsarbox::layerColor(0, 0.8f) : IM_COL32(32, 36, 46, 255), 3.0f);
            if(i % 4 == 0) dl->AddText(ImVec2(a.x + 3, b.y - 15), IM_COL32(10, 12, 18, 200), ofToString(i + 1).c_str());
        }
        ImGui::TextDisabled("Steps");
        ImGui::SameLine();
        if(ImGui::SmallButton("-") && n > 1){ s.pop_back(); changed = true; }
        ImGui::SameLine();
        ImGui::Text("%d", int(s.size()));
        ImGui::SameLine();
        if(ImGui::SmallButton("+") && n < SIEVE_MAX){ s.push_back(1); changed = true; }
        ImGui::SameLine(0, 16);
        if(ImGui::SmallButton("All")){ std::fill(s.begin(), s.end(), 1); changed = true; }
        ImGui::SameLine();
        if(ImGui::SmallButton("Invert")){ for(int& x : s) x = !x; changed = true; }
        ImGui::SameLine();
        if(ImGui::SmallButton("<")){ std::rotate(s.begin(), s.begin() + 1, s.end()); changed = true; }
        ImGui::SameLine(0, 2);
        if(ImGui::SmallButton(">")){ std::rotate(s.rbegin(), s.rbegin() + 1, s.rend()); changed = true; }
        ImGui::SameLine();
        if(ImGui::SmallButton("Random")){ for(int& x : s) x = ofRandom(1.0f) < 0.5f; changed = true; }
        // Euclidean: k pulses spread evenly over the steps
        ImGui::SameLine(0, 16);
        ImGui::SetNextItemWidth(80);
        ImGui::SliderInt("##euk", &euclidHits, 1, int(s.size()), "%d hits");
        ImGui::SameLine();
        if(ImGui::SmallButton("Euclid")){
            const int k = std::clamp(euclidHits, 1, int(s.size())), len = int(s.size());
            for(int i = 0; i < len; i++) s[i] = ((i * k) % len) < k;
            changed = true;
        }
        if(changed){ if(s.empty()) s = {1}; sieve = s; }
    }

    void drawSpaceTab(){
        if(!ImGui::BeginTable("##pbspace", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) return;
        ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableNextColumn();
        heading("WHERE EACH PULSE GOES");
        combo(panMode, pulsarbox::panModeNames());
        const char* help[] = {"Voice v plays on output v (+ layer Pan)", "Every pulse moves one output further",
                              "Every pulse lands on a random output", "Layer g of voice v plays on output v + g"};
        ImGui::TextDisabled("%s", help[std::clamp(panMode.get(), 0, 3)]);
        sliderFloat(rotate, "%.2f ch");
        sliderFloat(panJitter, "%.2f ch");
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Every pulse moved at random by up to this many outputs");
        ImGui::TextDisabled("Layer offsets: Pan in each formant.");
        ImGui::TableNextColumn();
        drawOutputRing(ImVec2(ImGui::GetContentRegionAvail().x, 190));
        ImGui::EndTable();
    }

    // The outputs on a ring, and where the next pulses of the viewed voice land
    void drawOutputRing(ImVec2 size){
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##ring", size);
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), IM_COL32(13, 16, 24, 255), 5.0f);
        const int O = outputs(), v = std::clamp(viewVoice, 0, voices() - 1);
        const ImVec2 c(o.x + size.x * 0.5f, o.y + size.y * 0.5f);
        const float r = size.y * 0.38f;
        auto at = [&](float pos){
            if(O == 1) return c;
            if(O == 2){ const float p = std::fmod(std::abs(pos), 2.0f); const float x = p <= 1 ? p : 2 - p; return ImVec2(c.x - r * 1.4f + x * r * 2.8f, c.y); }
            const float a = -1.5708f + 6.28318f * pos / O;
            return ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
        };
        for(int k = 0; k < O; k++){
            const ImVec2 p = at(float(k));
            dl->AddCircle(p, 11, IM_COL32(200, 208, 218, 140), 20, 1.5f);
            dl->AddText(ImVec2(p.x - 4, p.y - 7), IM_COL32(200, 208, 218, 200), ofToString(k + 1).c_str());
        }
        if(panMode.get() == 2){
            dl->AddText(ImVec2(o.x + 8, o.y + 6), IM_COL32(150, 162, 178, 220), "Random: any output, every pulse");
            return;
        }
        // The next 8 pulses (the first brightest)
        for(int j = 7; j >= 0; j--){
            if(!pulseOn(j)) continue;
            for(int g = 0; g < layers(); g++){
                const int pm = panMode.get();
                const float base = pm == 0 ? float(v) : pm == 1 ? float(j + v) : float(v + g);
                const float pos = base + layerPan[g].get() + rotate.get();
                const ImVec2 p = at(O > 2 ? std::fmod(std::fmod(pos, float(O)) + O, float(O)) : pos);
                const ImVec2 q(p.x + (g - 1.5f) * 4.0f, p.y + (g - 1.5f) * 4.0f);
                dl->AddCircleFilled(q, 6.0f - j * 0.5f, pulsarbox::layerColor(g, 1.0f - j * 0.11f));
            }
        }
        dl->AddText(ImVec2(o.x + 8, o.y + 6), IM_COL32(150, 162, 178, 220), "next pulses (brightest first)");
    }

    // ── Table browser ──────────────────────────────────────────────────────
    void drawTableBrowser(){
        if(browseOpen){ ImGui::OpenPopup("##pbtables"); browseOpen = false; browseHover.clear(); }
        ImGui::SetNextWindowSize(ImVec2(720, 470), ImGuiCond_Appearing);
        if(!ImGui::BeginPopup("##pbtables")) return;
        const int g = std::clamp(browseLayer, 0, LAYERS - 1);
        const bool env = browseKind == 1;
        auto& target = env ? envelope[g] : pulsaret[g];
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(pulsarbox::layerColor(g)));
        ImGui::Text("FORMANT %c  %s", char('A' + g), env ? "ENVELOPE" : "PULSARET");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", env ? "the shape of the whole pulse" : "one cycle, played at the formant");
        ImGui::SameLine(ImGui::GetWindowWidth() - 130);
        if(ImGui::Button("Load WAV...")){ pendingDialog = 1 + g * 2 + browseKind; ImGui::CloseCurrentPopup(); }
        const float listH = 300;
        ImGui::BeginChild("##builtins", ImVec2(160, listH), true);
        ImGui::TextDisabled("BUILT-IN");
        for(const auto& name : env ? pulsarbox::envelopeNames() : pulsarbox::pulsaretNames()){
            if(ImGui::Selectable(name.c_str(), target.get() == name)) target = name;
            if(ImGui::IsItemHovered()) browseHover = name;
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##library", ImVec2(0, listH), true);
        if(!libraryScanned) scanLibrary();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##search", "Search the nuPG tables...", librarySearch.data(), librarySearch.size());
        const std::string filter = ofToLower(std::string(librarySearch.data()));
        for(const auto& path : library){
            const std::string stem = std::filesystem::path(path).stem().string();
            if(!filter.empty() && ofToLower(stem).find(filter) == std::string::npos) continue;
            if(ImGui::Selectable(stem.c_str(), target.get() == path)) target = path;
            if(ImGui::IsItemHovered()) browseHover = path;
        }
        if(library.empty()) ImGui::TextDisabled("No WAVs in Supercollider/PulsarBox/nuPG_TABLES or /Tables");
        ImGui::EndChild();
        // Preview of the hovered (else the current) table
        const std::string shown = browseHover.empty() ? target.get() : browseHover;
        const auto& t = tableFor(shown, env);
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const ImVec2 size(ImGui::GetContentRegionAvail().x, 90);
        ImGui::Dummy(size);
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), IM_COL32(13, 16, 24, 255), 4.0f);
        const float mid = env ? o.y + size.y - 6 : o.y + size.y * 0.5f, amp = env ? size.y - 22 : size.y * 0.5f - 12;
        constexpr int n = 400;
        std::vector<ImVec2> pts(n);
        for(int i = 0; i < n; i++){
            const float x = float(i) / (n - 1);
            pts[i] = ImVec2(o.x + 4 + x * (size.x - 8), mid - (env ? pulsarbox::clampRead(t, x) : pulsarbox::wrapRead(t, x * 0.9999)) * amp);
        }
        dl->AddPolyline(pts.data(), n, env ? IM_COL32(235, 240, 245, 220) : pulsarbox::layerColor(g), 0, 1.6f);
        dl->AddText(ImVec2(o.x + 6, o.y + 3), IM_COL32(200, 208, 218, 220), std::filesystem::path(shown).stem().string().c_str());
        ImGui::EndPopup();
    }

    // ════════════════════════════════════════════════════════════════════════
    // Members
    // ════════════════════════════════════════════════════════════════════════

    // Node
    ofParameter<bool> showWindow;
    ofParameter<int> numVoices, numOutputs;
    // Pulse / voice
    ofParameter<std::vector<float>> fundamental, levels, gate, pitchP;
    scScheduledInputs noteInputs;    // the timeline's notes, timetagged
    scAudioParam gateIn, pitchIn;
    bool pitchTouched = false;
    void refreshOutputs() { for(auto& output : scNode::outputs) output = output; }
    ofParameter<float> transpose, glide, attack, release, volume, rateJitter;
    ofParameter<float> decay, sustain, attackCurve, decayCurve, releaseCurve;
    ofParameter<int> gateMode;            // 0 Auto (gated once the Gate is used), 1 Gated, 2 Drone
    bool gateTouched = false;
    int ampGatedSent = -1;
    // Formants
    ofParameter<int> numLayers;
    ofParameter<float> formantShift, cyclesScale, formantJitter, levelJitter;
    std::array<ofParameter<int>, LAYERS> formantMode;
    std::array<ofParameter<float>, LAYERS> formantHz, formantRatio, cycles, layerLevel, layerDelay, layerPan;
    std::array<ofParameter<std::string>, LAYERS> pulsaret, envelope;
    // Masking
    ofParameter<float> probability;
    ofParameter<int> burst, rest;
    ofParameter<std::vector<int>> sieve;
    // Space
    ofParameter<int> panMode;
    ofParameter<float> rotate, panJitter;

    // State
    std::map<ofxSCServer*, ServerState> states;
    std::array<std::array<std::vector<float>, 2>, LAYERS> tables;   // [layer][pulsaret, envelope]
    std::map<std::string, std::vector<float>> tableCache;
    std::set<std::string> failedTables;
    bool controlsDirty = true, tablesDirty = false, structureDirty = false, viewDirty = true, modDirty = false;
    scModulation mod;
    float lastViewTime = -1;
    int viewVoice = 0;
    float viewHeight = 330.0f;
    int pendingDialog = 0;
    int sievePaint = 1, euclidHits = 3;

    // Table browser
    bool browseOpen = false;
    int browseLayer = 0, browseKind = 0;
    std::string browseHover;
    std::vector<std::string> library;
    bool libraryScanned = false;
    std::array<char, 96> librarySearch{};

    // Publishing and presets
    std::vector<PublishAction> publishActions;
    std::vector<std::string> publishedKeys;
    scTimelineMenu timeline;              // "Add to Timeline" from the window
    std::map<std::string, std::shared_ptr<ofxOceanodeAbstractParameter>> publishedHandles;
    bool publishedSeparatorAdded = false;
    // Snapshots and named presets (data/Supercollider/PulsarBox/Presets): scSnapshots
    scSnapshots snaps;
    bool loadingBankPreset = false;   // a whole sound is being set (structure changes wait)

    ofEventListeners listeners;
};
