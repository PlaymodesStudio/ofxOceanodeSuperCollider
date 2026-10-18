//
//  scSlicerBox.cpp
//  ofxOceanodeSuperCollider
//

#include "scSlicerBox.h"
#include "scEnvelopeCurve.h"
#include "serverManager.h"
#include "ofxSCServer.h"
#include "ofxOceanodeShared.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <array>

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#endif

namespace fs = std::filesystem;

namespace {
const char* eqBandNames[5] = {"Low", "Low Mid", "Mid", "High Mid", "High"};
const std::vector<std::string> sliceModeNames = {"Grid", "Transients"};
const std::vector<std::string> playModeNames = {"One-shot", "Gated"};
const std::vector<std::string> seqDivisionNames = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
const int seqDivisionPerBeat[6] = {1, 2, 3, 4, 6, 8};
const std::vector<std::string> gateModeNames = {"One-shot", "Gated"};
constexpr int CAPS[3] = {16, 32, 64};

bool bufferIsLive(ofxSCServer* server, ofxSCBuffer* buffer){
    if(!server || !buffer || buffer->index < 0 || buffer->index >= (int)server->buffers.size()) return false;
    return server->buffers[buffer->index] == buffer;
}
bool controlBusIsLive(ofxSCServer* server, ofxSCBus* bus){
    if(!server || !bus || bus->index < 0 || bus->index >= (int)server->controlBusses.size()) return false;
    return server->controlBusses[bus->index] == bus;
}
bool audioBusIsLive(ofxSCServer* server, ofxSCBus* bus){
    if(!server || !bus || bus->index < 0 || bus->index >= (int)server->audioBusses.size()) return false;
    return server->audioBusses[bus->index] == bus;
}

// Text <-> value for the typed-value popup ("0.2" or "0.1, 0.5, 0.9")
std::string toText(float v){ return ofToString(v); }
std::string toText(int v){ return ofToString(v); }
std::string toText(bool v){ return v ? "1" : "0"; }
template<typename T> std::string toText(const std::vector<T>& v){
    std::string s;
    for(size_t i = 0; i < v.size(); i++) s += (i ? ", " : "") + ofToString(v[i]);
    return s;
}
void fromText(const std::string& s, float& v){ v = ofToFloat(s); }
void fromText(const std::string& s, int& v){ v = ofToInt(s); }
void fromText(const std::string& s, bool& v){ v = ofToFloat(s) > 0.5f; }
template<typename T> void fromText(const std::string& s, std::vector<T>& v){
    v.clear();
    for(auto& item : ofSplitString(s, ",", true, true)){ T x; fromText(item, x); v.push_back(x); }
}

std::string lower(std::string s){
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s;
}

// 32-bit float WAV
bool writeWav(const std::string& path, const std::vector<float>& interleaved, int channels, float sampleRate){
    std::ofstream out(path, std::ios::binary);
    if(!out) return false;
    auto u32 = [&](uint32_t v){ out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v){ out.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t dataBytes = (uint32_t)(interleaved.size() * sizeof(float));
    out.write("RIFF", 4); u32(36 + dataBytes); out.write("WAVE", 4);
    out.write("fmt ", 4); u32(16); u16(3); u16((uint16_t)channels); u32((uint32_t)sampleRate);
    u32((uint32_t)(sampleRate * channels * 4)); u16((uint16_t)(channels * 4)); u16(32);
    out.write("data", 4); u32(dataBytes);
    out.write(reinterpret_cast<const char*>(interleaved.data()), dataBytes);
    return (bool)out;
}

// In-place radix-2 FFT
void fft(std::vector<std::complex<float>>& a){
    const int n = (int)a.size();
    for(int i = 1, j = 0; i < n; i++){
        int bit = n >> 1;
        for(; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if(i < j) std::swap(a[i], a[j]);
    }
    for(int len = 2; len <= n; len <<= 1){
        const float angle = -2.0f * (float)M_PI / len;
        const std::complex<float> step(std::cos(angle), std::sin(angle));
        for(int i = 0; i < n; i += len){
            std::complex<float> w(1.0f, 0.0f);
            for(int j = 0; j < len / 2; j++){
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= step;
            }
        }
    }
}
}

// ════════════════════════════════════════════════════════════════════════════
// Audio: decoding and transients
// ════════════════════════════════════════════════════════════════════════════

scSlicerBox::Audio scSlicerBox::decodeFile(std::string path, std::string tmpDir, bool analyse){
    Audio a;
    a.path = path;
#ifdef __APPLE__
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)path.c_str(), (CFIndex)path.size(), false);
    if(!url){ a.error = "bad path"; return a; }
    ExtAudioFileRef file = nullptr;
    OSStatus status = ExtAudioFileOpenURL(url, &file);
    CFRelease(url);
    if(status != noErr || !file){ a.error = "cannot open this file as audio"; return a; }

    AudioStreamBasicDescription format{};
    UInt32 size = sizeof(format);
    ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileDataFormat, &size, &format);
    const int channels = std::max(1, (int)format.mChannelsPerFrame);
    a.sampleRate = format.mSampleRate > 0 ? (float)format.mSampleRate : 44100.0f;

    AudioStreamBasicDescription client{};
    client.mSampleRate = format.mSampleRate;
    client.mFormatID = kAudioFormatLinearPCM;
    client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    client.mBitsPerChannel = 32;
    client.mChannelsPerFrame = channels;
    client.mFramesPerPacket = 1;
    client.mBytesPerFrame = 4 * channels;
    client.mBytesPerPacket = 4 * channels;
    if(ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat, sizeof(client), &client) != noErr){
        ExtAudioFileDispose(file);
        a.error = "cannot decode this file";
        return a;
    }
    std::vector<float> interleaved;
    std::vector<float> chunk(8192 * channels);
    while(true){
        AudioBufferList list;
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = channels;
        list.mBuffers[0].mDataByteSize = (UInt32)(chunk.size() * sizeof(float));
        list.mBuffers[0].mData = chunk.data();
        UInt32 frames = 8192;
        if(ExtAudioFileRead(file, &frames, &list) != noErr || frames == 0) break;
        interleaved.insert(interleaved.end(), chunk.begin(), chunk.begin() + frames * channels);
    }
    ExtAudioFileDispose(file);
    a.frames = (int)(interleaved.size() / channels);
    if(a.frames < 2){ a.error = "the file holds no audio"; return a; }

    a.mono.resize(a.frames);
    for(int f = 0; f < a.frames; f++){
        float sum = 0;
        for(int c = 0; c < channels; c++) sum += interleaved[(size_t)f * channels + c];
        a.mono[f] = sum / channels;
    }

    // scsynth reads WAV / AIFF / FLAC itself; anything else (and more than two
    // channels) goes to it as a temporary WAV of the first two
    a.channels = std::min(channels, 2);
    const std::string ext = lower(fs::path(path).extension().string());
    const bool readable = ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac";
    if(readable && channels <= 2){
        a.playPath = path;
    }else{
        std::vector<float> played((size_t)a.frames * a.channels);
        for(int f = 0; f < a.frames; f++)
            for(int c = 0; c < a.channels; c++) played[(size_t)f * a.channels + c] = interleaved[(size_t)f * channels + c];
        std::error_code ec;
        fs::create_directories(tmpDir, ec);
        a.playPath = (fs::path(tmpDir) / ("slicerbox_" + ofToString(std::hash<std::string>{}(path)) + ".wav")).string();
        if(!writeWav(a.playPath, played, a.channels, a.sampleRate)){ a.error = "cannot write " + a.playPath; return a; }
    }

    if(!analyse) return a;
    // Spectral flux of the mixdown: how much each band grows from one hop to
    // the next, on a log magnitude (onsets of quiet and loud sounds alike)
    const int N = a.fluxSize, H = a.fluxHop;
    std::vector<float> window(N);
    for(int i = 0; i < N; i++) window[i] = 0.5f - 0.5f * std::cos(2.0f * (float)M_PI * i / (N - 1));
    std::vector<float> prev(N / 2, 0.0f), mag(N / 2);
    std::vector<std::complex<float>> frame(N);
    for(int start = 0; start + N <= a.frames; start += H){
        for(int i = 0; i < N; i++) frame[i] = std::complex<float>(a.mono[start + i] * window[i], 0.0f);
        fft(frame);
        float flux = 0;
        for(int k = 1; k < N / 2; k++){
            mag[k] = std::log1p(100.0f * std::abs(frame[k]));
            flux += std::max(0.0f, mag[k] - prev[k]);
        }
        prev = mag;
        a.flux.push_back(flux);
    }
    const float peak = a.flux.empty() ? 0.0f : *std::max_element(a.flux.begin(), a.flux.end());
    if(peak > 0) for(auto& v : a.flux) v /= peak;
#else
    a.error = "decoding audio files is only implemented on macOS";
#endif
    return a;
}

std::vector<int> scSlicerBox::detectOnsets(const Audio& a, float sensitivity, int minFrames, int from, int to){
    std::vector<int> onsets;
    const auto& flux = a.flux;
    const int n = (int)flux.size();
    if(n < 8) return onsets;
    const int H = a.fluxHop, N = a.fluxSize;
    // Threshold: the local mean (about 0.15 s around) plus a margin that
    // falls as the sensitivity rises
    const int W = std::max(2, (int)(0.15f * a.sampleRate / H / 2));
    // (exponential: the onsets that matter differ from the noise by 0.01..0.1)
    const float delta = 0.4f * std::pow(0.012f / 0.4f, ofClamp(sensitivity, 0.0f, 1.0f));
    std::vector<float> prefix(n + 1, 0.0f);
    for(int i = 0; i < n; i++) prefix[i + 1] = prefix[i] + flux[i];
    struct Peak { int frame; float strength; };
    std::vector<Peak> peaks;
    for(int i = 1; i < n - 1; i++){
        bool localMax = true;
        for(int j = std::max(0, i - 3); j <= std::min(n - 1, i + 3) && localMax; j++)
            if(j != i && flux[j] > flux[i]) localMax = false;
        if(!localMax) continue;
        const int lo = std::max(0, i - W), hi = std::min(n, i + W + 1);
        const float mean = (prefix[hi] - prefix[lo]) / (hi - lo);
        if(flux[i] < mean + delta) continue;
        // The flux peaks when the attack reaches the middle of the window
        const int estimate = i * H + N / 2;
        if(estimate <= from || estimate >= to) continue;
        peaks.push_back({estimate, flux[i] - mean});
    }
    // At least minFrames apart: the stronger of two close onsets stays
    std::vector<Peak> kept;
    for(auto& p : peaks){
        if(!kept.empty() && p.frame - kept.back().frame < minFrames){
            if(p.strength > kept.back().strength) kept.back() = p;
            continue;
        }
        kept.push_back(p);
    }
    // At most MAX_SLICES - 1 boundaries inside: the strongest
    if((int)kept.size() > MAX_SLICES - 1){
        std::sort(kept.begin(), kept.end(), [](const Peak& x, const Peak& y){ return x.strength > y.strength; });
        kept.resize(MAX_SLICES - 1);
        std::sort(kept.begin(), kept.end(), [](const Peak& x, const Peak& y){ return x.frame < y.frame; });
    }
    // Back to the start of the attack: the quiet point before the energy
    // peak, then the nearest zero crossing. The flux peaks as soon as the
    // attack is inside its window, so the attack lies anywhere in the
    // estimate's window: search a window either side of it.
    const int E = 64;
    for(auto& p : kept){
        const int lo = std::max(from, p.frame - N), hi = std::min(to - 1, p.frame + N);
        if(hi - lo < 2 * E){ onsets.push_back(p.frame); continue; }
        std::vector<float> energy;
        for(int s = lo; s + E <= hi; s += E / 2){
            float e = 0;
            for(int k = 0; k < E; k++) e += a.mono[s + k] * a.mono[s + k];
            energy.push_back(e);
        }
        const int top = (int)(std::max_element(energy.begin(), energy.end()) - energy.begin());
        const float floor = energy[top] * 0.08f;
        int j = top;
        while(j > 0 && energy[j - 1] < energy[j] && energy[j] > floor) j--;
        int frame = lo + std::max(0, j - 1) * (E / 2);   // the window before: it may hold the attack's first samples
        for(int d = 0; d < 64; d++){
            const int s = frame - d;
            if(s <= from + 1) break;
            if((a.mono[s - 1] <= 0.0f) != (a.mono[s] <= 0.0f)){ frame = s; break; }
        }
        if(onsets.empty() || frame - onsets.back() >= minFrames / 2) onsets.push_back(frame);
    }
    return onsets;
}

// ════════════════════════════════════════════════════════════════════════════
// Lifetime
// ════════════════════════════════════════════════════════════════════════════

