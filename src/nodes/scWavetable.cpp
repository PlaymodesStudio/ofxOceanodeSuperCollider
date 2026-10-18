//
//  scWavetable.cpp
//  ofxOceanodeSuperCollider
//

#include "scWavetable.h"
#include "serverManager.h"
#include "ofxSCServer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {
// A request waits this long for more changes before decoding (a slider
// dragged over many values decodes once).
constexpr float decodeDebounce = 0.15f;
// /b_allocRead is asynchronous; if the /sync never comes back, swap anyway.
constexpr float loadTimeout = 5.0f;
constexpr int maxDrawnFrames = 64;
constexpr int maxDrawnVoices = 8;

const std::vector<std::string> warpNames = {
    "Off", "Sync", "Bend+", "Bend-", "PWM", "Asym", "Mirror", "Quantize", "PD Sine"};
const std::vector<std::string> cycleModeNames = {
    "Auto", "256", "512", "600 (AKWF)", "1024", "2048", "4096", "Whole File"};
const int cycleModeSizes[] = {0, 256, 512, 600, 1024, 2048, 4096, -1};
const std::vector<std::string> oversampleNames = {"1x", "2x", "4x", "8x"};
const std::vector<std::string> bSourceNames = {"Sine", "Table"};
const std::vector<std::string> sourceNames = {"File", "Live"};
// Floats per /b_setn: 8 KB messages, well under the UDP limit
constexpr int floatsPerMessage = 2048;

void putU32(std::ofstream& out, uint32_t v){ out.write(reinterpret_cast<const char*>(&v), 4); }
void putU16(std::ofstream& out, uint16_t v){ out.write(reinterpret_cast<const char*>(&v), 2); }

// Mono 32-bit float WAV, which scsynth's /b_allocRead reads as is.
bool writeFloatWav(const std::string& path, const std::vector<float>& data, int sampleRate = 48000){
    std::ofstream out(path, std::ios::binary);
    if(!out) return false;
    const uint32_t dataBytes = static_cast<uint32_t>(data.size() * sizeof(float));
    out.write("RIFF", 4); putU32(out, 36 + dataBytes); out.write("WAVE", 4);
    out.write("fmt ", 4); putU32(out, 16);
    putU16(out, 3);                                 // IEEE float
    putU16(out, 1);                                 // mono
    putU32(out, sampleRate);
    putU32(out, sampleRate * sizeof(float));        // byte rate
    putU16(out, sizeof(float));                     // block align
    putU16(out, 32);                                // bits per sample
    out.write("data", 4); putU32(out, dataBytes);
    out.write(reinterpret_cast<const char*>(data.data()), dataBytes);
    return static_cast<bool>(out);
}

uint16_t rd16(const unsigned char* p){ return uint16_t(p[0] | (p[1] << 8)); }
uint32_t rd32(const unsigned char* p){ return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

struct WavData {
    std::vector<float> mono;
    int clm = 0;          // samples per frame from Serum's `clm ` chunk ("<!>2048 ..."), 0 if none
    std::string error;
};

// Any PCM (8/16/24/32-bit) or float (32/64-bit) WAV, channels averaged to mono
WavData readWav(const std::string& path){
    WavData w;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if(!file){ w.error = "Cannot open " + fs::path(path).filename().string(); return w; }
    const size_t size = static_cast<size_t>(file.tellg());
    if(size > (size_t(1) << 29)){ w.error = "File too large"; return w; }
    std::vector<unsigned char> d(size);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(d.data()), size);
    if(size < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0){
        w.error = "Not a WAV file"; return w;
    }
    int format = 0, channels = 0, bits = 0;
    size_t dataOffset = 0, dataSize = 0;
    size_t i = 12;
    while(i + 8 <= size){
        const unsigned char* id = d.data() + i;
        const size_t chunk = rd32(d.data() + i + 4);
        const size_t body = i + 8;
        const size_t available = std::min(chunk, size - body);
        if(std::memcmp(id, "fmt ", 4) == 0 && available >= 16){
            format = rd16(d.data() + body);
            channels = rd16(d.data() + body + 2);
            bits = rd16(d.data() + body + 14);
            if(format == 0xFFFE && available >= 26) format = rd16(d.data() + body + 24);   // extensible
        } else if(std::memcmp(id, "clm ", 4) == 0){
            const std::string text(reinterpret_cast<const char*>(d.data() + body), available);
            const size_t marker = text.find("<!>");
            if(marker != std::string::npos) w.clm = std::max(0, std::atoi(text.c_str() + marker + 3));
        } else if(std::memcmp(id, "data", 4) == 0){
            dataOffset = body;
            dataSize = available;
        }
        i = body + chunk + (chunk & 1);
    }
    const bool pcm = format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    const bool flt = format == 3 && (bits == 32 || bits == 64);
    if(channels < 1 || (!pcm && !flt) || dataOffset == 0){
        w.error = "Unsupported WAV format"; return w;
    }
    const int bytes = bits / 8;
    const size_t frames = dataSize / (size_t(bytes) * channels);
    w.mono.resize(frames);
    const unsigned char* p = d.data() + dataOffset;
    for(size_t f = 0; f < frames; f++){
        double sum = 0;
        for(int c = 0; c < channels; c++, p += bytes){
            double v = 0;
            if(flt && bits == 32){ float x; std::memcpy(&x, p, 4); v = x; }
            else if(flt){ double x; std::memcpy(&x, p, 8); v = x; }
            else if(bits == 8) v = (p[0] - 128) / 128.0;
            else if(bits == 16) v = int16_t(rd16(p)) / 32768.0;
            else if(bits == 24) v = (int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) >> 8) / 8388608.0;
            else v = int32_t(rd32(p)) / 2147483648.0;
            sum += v;
        }
        w.mono[f] = static_cast<float>(sum / channels);
    }
    return w;
}

// One cycle of any length to CYCLE samples through its harmonics: the
// resampled cycle is band-limited (no harmonic above the source's own
// Nyquist) and loops seamlessly. Exact copy when it already has CYCLE samples.
void resampleCycle(const float* in, int len, float* out, bool removeDC){
    constexpr int N = scWavetable::CYCLE;
    double mean = 0;
    for(int n = 0; n < len; n++) mean += in[n];
    mean /= len;
    if(len == N){
        for(int n = 0; n < N; n++) out[n] = static_cast<float>(in[n] - (removeDC ? mean : 0.0));
        return;
    }
    const int K = std::min((len - 1) / 2, N / 2 - 1);
    std::vector<double> cosIn(len), sinIn(len);
    for(int n = 0; n < len; n++){
        cosIn[n] = std::cos(2 * M_PI * n / len);
        sinIn[n] = std::sin(2 * M_PI * n / len);
    }
    std::vector<double> a(K + 1, 0.0), b(K + 1, 0.0);
    for(int k = 1; k <= K; k++){
        double ak = 0, bk = 0;
        int index = 0;
        for(int n = 0; n < len; n++){
            ak += in[n] * cosIn[index];
            bk += in[n] * sinIn[index];
            index += k;
            if(index >= len) index -= len;
        }
        a[k] = ak * 2 / len;
        b[k] = bk * 2 / len;
    }
    std::vector<double> cosOut(N), sinOut(N);
    for(int m = 0; m < N; m++){
        cosOut[m] = std::cos(2 * M_PI * m / N);
        sinOut[m] = std::sin(2 * M_PI * m / N);
    }
    for(int m = 0; m < N; m++){
        double v = removeDC ? 0.0 : mean;
        for(int k = 1; k <= K; k++){
            const int index = (k * m) & (N - 1);
            v += a[k] * cosOut[index] + b[k] * sinOut[index];
        }
        out[m] = static_cast<float>(v);
    }
}

// PREVIEW_POINTS per frame for the waterfall (first point repeated at the end)
std::vector<std::vector<float>> buildPreview(const std::vector<float>& data, int frames){
    constexpr int N = scWavetable::CYCLE, P = scWavetable::PREVIEW_POINTS;
    std::vector<std::vector<float>> preview(frames, std::vector<float>(P));
    for(int f = 0; f < frames; f++){
        for(int i = 0; i < P; i++){
            const int index = int(std::round(float(i) * N / (P - 1))) % N;
            preview[f][i] = data[size_t(f) * N + index];
        }
    }
    return preview;
}

std::string lower(std::string s){
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s;
}