scSlicerBox::scSlicerBox(std::vector<serverManager*> outputServers)
    : scNode("SlicerBox"), servers(outputServers) {
    description = "Cuts an audio file into slices (a grid or its transients) and plays each slice from its own lane "
                  "of the Gate vector: Oceanode vectors or an audio-rate gate. Each slice is one-shot or gated, with "
                  "its own pitch and direction; an EQ on the output. Show opens the waveform, the pads and every "
                  "control; right-click a control there to publish it to the node.";
    for(auto* sm : servers) if(sm && sm->getServer()) { browser.setPreviewServer(sm->getServer()); break; }
    std::string dir = ofToDataPath("Supercollider/Samples", true);
    if(!std::filesystem::exists(dir)) dir = ofToDataPath("", true);
    browser.refresh(dir);
    browser.onActivate = [this](const std::string& path){ pendingDroppedFile = path; browser.stopPreview(); };
}

scSlicerBox::~scSlicerBox(){
    for(auto& [server, st] : states){
        freeChain(st);
        if(st.buffer && bufferIsLive(server, st.buffer)){ st.buffer->free(); }
        delete st.buffer;
        if(st.vis && controlBusIsLive(server, st.vis)) st.vis->free();
        delete st.vis;
        for(auto& f : st.fx){
            if(f.in && audioBusIsLive(server, f.in)) f.in->free();
            delete f.in;
        }
        if(st.seqBus && audioBusIsLive(server, st.seqBus)) st.seqBus->free();
        delete st.seqBus;
        if(st.seqPos && controlBusIsLive(server, st.seqPos)) st.seqPos->free();
        delete st.seqPos;
    }
    states.clear();
    if(decodeTask.valid()) decodeTask.wait();
}

void scSlicerBox::setup(){
    addParameter(showWindow.set("Show", false));
    gate.set("Gate", {0.0f}, {0.0f}, {1.0f});
    auto gateHandle = addParameter(gate);
    // An audio cable plugged into Gate: read sample accurately by the synth
    gateHandle->addReceiveFunc<nodePort>([this](const nodePort& port){
        *gateInput = port;
        for(auto& output : outputs) output = output;
    });
    gateHandle->addDisconnectFunc([this](){
        *gateInput = nodePort();
        for(auto& output : outputs) output = output;
    });
    availableInputs.push_back(gateInput);
    // Reset: restarts the sequence at step 1; an audio cable restarts it on
    // each rising edge, sample accurately
    auto resetHandle = addParameter(seqReset.set("Reset"));
    resetHandle->addReceiveFunc<nodePort>([this](const nodePort& port){
        *resetInput = port;
        for(auto& output : outputs) output = output;
    });
    resetHandle->addDisconnectFunc([this](){
        *resetInput = nodePort();
        for(auto& output : outputs) output = output;
    });
    availableInputs.push_back(resetInput);
    listeners.push(seqReset.newListener([this](){
        for(auto& [server, st] : states) if(st.seq) st.seq->set("reset", 1.0f);
    }));
    scNode::addOutput("Out");

    volume.set("Volume", 1.0f, 0.0f, 2.0f);
    pitch.set("Pitch", 0.0f, -24.0f, 24.0f);
    sliceMode.set("Slice Mode", SliceTransients, 0, NumSliceModes - 1);
    gridSlices.set("Grid Slices", 8, 1, MAX_SLICES);
    sensitivity.set("Sensitivity", 0.5f, 0.0f, 1.0f);
    minLength.set("Min Length", 60.0f, 5.0f, 1000.0f);
    regionStart.set("Start", 0.0f, 0.0f, 1.0f);
    regionEnd.set("End", 1.0f, 0.0f, 1.0f);
    sliceReverse.set("Slice Reverse", {0.0f}, {0.0f}, {1.0f});
    playMode.set("Play Mode", GateOneShot, 0, NumGateModes - 1);
    eqOn.set("EQ On", false);
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
    ampAttack.set("Amp Attack", 0.002f, 0.001f, 5.0f);
    ampDecay.set("Amp Decay", 0.1f, 0.001f, 5.0f);
    ampSustain.set("Amp Sustain", 1.0f, 0.0f, 1.0f);
    ampRelease.set("Amp Release", 0.008f, 0.003f, 5.0f);
    ampCurveA.set("Amp Attack Curve", scEnvCurve::kDefault, scEnvCurve::kMin, scEnvCurve::kMax);
    ampCurveD.set("Amp Decay Curve", scEnvCurve::kDefault, scEnvCurve::kMin, scEnvCurve::kMax);
    ampCurveR.set("Amp Release Curve", scEnvCurve::kDefault, scEnvCurve::kMin, scEnvCurve::kMax);
    filterOn.set("Filter On", false);
    filterType.set("Filter Type", 0, 0, 2);
    filterSlope.set("Filter Slope", 0, 0, 1);
    filterCutoff.set("Filter Cutoff", 2000.0f, 20.0f, 20000.0f);
    filterResonance.set("Filter Resonance", 0.0f, 0.0f, 0.95f);
    // MixerPro's ranges and defaults
    compOn.set("Comp On", false);
    compThreshold.set("Comp Threshold", -18.0f, -60.0f, 0.0f);
    compRatio.set("Comp Ratio", 3.0f, 1.0f, 20.0f);
    compKnee.set("Comp Knee", 6.0f, 0.0f, 24.0f);
    compAttack.set("Comp Attack", 10.0f, 0.01f, 200.0f);
    compRelease.set("Comp Release", 150.0f, 1.0f, 2000.0f);
    compMakeup.set("Comp Makeup", 0.0f, -12.0f, 24.0f);
    compAuto.set("Comp Auto Makeup", false);
    compRms.set("Comp RMS", 0.0f, 0.0f, 1.0f);
    compHpf.set("Comp SC HPF", 20.0f, 10.0f, 2000.0f);
    compMix.set("Comp Mix", 1.0f, 0.0f, 1.0f);
    satOn.set("Sat On", false);
    satDrive.set("Sat Drive", 6.0f, 0.0f, 36.0f);
    satWarmth.set("Sat Warmth", 0.0f, 0.0f, 1.0f);
    satTone.set("Sat Tone", 20000.0f, 500.0f, 20000.0f);
    satOutput.set("Sat Output", 0.0f, -24.0f, 24.0f);
    satAuto.set("Sat Auto Gain", true);
    satMix.set("Sat Mix", 1.0f, 0.0f, 1.0f);
    echoOn.set("Echo On", false);
    echoDelay.set("Echo Delay", 0.375f, 0.005f, 4.0f);
    echoBeats.set("Echo Sync", false);
    echoBeatVal.set("Echo Beats", 0.75f, 0.0625f, 4.0f);
    echoFeed.set("Echo Feed", 0.5f, 0.0f, 0.98f);
    echoCutoff.set("Echo Cutoff", 84.0f, 12.0f, 130.0f);
    echoResonance.set("Echo Resonance", 0.2f, 0.0f, 0.9f);
    echoFilter.set("Echo Filter", 0, 0, 3);
    echoPingPong.set("Echo Ping-pong", 0.0f, 0.0f, 1.0f);
    echoMix.set("Echo Mix", 0.35f, 0.0f, 1.0f);
    rvbOn.set("Reverb On", false);
    rvbMix.set("Reverb Mix", 0.25f, 0.0f, 1.0f);
    rvbDecay.set("Reverb Decay", 2.5f, 0.05f, 30.0f);
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
    seqOn.set("Sequence", false);
    seqSteps.set("Seq Steps", 16, 1, SEQ_STEPS);
    seqDivision.set("Seq Division", 3, 0, (int)seqDivisionNames.size() - 1);
    seqGate.set("Seq Gate", 1.0f, 0.1f, 2.0f);
    seqRandom.set("Seq Random", 0.0f, 0.0f, 1.0f);
    initializePublishActions();

    listeners.push(gate.newListener([this](std::vector<float>& v){
        lastGate = v;
        for(auto& [server, st] : states) sendGate(st.player);
    }));
    auto slicesChanged = [this](std::vector<float>&){ slicesDirty = true; };
    listeners.push(sliceReverse.newListener(slicesChanged));
    listeners.push(playMode.newListener([this](int&){ slicesDirty = true; }));
    listeners.push(pitch.newListener([this](float&){ slicesDirty = true; }));
    listeners.push(volume.newListener([this](float& v){
        for(auto& [server, st] : states) if(st.player) st.player->set("level", (double)v);
    }));
    // The slicing settings cut the file again (not while a preset brings its
    // own markers)
    auto resliceOnChange = [this](float&){ if(!ofxOceanodeShared::isPresetLoading()) reslice(); };
    auto resliceOnChangeInt = [this](int&){ if(!ofxOceanodeShared::isPresetLoading()) reslice(); };
    listeners.push(sliceMode.newListener(resliceOnChangeInt));
    listeners.push(gridSlices.newListener(resliceOnChangeInt));
    listeners.push(sensitivity.newListener(resliceOnChange));
    listeners.push(minLength.newListener(resliceOnChange));
    listeners.push(regionStart.newListener(resliceOnChange));
    listeners.push(regionEnd.newListener(resliceOnChange));
    // Switching a stage on / off: create, wake or fade it (and re-route)
    auto stagesChanged = [this](bool&){
        for(auto& [server, st] : states) if(st.chainCreated && st.player) updateStages(server, st);
    };
    for(auto* p : {&eqOn, &filterOn, &compOn, &satOn, &echoOn, &rvbOn}) listeners.push(p->newListener(stagesChanged));
    // A stage's controls
    auto onStage = [this](int stage){
        return [this, stage](float&){ sendStageAll(stage); };
    };
    for(auto* p : {&filterCutoff, &filterResonance}) listeners.push(p->newListener(onStage(StFilter)));
    for(auto* p : {&filterType, &filterSlope}) listeners.push(p->newListener([this](int&){ sendStageAll(StFilter); }));
    for(auto* p : {&compThreshold, &compRatio, &compKnee, &compAttack, &compRelease, &compMakeup, &compRms, &compHpf, &compMix})
        listeners.push(p->newListener(onStage(StComp)));
    listeners.push(compAuto.newListener([this](bool&){ sendStageAll(StComp); }));
    for(auto* p : {&satDrive, &satWarmth, &satTone, &satOutput, &satMix}) listeners.push(p->newListener(onStage(StSat)));
    listeners.push(satAuto.newListener([this](bool&){ sendStageAll(StSat); }));
    for(auto* p : {&echoDelay, &echoBeatVal, &echoFeed, &echoCutoff, &echoResonance, &echoPingPong, &echoMix})
        listeners.push(p->newListener(onStage(StEcho)));
    listeners.push(echoBeats.newListener([this](bool&){ sendStageAll(StEcho); }));
    listeners.push(echoFilter.newListener([this](int&){ sendStageAll(StEcho); }));
    for(auto* p : {&rvbMix, &rvbDecay, &rvbSize, &rvbPredelay, &rvbLate, &rvbSpread, &rvbLowpass,
                   &rvbHiDamp, &rvbLoDamp, &rvbModRate, &rvbSpin, &rvbWander})
        listeners.push(p->newListener(onStage(StRvb)));
    // The sequencer
    listeners.push(seqOn.newListener([this](bool&){
        for(auto& [server, st] : states) if(st.chainCreated && st.player) ensureSeq(server, st);
    }));
    listeners.push(seqSteps.newListener([this](int&){ seqDirty = true; }));
    listeners.push(seqDivision.newListener([this](int&){ seqDirty = true; }));
    listeners.push(seqGate.newListener([this](float&){ seqDirty = true; }));
    listeners.push(seqRandom.newListener([this](float&){ seqDirty = true; }));
    // The amplitude envelope
    for(auto* p : {&ampAttack, &ampDecay, &ampSustain, &ampRelease, &ampCurveA, &ampCurveD, &ampCurveR})
        listeners.push(p->newListener([this](float&){ for(auto& [server, st] : states) sendAmp(st.player); }));
    auto eqChanged = [this](float&){ eqCurveDirty = true; sendEqAll(); };
    for(int b = 0; b < 5; b++){
        listeners.push(eqGain[b].newListener(eqChanged));
        listeners.push(eqFreq[b].newListener(eqChanged));
        listeners.push(eqShape[b].newListener(eqChanged));
    }
    listeners.push(eqMix.newListener(eqChanged));

    // Files dropped on the window
    dragListener = ofEvents().fileDragEvent.newListener([this](ofDragInfo& info){
        if(!showWindow || !windowHovered || info.files.empty()) return;
        pendingDroppedFile = info.files.front().string();
    });
    reslice();
}

// ════════════════════════════════════════════════════════════════════════════
// Slices
// ════════════════════════════════════════════════════════════════════════════

void scSlicerBox::reslice(){
    float a = ofClamp(regionStart.get(), 0.0f, 1.0f), b = ofClamp(regionEnd.get(), 0.0f, 1.0f);
    if(b - a < 0.001f){ b = std::min(1.0f, a + 0.001f); a = std::max(0.0f, b - 0.001f); }
    points.clear();
    if(sliceMode.get() == SliceTransients && audio.frames > 0 && !audio.flux.empty()){
        const int from = (int)(a * audio.frames), to = (int)(b * audio.frames);
        const int minFrames = std::max(1, (int)(minLength.get() * 0.001f * audio.sampleRate));
        points.push_back(a);
        for(int onset : detectOnsets(audio, sensitivity.get(), minFrames, from, to)){
            if(onset - from < minFrames / 2) continue;   // the region's start is already a boundary
            points.push_back((float)onset / audio.frames);
        }
        points.push_back(b);
    }else{
        const int n = ofClamp(gridSlices.get(), 1, MAX_SLICES);
        for(int i = 0; i <= n; i++) points.push_back(a + (b - a) * i / n);
    }
    selectedSlice = ofClamp(selectedSlice, 0, std::max(0, numSlices() - 1));
    slicesDirty = true;
    for(auto& [server, st] : states) if(st.chainCreated && st.builtCap != sliceCap()) rebuildChain(server, st);
}

void scSlicerBox::sliceBounds(int slice, int& startFrame, int& endFrame) const {
    startFrame = endFrame = 0;
    if(slice < 0 || slice >= numSlices() || audio.frames <= 0) return;
    startFrame = (int)std::round(points[slice] * audio.frames);
    endFrame = std::max(startFrame + 1, (int)std::round(points[slice + 1] * audio.frames));
}

float scSlicerBox::sliceValue(const std::vector<float>& values, int slice, float fallback) const {
    if(values.empty()) return fallback;
    return values[std::min(slice, (int)values.size() - 1)];
}

// One slice's value (the rest keep theirs), or every slice's when editing all
void scSlicerBox::setSliceValue(ofParameter<std::vector<float>>& p, int slice, float value){
    if(editAll){ p = std::vector<float>{value}; return; }
    std::vector<float> values = p.get();
    const int n = std::max(numSlices(), slice + 1);
    const float last = values.empty() ? 0.0f : values.back();
    values.resize(n, last);
    values[slice] = value;
    p = values;
}

int scSlicerBox::sliceCap() const {
    for(int cap : CAPS) if(numSlices() <= cap) return cap;
    return CAPS[2];
}

// ════════════════════════════════════════════════════════════════════════════
// Synths: SlicerBoxPlayer<S>_<C> [-> graphiceq2] -> out
// ════════════════════════════════════════════════════════════════════════════

std::string scSlicerBox::playerDefName() const {
    return "SlicerBoxPlayer" + ofToString(sliceCap()) + "_" + ofToString(std::max(1, audio.channels));
}

void scSlicerBox::ensureBuffer(ofxSCServer* server, ServerState& st){
    if(audio.playPath.empty()) return;
    const bool live = bufferIsLive(server, st.buffer);
    if(live && st.bufferPath == audio.playPath) return;
    if(live) st.buffer->free();
    delete st.buffer;   // after a server reboot its index belongs to nobody
    st.buffer = new ofxSCBuffer(audio.frames, audio.channels, server);
    if(st.buffer->index < 0) { delete st.buffer; st.buffer = nullptr; st.bufferPath.clear(); return; }
    st.buffer->read(audio.playPath);
    st.bufferPath = audio.playPath;
    st.bufferChannels = audio.channels;
}

void scSlicerBox::freeChain(ServerState& st){
    if(st.seq){ st.seq->free(); delete st.seq; st.seq = nullptr; }
    if(st.player){ st.player->free(); delete st.player; st.player = nullptr; }
    for(auto& f : st.fx){
        if(f.synth){ f.synth->free(); delete f.synth; f.synth = nullptr; }
        f.running = false;
        f.offAt = -1.0f;
    }
    st.builtCap = st.builtChannels = 0;
}

void scSlicerBox::rebuildChain(ofxSCServer* server, ServerState& st){
    if(!server || !st.chainCreated) return;
    if(audio.frames <= 0 || audio.playPath.empty()){ freeChain(st); return; }
    ensureBuffer(server, st);
    if(!controlBusIsLive(server, st.vis)){ delete st.vis; st.vis = new ofxSCBus(RATE_CONTROL, MAX_SLICES, server); }
    // A pool ran out (buffers or buses): play nothing rather than write to -1
    if(!st.buffer || !controlBusIsLive(server, st.vis)){
        ofLogError("SlicerBox") << "no free buffer or bus on the server";
        freeChain(st);
        return;
    }

    // Timetagged like the graph's own messages, so everything lands in order
    const bool wasLatency = server->getBLatency();
    server->setBLatency(true);

    // The player: a new definition (slice cap, channels) replaces it in place
    if(!st.player || st.builtCap != sliceCap() || st.builtChannels != audio.channels){
        auto* fresh = new ofxSCSynth(playerDefName(), server);
        fresh->set("buf", st.buffer->index);
        fresh->set("out", st.outBus);
        fresh->set("vis", st.vis->index);
        fresh->set("level", (double)volume.get());
        sendAmp(fresh);
        sendSlices(fresh, true);
        sendGate(fresh);
        sendGateRouting(fresh, st);
        ofxSCSynth* firstFx = nullptr;
        for(auto& f : st.fx) if(f.synth){ firstFx = f.synth; break; }
        if(st.player) fresh->createAndRun(4, st.player->nodeID, getActive());   // replace
        else if(firstFx) fresh->createAndRun(2, firstFx->nodeID, getActive());  // addBefore the effects
        else fresh->createAndRun(0, 1, getActive());
        delete st.player;
        st.player = fresh;
        st.builtCap = sliceCap();
        st.builtChannels = audio.channels;
    }else{
        st.player->set("buf", st.buffer->index);
    }
    updateStages(server, st);
    ensureSeq(server, st);
    server->setBLatency(wasLatency);
}

// ── Step sequencer ─────────────────────────────────────────────────────────

int scSlicerBox::seqPerBeat() const {
    return seqDivisionPerBeat[std::clamp(seqDivision.get(), 0, (int)seqDivisionNames.size() - 1)];
}

void scSlicerBox::sendSeq(ofxSCSynth* s, bool notes){
    if(!s) return;
    s->set("steps", (float)seqSteps.get());
    s->set("perBeat", (float)seqPerBeat());
    s->set("bpm", currentBpm);
    s->set("gatescale", seqGate.get());
    s->set("randamt", seqRandom.get());
    s->set("nslices", (float)std::max(1, numSlices()));
    if(notes){
        s->set("slice", seqSlice);
        s->set("len", seqLen);
    }
}

// The sequencer synth exists while Sequence is on (switching it on starts at
// step 1); the player then reads its gates
void scSlicerBox::ensureSeq(ofxSCServer* server, ServerState& st){
    if(seqOn.get() && st.player){
        if(!audioBusIsLive(server, st.seqBus)){
            delete st.seqBus;
            st.seqBus = new ofxSCBus(RATE_AUDIO, SEQ_STEPS, server);
            if(!audioBusIsLive(server, st.seqBus)){ st.seqBus->free(); delete st.seqBus; st.seqBus = nullptr; }
        }
        if(st.seqPos && (!controlBusIsLive(server, st.seqPos) || st.seqPos->channels != 1 + SEQ_STEPS)){
            if(controlBusIsLive(server, st.seqPos)) st.seqPos->free();
            delete st.seqPos;
            st.seqPos = nullptr;
        }
        if(!st.seqPos){
            st.seqPos = new ofxSCBus(RATE_CONTROL, 1 + SEQ_STEPS, server);   // position, then each step's slice now
            if(!controlBusIsLive(server, st.seqPos)){ st.seqPos->free(); delete st.seqPos; st.seqPos = nullptr; }
        }
        if(st.seqBus && st.seqPos && !st.seq){
            st.seq = new ofxSCSynth("SlicerBoxSeq", server);
            st.seq->set("out", st.seqBus->index);
            st.seq->set("posbus", st.seqPos->index);
            st.seq->set("ext", st.gateBus);
            st.seq->set("resetbus", st.resetBus);
            st.seq->set("run", 1.0f);
            sendSeq(st.seq);
            st.seq->createAndRun(2, st.player->nodeID, getActive());   // addBefore the player
        }
    }else if(st.seq){
        st.seq->free(); delete st.seq; st.seq = nullptr;
        seqPosition = -1.0f;
    }
    sendGateRouting(st.player, st);
}

// ── Effects ────────────────────────────────────────────────────────────────

bool scSlicerBox::stageOn(int stage) const {
    switch(stage){
        case StEq: return eqOn.get();
        case StFilter: return filterOn.get();
        case StComp: return compOn.get();
        case StSat: return satOn.get();
        case StEcho: return echoOn.get();
        case StRvb: return rvbOn.get();
    }
    return false;
}

const char* scSlicerBox::stageDef(int stage){
    static const char* defs[NumStages] = {"graphiceq2", "SlicerBoxFilter", "mixerProComp2", "mixerProSat2", "mixerProEcho2", "mixerProRvb2"};
    return defs[stage];
}

// How long a switched-off stage keeps running (MixerPro's tails)
float scSlicerBox::stageTail(int stage) const {
    if(stage == StEq) return 0.0f;    // no fade: paused at once
    if(stage == StEcho){
        const float feed = ofClamp(echoFeed.get(), 0.0f, 0.98f);
        const float repeats = feed > 0.001f ? std::log(0.001f) / std::log(feed) : 0.0f;
        return std::min(30.0f, echoDelaySec() * (repeats + 1.0f) + 0.2f);
    }
    if(stage == StRvb) return std::min(30.0f, rvbDecay.get() + rvbPredelay.get() * 0.001f + 0.3f);
    return 0.15f;
}

float scSlicerBox::echoDelaySec() const {
    if(!echoBeats.get()) return ofClamp(echoDelay.get(), 0.005f, 4.0f);
    return ofClamp(echoBeatVal.get() * 60.0f / std::max(1.0f, currentBpm), 0.005f, 4.0f);
}

void scSlicerBox::setBpm(float bpm){
    currentBpm = bpm;
    if(echoBeats.get()) sendStageAll(StEcho);
    for(auto& [server, st] : states) if(st.seq) st.seq->set("bpm", currentBpm);
}

void scSlicerBox::sendAmp(ofxSCSynth* s){
    if(!s) return;
    s->set("atk", ampAttack.get());
    s->set("dec", ampDecay.get());
    s->set("sus", ampSustain.get());
    s->set("rel", ampRelease.get());
    s->set("curveA", ampCurveA.get());
    s->set("curveD", ampCurveD.get());
    s->set("curveR", ampCurveR.get());
}

void scSlicerBox::sendStage(int stage, ofxSCSynth* s){
    if(!s) return;
    const float on = stageOn(stage) ? 1.0f : 0.0f;
    switch(stage){
        case StEq:
            sendEq(s);
            break;
        case StFilter:
            s->set("type", (float)ofClamp(filterType.get(), 0, 2));
            s->set("slope", (float)ofClamp(filterSlope.get(), 0, 1));
            s->set("cutoff", ofClamp(filterCutoff.get(), 20.0f, 20000.0f));
            s->set("resonance", ofClamp(filterResonance.get(), 0.0f, 0.95f));
            s->set("on", on);
            break;
        case StComp:
            s->set("threshold", compThreshold.get());
            s->set("ratio", std::max(1.0f, compRatio.get()));
            s->set("knee", std::max(0.0f, compKnee.get()));
            s->set("attack", std::max(0.01f, compAttack.get()));
            s->set("release", std::max(1.0f, compRelease.get()));
            s->set("makeup", compMakeup.get());
            s->set("automakeup", compAuto.get() ? 1.0f : 0.0f);
            s->set("rms", ofClamp(compRms.get(), 0.0f, 1.0f));
            s->set("schpf", ofClamp(compHpf.get(), 10.0f, 2000.0f));
            s->set("mix", ofClamp(compMix.get(), 0.0f, 1.0f));
            s->set("grBus", -1.0f);
            s->set("on", on);
            break;
        case StSat:
            s->set("drive", ofClamp(satDrive.get(), 0.0f, 36.0f));
            s->set("bias", ofClamp(satWarmth.get(), 0.0f, 1.0f));
            s->set("tone", ofClamp(satTone.get(), 500.0f, 20000.0f));
            s->set("output", ofClamp(satOutput.get(), -24.0f, 24.0f));
            s->set("autogain", satAuto.get() ? 1.0f : 0.0f);
            s->set("mix", ofClamp(satMix.get(), 0.0f, 1.0f));
            s->set("on", on);
            break;
        case StEcho:
            s->set("delay", echoDelaySec());
            s->set("feed", ofClamp(echoFeed.get(), 0.0f, 0.98f));
            s->set("cutoff", ofClamp(echoCutoff.get(), 12.0f, 130.0f));
            s->set("resonance", ofClamp(echoResonance.get(), 0.0f, 0.9f));
            s->set("filtertype", (float)ofClamp(echoFilter.get(), 0, 3));
            s->set("pingpong", ofClamp(echoPingPong.get(), 0.0f, 1.0f));
            s->set("mix", ofClamp(echoMix.get(), 0.0f, 1.0f));
            s->set("on", on);
            break;
        case StRvb:
            s->set("mix", ofClamp(rvbMix.get(), 0.0f, 1.0f));
            s->set("decay", std::max(0.05f, rvbDecay.get()));
            s->set("size", ofClamp(rvbSize.get(), 0.0f, 60.0f));
            s->set("predelay", ofClamp(rvbPredelay.get(), 0.0f, 500.0f));
            s->set("position", ofClamp(rvbLate.get(), 0.0f, 1.0f));
            s->set("spread", ofClamp(rvbSpread.get(), -1.0f, 1.0f));
            s->set("lowpass", ofClamp(rvbLowpass.get(), 20.0f, 20000.0f));
            s->set("highdamp", ofClamp(rvbHiDamp.get(), 0.0f, 60.0f));
            s->set("lowdamp", ofClamp(rvbLoDamp.get(), 0.0f, 60.0f));
            s->set("modfreq", ofClamp(rvbModRate.get(), 0.001f, 20.0f));
            s->set("spin", ofClamp(rvbSpin.get(), 0.0f, 20.0f));
            s->set("wander", ofClamp(rvbWander.get(), 0.0f, 20.0f));
            s->set("on", on);
            break;
    }
}