std::string sanitizeFileName(std::string s){
    for(char& c : s) if(!(std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) c = '_';
    return s.empty() ? "wavetable" : s;
}

ImU32 voiceColor(int voice, float alpha = 1.0f){
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(std::fmod(voice * 0.137f + 0.08f, 1.0f), 0.7f, 1.0f, r, g, b);
    return ImGui::GetColorU32(ImVec4(r, g, b, alpha));
}

template<typename T>
T valueFor(const std::vector<T>& v, int i, T fallback){
    return v.empty() ? fallback : v[i % v.size()];
}

// Text <-> value for the typed-value popup ("0.2" or "0.1, 0.5, 0.9")
std::string toText(float v){ return ofToString(v); }
std::string toText(int v){ return ofToString(v); }
std::string toText(bool v){ return v ? "1" : "0"; }
std::string toText(const std::string& v){ return v; }
template<typename T>
std::string toText(const std::vector<T>& v){
    std::string s;
    for(size_t i = 0; i < v.size(); i++) s += (i ? ", " : "") + ofToString(v[i]);
    return s;
}
void fromText(const std::string& s, float& v){ v = ofToFloat(s); }
void fromText(const std::string& s, int& v){ v = ofToInt(s); }
void fromText(const std::string& s, bool& v){ v = ofToBool(s) || ofToInt(s) != 0; }
void fromText(const std::string& s, std::string& v){ v = s; }
template<typename T>
void fromText(const std::string& s, std::vector<T>& v){
    v.clear();
    for(auto& item : ofSplitString(s, ",", true, true)){ T x; fromText(item, x); v.push_back(x); }
}
}

// ════════════════════════════════════════════════════════════════════════════
// Lifetime
// ════════════════════════════════════════════════════════════════════════════

scWavetable::scWavetable(std::vector<serverManager*> outputServers)
    : scNode("Wavetable"), servers(outputServers) {
    description = "Wavetable oscillator (Serum / Vital / AKWF tables, or a folder of single cycles) "
                  "with position, 2D grid, phase warps and FM, band-limited at any pitch. "
                  "Show opens the waterfall view and every control; right-click a control there "
                  "to publish it to the node.";
}

scWavetable::~scWavetable(){
    for(auto& [server, synth] : synths){ if(synth){ synth->free(); delete synth; } }
    synths.clear();
    for(auto& [server, b] : buffers){ releaseBuffer(server, b.table); releaseBuffer(server, b.sine); }
    for(auto& [server, p] : pendingLoads){ releaseBuffer(server, p.buffers.table); releaseBuffer(server, p.buffers.sine); }
    for(auto& [server, l] : live){ releaseBuffer(server, l.buffer[0]); releaseBuffer(server, l.buffer[1]); }
    if(decodeTask.valid()) decodeTask.wait();
    std::error_code ec;
    for(const auto& f : oldWavPaths) fs::remove(f, ec);
    if(!table.wavPath.empty()) fs::remove(table.wavPath, ec);
    if(!sineWavPath.empty()) fs::remove(sineWavPath, ec);
}

void scWavetable::setup(){
    // ── Node ─────────────────────────────────────────────────────────────────
    addParameter(showWindow.set("Show", false));
    addParameter(numChannels.set("N Chan", 1, 1, MAX_CHANNELS));
    addOutputParameter(framesOut.set("Frames", 0, 0, MAX_FRAMES));
    scNode::addOutput("Out");

    // ── Window (any of these can be published to the node) ──────────────────
    source.set("Source", 0, 0, int(sourceNames.size()) - 1);
    liveIn.set("Live In", {0.0f}, {-1.0f}, {1.0f});
    liveFrames.set("Live Frames", 1, 1, 64);
    rescale.set("Rescale", false);
    liveXFade.set("Live XFade", 15.0f, 1.0f, 500.0f);
    path.set("Path", "");
    cycleMode.set("Cycle Size", 0, 0, int(cycleModeNames.size()) - 1);
    maxFrames.set("Max Frames", 256, 1, MAX_FRAMES);
    normalize.set("Normalize", true);
    removeDC.set("Remove DC", true);
    embedTable.set("Embed Table", false);
    pitch.set("Pitch", {60.0f}, {0.0f}, {127.0f});
    levels.set("Levels", {0.5f}, {0.0f}, {1.0f});
    position.set("Position", {0.0f}, {0.0f}, {1.0f});
    rows.set("Rows", 1, 1, 16);
    posY.set("Pos Y", {0.0f}, {0.0f}, {1.0f});
    warp1.set("Warp 1", {0}, {0}, {NumWarps - 1});
    warp1Amt.set("Warp 1 Amt", {0.0f}, {0.0f}, {1.0f});
    warp2.set("Warp 2", {0}, {0}, {NumWarps - 1});
    warp2Amt.set("Warp 2 Amt", {0.0f}, {0.0f}, {1.0f});
    bSource.set("B Source", 0, 0, int(bSourceNames.size()) - 1);
    fm.set("FM", {0.0f}, {0.0f}, {10.0f});
    fmRatio.set("FM Ratio", {1.0f}, {0.0f}, {16.0f});
    bPos.set("B Pos", {0.0f}, {0.0f}, {1.0f});
    bFeedback.set("B Feedback", {0.0f}, {0.0f}, {10.0f});
    fmDamp.set("FM Damp", {1.0f}, {0.0f}, {1.0f});
    phase.set("Phase", {0.0f}, {0.0f}, {1.0f});
    trig.set("Trig", {0}, {0}, {1});
    oversample.set("Oversample", 0, 0, int(oversampleNames.size()) - 1);

    initializePublishActions();
    publishByKey(pitch.getEscapedName());
    publishByKey(levels.getEscapedName());
    publishByKey(position.getEscapedName());

    // ── Listeners ────────────────────────────────────────────────────────────
    nodeListeners.push(numChannels.newListener([this](int& n){
        if(n != builtChannels) rebuildGraph();
    }));
    nodeListeners.push(rows.newListener([this](int& r){
        if((r > 1) != built2D) rebuildGraph();
        else for(auto& [server, synth] : synths) if(synth) synth->set("rows", r);
    }));
    nodeListeners.push(oversample.newListener([this](int& o){
        if(o != builtOversample) rebuildGraph();
    }));
    for(auto* p : {&path}) nodeListeners.push(p->newListener([this](std::string&){ requestDecode(); }));
    nodeListeners.push(source.newListener([this](int& s){ setSource(s); }));
    nodeListeners.push(liveIn.newListener([this](std::vector<float>&){ liveDirty = true; }));
    nodeListeners.push(liveFrames.newListener([this](int&){ liveDirty = true; }));
    nodeListeners.push(rescale.newListener([this](bool&){ liveDirty = true; }));
    nodeListeners.push(liveXFade.newListener([this](float& ms){
        for(auto& [server, synth] : synths) if(synth) synth->set("xftime", ms / 1000.0f);
    }));
    for(auto* p : {&cycleMode, &maxFrames}) nodeListeners.push(p->newListener([this](int&){ requestDecode(); }));
    for(auto* p : {&normalize, &removeDC}) nodeListeners.push(p->newListener([this](bool&){ requestDecode(); liveDirty = true; }));

    auto bindFloat = [this](ofParameter<std::vector<float>>& p, std::string control, float fallback){
        nodeListeners.push(p.newListener([this, control, fallback](std::vector<float>& v){ sendVector(control, v, fallback); }));
    };
    bindFloat(pitch, "pitch", 60);
    bindFloat(levels, "levels", 0);
    bindFloat(position, "pos", 0);
    bindFloat(posY, "posy", 0);
    bindFloat(warp1Amt, "w1amt", 0);
    bindFloat(warp2Amt, "w2amt", 0);
    bindFloat(fm, "fm", 0);
    bindFloat(fmRatio, "ratio", 1);
    bindFloat(bPos, "bpos", 0);
    bindFloat(bFeedback, "fb", 0);
    bindFloat(fmDamp, "damp", 1);
    bindFloat(phase, "phase", 0);
    auto bindInt = [this](ofParameter<std::vector<int>>& p, std::string control){
        nodeListeners.push(p.newListener([this, control](std::vector<int>& v){
            sendVector(control, std::vector<float>(v.begin(), v.end()), 0);
        }));
    };
    bindInt(warp1, "w1mode");
    bindInt(warp2, "w2mode");
    nodeListeners.push(bSource.newListener([this](int&){
        for(auto& [server, synth] : synths) sendBuffers(server);
    }));
    nodeListeners.push(trig.newListener([this](std::vector<int>& v){ sendTriggers(v); }));

    // Oscillator B's sine: one cycle, written once per node
    std::error_code ec;
    fs::create_directories(getTmpDir(), ec);
    sineWavPath = getTmpDir() + "/wt_" + ofToString(reinterpret_cast<uintptr_t>(this)) + "_sine.wav";
    std::vector<float> sine(CYCLE);
    for(int i = 0; i < CYCLE; i++) sine[i] = std::sin(2 * M_PI * i / CYCLE);
    writeFloatWav(sineWavPath, sine);

    requestDecode();   // the built-in table until a file is chosen
}

// ════════════════════════════════════════════════════════════════════════════
// Frame
// ════════════════════════════════════════════════════════════════════════════

void scWavetable::update(ofEventArgs&){
    const float now = ofGetElapsedTimef();

    if(pendingDialog){
        const bool folder = pendingDialog == 2;
        pendingDialog = 0;
        openDialog(folder);
    }
    if(decodeTask.valid() && decodeTask.wait_for(std::chrono::seconds(0)) == std::future_status::ready){
        applyTable(decodeTask.get());
    }
    if(decodeRequested && !decodeTask.valid() && now - decodeRequestTime >= decodeDebounce){
        startDecode();
    }

    std::vector<ofxSCServer*> synced;
    for(auto& [server, pending] : pendingLoads){
        if(!server->isNRTSyncPending() || now - pending.startTime > loadTimeout) synced.push_back(server);
    }
    for(auto* server : synced) swapBuffers(server);

    // Live: new buffers allocated -> first upload; new data -> upload
    for(auto& [server, l] : live){
        if(l.allocPending && (!server->isNRTSyncPending() || now - l.allocTime > loadTimeout)){
            l.allocPending = false;
            uploadLive(server);
        } else if(l.uploadPending){
            uploadLive(server);
        }
    }
    if(liveDirty && isLive()){
        liveDirty = false;
        processLive();
    }
    if(pendingLoads.empty() && !oldWavPaths.empty()){
        std::error_code ec;
        for(const auto& f : oldWavPaths) fs::remove(f, ec);
        oldWavPaths.clear();
    }
}

void scWavetable::draw(ofEventArgs&){
    if(showWindow) drawWindow();
}

// ════════════════════════════════════════════════════════════════════════════
// Decoding
// ════════════════════════════════════════════════════════════════════════════

std::string scWavetable::getTmpDir() const {
    return ofToDataPath("Supercollider/tmp/wavetable", true);
}

std::string scWavetable::resolvePath(const std::string& p) const {
    if(p.empty()) return "";
    std::error_code ec;
    if(fs::path(p).is_absolute()) return p;
    const std::string data = ofToDataPath(p, true);
    if(fs::exists(data, ec)) return data;
    return ofToDataPath("Supercollider/Wavetables/" + p, true);
}

void scWavetable::requestDecode(){
    decodeRequested = true;
    decodeRequestTime = ofGetElapsedTimef();
}

void scWavetable::startDecode(){
    decodeRequested = false;
    if(isLive()) return;   // decoded again when the source goes back to File
    DecodeSettings settings;
    settings.path = resolvePath(path.get());
    settings.cycleMode = cycleMode;
    settings.normalize = normalize;
    settings.removeDC = removeDC;
    settings.maxFrames = maxFrames;
    status = "Loading...";
    const std::string prefix = "wt_" + ofToString(reinterpret_cast<uintptr_t>(this));
    decodeTask = std::async(std::launch::async, &scWavetable::decode, settings, ++decodeGeneration, getTmpDir(), prefix);
}

scWavetable::Table scWavetable::builtinTable(){
    Table t;
    t.name = "Built-in: sine, triangle, saw, square";
    t.frames = 4;
    t.data.resize(4 * CYCLE);
    for(int i = 0; i < CYCLE; i++){
        const float p = float(i) / CYCLE;
        t.data[i] = std::sin(2 * M_PI * p);
        t.data[CYCLE + i] = p < 0.25f ? 4 * p : (p < 0.75f ? 2 - 4 * p : 4 * p - 4);
        t.data[2 * CYCLE + i] = p < 0.5f ? 2 * p : 2 * p - 2;
        t.data[3 * CYCLE + i] = p < 0.5f ? 1.0f : -1.0f;
    }
    return t;
}

scWavetable::Table scWavetable::decode(DecodeSettings s, uint64_t generation, std::string tmpDir, std::string prefix){
    Table t;
    std::error_code ec;
    if(s.path.empty()){
        t = builtinTable();
    } else {
        std::vector<fs::path> files;
        const bool folder = fs::is_directory(s.path, ec);
        if(folder){
            for(const auto& entry : fs::directory_iterator(s.path, ec)){
                if(entry.is_regular_file(ec) && lower(entry.path().extension().string()) == ".wav")
                    files.push_back(entry.path());
            }
            std::sort(files.begin(), files.end());
            if(files.empty()){ t.error = "No WAV files in " + fs::path(s.path).filename().string(); t.generation = generation; return t; }
        } else {
            files.push_back(s.path);
        }

        const int mode = std::clamp(s.cycleMode, 0, int(cycleModeNames.size()) - 1);
        const int maxFrames = std::clamp(s.maxFrames, 1, MAX_FRAMES);
        int filesUsed = 0;
        for(const auto& file : files){
            if(t.frames >= maxFrames) break;
            WavData w = readWav(file.string());
            if(!w.error.empty()){
                if(!folder){ t.error = w.error; t.generation = generation; return t; }
                continue;
            }
            const int len = int(w.mono.size());
            if(len < 2) continue;
            int cycle = cycleModeSizes[mode];
            if(cycle == 0){
                // Auto: Serum's frame size, else 2048-sample frames, else the
                // whole (short) file is one cycle (AKWF: 600 samples)
                if(w.clm > 0) cycle = w.clm;
                else if(len % CYCLE == 0) cycle = CYCLE;
                else if(len <= 2 * CYCLE) cycle = len;
                else cycle = CYCLE;
            } else if(cycle < 0){
                cycle = len;
            }
            cycle = std::min(cycle, len);
            const int count = len / cycle;
            for(int c = 0; c < count && t.frames < maxFrames; c++){
                t.data.resize(size_t(t.frames + 1) * CYCLE);
                resampleCycle(w.mono.data() + size_t(c) * cycle, cycle, t.data.data() + size_t(t.frames) * CYCLE, s.removeDC);
                t.frames++;
            }
            filesUsed++;
        }
        if(t.frames == 0){ t.error = "No cycles found"; t.generation = generation; return t; }
        t.name = folder ? fs::path(s.path).filename().string() + " (" + std::to_string(filesUsed) + " files)"
                        : fs::path(s.path).stem().string();
        if(s.normalize){
            float peak = 0;
            for(float v : t.data) peak = std::max(peak, std::abs(v));
            if(peak > 1e-9f) for(float& v : t.data) v /= peak;
        }
    }
    t.generation = generation;
    fs::create_directories(tmpDir, ec);
    t.wavPath = tmpDir + "/" + prefix + "_" + std::to_string(generation) + ".wav";
    if(!writeFloatWav(t.wavPath, t.data)){
        t.error = "Cannot write " + t.wavPath;
        t.wavPath.clear();
    }
    return t;
}

void scWavetable::applyTable(Table decoded){
    if(decoded.generation != decodeGeneration){   // superseded by a newer request
        std::error_code ec;
        if(!decoded.wavPath.empty()) fs::remove(decoded.wavPath, ec);
        return;
    }
    if(isLive()){   // switched to Live while decoding: keep the live table in view
        std::error_code ec;
        if(!decoded.wavPath.empty()) fs::remove(decoded.wavPath, ec);
        return;
    }
    if(!decoded.error.empty()){
        status = decoded.error;
        ofLogWarning("scWavetable") << decoded.error;
        return;
    }
    if(!table.wavPath.empty()) oldWavPaths.push_back(table.wavPath);
    table = std::move(decoded);

    preview = buildPreview(table.data, table.frames);
    framesOut = table.frames;
    status = table.name + "  |  " + ofToString(table.frames) + (table.frames == 1 ? " frame" : " frames");
    for(auto& [server, synth] : synths) loadBuffersOnServer(server);
}

// ════════════════════════════════════════════════════════════════════════════
// Buffers
// ════════════════════════════════════════════════════════════════════════════

bool scWavetable::bufferIsLive(ofxSCServer* server, ofxSCBuffer* buffer){
    if(!server || !buffer || buffer->index < 0 || buffer->index >= (int)server->buffers.size()) return false;
    return server->buffers[buffer->index] == buffer;
}

void scWavetable::releaseBuffer(ofxSCServer* server, ofxSCBuffer*& buffer){
    if(!buffer) return;
    if(bufferIsLive(server, buffer)) buffer->free();
    delete buffer;
    buffer = nullptr;
}

bool scWavetable::buffersReady(ofxSCServer* server) const {
    auto it = buffers.find(server);
    // B's sine always; the file table only when it is what plays
    return it != buffers.end() && bufferIsLive(server, it->second.sine)
        && (isLive() || bufferIsLive(server, it->second.table));
}

bool scWavetable::synthReady(ofxSCServer* server) const {
    if(!buffersReady(server)) return false;
    if(!isLive()) return true;
    auto it = live.find(server);
    return it != live.end() && it->second.playing >= 0 && bufferIsLive(server, it->second.buffer[it->second.playing]);
}