void scSlicerBox::sendStageAll(int stage){
    for(auto& [server, st] : states){
        auto& f = st.fx[stage];
        if(!f.synth) continue;
        sendStage(stage, f.synth);
        if(!stageOn(stage) && stage != StEq) f.synth->set("on", 0.0f);
    }
}

// Create / wake the stages switched on, fade out the ones switched off, then
// route the chain through the running ones
void scSlicerBox::updateStages(ofxSCServer* server, ServerState& st){
    for(int s = 0; s < NumStages; s++){
        auto& f = st.fx[s];
        if(stageOn(s)){
            if(!audioBusIsLive(server, f.in)){
                delete f.in;
                f.in = new ofxSCBus(RATE_AUDIO, 2, server);
                if(!audioBusIsLive(server, f.in)){ f.in->free(); delete f.in; f.in = nullptr; continue; }
                if(f.synth) f.synth->set("in", f.in->index);
            }
            if(!f.synth){
                f.synth = new ofxSCSynth(stageDef(s), server);
                f.synth->set("in", f.in->index);
                f.synth->set("out", st.outBus);
                sendStage(s, f.synth);
                // right after the node before it in the chain
                ofxSCSynth* prev = st.player;
                for(int k = 0; k < s; k++) if(st.fx[k].synth) prev = st.fx[k].synth;
                if(prev) f.synth->createAndRun(3, prev->nodeID, getActive());    // addAfter
                else f.synth->createAndRun(0, 1, getActive());
            }else{
                sendStage(s, f.synth);
                if(!f.running && getActive()) f.synth->run(true);
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

void scSlicerBox::routeChain(ServerState& st){
    int next = st.outBus;
    for(int s = NumStages - 1; s >= 0; s--){
        auto& f = st.fx[s];
        if(!f.synth || !f.running || !f.in) continue;
        f.synth->set("out", next);
        next = f.in->index;
    }
    if(st.player) st.player->set("out", next);
}

void scSlicerBox::sendSlices(ofxSCSynth* synth, bool force){
    if(!synth) return;
    const int cap = sliceCap();
    std::vector<float> start(cap, 0.0f), end(cap, 0.0f), rate(cap, 1.0f), rev(cap, 0.0f), mode(cap, 0.0f);
    for(int i = 0; i < std::min(cap, numSlices()); i++){
        int s, e;
        sliceBounds(i, s, e);
        start[i] = (float)s;
        end[i] = (float)e;
        rate[i] = std::pow(2.0f, pitch.get() / 12.0f);
        rev[i] = sliceValue(sliceReverse.get(), i, 0.0f) > 0.5f ? 1.0f : 0.0f;
        mode[i] = playMode.get() == GateGated ? 1.0f : 0.0f;
    }
    synth->set("start", start);
    synth->set("end", end);
    synth->set("rate", rate);
    synth->set("rev", rev);
    synth->set("mode", mode);
}

void scSlicerBox::sendSlicesAll(){
    for(auto& [server, st] : states) sendSlices(st.player, false);
}

void scSlicerBox::sendGate(ofxSCSynth* synth){
    if(!synth) return;
    std::vector<float> g(sliceCap(), 0.0f);
    const auto& v = gate.get();
    for(int i = 0; i < (int)g.size() && i < (int)v.size(); i++) g[i] = v[i];
    synth->set("gate", g);
}

void scSlicerBox::sendGateRouting(ofxSCSynth* synth, const ServerState& st){
    if(st.seq) st.seq->set("ext", st.gateBus);
    if(!synth) return;
    if(st.seq && st.seqBus){       // the sequencer's gates (with the cable's in them)
        synth->set("gate_sel", 1);
        synth->mapan("gate_ar", st.seqBus->index, sliceCap());
    }else if(st.gateBus >= 0){
        synth->set("gate_sel", 1);
        synth->mapan("gate_ar", st.gateBus, sliceCap());
    }else{
        synth->set("gate_sel", 0);
    }
}

void scSlicerBox::sendEq(ofxSCSynth* s){
    if(!s) return;
    auto vec = [](float v){ return std::vector<float>(2, v); };
    // graphiceq2: bNgain dB, bNfreq Hz, shelves bNslope (= 1/S), peaks bNrq = 1/Q
    for(int b = 0; b < 5; b++){
        const std::string k = "b" + ofToString(b + 1);
        s->set(k + "gain", vec(eqGain[b].get()));
        s->set(k + "freq", vec(eqFreq[b].get()));
        if(scEQEditor::isShelf(b)) s->set(k + "slope", vec(1.0f / scEQEditor::safeShelfSlope(eqShape[b].get(), eqGain[b].get())));
        else s->set(k + "rq", vec(1.0f / std::max(0.01f, eqShape[b].get())));
    }
    s->set("mix", (double)eqMix.get());
}

void scSlicerBox::sendEqAll(){
    for(auto& [server, st] : states) sendEq(st.fx[StEq].synth);
}

void scSlicerBox::audition(int slice, bool on){
    auditioning = on ? slice : -1;
    std::vector<float> a(sliceCap(), 0.0f);
    if(on && slice >= 0 && slice < (int)a.size()) a[slice] = 1.0f;
    for(auto& [server, st] : states) if(st.player) st.player->set("audition", a);
}

// ── Graph ──────────────────────────────────────────────────────────────────

void scSlicerBox::buildSynth(ofxSCServer* server){
    if(!server) return;
    auto& st = states[server];
    freeChain(st);
    st.chainCreated = false;
}

void scSlicerBox::createSynth(ofxSCServer* server){
    auto it = states.find(server);
    if(it == states.end()) return;
    it->second.chainCreated = true;
    rebuildChain(server, it->second);
}

void scSlicerBox::free(ofxSCServer* server){
    auto it = states.find(server);
    if(it == states.end()) return;
    freeChain(it->second);
    it->second.chainCreated = false;
    it->second.outBus = -1;
}

void scSlicerBox::setOutputBus(ofxSCServer* server, int index, int bus){
    if(index != 0) return;
    auto& st = states[server];
    st.outBus = bus;
    routeChain(st);
}

int scSlicerBox::getOutputBusIndex(ofxSCServer* server, int index){
    auto it = states.find(server);
    return (index == 0 && it != states.end()) ? it->second.outBus : -1;
}

void scSlicerBox::moveSynthBefore(ofxSCServer* server, int nodeID){
    auto it = states.find(server);
    if(it == states.end()) return;
    auto& st = it->second;
    routeChain(st);
    // the last node before nodeID, every earlier one before the next
    std::vector<ofxSCSynth*> chain;
    if(st.seq) chain.push_back(st.seq);
    if(st.player) chain.push_back(st.player);
    for(auto& f : st.fx) if(f.synth) chain.push_back(f.synth);
    int before = nodeID;
    for(int k = (int)chain.size() - 1; k >= 0; k--){
        chain[k]->moveBefore(before);
        before = chain[k]->nodeID;
    }
}

// The earliest of this node's synths: the player
int scSlicerBox::getLastSynthID(ofxSCServer* server){
    auto it = states.find(server);
    if(it == states.end()) return -1;
    if(it->second.seq) return it->second.seq->nodeID;
    if(it->second.player) return it->second.player->nodeID;
    for(auto& f : it->second.fx) if(f.synth) return f.synth->nodeID;
    return -1;
}

void scSlicerBox::setInputBus(ofxSCServer* server, scNode* node, int bus){
    auto& st = states[server];
    if(resetInput->getNodeRef() == node){
        st.resetBus = bus;
        if(st.seq) st.seq->set("resetbus", bus);
    }
    if(gateInput->getNodeRef() != node) return;
    st.gateBus = bus;
    sendGateRouting(st.player, st);
}

void scSlicerBox::resetInputBusses(ofxSCServer* server, int targetBus){
    silentBus = targetBus;
    auto& st = states[server];
    st.gateBus = -1;
    st.resetBus = -1;
    if(st.seq) st.seq->set("resetbus", -1.0f);
    sendGateRouting(st.player, st);
}

void scSlicerBox::resendParametersForNRT(){
    for(auto& [server, st] : states){
        if(!st.player) continue;
        st.player->set("level", (double)volume.get());
        sendSlices(st.player, true);
        sendGate(st.player);
        sendGateRouting(st.player, st);
        sendAmp(st.player);
        for(int k = 0; k < NumStages; k++) if(st.fx[k].synth) sendStage(k, st.fx[k].synth);
        sendSeq(st.seq);
    }
}

void scSlicerBox::activate(){
    for(auto& [server, st] : states){
        if(st.seq) st.seq->run(true);
        if(st.player) st.player->run(true);
        for(auto& f : st.fx) if(f.synth && f.running) f.synth->run(true);
    }
}

void scSlicerBox::deactivate(){
    for(auto& [server, st] : states){
        if(st.seq) st.seq->run(false);
        if(st.player) st.player->run(false);
        for(auto& f : st.fx) if(f.synth) f.synth->run(false);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Update
// ════════════════════════════════════════════════════════════════════════════

void scSlicerBox::update(ofEventArgs&){
    // A file chosen in the window or dropped on it
    if(pendingLoadDialog){
        pendingLoadDialog = false;
        auto r = ofSystemLoadDialog("Audio file to slice");
        if(r.bSuccess) pendingDroppedFile = r.getPath();
    }
    if(!pendingDroppedFile.empty() && !decodeTask.valid()){
        pendingPath = pendingDroppedFile;
        pendingDroppedFile.clear();
        loadError.clear();
        decodeTask = std::async(std::launch::async, &scSlicerBox::decodeFile, pendingPath,
                                ofToDataPath("Supercollider/tmp/SlicerBox", true), true);
    }
    if(decodeTask.valid() && decodeTask.wait_for(std::chrono::seconds(0)) == std::future_status::ready){
        Audio decoded = decodeTask.get();
        pendingPath.clear();
        if(!decoded.error.empty()){
            loadError = decoded.error;
            pointsFromPreset = false;
        }else{
            audio = std::move(decoded);
            // Peaks of the whole file for the waveform
            overview.assign(OVERVIEW_BINS, 0.0f);
            for(int f = 0; f < audio.frames; f++){
                const int bin = (int)((int64_t)f * OVERVIEW_BINS / audio.frames);
                overview[bin] = std::max(overview[bin], std::abs(audio.mono[f]));
            }
            viewStart = 0.0f; viewEnd = 1.0f;
            if(pointsFromPreset && points.size() >= 2) slicesDirty = true;
            else reslice();
            pointsFromPreset = false;
            for(auto& [server, st] : states) if(st.chainCreated) rebuildChain(server, st);
        }
    }
    // A rebooted server lost the buffer and the synths
    for(auto& [server, st] : states){
        if(st.chainCreated && audio.frames > 0 && st.buffer && !bufferIsLive(server, st.buffer)){
            freeChain(st);
            rebuildChain(server, st);
        }
    }
    if(slicesDirty){
        slicesDirty = false;
        sendSlicesAll();
    }
    if(numSlices() != sentSlices){ sentSlices = numSlices(); seqDirty = true; }   // Random picks among them
    if(seqDirty){
        seqDirty = false;
        for(auto& [server, st] : states) sendSeq(st.seq);
    }
    seqPosition = -1.0f;
    seqPlaying.clear();
    if(showWindow){
        for(auto& [server, st] : states){
            if(!st.seq || !controlBusIsLive(server, st.seqPos)) continue;
            if(!st.seqPos->readValues.empty()) seqPosition = st.seqPos->readValues[0];
            if((int)st.seqPos->readValues.size() >= 1 + SEQ_STEPS)
                seqPlaying.assign(st.seqPos->readValues.begin() + 1, st.seqPos->readValues.begin() + 1 + SEQ_STEPS);
            const float now = ofGetElapsedTimef();
            if(now >= nextSeqRead){
                nextSeqRead = now + 1.0f / 30.0f;
                server->requestControlValues(st.seqPos->index, 1 + std::clamp(seqSteps.get(), 1, SEQ_STEPS));
            }
            break;
        }
    }
    // Stages switched off: once their tail is over, pause them and go around
    const float now = ofGetElapsedTimef();
    for(auto& [server, st] : states){
        for(auto& f : st.fx){
            if(f.offAt < 0.0f || now < f.offAt) continue;
            f.offAt = -1.0f;
            f.running = false;
            routeChain(st);
            if(f.synth) f.synth->run(false);
        }
    }
    // Playheads, only while the window is shown
    playheads.clear();
    if(showWindow){
        for(auto& [server, st] : states){
            if(!st.player || !controlBusIsLive(server, st.vis)) continue;
            playheads = st.vis->readValues;
            if(now >= nextPlayheadRead){
                nextPlayheadRead = now + 1.0f / 30.0f;
                server->requestControlValues(st.vis->index, std::clamp(numSlices(), 1, MAX_SLICES));
            }
            break;
        }
    }
}

void scSlicerBox::draw(ofEventArgs&){
    if(showWindow) drawWindow();
    else { windowHovered = false; browser.stopPreview(); }
}

// ════════════════════════════════════════════════════════════════════════════
// Presets
// ════════════════════════════════════════════════════════════════════════════

void scSlicerBox::presetSave(ofJson& json){
    json["publishedEditorParameters"] = publishedKeys;
    json["file"] = audio.path.empty() ? pendingPath : audio.path;
    json["points"] = points;
    json["browseDir"] = browser.getDir();
    json["seqSlice"] = seqSlice;
    json["seqLen"] = seqLen;
    json["browserWidth"] = browserWidth;
    json["waveHeight"] = waveHeight / std::max(0.01f, ofxOceanodeShared::getZoomLevel());
    for(const auto& action : publishActions)
        if(action.save && !isPublished(action.key)) action.save(json);
}

void scSlicerBox::loadBeforeConnections(ofJson& json){
    if(!json.contains("publishedEditorParameters") || !json["publishedEditorParameters"].is_array()) return;
    try { syncPublished(json["publishedEditorParameters"].get<std::vector<std::string>>()); } catch(...) {}
}

void scSlicerBox::presetRecallBeforeSettingParameters(ofJson& json){
    loadBeforeConnections(json);
}

void scSlicerBox::presetRecallAfterSettingParameters(ofJson& json){
    for(const auto& action : publishActions){
        if(isPublished(action.key) || !json.contains(action.key) || !action.load) continue;
        action.load(json[action.key]);
    }
    // The preset's own markers, kept when its file arrives
    if(json.contains("points") && json["points"].is_array()){
        try {
            auto saved = json["points"].get<std::vector<float>>();
            if(saved.size() >= 2){ points = saved; pointsFromPreset = true; slicesDirty = true; }
        } catch(...) {}
    }
    if(json.contains("file") && json["file"].is_string()){
        const std::string file = json["file"].get<std::string>();
        if(!file.empty() && file != audio.path) pendingDroppedFile = file;
        else if(file == audio.path) pointsFromPreset = false;
    }
    if(json.contains("browseDir") && json["browseDir"].is_string()) browser.refresh(json["browseDir"].get<std::string>());
    try {
        if(json.contains("seqSlice") && json["seqSlice"].is_array()) seqSlice = json["seqSlice"].get<std::vector<float>>();
        if(json.contains("seqLen") && json["seqLen"].is_array()) seqLen = json["seqLen"].get<std::vector<float>>();
    } catch(...) {}
    seqSlice.resize(SEQ_STEPS, -1.0f);
    seqLen.resize(SEQ_STEPS, 1.0f);
    seqDirty = true;
    if(json.contains("browserWidth") && json["browserWidth"].is_number()) browserWidth = json["browserWidth"].get<float>();
    if(json.contains("waveHeight") && json["waveHeight"].is_number())
        waveHeight = std::max(0.0f, json["waveHeight"].get<float>()) * ofxOceanodeShared::getZoomLevel();
    selectedSlice = ofClamp(selectedSlice, 0, std::max(0, numSlices() - 1));
}

// ════════════════════════════════════════════════════════════════════════════
// Publishing window controls into the node (as AdditiveBox / GrainBox)
// ════════════════════════════════════════════════════════════════════════════

template<typename T>
void scSlicerBox::addPublishAction(ofParameter<T>& parameter, std::vector<std::string> dropdown){
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
    action.save = [key, p](ofJson& json){ json[key] = p->get(); };
    action.load = [p](const ofJson& value){ try { p->set(value.get<T>()); } catch(...) {} };
    action.valueText = [p](){ return toText(p->get()); };
    action.applyText = [p](const std::string& text){ T v; fromText(text, v); p->set(v); };
    publishActions.push_back(std::move(action));
}

void scSlicerBox::initializePublishActions(){
    publishActions.clear();
    addPublishAction(volume);
    addPublishAction(pitch);
    addPublishAction(sliceReverse);
    addPublishAction(playMode, playModeNames);
    addPublishAction(sliceMode, sliceModeNames);
    addPublishAction(gridSlices);
    addPublishAction(sensitivity);
    addPublishAction(minLength);
    addPublishAction(regionStart);
    addPublishAction(regionEnd);
    addPublishAction(eqOn);
    for(int b = 0; b < 5; b++){ addPublishAction(eqGain[b]); addPublishAction(eqFreq[b]); addPublishAction(eqShape[b]); }
    addPublishAction(eqMix);
    for(auto* p : {&ampAttack, &ampDecay, &ampSustain, &ampRelease, &ampCurveA, &ampCurveD, &ampCurveR}) addPublishAction(*p);
    addPublishAction(seqOn);
    addPublishAction(seqSteps);
    addPublishAction(seqDivision, seqDivisionNames);
    addPublishAction(seqGate);
    addPublishAction(seqRandom);
    addPublishAction(filterOn);
    addPublishAction(filterType, std::vector<std::string>{"Low pass", "High pass", "Band pass"});
    addPublishAction(filterSlope, std::vector<std::string>{"12 dB/oct", "24 dB/oct"});
    addPublishAction(filterCutoff);
    addPublishAction(filterResonance);
    addPublishAction(compOn);
    for(auto* p : {&compThreshold, &compRatio, &compKnee, &compAttack, &compRelease, &compMakeup, &compRms, &compHpf, &compMix})
        addPublishAction(*p);
    addPublishAction(compAuto);
    addPublishAction(satOn);
    for(auto* p : {&satDrive, &satWarmth, &satTone, &satOutput, &satMix}) addPublishAction(*p);
    addPublishAction(satAuto);
    addPublishAction(echoOn);
    for(auto* p : {&echoDelay, &echoBeatVal, &echoFeed, &echoCutoff, &echoResonance, &echoPingPong, &echoMix})
        addPublishAction(*p);
    addPublishAction(echoBeats);
    addPublishAction(echoFilter, std::vector<std::string>{"LowPass", "HighPass", "BandPass", "PeakEQ"});
    addPublishAction(rvbOn);
    for(auto* p : {&rvbMix, &rvbDecay, &rvbSize, &rvbPredelay, &rvbLate, &rvbSpread, &rvbLowpass,
                   &rvbHiDamp, &rvbLoDamp, &rvbModRate, &rvbSpin, &rvbWander})
        addPublishAction(*p);
}

const scSlicerBox::PublishAction* scSlicerBox::findPublishAction(const std::string& key) const {
    for(const auto& action : publishActions) if(action.key == key) return &action;
    return nullptr;
}

bool scSlicerBox::isPublished(const std::string& key) const {
    return publishedHandles.count(key) != 0;
}

bool scSlicerBox::hasPublishedConnection(const std::string& key) const {
    auto it = publishedHandles.find(key);
    return it != publishedHandles.end() && it->second
        && (it->second->hasInConnection() || it->second->hasOutConnections());
}

bool scSlicerBox::publishByKey(const std::string& key){
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

bool scSlicerBox::unpublishByKey(const std::string& key){
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

void scSlicerBox::syncPublished(const std::vector<std::string>& keys){
    const auto current = publishedKeys;
    for(const auto& key : current)
        if(std::find(keys.begin(), keys.end(), key) == keys.end() && !hasPublishedConnection(key))
            unpublishByKey(key);
    for(const auto& key : keys) if(!isPublished(key)) publishByKey(key);
}

void scSlicerBox::controlMenu(const std::string& key){
    if(isPublished(key)){
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y - 1.0f), ImVec2(b.x, b.y - 1.0f),
                                            ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
    }
    if(!ImGui::BeginPopupContextItem(("##PublishToNode_" + key).c_str())) return;
    controlMenuItems(key);
    ImGui::EndPopup();
}

void scSlicerBox::controlMenuItems(const std::string& key, const char* title){
    ImGui::PushID(key.c_str());
    const PublishAction* action = findPublishAction(key);
    if(action && action->valueText && action->applyText){
        static std::map<std::string, std::array<char, 2048>> buffers;
        auto& buffer = buffers[key];
        const bool appearing = ImGui::IsWindowAppearing();
        if(appearing) std::snprintf(buffer.data(), buffer.size(), "%s", action->valueText().c_str());
        ImGui::TextDisabled("%s", title ? title : (action->isVector ? "Value (one for all slices, or a,b,c...)" : "Value"));
        ImGui::SetNextItemWidth(180.0f * ofxOceanodeShared::getZoomLevel());
        if(appearing && !title) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##sbTypedValue", buffer.data(), buffer.size(),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if(enter || ImGui::IsItemDeactivatedAfterEdit()){
            action->applyText(std::string(buffer.data()));
            if(enter) ImGui::CloseCurrentPopup();
        }
        ImGui::Separator();
    }
    if(action == nullptr){
        ImGui::TextDisabled("Not publishable");
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
// Window
// ════════════════════════════════════════════════════════════════════════════

ImU32 scSlicerBox::sliceColor(int slice, float alpha) const {
    // Hues walk the circle by the golden angle: neighbours always differ
    const float hue = std::fmod(0.58f + slice * 0.381966f, 1.0f);
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.95f, r, g, b);
    return ImGui::GetColorU32(ImVec4(r, g, b, alpha));
}

float scSlicerBox::frameToX(double frame, float x0, float width) const {
    if(audio.frames <= 0) return x0;
    const double u = frame / audio.frames;
    return x0 + (float)((u - viewStart) / std::max(1e-6f, viewEnd - viewStart)) * width;
}

double scSlicerBox::xToFrame(float x, float x0, float width) const {
    const double u = viewStart + (double)(x - x0) / std::max(1.0f, width) * (viewEnd - viewStart);
    return ofClamp(u, 0.0, 1.0) * audio.frames;
}

bool scSlicerBox::sliderFloat(ofParameter<float>& p, const char* format, bool logarithmic, float width){
    float v = p.get();
    const std::string label = p.getName() + "  " + format;
    ImGui::SetNextItemWidth(width);
    const bool changed = ImGui::SliderFloat(("##" + p.getEscapedName()).c_str(), &v, p.getMin(), p.getMax(), label.c_str(),
                                            logarithmic ? ImGuiSliderFlags_Logarithmic : 0);
    if(changed) p = v;
    controlMenu(p.getEscapedName());
    return changed;
}

bool scSlicerBox::sliderInt(ofParameter<int>& p, float width){
    int v = p.get();
    ImGui::SetNextItemWidth(width);
    const bool changed = ImGui::SliderInt(("##" + p.getEscapedName()).c_str(), &v, p.getMin(), p.getMax(),
                                          (p.getName() + "  %d").c_str());
    if(changed) p = v;
    controlMenu(p.getEscapedName());
    return changed;
}

bool scSlicerBox::comboInt(ofParameter<int>& p, const std::vector<std::string>& options, float width){
    int v = ofClamp(p.get(), 0, (int)options.size() - 1);
    ImGui::SetNextItemWidth(width);
    bool changed = false;
    if(ImGui::BeginCombo(("##" + p.getEscapedName()).c_str(), (p.getName() + ": " + options[v]).c_str())){
        for(int i = 0; i < (int)options.size(); i++)
            if(ImGui::Selectable(options[i].c_str(), i == v)){ p = i; changed = true; }
        ImGui::EndCombo();
    }
    controlMenu(p.getEscapedName());
    return changed;
}

bool scSlicerBox::checkbox(ofParameter<bool>& p){
    bool v = p.get();
    const bool changed = ImGui::Checkbox(p.getName().c_str(), &v);
    if(changed) p = v;
    controlMenu(p.getEscapedName());
    return changed;
}

void scSlicerBox::drawWindow(){
    const std::string title = getWindowTitle();
    bool open = showWindow.get();
    ImGui::SetNextWindowSize(ImVec2(1120.0f, 720.0f), ImGuiCond_FirstUseEver);
    if(ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)){
        ImGui::PushID(this);
        windowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        // Left: the sample browser and its splitter; right: everything else
        const float z = ofxOceanodeShared::getZoomLevel();
        const float totalW = ImGui::GetContentRegionAvail().x, totalH = ImGui::GetContentRegionAvail().y;
        const float bw = std::max(1.0f, std::min(browserWidth * z, totalW * 0.45f));
        ImGui::BeginChild("##sbBrowser", ImVec2(bw, std::max(1.0f, totalH)), false);
        browser.draw(bw, totalH);
        ImGui::EndChild();
        ImGui::SameLine(0, 0);
        scSampleBrowser::drawSplitter("##sbSplit", browserWidth, 5.0f * z, totalH,
                                      100.0f, std::max(100.0f, totalW * 0.45f / z), 1.0f / z);
        ImGui::SameLine(0, 0);
        ImGui::BeginChild("##sbMain", ImVec2(0, std::max(1.0f, totalH)), false);
        drawHeader();
        const float w = ImGui::GetContentRegionAvail().x;
        if(waveHeight <= 0.0f) waveHeight = 220.0f * z;
        drawWaveform(ImVec2(w, waveHeight));
        drawWaveformGrip(w);
        ImGui::Spacing();
        drawPads(w);
        ImGui::Spacing();
        if(ImGui::BeginTabBar("##sbtabs")){
            if(ImGui::BeginTabItem("Slice")){ drawSliceSettings(); ImGui::EndTabItem(); }
            if(ImGui::BeginTabItem("Slicing")){ drawSlicing(); ImGui::EndTabItem(); }
            const float tw = ImGui::GetContentRegionAvail().x;
            if(ImGui::BeginTabItem("Sequence")){ ImGui::Spacing(); drawSequence(tw); ImGui::EndTabItem(); }
            if(ImGui::BeginTabItem("Amp")){ ImGui::Spacing(); drawAmp(tw); ImGui::EndTabItem(); }
            if(ImGui::BeginTabItem("Filter & EQ")){ ImGui::Spacing(); drawFilterEq(tw); ImGui::EndTabItem(); }
            if(ImGui::BeginTabItem("Dynamics")){ ImGui::Spacing(); drawDynamics(tw); ImGui::EndTabItem(); }
            if(ImGui::BeginTabItem("Space")){ ImGui::Spacing(); drawSpace(tw); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        ImGui::PopID();
    }else{
        windowHovered = false;
        browser.stopPreview();
    }
    if(!open) browser.stopPreview();
    ImGui::End();
    if(open != showWindow.get()) showWindow = open;
}

void scSlicerBox::drawHeader(){
    if(ImGui::Button("Load...")) pendingLoadDialog = true;
    ImGui::SameLine();
    if(!pendingPath.empty()){
        ImGui::TextDisabled("Decoding %s ...", ofFilePath::getFileName(pendingPath).c_str());
    }else if(audio.frames > 0){
        ImGui::Text("%s", ofFilePath::getFileName(audio.path).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%.2f s  ·  %d ch  ·  %.0f Hz  ·  %d slices", audio.frames / audio.sampleRate,
                            audio.channels, audio.sampleRate, numSlices());
    }else{
        ImGui::TextDisabled("No file: Load one, or drop it on this window");
    }
    if(!loadError.empty()){
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", loadError.c_str());
    }
    // Volume | Pitch | Play Mode: every slice
    const float third = (ImGui::GetContentRegionAvail().x - 2.0f * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
    sliderFloat(volume, "%.2f", false, third);
    ImGui::SameLine();
    sliderFloat(pitch, "%+.2f st", false, third);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Every slice, resampled (speed follows)\nDouble-click to type a value");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(third);
    {
        int v = ofClamp(playMode.get(), 0, NumGateModes - 1);
        if(ImGui::BeginCombo("##playMode", (std::string("Play Mode: ") + playModeNames[v]).c_str())){
            for(int k = 0; k < NumGateModes; k++)
                if(ImGui::Selectable(playModeNames[k].c_str(), k == v)) playMode = k;
            ImGui::EndCombo();
        }
        controlMenu(playMode.getEscapedName());
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("One-shot: a slice plays to its end on the gate's rising edge\nGated: it stops when its gate falls");
}

// GrainBox's grip under the waveform: drag it up / down (double-click: the
// default height); the height is kept with the patch
void scSlicerBox::drawWaveformGrip(float w){
    const float z = ofxOceanodeShared::getZoomLevel();
    const float gripH = 8.0f * z;
    const ImVec2 g = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##sbWaveResize", ImVec2(std::max(1.0f, w), gripH));
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    const ImU32 col = hov || act ? IM_COL32(180, 180, 180, 200) : IM_COL32(90, 90, 90, 150);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = g.y + gripH * 0.5f;
    dl->AddLine(ImVec2(g.x, cy), ImVec2(g.x + w, cy), col, (hov || act ? 2.0f : 1.0f) * z);
    for(int k = -1; k <= 1; k++) dl->AddCircleFilled(ImVec2(g.x + w * 0.5f + k * 7.0f * z, cy), 2.0f * z, col);
    if(hov || act) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if(hov && !act) ImGui::SetTooltip("Drag to resize the waveform\n(double-click: default height)");
    if(act && ImGui::IsMouseDragging(0, 0.0f))
        waveHeight = std::max(60.0f * z, std::min(1200.0f * z, waveHeight + ImGui::GetIO().MouseDelta.y));
    if(hov && ImGui::IsMouseDoubleClicked(0)) waveHeight = 0.0f;
}

void scSlicerBox::drawWaveform(ImVec2 size){
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    const float midY = (p0.y + p1.y) * 0.5f, halfH = size.y * 0.42f;
    dl->AddRectFilled(p0, p1, IM_COL32(16, 18, 24, 255), 4.0f);
    ImGui::InvisibleButton("##sbwave", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    {   // a file dragged from the browser
        std::string dropped;
        if(browser.acceptDropOnLastItem(dropped)) pendingDroppedFile = dropped;
        if(hovered && browser.isDraggingFile())
            dl->AddRect(p0, p1, IM_COL32(120, 190, 255, 255), 4.0f, 0, 2.0f);
    }
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if(audio.frames <= 0){
        const char* hint = "Double-click a file in the browser, drag one here, or Load...";
        const ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2((p0.x + p1.x - ts.x) * 0.5f, midY - ts.y * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
        return;
    }
    dl->PushClipRect(p0, p1, true);

    // Slices: tinted bands, the selected one brighter, its number at the top
    const int n = numSlices();
    int hoveredSlice = -1;
    for(int i = 0; i < n; i++){
        int s, e;
        sliceBounds(i, s, e);
        const float x0 = frameToX(s, p0.x, size.x), x1 = frameToX(e, p0.x, size.x);
        if(x1 < p0.x || x0 > p1.x) continue;
        const bool active = auditioning == i || (i < (int)lastGate.size() && lastGate[i] > 0)
                            || (i < (int)playheads.size() && playheads[i] > 0);
        dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(x1, p1.y), sliceColor(i, i == selectedSlice ? 0.20f : (active ? 0.16f : 0.07f)));
        if(x1 - x0 > 14){
            char label[8];
            std::snprintf(label, sizeof(label), "%d", i + 1);
            dl->AddText(ImVec2(x0 + 4, p0.y + 3), sliceColor(i, 0.95f), label);
        }
        if(hovered && mouse.x >= x0 && mouse.x < x1) hoveredSlice = i;
    }
    // Outside the region: dimmed
    const float ra = frameToX(regionStart.get() * audio.frames, p0.x, size.x);
    const float rb = frameToX(regionEnd.get() * audio.frames, p0.x, size.x);
    if(ra > p0.x) dl->AddRectFilled(p0, ImVec2(ra, p1.y), IM_COL32(0, 0, 0, 120));
    if(rb < p1.x) dl->AddRectFilled(ImVec2(rb, p0.y), p1, IM_COL32(0, 0, 0, 120));

    // Waveform: peaks of the frames under each pixel
    const ImU32 waveCol = IM_COL32(205, 215, 230, 230);
    const double framesPerPixel = (double)(viewEnd - viewStart) * audio.frames / std::max(1.0f, size.x);
    for(int x = 0; x < (int)size.x; x++){
        const double f0 = (viewStart * audio.frames) + x * framesPerPixel;
        const double f1 = f0 + std::max(1.0, framesPerPixel);
        float peak = 0;
        if(framesPerPixel > (double)audio.frames / OVERVIEW_BINS){
            const int b0 = (int)(f0 * OVERVIEW_BINS / audio.frames), b1 = std::max(b0 + 1, (int)(f1 * OVERVIEW_BINS / audio.frames));
            for(int b = std::max(0, b0); b < std::min(OVERVIEW_BINS, b1); b++) peak = std::max(peak, overview[b]);
        }else{
            for(int f = std::max(0, (int)f0); f < std::min(audio.frames, (int)f1 + 1); f++) peak = std::max(peak, std::abs(audio.mono[f]));
        }
        const float h = std::min(1.0f, peak) * halfH;
        dl->AddLine(ImVec2(p0.x + x + 0.5f, midY - h), ImVec2(p0.x + x + 0.5f, midY + h + 1), waveCol);
    }

    // Markers (interior boundaries are draggable)
    int hoveredMarker = -1;
    for(int i = 0; i < (int)points.size(); i++){
        const float x = frameToX(points[i] * audio.frames, p0.x, size.x);
        if(x < p0.x - 2 || x > p1.x + 2) continue;
        const bool interior = i > 0 && i < (int)points.size() - 1;
        if(interior && hovered && std::abs(mouse.x - x) < 5.0f && dragMarker < 0) hoveredMarker = i;
        const bool hot = hoveredMarker == i || dragMarker == i;
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), hot ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, interior ? 140 : 70), hot ? 2.0f : 1.0f);
        if(interior) dl->AddTriangleFilled(ImVec2(x - 4, p0.y), ImVec2(x + 4, p0.y), ImVec2(x, p0.y + 6), IM_COL32(255, 255, 255, hot ? 255 : 160));
    }

    // Playheads: one per slice playing
    for(int i = 0; i < std::min(n, (int)playheads.size()); i++){
        if(playheads[i] <= 0) continue;
        int s, e;
        sliceBounds(i, s, e);
        const float progress = ofClamp(playheads[i], 0.0f, 1.0f);
        const bool reversed = sliceValue(sliceReverse.get(), i, 0.0f) > 0.5f;
        const double frame = reversed ? e - progress * (e - s) : s + progress * (e - s);
        const float x = frameToX(frame, p0.x, size.x);
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), sliceColor(i, 1.0f), 2.0f);
    }
    dl->PopClipRect();
    dl->AddRect(p0, p1, IM_COL32(60, 66, 80, 255), 4.0f);

    // ── Interaction ─────────────────────────────────────────────────────────
    if(hoveredMarker >= 0 || dragMarker >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if(ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
        if(hoveredMarker >= 0) dragMarker = hoveredMarker;
        else if(hoveredSlice >= 0 && !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)){
            selectedSlice = hoveredSlice;
            audition(hoveredSlice, true);
        }
    }
    if(dragMarker >= 0){
        if(ImGui::IsMouseDown(ImGuiMouseButton_Left)){
            const float lo = points[dragMarker - 1] + 0.0005f, hi = points[dragMarker + 1] - 0.0005f;
            const float u = (float)(xToFrame(mouse.x, p0.x, size.x) / audio.frames);
            if(lo < hi){ points[dragMarker] = ofClamp(u, lo, hi); slicesDirty = true; }
        }else{
            dragMarker = -1;
        }
    }
    if(auditioning >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) audition(auditioning, false);
    // Double-click: a new marker there
    if(hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hoveredMarker < 0 && n < MAX_SLICES){
        const float u = (float)(xToFrame(mouse.x, p0.x, size.x) / audio.frames);
        auto it = std::upper_bound(points.begin(), points.end(), u);
        if(it != points.begin() && it != points.end()){
            const int at = (int)(it - points.begin());
            points.insert(it, u);
            selectedSlice = at;
            slicesDirty = true;
            for(auto& [server, st] : states) if(st.chainCreated && st.builtCap != sliceCap()) rebuildChain(server, st);
        }
    }
    // Right-click: delete a marker, or zoom / re-slice
    static int menuMarker = -1;
    if(hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)){
        menuMarker = hoveredMarker;
        ImGui::OpenPopup("##sbWaveMenu");
    }
    if(ImGui::BeginPopup("##sbWaveMenu")){
        if(menuMarker > 0 && menuMarker < (int)points.size() - 1){
            if(ImGui::MenuItem("Delete marker")){
                points.erase(points.begin() + menuMarker);
                selectedSlice = ofClamp(selectedSlice, 0, std::max(0, numSlices() - 1));
                slicesDirty = true;
            }
            ImGui::Separator();
        }
        if(ImGui::MenuItem("Zoom to fit")){ viewStart = 0.0f; viewEnd = 1.0f; }
        if(ImGui::MenuItem("Re-slice (drops edited markers)")) reslice();
        ImGui::EndPopup();
    }
    // Wheel: zoom around the mouse; horizontal wheel / shift: scroll
    if(hovered){
        const ImGuiIO& io = ImGui::GetIO();
        const float span = viewEnd - viewStart;
        float scroll = io.MouseWheelH;
        if(io.KeyShift) scroll += io.MouseWheel;
        else if(io.MouseWheel != 0){
            const float anchor = viewStart + (mouse.x - p0.x) / size.x * span;
            const float newSpan = ofClamp(span * std::pow(0.85f, io.MouseWheel), 0.0005f, 1.0f);
            viewStart = anchor - (anchor - viewStart) * newSpan / span;
            viewEnd = viewStart + newSpan;
        }
        if(scroll != 0){ viewStart -= scroll * span * 0.05f; viewEnd = viewStart + span; }
        const float s = viewEnd - viewStart;
        if(viewStart < 0){ viewStart = 0; viewEnd = s; }
        if(viewEnd > 1){ viewEnd = 1; viewStart = std::max(0.0f, 1 - s); }
        if(hoveredSlice >= 0 && dragMarker < 0 && hoveredMarker < 0)
            ImGui::SetTooltip("Slice %d  ·  click: select and play  ·  double-click: add a marker", hoveredSlice + 1);
    }
}

void scSlicerBox::drawPads(float width){
    const int n = numSlices();
    if(n == 0) return;
    const float padW = 46.0f, padH = 34.0f, gap = 4.0f;
    const int perRow = std::max(1, (int)((width + gap) / (padW + gap)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for(int i = 0; i < n; i++){
        if(i % perRow != 0) ImGui::SameLine(0, gap);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        ImGui::InvisibleButton("##pad", ImVec2(padW, padH));
        const bool held = ImGui::IsItemActive();
        if(ImGui::IsItemActivated()){ selectedSlice = i; audition(i, true); }
        if(ImGui::IsItemDeactivated() && auditioning == i) audition(i, false);
        const bool playing = held || (i < (int)lastGate.size() && lastGate[i] > 0)
                             || (i < (int)playheads.size() && playheads[i] > 0);
        const ImVec2 p1(p0.x + padW, p0.y + padH);
        dl->AddRectFilled(p0, p1, sliceColor(i, playing ? 0.85f : 0.28f), 4.0f);
        if(i == selectedSlice) dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 230), 4.0f, 0, 2.0f);
        char label[8];
        std::snprintf(label, sizeof(label), "%d", i + 1);
        dl->AddText(ImVec2(p0.x + 5, p0.y + 3), playing ? IM_COL32(15, 15, 20, 255) : IM_COL32(235, 235, 240, 255), label);
        // Badges: reversed, gated, transposed
        std::string badges;
        if(sliceValue(sliceReverse.get(), i, 0.0f) > 0.5f) badges += "R";
        if(!badges.empty()) dl->AddText(ImVec2(p0.x + 5, p0.y + padH - 16), playing ? IM_COL32(15, 15, 20, 200) : IM_COL32(235, 235, 240, 150), badges.c_str());
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Slice %d: hold to play  (R reversed)", i + 1);
        ImGui::PopID();
    }
}

void scSlicerBox::drawSliceSettings(){
    const int n = numSlices();
    if(n == 0){ ImGui::TextDisabled("No slices"); return; }
    selectedSlice = ofClamp(selectedSlice, 0, n - 1);
    const int i = selectedSlice;
    int s, e;
    sliceBounds(i, s, e);
    ImGui::Text("Slice %d", i + 1);
    ImGui::SameLine();
    ImGui::TextDisabled("%.3f s  (%.3f .. %.3f s)", (e - s) / std::max(1.0f, audio.sampleRate),
                        s / std::max(1.0f, audio.sampleRate), e / std::max(1.0f, audio.sampleRate));
    ImGui::SameLine();
    ImGui::Checkbox("Edit all slices", &editAll);

    // Reverse
    bool reversed = sliceValue(sliceReverse.get(), i, 0.0f) > 0.5f;
    if(ImGui::Checkbox(editAll ? "Reverse (all)" : "Reverse", &reversed)) setSliceValue(sliceReverse, i, reversed ? 1.0f : 0.0f);
    controlMenu(sliceReverse.getEscapedName());
    ImGui::Spacing();
    if(ImGui::Button("Reset all slices")){
        sliceReverse = std::vector<float>{0.0f};
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Right-click a control to publish it to the node (as a vector: one value per slice)");
}

void scSlicerBox::drawSlicing(){
    comboInt(sliceMode, sliceModeNames);
    if(sliceMode.get() == SliceGrid){
        sliderInt(gridSlices);
    }else{
        sliderFloat(sensitivity, "%.2f");
        sliderFloat(minLength, "%.0f ms", true);
        if(audio.frames > 0 && audio.flux.empty()) ImGui::TextDisabled("This file is too short to find transients");
    }
    sliderFloat(regionStart, "%.3f");
    sliderFloat(regionEnd, "%.3f");
    if(ImGui::Button("Re-slice")) reslice();
    ImGui::SameLine();
    ImGui::TextDisabled("%d slices  ·  edited markers stay until the settings change  ·  max %d", numSlices(), MAX_SLICES);
}

void scSlicerBox::drawEq(float w, float h){
    for(int b = 0; b < 5; b++) eqEditor.bands[b] = {eqFreq[b].get(), eqGain[b].get(), eqShape[b].get()};
    eqEditor.dbRange = 24.0f;
    eqEditor.gainLimit = eqGain[0].getMax();
    eqEditor.qMin = 0.1f; eqEditor.qMax = 20.0f;
    eqEditor.slopeMin = 0.1f; eqEditor.slopeMax = 4.0f;
    if(eqCurveDirty){
        eqEditor.recompute(servers.empty() || !servers[0] ? 44100.0f : (float)servers[0]->getSampleRate());
        eqCurveDirty = false;
    }
    eqEditor.bandMenu = [this](int b){
        ImGui::TextUnformatted(scEQEditor::bandName(b));
        ImGui::Separator();
        controlMenuItems(eqFreq[b].getEscapedName(), "Freq (Hz)");
        controlMenuItems(eqGain[b].getEscapedName(), "Gain (dB)");
        controlMenuItems(eqShape[b].getEscapedName(), scEQEditor::isShelf(b) ? "Slope" : "Q");
    };
    const bool off = !eqOn.get();
    if(off) ImGui::BeginDisabled();
    if(eqEditor.draw(std::max(120.0f, w), std::max(60.0f, h), scEQEditor::defaultColors(), !off, nullptr, 0, "##sbEqCurve")){
        for(int b = 0; b < 5; b++){
            eqFreq[b] = eqEditor.bands[b].freqHz;
            eqGain[b] = eqEditor.bands[b].gainDb;
            eqShape[b] = eqEditor.bands[b].shape;
        }
    }
    if(off) ImGui::EndDisabled();
}

// ════════════════════════════════════════════════════════════════════════════
// Window: Amp, Filter & EQ, Dynamics, Space (GrainBox's section headers,
// one column per stage)
// ════════════════════════════════════════════════════════════════════════════

namespace {
constexpr float kColGap = 12.0f;
const ImU32 kHeaderBg = IM_COL32(30, 32, 38, 255);
const ImU32 kHeaderText = IM_COL32(205, 205, 215, 255);
const ImU32 kAccent = IM_COL32(120, 190, 255, 140);
float colWidth(float w, int n){ return (w - kColGap * (n - 1)) / n; }
void nextColumn(){ ImGui::EndGroup(); ImGui::SameLine(0, kColGap); ImGui::BeginGroup(); }
}

void scSlicerBox::sectionHeader(const char* title, float w, ofParameter<bool>* on){
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    const float bh = ImGui::GetFrameHeight() + 6.0f;
    dl->AddRectFilled(cur, ImVec2(cur.x + w, cur.y + bh), kHeaderBg, 3.0f);
    dl->AddLine(ImVec2(cur.x, cur.y + bh), ImVec2(cur.x + w, cur.y + bh), kAccent, 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(cur.x + 6.0f, cur.y + 3.0f));
    if(on){
        bool v = on->get();
        ImGui::PushID(title);
        if(ImGui::Checkbox("##on", &v)) *on = v;
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s on / off", title);
        controlMenu(on->getEscapedName());
        ImGui::PopID();
        ImGui::SameLine(0, 4.0f);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, kHeaderText);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(cur.x, cur.y + bh + 4.0f));
    ImGui::Dummy(ImVec2(w, 0.0f));
}

void scSlicerBox::drawAmp(float w){
    const float cw = colWidth(w, 3);
    ImGui::BeginGroup();
    sectionHeader("Amplitude Envelope", cw);
    sliderFloat(ampAttack, "%.3f s", true, cw);
    sliderFloat(ampDecay, "%.3f s", true, cw);
    sliderFloat(ampSustain, "%.2f", false, cw);
    sliderFloat(ampRelease, "%.3f s", true, cw);
    sliderFloat(ampCurveA, "%.2f", false, cw);
    sliderFloat(ampCurveD, "%.2f", false, cw);
    sliderFloat(ampCurveR, "%.2f", false, cw);
    nextColumn();
    // The shape, Alt+drag a segment to bend it
    const float gw = 2 * cw + kColGap;
    sectionHeader("Shape   (drag the points; Alt+drag a segment to bend it)", gw);
    {
        const float gh = 7 * ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y;
        scEnvCurve::ADSR e{ampAttack.get(), ampDecay.get(), ampSustain.get(), ampRelease.get(),
                           {ampCurveA.get(), ampCurveD.get(), ampCurveR.get()}};
        if(scEnvCurve::editADSR("##ampShape", ImVec2(gw, gh), e, IM_COL32(120, 190, 255, 230), IM_COL32(11, 12, 14, 255),
                                false, ampRelease.getMax())){
            ampAttack = ofClamp(e.a, ampAttack.getMin(), ampAttack.getMax());
            ampDecay = ofClamp(e.d, ampDecay.getMin(), ampDecay.getMax());
            ampSustain = e.s;
            ampRelease = ofClamp(e.r, ampRelease.getMin(), ampRelease.getMax());
            ampCurveA = e.curve[0]; ampCurveD = e.curve[1]; ampCurveR = e.curve[2];
        }
    }
    ImGui::EndGroup();
    ImGui::TextDisabled("Every slice. One-shot: the release ends with the slice. Gated: it starts when the slice's gate falls. "
                        "Curves: 0 linear, -4 default.");
}

void scSlicerBox::drawFilterEq(float w){
    const float cw = colWidth(w, 3);
    ImGui::BeginGroup();
    sectionHeader("Filter", cw, &filterOn);
    if(!filterOn.get()) ImGui::BeginDisabled();
    comboInt(filterType, {"Low pass", "High pass", "Band pass"}, cw);
    comboInt(filterSlope, {"12 dB/oct", "24 dB/oct"}, cw);
    sliderFloat(filterCutoff, "%.0f Hz", true, cw);
    sliderFloat(filterResonance, "%.2f", false, cw);
    if(!filterOn.get()) ImGui::EndDisabled();
    nextColumn();
    const float ew = 2 * cw + kColGap;
    sectionHeader("EQ", ew, &eqOn);
    if(!eqOn.get()) ImGui::BeginDisabled();
    sliderFloat(eqMix, "%.2f", false, std::min(ew, 260.0f));
    if(!eqOn.get()) ImGui::EndDisabled();
    drawEq(ew, 200.0f);
    ImGui::EndGroup();
}

void scSlicerBox::drawDynamics(float w){
    const float cw = colWidth(w, 3);
    ImGui::BeginGroup();
    sectionHeader("Compressor", cw, &compOn);
    if(!compOn.get()) ImGui::BeginDisabled();
    sliderFloat(compThreshold, "%.1f dB", false, cw);
    sliderFloat(compRatio, "%.1f : 1", true, cw);
    sliderFloat(compKnee, "%.1f dB", false, cw);
    sliderFloat(compAttack, "%.2f ms", true, cw);
    sliderFloat(compRelease, "%.0f ms", true, cw);
    if(!compOn.get()) ImGui::EndDisabled();
    nextColumn();
    sectionHeader("Compressor Output", cw);
    if(!compOn.get()) ImGui::BeginDisabled();
    sliderFloat(compMakeup, "%+.1f dB", false, cw);
    checkbox(compAuto);
    sliderFloat(compRms, "%.2f", false, cw);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Detector: 0 = peak, 1 = RMS");
    sliderFloat(compHpf, "%.0f Hz", true, cw);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Sidechain high-pass: the lows push the detector less");
    sliderFloat(compMix, "%.2f", false, cw);
    if(!compOn.get()) ImGui::EndDisabled();
    nextColumn();
    sectionHeader("Saturator", cw, &satOn);
    if(!satOn.get()) ImGui::BeginDisabled();
    sliderFloat(satDrive, "%.1f dB", false, cw);
    sliderFloat(satWarmth, "%.2f", false, cw);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Bias: even harmonics");
    sliderFloat(satTone, "%.0f Hz", true, cw);
    sliderFloat(satOutput, "%+.1f dB", false, cw);
    checkbox(satAuto);
    sliderFloat(satMix, "%.2f", false, cw);
    if(!satOn.get()) ImGui::EndDisabled();
    ImGui::EndGroup();
}

void scSlicerBox::drawSpace(float w){
    const float cw = colWidth(w, 3);
    ImGui::BeginGroup();
    sectionHeader("Echo", cw, &echoOn);
    if(!echoOn.get()) ImGui::BeginDisabled();
    checkbox(echoBeats);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Delay as a note value, following the tempo");
    if(echoBeats.get()){
        // note values (in beats), as MixerPro / GrainBox
        static const std::vector<std::pair<const char*, float>> notes = {
            {"1/32", 0.125f}, {"1/16T", 1.0f / 6.0f}, {"1/16", 0.25f}, {"1/8T", 1.0f / 3.0f}, {"1/16D", 0.375f},
            {"1/8", 0.5f}, {"1/4T", 2.0f / 3.0f}, {"1/8D", 0.75f}, {"1/4", 1.0f}, {"1/2T", 4.0f / 3.0f},
            {"1/4D", 1.5f}, {"1/2", 2.0f}, {"1/2D", 3.0f}, {"1 bar", 4.0f}};
        int k = 0;
        for(int i = 0; i < (int)notes.size(); i++)
            if(std::abs(notes[i].second - echoBeatVal.get()) < std::abs(notes[k].second - echoBeatVal.get())) k = i;
        ImGui::SetNextItemWidth(cw);
        const std::string label = std::string("Echo Beats  ") + notes[k].first;
        if(ImGui::SliderInt("##echoBeats", &k, 0, (int)notes.size() - 1, label.c_str())) echoBeatVal = notes[k].second;
        controlMenu(echoBeatVal.getEscapedName());
    }else{
        sliderFloat(echoDelay, "%.3f s", true, cw);
    }
    sliderFloat(echoFeed, "%.2f", false, cw);
    sliderFloat(echoCutoff, "%.0f (note)", false, cw);
    sliderFloat(echoResonance, "%.2f", false, cw);
    comboInt(echoFilter, {"LowPass", "HighPass", "BandPass", "PeakEQ"}, cw);
    sliderFloat(echoPingPong, "%.2f", false, cw);
    sliderFloat(echoMix, "%.2f", false, cw);
    if(!echoOn.get()) ImGui::EndDisabled();
    nextColumn();
    sectionHeader("Reverb", cw, &rvbOn);
    if(!rvbOn.get()) ImGui::BeginDisabled();
    sliderFloat(rvbMix, "%.2f", false, cw);
    sliderFloat(rvbDecay, "%.2f s", true, cw);
    sliderFloat(rvbSize, "%.1f", false, cw);
    sliderFloat(rvbPredelay, "%.0f ms", false, cw);
    sliderFloat(rvbLate, "%.2f", false, cw);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("0 = early reflections, 1 = the late tail");
    sliderFloat(rvbSpread, "%+.2f", false, cw);
    if(!rvbOn.get()) ImGui::EndDisabled();
    nextColumn();
    sectionHeader("Reverb Tone", cw);
    if(!rvbOn.get()) ImGui::BeginDisabled();
    sliderFloat(rvbLowpass, "%.0f Hz", true, cw);
    sliderFloat(rvbHiDamp, "%.1f dB", false, cw);
    sliderFloat(rvbLoDamp, "%.1f dB", false, cw);
    sliderFloat(rvbModRate, "%.3f Hz", true, cw);
    sliderFloat(rvbSpin, "%.2f", false, cw);
    sliderFloat(rvbWander, "%.2f", false, cw);
    if(!rvbOn.get()) ImGui::EndDisabled();
    ImGui::EndGroup();
}

// ════════════════════════════════════════════════════════════════════════════
// Window: Sequence (a piano roll: a row per slice, a column per step)
//   • click an empty cell: a note there (one per step), drag to set its length
//   • drag a note: move it (another step / slice); drag its right edge: length
//   • right-click a note: delete it
// ════════════════════════════════════════════════════════════════════════════

void scSlicerBox::drawSequence(float w){
    const int n = numSlices();
    const int steps = ofClamp(seqSteps.get(), 1, SEQ_STEPS);
    const int perBeat = seqPerBeat();

    // Transport and pattern tools
    {
        bool on = seqOn.get();
        if(ImGui::Button(on ? "Stop" : "Play", ImVec2(64, 0))) seqOn = !on;
        controlMenu(seqOn.getEscapedName());
        ImGui::SameLine();
        sliderInt(seqSteps, 150.0f);
        ImGui::SameLine();
        comboInt(seqDivision, seqDivisionNames, 150.0f);
        ImGui::SameLine();
        sliderFloat(seqGate, "%.2fx", false, 150.0f);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Scales every note's length (Gated play mode)");
        ImGui::SameLine();
        sliderFloat(seqRandom, "%.2f", false, 150.0f);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Continuous random: every time a step comes round, it plays a random\n"
                                                     "slice instead of its own with this chance (rests stay rests).\n"
                                                     "Outlines: what plays this pass.");
        ImGui::SameLine();
        ImGui::TextDisabled("%.1f BPM", currentBpm);
        auto fill = [&](std::function<int(int)> sliceAt){
            for(int j = 0; j < SEQ_STEPS; j++){ seqSlice[j] = (float)sliceAt(j); seqLen[j] = 1.0f; }
            seqDirty = true;
        };
        if(n > 0){
            if(ImGui::Button("In order")) fill([&](int j){ return j % n; });
            ImGui::SameLine();
            if(ImGui::Button("Reversed")) fill([&](int j){ return n - 1 - (j % n); });
            ImGui::SameLine();
            if(ImGui::Button("Random")) fill([&](int){ return (int)ofRandom(n) % n; });
            ImGui::SameLine();
            if(ImGui::Button("Random (with rests)")) fill([&](int){ return ofRandom(1.0f) < 0.3f ? -1 : (int)ofRandom(n) % n; });
            ImGui::SameLine();
        }
        if(ImGui::Button("Clear")) fill([](int){ return -1; });
        ImGui::SameLine();
        ImGui::TextDisabled(playMode.get() == GateGated ? "Gated: notes last their length"
                                                        : "One-shot: a note plays its whole slice");
    }
    if(n == 0){ ImGui::TextDisabled("No slices yet"); return; }

    // The roll: rows from slice 1 (top), at most ~320 px tall (scrolls)
    const float labelW = 34.0f;
    const float rowH = ofClamp(300.0f / n, 12.0f, 22.0f);
    const float gridH = rowH * n;
    const float viewH = std::min(gridH, 330.0f);
    ImGui::BeginChild("##seqRoll", ImVec2(w, viewH + 2.0f), false, gridH > viewH ? 0 : ImGuiWindowFlags_NoScrollbar);
    const float cw = (ImGui::GetContentRegionAvail().x - labelW) / steps;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const ImVec2 g0(o.x + labelW, o.y);
    const ImVec2 g1(g0.x + cw * steps, g0.y + gridH);

    // labels and rows
    for(int r = 0; r < n; r++){
        const float y = o.y + r * rowH;
        dl->AddRectFilled(ImVec2(o.x, y), ImVec2(o.x + labelW - 2, y + rowH - 1), sliceColor(r, 0.55f), 2.0f);
        char label[8];
        std::snprintf(label, sizeof(label), "%d", r + 1);
        dl->AddText(ImVec2(o.x + 4, y + (rowH - ImGui::GetTextLineHeight()) * 0.5f), IM_COL32(235, 235, 240, 255), label);
        dl->AddRectFilled(ImVec2(g0.x, y), ImVec2(g1.x, y + rowH), (r % 2) ? IM_COL32(20, 22, 27, 255) : IM_COL32(16, 18, 22, 255));
    }
    // step / beat lines
    for(int j = 0; j <= steps; j++){
        const float x = g0.x + j * cw;
        const bool beat = j % perBeat == 0, bar = j % (perBeat * 4) == 0;
        dl->AddLine(ImVec2(x, g0.y), ImVec2(x, g1.y), bar ? IM_COL32(255, 255, 255, 70) : beat ? IM_COL32(255, 255, 255, 34) : IM_COL32(255, 255, 255, 12));
    }
    // notes (a long note wraps past the last step)
    auto noteRects = [&](int j, std::vector<std::pair<ImVec2, ImVec2>>& out){
        out.clear();
        const int r = (int)seqSlice[j];
        if(r < 0 || r >= n || j >= steps) return;
        const float len = std::max(0.05f, seqLen[j] * seqGate.get());
        const float y = o.y + r * rowH;
        float start = (float)j, remaining = std::min(len, (float)steps);
        while(remaining > 0.0f){
            const float end = std::min((float)steps, start + remaining);
            out.push_back({ImVec2(g0.x + start * cw + 1, y + 2), ImVec2(g0.x + end * cw - 1, y + rowH - 2)});
            remaining -= end - start;
            start = 0.0f;
        }
    };
    std::vector<std::pair<ImVec2, ImVec2>> rects;
    for(int j = 0; j < steps; j++){
        noteRects(j, rects);
        for(size_t k = 0; k < rects.size(); k++){
            const int r = (int)seqSlice[j];
            dl->AddRectFilled(rects[k].first, rects[k].second, sliceColor(r, k == 0 ? 0.95f : 0.6f), 3.0f);
            if(k == 0) dl->AddRectFilled(rects[k].first, ImVec2(rects[k].first.x + 3, rects[k].second.y), IM_COL32(255, 255, 255, 200), 2.0f);
        }
    }
    // Random: where a step plays another slice this pass, an outline there
    if(seqRandom.get() > 0.0f && (int)seqPlaying.size() >= steps){
        for(int j = 0; j < steps; j++){
            const int r = (int)std::lround(seqPlaying[j]);
            if(r < 0 || r >= n || r == (int)seqSlice[j]) continue;
            const float y = o.y + r * rowH;
            dl->AddRect(ImVec2(g0.x + j * cw + 1, y + 2), ImVec2(g0.x + (j + 1) * cw - 1, y + rowH - 2),
                        sliceColor(r, 0.95f), 3.0f, 0, 1.5f);
        }
    }
    // playhead
    if(seqPosition >= 0.0f){
        const float x = g0.x + seqPosition * cw;
        dl->AddLine(ImVec2(x, g0.y), ImVec2(x, g1.y), IM_COL32(255, 255, 255, 220), 2.0f);
    }

    // Mouse
    ImGui::SetCursorScreenPos(g0);
    ImGui::InvisibleButton("##seqGrid", ImVec2(std::max(1.0f, cw * steps), std::max(1.0f, gridH)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const ImVec2 m = ImGui::GetIO().MousePos;
    const float mStep = (m.x - g0.x) / cw;
    const int mRow = ofClamp((int)((m.y - g0.y) / rowH), 0, n - 1);
    const int mCol = ofClamp((int)mStep, 0, steps - 1);
    // the note under the mouse: its start step and whether on its right edge
    int hit = -1;
    bool edge = false;
    for(int j = 0; j < steps && hit < 0; j++){
        noteRects(j, rects);
        for(auto& rc : rects)
            if(m.x >= rc.first.x && m.x <= rc.second.x + 3 && m.y >= rc.first.y && m.y <= rc.second.y){
                hit = j;
                edge = &rc == &rects.back() && m.x >= rc.second.x - 5;
                break;
            }
    }
    if(ImGui::IsItemHovered()){
        if(edge || seqDragMode == 1) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        else if(hit >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if(seqDragMode == 0)
            ImGui::SetTooltip(hit >= 0 ? "Step %d: slice %d, %.2f steps\nDrag: move  ·  right edge: length  ·  right-click: delete"
                                       : "Step %d, slice %d: click to add a note (drag for its length)",
                              hit >= 0 ? hit + 1 : mCol + 1, hit >= 0 ? (int)seqSlice[hit] + 1 : mRow + 1,
                              hit >= 0 ? seqLen[hit] : 0.0f);
    }
    if(ImGui::IsItemClicked(ImGuiMouseButton_Left)){
        if(hit >= 0 && edge){ seqDragMode = 1; seqDragStep = hit; }
        else if(hit >= 0){ seqDragMode = 2; seqDragStep = hit; seqDragGrab = mStep - hit; }
        else{
            seqSlice[mCol] = (float)mRow;
            seqLen[mCol] = 1.0f;
            seqDragMode = 1; seqDragStep = mCol;
            seqDirty = true;
            audition(mRow, true);
        }
    }
    if(ImGui::IsItemClicked(ImGuiMouseButton_Right) && hit >= 0){
        seqSlice[hit] = -1.0f;
        seqDirty = true;
    }
    if(seqDragMode && ImGui::IsMouseDown(ImGuiMouseButton_Left)){
        if(seqDragMode == 1 && seqDragStep >= 0){
            // length in quarter steps (Shift: free)
            float len = (mStep - seqDragStep) / std::max(0.01f, seqGate.get());
            if(!ImGui::GetIO().KeyShift) len = std::round(len * 4.0f) / 4.0f;
            len = ofClamp(len, 0.25f, (float)SEQ_STEPS);
            if(len != seqLen[seqDragStep]){ seqLen[seqDragStep] = len; seqDirty = true; }
        }else if(seqDragMode == 2 && seqDragStep >= 0){
            const int to = ofClamp((int)std::floor(mStep - seqDragGrab + 0.5f), 0, steps - 1);
            const int row = mRow;
            if(to != seqDragStep || row != (int)seqSlice[seqDragStep]){
                const float len = seqLen[seqDragStep];
                seqSlice[seqDragStep] = -1.0f;
                seqSlice[to] = (float)row;
                seqLen[to] = len;
                seqDragStep = to;
                seqDirty = true;
            }
        }
    }
    if(seqDragMode && !ImGui::IsMouseDown(ImGuiMouseButton_Left)){
        seqDragMode = 0;
        seqDragStep = -1;
        if(auditioning >= 0) audition(auditioning, false);
    }
    ImGui::EndChild();
}