void scWavetable::loadBuffersOnServer(ofxSCServer* server){
    // Live: only B's sine (the table comes from Live In); File: table + sine
    const bool withTable = !isLive() && !table.wavPath.empty();
    if(server == nullptr || (!withTable && !isLive())) return;
    auto pending = pendingLoads.find(server);
    if(pending != pendingLoads.end()){
        releaseBuffer(server, pending->second.buffers.table);
        releaseBuffer(server, pending->second.buffers.sine);
        pendingLoads.erase(pending);
    }
    PendingLoad load;
    load.startTime = ofGetElapsedTimef();
    if(withTable){
        load.buffers.table = new ofxSCBuffer(0, 0, server);
        load.buffers.table->read(table.wavPath);
        load.buffers.frames = table.frames;
    }
    load.buffers.sine = new ofxSCBuffer(0, 0, server);
    load.buffers.sine->read(sineWavPath);
    pendingLoads[server] = load;
    // /synced comes back once the reads above have completed
    server->requestNRTSync();
}

void scWavetable::swapBuffers(ofxSCServer* server){
    auto pending = pendingLoads.find(server);
    if(pending == pendingLoads.end()) return;
    ServerBuffers old = buffers[server];
    buffers[server] = pending->second.buffers;
    pendingLoads.erase(pending);

    sendBuffers(server);
    auto found = synths.find(server);
    if(found != synths.end() && found->second && found->second->isCreated()) found->second->run(getActive() && synthReady(server));
    if(isLive() && !live.count(server)) uploadLive(server);
    // After the synth moved on: /b_free is processed after the /n_set
    releaseBuffer(server, old.table);
    releaseBuffer(server, old.sine);
}

void scWavetable::sendBuffers(ofxSCServer* server){
    auto synth = synths.find(server);
    auto b = buffers.find(server);
    if(synth == synths.end() || !synth->second || b == buffers.end() || !b->second.sine) return;
    const bool sineB = bSource == 0;
    if(isLive()){
        // The A/B pair, and which of the two to hear (the synth fades to it)
        auto l = live.find(server);
        if(l == live.end() || l->second.playing < 0) return;   // nothing uploaded yet
        const auto& L = l->second;
        synth->second->set("buf", L.buffer[0]->index);
        synth->second->set("buf2", L.buffer[1]->index);
        synth->second->set("cycles", L.frames);
        synth->second->set("bbuf", sineB ? b->second.sine->index : L.buffer[0]->index);
        synth->second->set("bbuf2", sineB ? b->second.sine->index : L.buffer[1]->index);
        synth->second->set("bcycles", sineB ? 1 : L.frames);
        synth->second->set("xf", L.playing);
        return;
    }
    ofxSCBuffer* tableBuffer = b->second.table;
    const int frames = b->second.frames;
    if(!tableBuffer) return;
    synth->second->set("buf", tableBuffer->index);
    synth->second->set("cycles", frames);
    synth->second->set("bbuf", sineB ? b->second.sine->index : tableBuffer->index);
    synth->second->set("bcycles", sineB ? 1 : frames);
}

// ════════════════════════════════════════════════════════════════════════════
// Live input
// ════════════════════════════════════════════════════════════════════════════

void scWavetable::setSource(int s){
    if(s == lastSource) return;
    lastSource = s;
    if(isLive()){
        if(!isPublished(liveIn.getEscapedName())) publishByKey(liveIn.getEscapedName());
        status = "Live: waiting for Live In";
        liveDirty = true;
        rebuildGraph();   // WavetableOscLive: silent until the first upload
    } else {
        // Back to the file def and buffers before the live ones are freed
        rebuildGraph();
        for(auto& [server, l] : live){ releaseBuffer(server, l.buffer[0]); releaseBuffer(server, l.buffer[1]); }
        live.clear();
        liveData.clear();
        liveDataFrames = 0;
        requestDecode();
    }
}

void scWavetable::processLive(){
    const auto& v = liveIn.get();
    const int F = std::clamp(liveFrames.get(), 1, 64);
    const int n = int(v.size());
    if(n < 2 * F){
        status = "Live: waiting for Live In (at least 2 values per frame)";
        return;
    }
    // Equal parts, one per frame, each resampled (linear, periodic) to CYCLE
    // Through the harmonics (smooth, band-limited: a few points become a
    // smooth curve, not corners) when that costs little per update, else
    // linearly
    const int part = n / F;
    const double harmonics = std::min((part - 1) / 2, CYCLE / 2 - 1);
    const bool smooth = part != CYCLE && double(part + CYCLE) * harmonics * F <= 8e6;
    std::vector<float> data(size_t(F) * CYCLE);
    std::vector<float> segment(part);
    for(int f = 0; f < F; f++){
        const float* in = v.data() + size_t(f) * part;
        float* out = data.data() + size_t(f) * CYCLE;
        for(int i = 0; i < part; i++) segment[i] = rescale ? in[i] * 2 - 1 : in[i];
        if(smooth){
            resampleCycle(segment.data(), part, out, removeDC);
            continue;
        }
        for(int i = 0; i < CYCLE; i++){
            if(part == CYCLE){ out[i] = segment[i]; continue; }
            const float x = float(i) * part / CYCLE;
            const int i0 = int(x), i1 = (i0 + 1) % part;
            out[i] = segment[i0] + (segment[i1] - segment[i0]) * (x - i0);
        }
        if(removeDC){
            double mean = 0;
            for(int i = 0; i < CYCLE; i++) mean += out[i];
            mean /= CYCLE;
            for(int i = 0; i < CYCLE; i++) out[i] -= float(mean);
        }
    }
    if(normalize){
        float peak = 0;
        for(float x : data) peak = std::max(peak, std::abs(x));
        if(peak > 1e-9f) for(float& x : data) x /= peak;
    }
    if(F == liveDataFrames && data == liveData) return;   // nothing new to send
    liveData = std::move(data);
    liveDataFrames = F;

    table.data = liveData;   // the view; table.wavPath (the file) is kept
    table.frames = F;
    table.name = "Live";
    preview = buildPreview(table.data, table.frames);
    framesOut = F;
    status = "Live: " + ofToString(n) + " values -> " + ofToString(F) + (F == 1 ? " frame" : " frames");
    for(auto& [server, synth] : synths) uploadLive(server);
}

void scWavetable::uploadLive(ofxSCServer* server){
    if(server == nullptr || liveDataFrames == 0) return;
    auto& l = live[server];
    if(l.allocPending) return;   // uploaded once the buffers exist

    auto synth = synths.find(server);
    const bool hasSynth = synth != synths.end() && synth->second && synth->second->isCreated();

    if(l.frames != liveDataFrames || !bufferIsLive(server, l.buffer[0]) || !bufferIsLive(server, l.buffer[1])){
        // New size: silence the synth while it may read the old buffers,
        // allocate a new pair and upload once /b_alloc is done
        if(l.playing >= 0 && hasSynth) synth->second->run(false);
        releaseBuffer(server, l.buffer[0]);
        releaseBuffer(server, l.buffer[1]);
        for(auto& b : l.buffer){
            b = new ofxSCBuffer(liveDataFrames * CYCLE, 1, server);
            b->alloc();
        }
        l.frames = liveDataFrames;
        l.playing = -1;
        l.allocPending = true;
        l.allocTime = ofGetElapsedTimef();
        server->requestNRTSync();
        return;
    }

    // The buffer the synth is fading away from is still heard until the
    // fade is over: wait for it before writing into it
    const float now = ofGetElapsedTimef();
    if(l.playing >= 0 && now - l.lastSwitch < liveXFade.get() / 1000.0f * 1.25f + 0.004f){
        l.uploadPending = true;
        return;
    }
    l.uploadPending = false;

    // Write the buffer the synth is not hearing, then fade to it. The
    // /b_setn messages are processed before the /n_set that follows them.
    const int target = l.playing == 0 ? 1 : 0;
    const int total = int(liveData.size());
    for(int offset = 0; offset < total; offset += floatsPerMessage){
        const int count = std::min(floatsPerMessage, total - offset);
        ofxOscMessage m;
        m.setAddress("/b_setn");
        m.addIntArg(l.buffer[target]->index);
        m.addIntArg(offset);
        m.addIntArg(count);
        for(int i = 0; i < count; i++) m.addFloatArg(liveData[offset + i]);
        server->sendMsg(m);
    }
    l.playing = target;
    l.lastSwitch = now;
    sendBuffers(server);
    if(hasSynth) synth->second->run(getActive() && synthReady(server));
}

void scWavetable::releaseLive(ofxSCServer* server){
    auto it = live.find(server);
    if(it == live.end()) return;
    releaseBuffer(server, it->second.buffer[0]);
    releaseBuffer(server, it->second.buffer[1]);
    live.erase(it);
}

// ════════════════════════════════════════════════════════════════════════════
// Synth
// ════════════════════════════════════════════════════════════════════════════

std::string scWavetable::getSynthDefName() const {
    return std::string(isLive() ? "WavetableOscLive" : "WavetableOsc") + (rows > 1 ? "2D" : "")
        + ofToString(std::clamp(numChannels.get(), 1, MAX_CHANNELS));
}

void scWavetable::rebuildGraph(){
    // A new def (N Chan, 1D <-> 2D) or a new init-time value (os): replace
    // each running synth in place (/s_new action 4), every value baked in
    builtChannels = numChannels;
    built2D = rows > 1;
    builtLive = isLive();
    builtOversample = oversample;
    lastTrig.clear();
    for(auto& [server, synth] : synths){
        if(!server || !synth) continue;
        if(!synth->isCreated()){
            delete synth;
            synth = new ofxSCSynth(getSynthDefName(), server);
            sendAll(server);
            continue;
        }
        const int oldNodeID = synth->nodeID;
        delete synth;   // no /n_free: action 4 replaces the node atomically
        synth = new ofxSCSynth(getSynthDefName(), server);
        sendAll(server);
        synth->createAndRun(4, oldNodeID, getActive() && synthReady(server));
    }
}

std::vector<float> scWavetable::expand(const std::vector<float>& values, float fallback) const {
    const int n = std::clamp(numChannels.get(), 1, MAX_CHANNELS);
    std::vector<float> out(n);
    for(int i = 0; i < n; i++) out[i] = valueFor(values, i, fallback);
    return out;
}

void scWavetable::sendVector(const std::string& control, const std::vector<float>& values, float fallback){
    auto v = expand(values, fallback);
    // A parameter re-notified with the same value (anything driven per frame)
    // does not resend
    auto& last = lastSent[control];
    if(last == v) return;
    last = v;
    for(auto& [server, synth] : synths) if(synth) synth->set(control, v);
}

void scWavetable::sendAll(ofxSCServer* server){
    auto it = synths.find(server);
    if(it == synths.end() || !it->second) return;
    auto* synth = it->second;
    auto setF = [&](const std::string& control, const std::vector<float>& v, float fallback){
        auto e = expand(v, fallback);
        lastSent[control] = e;
        synth->set(control, e);
    };
    auto setI = [&](const std::string& control, const std::vector<int>& v){
        setF(control, std::vector<float>(v.begin(), v.end()), 0);
    };
    setF("pitch", pitch, 60);
    setF("levels", levels, 0);
    setF("pos", position, 0);
    setF("posy", posY, 0);
    setI("w1mode", warp1);
    setF("w1amt", warp1Amt, 0);
    setI("w2mode", warp2);
    setF("w2amt", warp2Amt, 0);
    setF("fm", fm, 0);
    setF("ratio", fmRatio, 1);
    setF("bpos", bPos, 0);
    setF("fb", bFeedback, 0);
    setF("damp", fmDamp, 1);
    setF("phase", phase, 0);
    synth->set("rows", rows.get());
    synth->set("os", oversample.get());
    synth->set("xftime", liveXFade.get() / 1000.0f);
    if(outputBuses.count(server)) synth->set("out", outputBuses[server]);
    sendBuffers(server);
}

void scWavetable::sendTriggers(const std::vector<int>& values){
    // Rising edges only; `trig` is a trigger control, so a 1 fires once
    const int n = std::clamp(numChannels.get(), 1, MAX_CHANNELS);
    lastTrig.resize(n, 0);
    std::vector<float> out(n, 0.0f);
    bool any = false;
    for(int i = 0; i < n; i++){
        const int v = valueFor(values, i, 0);
        if(v > 0 && lastTrig[i] <= 0){ out[i] = 1.0f; any = true; }
        lastTrig[i] = v;
    }
    if(!any) return;
    for(auto& [server, synth] : synths) if(synth && synth->isCreated()) synth->set("trig", out);
}

void scWavetable::retrigAll(){
    const std::vector<float> out(std::clamp(numChannels.get(), 1, MAX_CHANNELS), 1.0f);
    for(auto& [server, synth] : synths) if(synth && synth->isCreated()) synth->set("trig", out);
}

void scWavetable::buildSynth(ofxSCServer* server){
    if(server == nullptr) return;
    if(synths.count(server) && synths[server]){ synths[server]->free(); delete synths[server]; }
    // Only the wrapper: setOutputBus() comes next, and every value set before
    // createSynth() is baked into /s_new (os is read when the synth starts)
    synths[server] = new ofxSCSynth(getSynthDefName(), server);
    builtChannels = numChannels;
    built2D = rows > 1;
    builtLive = isLive();
    builtOversample = oversample;
    sendAll(server);
}

void scWavetable::createSynth(ofxSCServer* server){
    auto it = synths.find(server);
    if(it == synths.end() || !it->second) return;
    // Silent until its buffers are loaded on this server
    const bool ready = synthReady(server);
    if(ready) sendBuffers(server);
    if(!it->second->isCreated()) it->second->createAndRun(0, 1, getActive() && ready);
    if(!buffersReady(server) && !pendingLoads.count(server)) loadBuffersOnServer(server);
    if(isLive() && !ready && !live.count(server)) uploadLive(server);
}

void scWavetable::free(ofxSCServer* server){
    if(synths.count(server)){
        if(synths[server]){ synths[server]->free(); delete synths[server]; }
        synths.erase(server);
    }
    // The server may be going away: reload from the WAVs when rebuilt
    if(buffers.count(server)){
        releaseBuffer(server, buffers[server].table);
        releaseBuffer(server, buffers[server].sine);
        buffers.erase(server);
    }
    if(pendingLoads.count(server)){
        releaseBuffer(server, pendingLoads[server].buffers.table);
        releaseBuffer(server, pendingLoads[server].buffers.sine);
        pendingLoads.erase(server);
    }
    releaseLive(server);
    outputBuses.erase(server);
}

void scWavetable::setOutputBus(ofxSCServer* server, int index, int bus){
    if(index != 0) return;
    outputBuses[server] = bus;
    if(synths.count(server) && synths[server]) synths[server]->set("out", bus);
}

int scWavetable::getOutputBusIndex(ofxSCServer* server, int index){
    auto it = outputBuses.find(server);
    return (index == 0 && it != outputBuses.end()) ? it->second : -1;
}

void scWavetable::moveSynthBefore(ofxSCServer* server, int nodeID){
    if(synths.count(server) && synths[server]) synths[server]->moveBefore(nodeID);
}

int scWavetable::getLastSynthID(ofxSCServer* server){
    if(synths.count(server) && synths[server]) return synths[server]->nodeID;
    return -1;
}

void scWavetable::activate(){
    for(auto& [server, synth] : synths) if(synth && synth->isCreated()) synth->run(synthReady(server));
}

void scWavetable::deactivate(){
    for(auto& [server, synth] : synths) if(synth && synth->isCreated()) synth->run(false);
}

void scWavetable::resendParametersForNRT(){
    for(auto& [server, synth] : synths) sendAll(server);
}

// ════════════════════════════════════════════════════════════════════════════
// Warps (same formulas as warp() in synthdefs/Defaults/Wavetable/wavetable.scd)
// ════════════════════════════════════════════════════════════════════════════

float scWavetable::warpPhase(float p, int mode, float amt){
    const float x = p * 2 - 1;
    const float sign = x > 0 ? 1.0f : (x < 0 ? -1.0f : 0.0f);
    switch(mode){
        case WarpSync:      {
            // Steered to a whole number of cycles over the last 5% (see wavetable.scd)
            const float k = 1 + amt * 15;
            const float s = std::clamp((p - 0.95f) / 0.05f, 0.0f, 1.0f);
            const float q = p * k + (std::round(k) - k) * s * s * (3 - 2 * s);
            return q - std::floor(q);
        }
        case WarpBendPlus:  return (sign * std::pow(std::abs(x), 1 + amt * 4) + 1) * 0.5f;
        case WarpBendMinus: return (sign * std::pow(std::abs(x), 1 / (1 + amt * 4)) + 1) * 0.5f;
        case WarpPWM:       return std::min(p / (1 - amt * 0.98f), 1.0f);
        case WarpAsym:      { const float k = 0.5f - amt * 0.49f; return p < k ? p * 0.5f / k : 0.5f + (p - k) * 0.5f / (1 - k); }
        case WarpMirror:    return p + (1 - std::abs(x) - p) * amt;
        case WarpQuantize:  { const float steps = std::round(std::pow(2.0f, 10 - amt * 9)); return std::floor(p * steps) / steps; }
        case WarpPDSine:    { const float q = p + amt * 0.25f * std::sin(2 * float(M_PI) * p); return q - std::floor(q); }
        default:            return p;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Publishing
// ════════════════════════════════════════════════════════════════════════════

template<typename T>
void scWavetable::addPublishAction(ofParameter<T>& parameter, std::vector<std::string> dropdown){
    auto* p = &parameter;
    const std::string key = parameter.getEscapedName();
    PublishAction action;
    action.key = key;
    action.dropdown = dropdown;
    action.publish = [this, p, dropdown]() -> std::shared_ptr<ofxOceanodeAbstractParameter> {
        auto handle = addParameter(*p);
        if(!dropdown.empty()) handle->setDropdownOptions(dropdown);
        return handle;
    };
    if(p != (void*)&trig && p != (void*)&liveIn){   // a saved trigger would fire on load; Live In comes from its connection
        action.save = [key, p](ofJson& json){ json[key] = p->get(); };
        action.load = [p](const ofJson& value){ try { p->set(value.get<T>()); } catch(...) {} };
    }
    action.valueText = [p](){ return toText(p->get()); };
    action.applyText = [p](const std::string& text){ T v; fromText(text, v); p->set(v); };
    publishActions.push_back(std::move(action));
}

void scWavetable::initializePublishActions(){
    publishActions.clear();
    addPublishAction(source, sourceNames);
    addPublishAction(liveIn);
    addPublishAction(liveFrames);
    addPublishAction(rescale);
    addPublishAction(liveXFade);
    addPublishAction(path);
    addPublishAction(cycleMode, cycleModeNames);
    addPublishAction(maxFrames);
    addPublishAction(normalize);
    addPublishAction(removeDC);
    addPublishAction(embedTable);
    addPublishAction(pitch);
    addPublishAction(levels);
    addPublishAction(position);
    addPublishAction(rows);
    addPublishAction(posY);
    addPublishAction(warp1, warpNames);
    addPublishAction(warp1Amt);
    addPublishAction(warp2, warpNames);
    addPublishAction(warp2Amt);
    addPublishAction(bSource, bSourceNames);
    addPublishAction(fm);
    addPublishAction(fmRatio);
    addPublishAction(bPos);
    addPublishAction(bFeedback);
    addPublishAction(fmDamp);
    addPublishAction(phase);
    addPublishAction(trig);
    addPublishAction(oversample, oversampleNames);
}

const scWavetable::PublishAction* scWavetable::findPublishAction(const std::string& key) const {
    for(const auto& action : publishActions) if(action.key == key) return &action;
    return nullptr;
}

bool scWavetable::isPublished(const std::string& key) const {
    return publishedHandles.count(key) != 0;
}

bool scWavetable::hasPublishedConnection(const std::string& key) const {
    auto it = publishedHandles.find(key);
    return it != publishedHandles.end() && it->second
        && (it->second->hasInConnection() || it->second->hasOutConnections());
}

bool scWavetable::publishByKey(const std::string& key){
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

bool scWavetable::unpublishByKey(const std::string& key){
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

void scWavetable::syncPublished(const std::vector<std::string>& keys){
    const auto current = publishedKeys;
    for(const auto& key : current)
        if(std::find(keys.begin(), keys.end(), key) == keys.end() && !hasPublishedConnection(key))
            unpublishByKey(key);
    for(const auto& key : keys) if(!isPublished(key)) publishByKey(key);
}

void scWavetable::controlMenu(const std::string& key){
    if(isPublished(key)){
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y - 1.0f), ImVec2(b.x, b.y - 1.0f),
                                            ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
    }
    if(!ImGui::BeginPopupContextItem(("##wtctx_" + key).c_str())) return;
    const PublishAction* action = findPublishAction(key);
    if(action && action->valueText && action->applyText){
        static char buffer[512];
        const bool appearing = ImGui::IsWindowAppearing();
        if(appearing) std::snprintf(buffer, sizeof(buffer), "%s", action->valueText().c_str());
        ImGui::TextDisabled("Value (a vector: 0.1, 0.5, ...)");
        ImGui::SetNextItemWidth(220.0f);
        if(appearing) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##wtvalue", buffer, sizeof(buffer),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if(enter || ImGui::IsItemDeactivatedAfterEdit()){
            action->applyText(buffer);
            if(enter) ImGui::CloseCurrentPopup();
        }
        ImGui::Separator();
    }
    if(isPublished(key)){
        if(hasPublishedConnection(key)) ImGui::TextDisabled("Connected: disconnect to unpublish");
        else if(ImGui::MenuItem("Unpublish from Node")) unpublishByKey(key);
    } else if(ImGui::MenuItem("Publish to Node")){
        publishByKey(key);
    }
    ImGui::EndPopup();
}

// ════════════════════════════════════════════════════════════════════════════
// Presets
// ════════════════════════════════════════════════════════════════════════════

void scWavetable::presetSave(ofJson& json){
    json["publishedEditorParameters"] = publishedKeys;
    json["waterfallHeight"] = waterfallHeight;
    // Window-only controls; published ones are saved by Oceanode as usual
    for(const auto& action : publishActions)
        if(action.save && !isPublished(action.key)) action.save(json);
}

void scWavetable::loadBeforeConnections(ofJson& json){
    if(!json.contains("publishedEditorParameters") || !json["publishedEditorParameters"].is_array()) return;
    try { syncPublished(json["publishedEditorParameters"].get<std::vector<std::string>>()); } catch(...) {}
}

void scWavetable::presetRecallBeforeSettingParameters(ofJson& json){
    loadBeforeConnections(json);
}

void scWavetable::presetRecallAfterSettingParameters(ofJson& json){
    if(json.contains("waterfallHeight") && json["waterfallHeight"].is_number())
        waterfallHeight = ofClamp(json["waterfallHeight"].get<float>(), 120.0f, 2000.0f);
    for(const auto& action : publishActions){
        if(isPublished(action.key) || !json.contains(action.key) || !action.load) continue;
        action.load(json[action.key]);
    }
    if(!pathToSetAfterLoad.empty()){
        // The embedded file is the decoded table: 2048-sample frames
        cycleMode = 0;
        path = pathToSetAfterLoad;
        pathToSetAfterLoad.clear();
    }
}

void scWavetable::macroSave(ofJson& json, std::string presetFolderPath){
    json["EmbedInProject"] = false;
    if(!embedTable || isLive() || path.get().empty() || table.frames == 0) return;
    const std::string folder = ofToDataPath(presetFolderPath + "/wavetables", true);
    std::error_code ec;
    fs::create_directories(folder, ec);
    const std::string fileName = sanitizeFileName(table.name) + ".wav";
    if(writeFloatWav(folder + "/" + fileName, table.data)){
        json["EmbedInProject"] = true;
        json["EmbeddedFilename"] = fileName;
    }
}

void scWavetable::macroLoad(ofJson& json, std::string presetFolderPath){
    pathToSetAfterLoad.clear();
    if(!json.value("EmbedInProject", false) || !json.contains("EmbeddedFilename")) return;
    const std::string file = ofToDataPath(presetFolderPath + "/wavetables/" + json["EmbeddedFilename"].get<std::string>(), true);
    std::error_code ec;
    if(fs::exists(file, ec)){
        embedTable = true;
        pathToSetAfterLoad = file;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Files
// ════════════════════════════════════════════════════════════════════════════

void scWavetable::openDialog(bool folder){
    std::string start = ofToDataPath("Supercollider/Wavetables", true);
    const std::string current = resolvePath(path.get());
    std::error_code ec;
    if(!current.empty() && fs::exists(current, ec)) start = fs::path(current).parent_path().string();
    auto result = ofSystemLoadDialog(folder ? "Select a folder of single cycles" : "Select a wavetable", folder, start);
    if(result.bSuccess) path = result.getPath();
}

void scWavetable::stepFile(int direction){
    const std::string current = resolvePath(path.get());
    std::error_code ec;
    if(current.empty() || !fs::exists(current, ec)) return;
    const bool folder = fs::is_directory(current, ec);
    std::vector<fs::path> siblings;
    for(const auto& entry : fs::directory_iterator(fs::path(current).parent_path(), ec)){
        if(folder ? entry.is_directory(ec)
                  : (entry.is_regular_file(ec) && lower(entry.path().extension().string()) == ".wav"))
            siblings.push_back(entry.path());
    }
    if(siblings.empty()) return;
    std::sort(siblings.begin(), siblings.end());
    auto it = std::find(siblings.begin(), siblings.end(), fs::path(current));
    int index = it == siblings.end() ? 0 : int(it - siblings.begin()) + direction;
    index = (index % int(siblings.size()) + int(siblings.size())) % int(siblings.size());
    path = siblings[index].string();
}

// ════════════════════════════════════════════════════════════════════════════
// Window
// ════════════════════════════════════════════════════════════════════════════

float scWavetable::tableAt(float frameIndex, float ph) const {
    if(table.frames == 0) return 0;
    const float fi = ofClamp(frameIndex, 0, table.frames - 1);
    const int f0 = int(fi), f1 = std::min(f0 + 1, table.frames - 1);
    const float ff = fi - f0;
    const float x = (ph - std::floor(ph)) * CYCLE;
    const int i0 = int(x) & (CYCLE - 1), i1 = (i0 + 1) & (CYCLE - 1);
    const float fx = x - std::floor(x);
    auto at = [&](int f){
        const float* d = table.data.data() + size_t(f) * CYCLE;
        return d[i0] + (d[i1] - d[i0]) * fx;
    };
    return at(f0) + (at(f1) - at(f0)) * ff;
}

// The frame index (fractional, for the depth in the waterfall) a voice reads
float scWavetable::voiceFrame(int voice) const {
    const int F = table.frames;
    if(F <= 1) return 0;
    const float x = ofClamp(valueFor(position.get(), voice, 0.0f), 0, 1);
    if(rows <= 1) return x * (F - 1);
    const int cols = std::max(1, F / rows);
    const float y = ofClamp(valueFor(posY.get(), voice, 0.0f), 0, 1) * (rows - 1);
    return ofClamp(y * cols + x * (cols - 1), 0, F - 1);
}

// What the synth reads for a voice at a phase: 1D position, or the
// bilinear blend of the two rows around Pos Y on a grid
float scWavetable::voiceValue(int voice, float ph) const {
    const int F = table.frames;
    if(F == 0) return 0;
    const float x = ofClamp(valueFor(position.get(), voice, 0.0f), 0, 1);
    if(rows <= 1) return tableAt(x * (F - 1), ph);
    const int cols = std::max(1, F / rows);
    const float y = ofClamp(valueFor(posY.get(), voice, 0.0f), 0, 1) * (rows - 1);
    const int r0 = int(y), r1 = std::min(r0 + 1, rows.get() - 1);
    const float fy = y - r0;
    const float a = tableAt(r0 * cols + x * (cols - 1), ph);
    const float b = tableAt(r1 * cols + x * (cols - 1), ph);
    return a + (b - a) * fy;
}

void scWavetable::drawWindow(){
    const std::string title = getWindowTitle();
    bool open = showWindow.get();
    ImGui::SetNextWindowSize(ImVec2(920.0f, 680.0f), ImGuiCond_FirstUseEver);
    if(ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)){
        ImGui::PushID(this);
        drawFileBar();

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float width = std::max(1.0f, avail.x);
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        constexpr float handleHeight = 8.0f, controlsHeight = 210.0f;
        waterfallHeight = ofClamp(waterfallHeight, 120.0f, std::max(120.0f, avail.y - handleHeight - controlsHeight));

        // One row: waterfall | cycle read | grid (2D only)
        const float h = waterfallHeight;
        const float gridSide = rows > 1 ? std::min(h, width * 0.18f) : 0.0f;
        const float rest = width - (gridSide > 0 ? gridSide + spacing : 0.0f) - spacing;
        const float waterfallWidth = std::max(1.0f, rest * 0.62f);
        drawWaterfall(ImVec2(waterfallWidth, h));
        ImGui::SameLine();
        drawCycleStrip(ImVec2(std::max(1.0f, rest - waterfallWidth), h));
        if(gridSide > 0){
            ImGui::SameLine();
            drawGrid(gridSide);
        }

        // Handle under the row: drag to resize its height
        const ImVec2 handle = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##wtresize", ImVec2(width, handleHeight));
        const bool handleHot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if(handleHot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if(ImGui::IsItemActive()) waterfallHeight += ImGui::GetIO().MouseDelta.y;
        const float midY = handle.y + handleHeight * 0.5f, midX = handle.x + width * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(midX - 24, midY), ImVec2(midX + 24, midY),
            ImGui::GetColorU32(handleHot ? ImGuiCol_Text : ImGuiCol_TextDisabled), 2.0f);
        ImGui::Spacing();
        drawControls();
        ImGui::PopID();
    }
    ImGui::End();
    if(open != showWindow.get()) showWindow = open;
}

void scWavetable::drawFileBar(){
    ImGui::SetNextItemWidth(130.0f);
    comboInt(source, sourceNames);
    ImGui::SameLine();

    if(isLive()){
        ImGui::TextUnformatted(status.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if(ImGui::IsItemHovered()){
            ImGui::SetTooltip("The table comes from the Live In vector (published on the node):\n"
                              "cut into Live Frames equal parts, each resampled to 2048 samples.\n"
                              "Every change is written to the buffer the synth is not reading,\n"
                              "then the synth switches to it (no tearing). Rescale maps 0..1 to -1..1.");
        }
        ImGui::SetNextItemWidth(170.0f);
        sliderInt(liveFrames);
        ImGui::SameLine();
        {
            float ms = liveXFade.get();
            ImGui::SetNextItemWidth(170.0f);
            if(ImGui::SliderFloat("##Live_XFade", &ms, 1.0f, 500.0f, "Live XFade  %.0f ms", ImGuiSliderFlags_Logarithmic)) liveXFade = ms;
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Fade from the old to the new table on each update.\nUpdates come at most once per fade.");
            controlMenu(liveXFade.getEscapedName());
        }
        ImGui::SameLine();
        for(auto* p : {&rescale, &normalize, &removeDC}){
            bool v = p->get();
            if(ImGui::Checkbox(p->getName().c_str(), &v)) p->set(v);
            controlMenu(p->getEscapedName());
            if(p != &removeDC) ImGui::SameLine();
        }
        return;
    }

    if(ImGui::Button("File...")) pendingDialog = 1;
    ImGui::SameLine();
    if(ImGui::Button("Folder...")) pendingDialog = 2;
    ImGui::SameLine();
    if(ImGui::ArrowButton("##prev", ImGuiDir_Left)) stepFile(-1);
    ImGui::SameLine(0, 2);
    if(ImGui::ArrowButton("##next", ImGuiDir_Right)) stepFile(1);
    ImGui::SameLine();
    if(!path.get().empty() && ImGui::SmallButton("x")) path = "";
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Back to the built-in table");
    if(!path.get().empty()) ImGui::SameLine();
    ImGui::TextUnformatted(status.c_str());
    if(ImGui::IsItemHovered() && !path.get().empty()) ImGui::SetTooltip("%s", path.get().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if(ImGui::IsItemHovered()){
        ImGui::SetTooltip("Serum / Vital / KRC wavetables (frame size from the file, else 2048),\n"
                          "single cycles of any length (AKWF), or a folder of single cycles\n"
                          "(each file one frame). Arrows step to the next file or folder.\n"
                          "Drag on the waterfall to scan. Right-click any control to publish it.");
    }
    controlMenu(path.getEscapedName());

    ImGui::SetNextItemWidth(170.0f);
    comboInt(cycleMode, cycleModeNames);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    sliderInt(maxFrames);
    ImGui::SameLine();
    for(auto* p : {&normalize, &removeDC, &embedTable}){
        bool v = p->get();
        if(ImGui::Checkbox(p->getName().c_str(), &v)) p->set(v);
        controlMenu(p->getEscapedName());
        if(p != &embedTable) ImGui::SameLine();
    }
}

void scWavetable::drawWaterfall(ImVec2 size){
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##waterfall", size);
    if(ImGui::IsItemActive()){
        const float x = ofClamp((ImGui::GetIO().MousePos.x - o.x) / size.x, 0, 1);
        if(position.get().size() != 1 || position.get()[0] != x) position = std::vector<float>{x};
    }

    ImVec4 bgv = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    bgv = ImVec4(bgv.x * 0.6f, bgv.y * 0.6f, bgv.z * 0.6f, 1.0f);
    const ImU32 bg = ImGui::GetColorU32(bgv);
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), bg, 4.0f);

    const int F = int(preview.size());
    if(F == 0){
        dl->AddText(ImVec2(o.x + 10, o.y + 10), ImGui::GetColorU32(ImGuiCol_TextDisabled), status.c_str());
        return;
    }
    const float pad = 12.0f;
    const float dx = size.x * 0.30f, dy = size.y * 0.40f;
    const float fw = size.x - dx - 2 * pad;
    const float amp = (size.y - dy - 2 * pad) * 0.45f;
    const float frontBase = o.y + size.y - pad - amp;
    auto geometry = [&](float frameIndex, float& x0, float& base){
        const float d = F > 1 ? frameIndex / (F - 1) : 0.0f;   // 0 front .. 1 back
        x0 = o.x + pad + d * dx;
        base = frontBase - d * dy;
        return d;
    };

    // Frames, back to front; each hides what is behind it
    std::vector<int> frames;
    const int stride = std::max(1, int(std::ceil(F / float(maxDrawnFrames))));
    for(int f = 0; f < F; f += stride) frames.push_back(f);
    if(frames.back() != F - 1) frames.push_back(F - 1);
    std::vector<ImVec2> pts(PREVIEW_POINTS);
    dl->PushClipRect(o, ImVec2(o.x + size.x, o.y + size.y), true);
    for(auto it = frames.rbegin(); it != frames.rend(); ++it){
        const int f = *it;
        float x0, base;
        const float d = geometry(float(f), x0, base);
        for(int i = 0; i < PREVIEW_POINTS; i++)
            pts[i] = ImVec2(x0 + fw * i / (PREVIEW_POINTS - 1), base - preview[f][i] * amp);
        for(int i = 0; i + 1 < PREVIEW_POINTS; i++)
            dl->AddQuadFilled(pts[i], pts[i + 1], ImVec2(pts[i + 1].x, base + amp), ImVec2(pts[i].x, base + amp), bg);
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(0.55f - 0.12f * d, 0.55f, 1.0f, r, g, b);
        dl->AddPolyline(pts.data(), PREVIEW_POINTS, ImGui::GetColorU32(ImVec4(r, g, b, 0.95f - 0.6f * d)), 0, 1.0f);
    }

    // Every voice: the cycle it reads, at its depth
    const int voices = std::min(std::clamp(numChannels.get(), 1, MAX_CHANNELS), maxDrawnVoices);
    for(int v = voices - 1; v >= 0; v--){
        float x0, base;
        geometry(voiceFrame(v), x0, base);
        for(int i = 0; i < PREVIEW_POINTS; i++){
            const float ph = float(i) / (PREVIEW_POINTS - 1);
            pts[i] = ImVec2(x0 + fw * ph, base - voiceValue(v, ph) * amp);
        }
        dl->AddPolyline(pts.data(), PREVIEW_POINTS, voiceColor(v), 0, 2.5f);
    }
    dl->PopClipRect();

    const std::string info = "frame " + ofToString(voiceFrame(0) + 1, 1) + " / " + ofToString(F);
    dl->AddText(ImVec2(o.x + 8, o.y + 6), ImGui::GetColorU32(ImGuiCol_TextDisabled), info.c_str());
}

void scWavetable::drawCycleStrip(ImVec2 size){
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##cycle", size);
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
    const float mid = o.y + size.y * 0.5f, amp = size.y * 0.42f;
    dl->AddLine(ImVec2(o.x, mid), ImVec2(o.x + size.x, mid), ImGui::GetColorU32(ImGuiCol_Border));
    if(table.frames == 0) return;

    const int points = std::clamp(int(size.x / 2), 32, 512);
    std::vector<ImVec2> pts(points);
    const int voices = std::min(std::clamp(numChannels.get(), 1, MAX_CHANNELS), maxDrawnVoices);
    for(int v = voices - 1; v >= 0; v--){
        const int m1 = valueFor(warp1.get(), v, 0), m2 = valueFor(warp2.get(), v, 0);
        const float a1 = valueFor(warp1Amt.get(), v, 0.0f), a2 = valueFor(warp2Amt.get(), v, 0.0f);
        for(int i = 0; i < points; i++){
            const float ph = float(i) / (points - 1);
            const float w = warpPhase(warpPhase(ph, m1, a1), m2, a2);
            pts[i] = ImVec2(o.x + size.x * ph, mid - ofClamp(voiceValue(v, w), -1.2f, 1.2f) * amp);
        }
        dl->AddPolyline(pts.data(), points, voiceColor(v, v == 0 ? 1.0f : 0.6f), 0, v == 0 ? 2.0f : 1.2f);
    }
    dl->AddText(ImVec2(o.x + 6, o.y + 4), ImGui::GetColorU32(ImGuiCol_TextDisabled), "cycle read (warps applied)");
}

void scWavetable::drawGrid(float side){
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##grid", ImVec2(side, side));
    if(ImGui::IsItemActive()){
        const ImVec2 m = ImGui::GetIO().MousePos;
        position = std::vector<float>{ofClamp((m.x - o.x) / side, 0, 1)};
        posY = std::vector<float>{ofClamp((m.y - o.y) / side, 0, 1)};
    }
    dl->AddRectFilled(o, ImVec2(o.x + side, o.y + side), ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
    const int cols = std::max(1, table.frames / std::max(1, rows.get()));
    const ImU32 line = ImGui::GetColorU32(ImGuiCol_Border);
    for(int c = 1; c < cols && cols <= 64; c++){
        const float x = o.x + side * c / cols;
        dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + side), line);
    }
    for(int r = 1; r < rows; r++){
        const float y = o.y + side * r / rows;
        dl->AddLine(ImVec2(o.x, y), ImVec2(o.x + side, y), line);
    }
    const int voices = std::min(std::clamp(numChannels.get(), 1, MAX_CHANNELS), maxDrawnVoices);
    for(int v = voices - 1; v >= 0; v--){
        const float x = ofClamp(valueFor(position.get(), v, 0.0f), 0, 1);
        const float y = ofClamp(valueFor(posY.get(), v, 0.0f), 0, 1);
        dl->AddCircleFilled(ImVec2(o.x + x * side, o.y + y * side), 4.0f, voiceColor(v));
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("%d rows x %d columns: drag to move Position / Pos Y", rows.get(), cols);
}

void scWavetable::beginGroup(const char* title, float){
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", title);
    ImGui::Separator();
}

void scWavetable::endGroup(){}

bool scWavetable::sliderVector(ofParameter<std::vector<float>>& p, const char* format){
    auto values = p.get();
    float v = values.empty() ? 0.0f : values[0];
    const float lo = p.getMin().empty() ? 0.0f : p.getMin()[0];
    const float hi = p.getMax().empty() ? 1.0f : p.getMax()[0];
    std::string label = p.getName() + (values.size() > 1 ? " [" + ofToString(values.size()) + "]" : "") + "  " + format;
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool changed = ImGui::SliderFloat(("##" + p.getEscapedName()).c_str(), &v, lo, hi, label.c_str());
    if(changed) p = std::vector<float>{v};
    if(ImGui::IsItemHovered() && values.size() > 1) ImGui::SetTooltip("%s", toText(values).c_str());
    controlMenu(p.getEscapedName());
    return changed;
}

bool scWavetable::comboVector(ofParameter<std::vector<int>>& p, const std::vector<std::string>& options){
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

bool scWavetable::comboInt(ofParameter<int>& p, const std::vector<std::string>& options){
    int v = std::clamp(p.get(), 0, int(options.size()) - 1);
    const std::string preview = p.getName() + ": " + options[v];
    bool changed = false;
    if(ImGui::BeginCombo(("##" + p.getEscapedName()).c_str(), preview.c_str())){
        for(int i = 0; i < (int)options.size(); i++){
            if(ImGui::Selectable(options[i].c_str(), i == v)){ p = i; changed = true; }
        }
        ImGui::EndCombo();
    }
    controlMenu(p.getEscapedName());
    return changed;
}

bool scWavetable::sliderInt(ofParameter<int>& p){
    int v = p.get();
    const std::string label = p.getName() + "  %d";
    const bool changed = ImGui::SliderInt(("##" + p.getEscapedName()).c_str(), &v, p.getMin(), p.getMax(), label.c_str());
    if(changed) p = v;
    controlMenu(p.getEscapedName());
    return changed;
}

void scWavetable::drawControls(){
    if(!ImGui::BeginTable("##wtcontrols", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) return;

    beginGroup("OSCILLATOR", 0);
    sliderVector(pitch, "%.2f");
    sliderVector(levels);
    sliderVector(position);
    ImGui::SetNextItemWidth(-FLT_MIN);
    sliderInt(rows);
    if(rows > 1) sliderVector(posY);
    else ImGui::TextDisabled("Rows > 1: the table is a grid");

    beginGroup("WARP", 0);
    comboVector(warp1, warpNames);
    sliderVector(warp1Amt);
    comboVector(warp2, warpNames);
    sliderVector(warp2Amt);

    beginGroup("FM  (B -> A)", 0);
    ImGui::SetNextItemWidth(-FLT_MIN);
    comboInt(bSource, bSourceNames);
    sliderVector(fm, "%.2f");
    sliderVector(fmRatio, "%.3f");
    if(bSource == 1) sliderVector(bPos);
    sliderVector(bFeedback, "%.2f");
    sliderVector(fmDamp);

    beginGroup("VOICE", 0);
    sliderVector(phase);
    if(ImGui::Button("Retrig", ImVec2(-FLT_MIN, 0))) retrigAll();
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Restart every voice at Phase (publish Trig to patch it)");
    controlMenu(trig.getEscapedName());
    ImGui::SetNextItemWidth(-FLT_MIN);
    comboInt(oversample, oversampleNames);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Mipmapping already band-limits plain playback;\n2x helps heavy warps and FM. Restarts the synth.");

    ImGui::EndTable();
}
