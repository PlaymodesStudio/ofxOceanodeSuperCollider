//
//  scDataBox.cpp
//  ofxOceanodeSuperCollider
//

#include "scDataBox.h"
#include "serverManager.h"
#include "ofxSCServer.h"
#include "ofxOceanodeShared.h"
#include "imgui.h"
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

const char* formatNames[] = {"int8", "uint8", "int16 LE", "int16 BE", "int32 LE", "float32 LE"};

void putU32(std::ofstream& out, uint32_t v){ out.write(reinterpret_cast<const char*>(&v), 4); }
void putU16(std::ofstream& out, uint16_t v){ out.write(reinterpret_cast<const char*>(&v), 2); }

// Mono 32-bit float WAV, which scsynth's /b_allocRead reads with its rate.
bool writeFloatWav(const std::string& path, const std::vector<float>& data, int sampleRate){
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

ImU32 headColor(int head, float alpha = 1.0f){
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(std::fmod(head * 0.137f + 0.55f, 1.0f), 0.65f, 1.0f, r, g, b);
    return ImGui::GetColorU32(ImVec4(r, g, b, alpha));
}

// Buffers loaded on this server
bool hasData(const std::map<ofxSCServer*, std::vector<ofxSCBuffer*>>& buffers, ofxSCServer* server){
    auto it = buffers.find(server);
    return it != buffers.end() && !it->second.empty();
}

std::string humanBytes(int64_t bytes){
    if(bytes >= (1 << 30)) return ofToString(bytes / double(1 << 30), 2) + " GB";
    if(bytes >= (1 << 20)) return ofToString(bytes / double(1 << 20), 2) + " MB";
    if(bytes >= (1 << 10)) return ofToString(bytes / double(1 << 10), 1) + " KB";
    return ofToString(bytes) + " B";
}
}

scDataBox::scDataBox(std::vector<serverManager*> outputServers)
    : scNode("DataBox"), servers(outputServers) {
    description = "Plays any file as audio: its bytes are decoded into samples and "
                  "scanned by one playhead per channel. Open the window (Show) to pick "
                  "the file, the region and how the bytes are read.";
}

scDataBox::~scDataBox(){
    for(auto& [server, list] : voices){
        for(auto* voice : list){ voice->free(); delete voice; }
    }
    voices.clear();
    for(auto& [server, list] : buffers) freeBuffers(list);
    for(auto& [server, pending] : pendingLoads) freeBuffers(pending.buffers);
    for(auto& [server, bus] : positionBuses){ if(bus){ bus->free(); delete bus; } }
    if(decodeTask.valid()) deleteWavFiles(decodeTask.get().wavPaths);
    if(mapTask.valid()) mapTask.wait();
    deleteWavFiles(wavPaths);
    deleteWavFiles(oldWavPaths);
}

// ════════════════════════════════════════════════════════════════════════════
// Setup
// ════════════════════════════════════════════════════════════════════════════

void scDataBox::setup(){
    addOutput("Out");

    addParameter(showWindow.set("Show", false));
    addParameter(numHeads.set("N Chan", 1, 1, MAX_HEADS));
    addParameter(position.set("Position", {0.0f}, {0.0f}, {1.0f}));
    addParameter(window.set("Window", {0.05f}, {0.0001f}, {1.0f}));
    addParameter(rate.set("Rate", {1.0f}, {-4.0f}, {4.0f}));
    addParameter(level.set("Level", {0.5f}, {0.0f}, {1.0f}));

    // Decode settings and the region: edited in the window, kept by presets.
    addInspectorParameter(path.set("Path", ""));
    addInspectorParameter(format.set("Format", Int8, 0, NumFormats - 1));
    addInspectorParameter(dataChannels.set("Data Chan", 1, 1, 16));
    addInspectorParameter(sampleRate.set("Sample Rate", 44100, 1000, 192000));
    addInspectorParameter(offset.set("Offset", 0, 0, INT_MAX));
    addInspectorParameter(length.set("Length", 0, 0, INT_MAX));
    addInspectorParameter(normalize.set("Normalize", false));
    addInspectorParameter(dcRemove.set("DC Remove", true));
    addInspectorParameter(lag.set("Lag", 0.05f, 0.0f, 5.0f));
    addInspectorParameter(fade.set("Fade", 0.02f, 0.0f, 0.5f));

    listeners.push(numHeads.newListener([this](int& n){ setNumHeads(n); }));
    listeners.push(position.newListener([this](std::vector<float>& v){ sendVectorToVoices("pos", v, 0.0f); }));
    listeners.push(window.newListener([this](std::vector<float>& v){ sendVectorToVoices("win", v, 0.05f); }));
    listeners.push(rate.newListener([this](std::vector<float>& v){ sendVectorToVoices("rate", v, 1.0f); }));
    listeners.push(level.newListener([this](std::vector<float>& v){ sendVectorToVoices("level", v, 0.5f); }));
    listeners.push(lag.newListener([this](float& v){ sendToVoices("lag", v); }));
    listeners.push(fade.newListener([this](float& v){ sendToVoices("fade", v); }));
    listeners.push(dcRemove.newListener([this](bool& v){ sendToVoices("dc", v ? 1.0f : 0.0f); }));

    listeners.push(path.newListener([this](std::string&){
        // A preset sets the region right after the path: keep it.
        if(!ofxOceanodeShared::isPresetLoading()) resetRegionToFile();
        fileMap = FileMap();
        if(!path.get().empty()) mapTask = std::async(std::launch::async, &scDataBox::scanFile, path.get());
        requestDecode();
    }));
    listeners.push(format.newListener([this](int&){ requestDecode(); }));
    listeners.push(dataChannels.newListener([this](int&){ requestDecode(); }));
    listeners.push(sampleRate.newListener([this](int&){ requestDecode(); }));
    listeners.push(offset.newListener([this](int&){ requestDecode(); }));
    listeners.push(length.newListener([this](int&){ requestDecode(); }));
    listeners.push(normalize.newListener([this](bool&){ requestDecode(); }));
}

// ════════════════════════════════════════════════════════════════════════════
// Frame
// ════════════════════════════════════════════════════════════════════════════

void scDataBox::update(ofEventArgs&){
    const float now = ofGetElapsedTimef();

    if(decodeTask.valid() && decodeTask.wait_for(std::chrono::seconds(0)) == std::future_status::ready){
        applyDecodeResult(decodeTask.get());
    }
    if(decodeRequested && !decodeTask.valid() && now - decodeRequestTime >= decodeDebounce
       && drag == Drag::None){
        startDecode();
    }
    if(mapTask.valid() && mapTask.wait_for(std::chrono::seconds(0)) == std::future_status::ready){
        FileMap map = mapTask.get();
        if(map.path == path.get()) fileMap = std::move(map);
    }

    std::vector<ofxSCServer*> synced;
    for(auto& [server, pending] : pendingLoads){
        if(!server->isNRTSyncPending() || now - pending.startTime > loadTimeout) synced.push_back(server);
    }
    for(auto* server : synced) swapBuffers(server);
    if(pendingLoads.empty() && !oldWavPaths.empty()){
        deleteWavFiles(oldWavPaths);
        oldWavPaths.clear();
    }

    // Playhead positions for the window
    if(showWindow){
        for(auto& [server, bus] : positionBuses){
            if(bus){ bus->requestValues(); break; }
        }
    }
}

void scDataBox::draw(ofEventArgs&){
    if(showWindow) drawWindow();
}

// ════════════════════════════════════════════════════════════════════════════
// Decoding
// ════════════════════════════════════════════════════════════════════════════

int scDataBox::bytesPerSample(int format){
    switch(format){
        case Int16LE: case Int16BE: return 2;
        case Int32LE: case Float32LE: return 4;
        default: return 1;
    }
}

void scDataBox::requestDecode(){
    decodeRequested = true;
    decodeRequestTime = ofGetElapsedTimef();
}

std::string scDataBox::getTmpDir() const {
    return ofToDataPath("Supercollider/tmp/databox", true);
}

void scDataBox::startDecode(){
    decodeRequested = false;
    if(path.get().empty()) return;
    DecodeSettings settings;
    settings.path = path.get();
    settings.format = format;
    settings.channels = dataChannels;
    settings.offset = offset;
    settings.length = length;
    settings.sampleRate = sampleRate;
    settings.normalize = normalize;
    const std::string prefix = "databox_" + ofToString(reinterpret_cast<uintptr_t>(this));
    status = "Decoding...";
    decodeTask = std::async(std::launch::async, &scDataBox::decode, settings, ++decodeGeneration, getTmpDir(), prefix);
}

scDataBox::DecodeResult scDataBox::decode(DecodeSettings s, uint64_t generation, std::string tmpDir, std::string prefix){
    DecodeResult result;
    result.generation = generation;

    std::ifstream file(s.path, std::ios::binary | std::ios::ate);
    if(!file){ result.error = "Cannot open " + s.path; return result; }
    const int64_t size = file.tellg();
    const int bps = bytesPerSample(s.format);
    const int channels = std::max(1, s.channels);
    const int64_t start = std::clamp<int64_t>(s.offset, 0, std::max<int64_t>(0, size - 1));
    int64_t bytes = s.length > 0 ? std::min<int64_t>(s.length, size - start) : size - start;

    int64_t frames = bytes / (bps * channels);
    if(frames * channels > MAX_SAMPLES){
        frames = MAX_SAMPLES / channels;
        result.capped = true;
    }
    if(frames < 2){ result.error = "Region too small for this format"; return result; }
    bytes = frames * channels * bps;

    std::vector<unsigned char> raw(bytes);
    file.seekg(start);
    file.read(reinterpret_cast<char*>(raw.data()), bytes);
    if(file.gcount() != bytes){ result.error = "Could not read the region"; return result; }

    std::vector<std::vector<float>> data(channels, std::vector<float>(frames));
    const unsigned char* p = raw.data();
    for(int64_t f = 0; f < frames; f++){
        for(int c = 0; c < channels; c++, p += bps){
            float v = 0;
            switch(s.format){
                case Int8:    v = static_cast<int8_t>(p[0]) / 128.0f; break;
                case UInt8:   v = p[0] / 127.5f - 1.0f; break;
                case Int16LE: v = static_cast<int16_t>(p[0] | (p[1] << 8)) / 32768.0f; break;
                case Int16BE: v = static_cast<int16_t>((p[0] << 8) | p[1]) / 32768.0f; break;
                case Int32LE: v = static_cast<int32_t>(uint32_t(p[0]) | (uint32_t(p[1]) << 8)
                                                       | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24)) / 2147483648.0f; break;
                case Float32LE: std::memcpy(&v, p, 4); if(!std::isfinite(v)) v = 0; break;
            }
            data[c][f] = v;
        }
    }

    for(auto& channel : data){
        if(s.normalize){
            float peak = 0;
            for(float v : channel) peak = std::max(peak, std::abs(v));
            if(peak > 0) for(float& v : channel) v /= peak;
        }
        // float32 data can hold any magnitude: never let it past full scale
        for(float& v : channel) v = std::clamp(v, -1.0f, 1.0f);
    }

    std::error_code ec;
    fs::create_directories(tmpDir, ec);
    result.preview.resize(channels);
    for(int c = 0; c < channels; c++){
        const std::string wav = (fs::path(tmpDir) / (prefix + "_" + std::to_string(generation) + "_c" + std::to_string(c) + ".wav")).string();
        if(!writeFloatWav(wav, data[c], s.sampleRate)){
            result.error = "Cannot write " + wav;
            for(auto& written : result.wavPaths) fs::remove(written, ec);
            result.wavPaths.clear();
            return result;
        }
        result.wavPaths.push_back(wav);

        auto& columns = result.preview[c];
        columns.resize(PREVIEW_COLUMNS);
        for(int col = 0; col < PREVIEW_COLUMNS; col++){
            const int64_t a = frames * col / PREVIEW_COLUMNS;
            const int64_t b = std::max(a + 1, frames * (col + 1) / PREVIEW_COLUMNS);
            float lo = 1, hi = -1;
            for(int64_t i = a; i < b; i++){ lo = std::min(lo, data[c][i]); hi = std::max(hi, data[c][i]); }
            columns[col] = {lo, hi};
        }
    }
    result.framesPerChannel = frames;
    return result;
}

void scDataBox::applyDecodeResult(DecodeResult result){
    if(result.generation != decodeGeneration){
        deleteWavFiles(result.wavPaths);     // superseded by a newer decode
        return;
    }
    if(!result.error.empty()){
        status = result.error;
        ofLogWarning("scDataBox") << result.error;
        return;
    }
    oldWavPaths.insert(oldWavPaths.end(), wavPaths.begin(), wavPaths.end());
    wavPaths = std::move(result.wavPaths);
    preview = std::move(result.preview);
    framesPerChannel = result.framesPerChannel;
    capped = result.capped;
    status = ofToString(framesPerChannel) + " samples x " + ofToString(wavPaths.size()) + " ch, "
           + ofToString(framesPerChannel / double(sampleRate.get()), 2) + " s"
           + (capped ? "  (region capped)" : "");
    for(auto& [server, list] : voices) loadBuffersOnServer(server);
}

void scDataBox::loadBuffersOnServer(ofxSCServer* server){
    if(server == nullptr || wavPaths.empty()) return;
    auto pending = pendingLoads.find(server);
    if(pending != pendingLoads.end()){
        freeBuffers(pending->second.buffers);
        pendingLoads.erase(pending);
    }
    PendingLoad load;
    load.startTime = ofGetElapsedTimef();
    for(const auto& wav : wavPaths){
        auto* buffer = new ofxSCBuffer(0, 0, server);
        buffer->read(wav);
        load.buffers.push_back(buffer);
    }
    pendingLoads[server] = std::move(load);
    // /synced comes back once the reads above have completed
    server->requestNRTSync();
}

void scDataBox::swapBuffers(ofxSCServer* server){
    auto pending = pendingLoads.find(server);
    if(pending == pendingLoads.end()) return;
    std::vector<ofxSCBuffer*> old = std::move(buffers[server]);
    buffers[server] = std::move(pending->second.buffers);
    pendingLoads.erase(pending);

    auto found = voices.find(server);
    if(found != voices.end()){
        auto& list = found->second;
        for(int i = 0; i < (int)list.size(); i++){
            list[i]->set("buf", bufferForHead(server, i));
            if(list[i]->isCreated()) list[i]->run(getActive());
        }
    }
    // After the voices moved on: /b_free is processed after the /n_set
    freeBuffers(old);
}

void scDataBox::freeBuffers(std::vector<ofxSCBuffer*>& list){
    for(auto* buffer : list){ buffer->free(); delete buffer; }
    list.clear();
}

void scDataBox::deleteWavFiles(const std::vector<std::string>& paths){
    std::error_code ec;
    for(const auto& p : paths) fs::remove(p, ec);
}

scDataBox::FileMap scDataBox::scanFile(std::string filePath){
    FileMap map;
    map.path = filePath;
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if(!file) return map;
    map.size = file.tellg();
    if(map.size <= 0) return map;

    // A sample of each column is enough to tell text, images and compressed
    // data apart, and keeps huge files fast.
    const int columns = MAP_COLUMNS;
    const int64_t perColumn = std::max<int64_t>(1, map.size / columns);
    const int64_t sampleBytes = std::min<int64_t>(perColumn, 16384);
    std::vector<unsigned char> chunk(sampleBytes);
    map.mean.assign(columns, 0);
    map.roughness.assign(columns, 0);
    for(int col = 0; col < columns; col++){
        const int64_t at = map.size * col / columns;
        if(at >= map.size) break;
        file.seekg(at);
        file.read(reinterpret_cast<char*>(chunk.data()), sampleBytes);
        const int64_t got = file.gcount();
        if(got <= 0) break;
        double sum = 0, delta = 0;
        for(int64_t i = 0; i < got; i++){
            sum += chunk[i];
            if(i > 0) delta += std::abs(int(chunk[i]) - int(chunk[i - 1]));
        }
        map.mean[col] = static_cast<float>(sum / got / 255.0);
        // Random bytes average a difference of ~85
        map.roughness[col] = got > 1 ? std::min(1.0f, static_cast<float>(delta / (got - 1) / 85.0)) : 0;
    }
    return map;
}

// ════════════════════════════════════════════════════════════════════════════
// Playheads
// ════════════════════════════════════════════════════════════════════════════

int scDataBox::bufferForHead(ofxSCServer* server, int head) const {
    auto it = buffers.find(server);
    if(it == buffers.end() || it->second.empty()) return 0;
    return it->second[head % it->second.size()]->index;
}

ofxSCSynth* scDataBox::makeVoice(ofxSCServer* server, int head){
    // While not created, these become the /s_new arguments
    auto* voice = new ofxSCSynth("DataBoxVoice", server);
    voice->set("chan", head);
    voice->set("buf", bufferForHead(server, head));
    if(positionBuses.count(server) && positionBuses[server]) voice->set("posBus", positionBuses[server]->index);
    if(outputBuses.count(server)) voice->set("out", outputBuses[server]);
    voice->set("pos", valueFor(position.get(), head, 0.0f));
    voice->set("win", valueFor(window.get(), head, 0.05f));
    voice->set("rate", valueFor(rate.get(), head, 1.0f));
    voice->set("level", valueFor(level.get(), head, 0.5f));
    voice->set("lag", lag.get());
    voice->set("fade", fade.get());
    voice->set("dc", dcRemove ? 1.0f : 0.0f);
    return voice;
}

void scDataBox::setNumHeads(int n){
    n = std::clamp(n, 1, MAX_HEADS);
    for(auto& [server, list] : voices){
        while((int)list.size() > n){
            list.back()->free();
            delete list.back();
            list.pop_back();
        }
        const bool running = !list.empty() && list.front()->isCreated();
        while((int)list.size() < n){
            auto* voice = makeVoice(server, (int)list.size());
            // Next to the others, so the graph order holds; without running
            // voices yet, createSynth() creates it with the rest.
            if(running) voice->createAndRun(3, list.back()->nodeID, getActive() && hasData(buffers, server));
            list.push_back(voice);
        }
    }
}

void scDataBox::sendVectorToVoices(const std::string& control, const std::vector<float>& values, float fallback){
    auto sent = lastSent.find(control);
    if(sent != lastSent.end() && sent->second == values) return;
    lastSent[control] = values;
    for(auto& [server, list] : voices){
        for(int i = 0; i < (int)list.size(); i++) list[i]->set(control, valueFor(values, i, fallback));
    }
}

void scDataBox::sendToVoices(const std::string& control, float value){
    for(auto& [server, list] : voices){
        for(auto* voice : list) voice->set(control, value);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// scNode interface
// ════════════════════════════════════════════════════════════════════════════

void scDataBox::buildSynth(ofxSCServer* server){
    if(server == nullptr) return;
    for(auto* voice : voices[server]){ voice->free(); delete voice; }
    voices[server].clear();
    if(!positionBuses[server]) positionBuses[server] = new ofxSCBus(RATE_CONTROL, MAX_HEADS, server);
    for(int i = 0; i < numHeads; i++) voices[server].push_back(makeVoice(server, i));
}

void scDataBox::createSynth(ofxSCServer* server){
    if(server == nullptr || !voices.count(server)) return;
    // Silent until its buffers are loaded on this server
    const bool ready = hasData(buffers, server);
    for(auto* voice : voices[server]){
        if(!voice->isCreated()) voice->createAndRun(0, 1, getActive() && ready);
    }
    if(!ready && !pendingLoads.count(server)) loadBuffersOnServer(server);
}

void scDataBox::free(ofxSCServer* server){
    if(voices.count(server)){
        for(auto* voice : voices[server]){ voice->free(); delete voice; }
        voices.erase(server);
    }
    // The server may be going away: reload from the WAVs when rebuilt
    if(buffers.count(server)){ freeBuffers(buffers[server]); buffers.erase(server); }
    if(pendingLoads.count(server)){ freeBuffers(pendingLoads[server].buffers); pendingLoads.erase(server); }
    if(positionBuses.count(server)){
        if(positionBuses[server]){ positionBuses[server]->free(); delete positionBuses[server]; }
        positionBuses.erase(server);
    }
    outputBuses.erase(server);
}

void scDataBox::setOutputBus(ofxSCServer* server, int index, int bus){
    if(index != 0) return;
    outputBuses[server] = bus;
    auto it = voices.find(server);
    if(it != voices.end()) for(auto* voice : it->second) voice->set("out", bus);
}

int scDataBox::getOutputBusIndex(ofxSCServer* server, int index){
    auto it = outputBuses.find(server);
    return index == 0 && it != outputBuses.end() ? it->second : -1;
}

void scDataBox::moveSynthBefore(ofxSCServer* server, int nodeID){
    auto it = voices.find(server);
    if(it != voices.end()) for(auto* voice : it->second) voice->moveBefore(nodeID);
}

int scDataBox::getLastSynthID(ofxSCServer* server){
    auto it = voices.find(server);
    if(it == voices.end() || it->second.empty()) return -1;
    return it->second.back()->nodeID;
}

void scDataBox::activate(){
    for(auto& [server, list] : voices){
        const bool ready = hasData(buffers, server);
        for(auto* voice : list) if(voice->isCreated()) voice->run(ready);
    }
}

void scDataBox::deactivate(){
    for(auto& [server, list] : voices){
        for(auto* voice : list) if(voice->isCreated()) voice->run(false);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Window
// ════════════════════════════════════════════════════════════════════════════

void scDataBox::openFile(){
    ofFileDialogResult result = ofSystemLoadDialog("Open any file to play as audio");
    if(result.bSuccess) path = result.getPath();
}

void scDataBox::resetRegionToFile(){
    std::error_code ec;
    const int64_t size = path.get().empty() ? 0 : static_cast<int64_t>(fs::file_size(path.get(), ec));
    const int64_t maxBytes = MAX_SAMPLES * bytesPerSample(format);
    offset = 0;
    length = static_cast<int>(std::min<int64_t>({ec ? 0 : size, maxBytes, INT_MAX}));
}

void scDataBox::drawWindow(){
    const float zoom = ofxOceanodeShared::getZoomLevel();
    const std::string title = "DataBox " + ofToString(getNumIdentifier());
    ImGui::SetNextWindowSize(ImVec2(900 * zoom, 540 * zoom), ImGuiCond_FirstUseEver);
    bool open = showWindow.get();
    if(ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)){
        if(!open) showWindow = false;

        if(ImGui::Button("Open...")) openFile();
        ImGui::SameLine();
        if(path.get().empty()){
            ImGui::TextDisabled("Any file: images, documents, applications...");
        }else{
            ImGui::Text("%s", ofFilePath::getFileName(path.get()).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s  |  %s", humanBytes(fileMap.size).c_str(), status.c_str());
        }

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float settingsHeight = ImGui::GetFrameHeightWithSpacing() * 3 + 8 * zoom;
        const float mapHeight = 64 * zoom;
        drawFileMap(avail.x, mapHeight);
        drawRegion(avail.x, std::max(60.0f * zoom, avail.y - mapHeight - settingsHeight - ImGui::GetTextLineHeightWithSpacing() * 2));
        drawSettings();
    }else if(!open){
        showWindow = false;
    }
    ImGui::End();
}

void scDataBox::drawFileMap(float width, float height){
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + width, p0.y + height);
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg));

    const int64_t size = fileMap.size;
    const int cols = static_cast<int>(fileMap.mean.size());
    if(cols > 0){
        for(int c = 0; c < cols; c++){
            const float x0 = p0.x + width * c / cols, x1 = p0.x + width * (c + 1) / cols;
            const float rough = fileMap.roughness[c];
            const float bright = 0.35f + 0.65f * fileMap.mean[c];
            // Cool for structured data (text, headers), warm for dense data
            const ImVec4 colour(bright * (0.25f + 0.7f * rough), bright * (0.5f + 0.1f * rough), bright * (0.85f - 0.6f * rough), 1.0f);
            const float h = height * (0.15f + 0.85f * rough);
            dl->AddRectFilled(ImVec2(x0, p1.y - h), ImVec2(std::max(x1, x0 + 1), p1.y), ImGui::GetColorU32(colour));
        }
    }else if(mapTask.valid()){
        dl->AddText(ImVec2(p0.x + 6, p0.y + 6), ImGui::GetColorU32(ImGuiCol_TextDisabled), "Reading file...");
    }

    ImGui::InvisibleButton("##filemap", ImVec2(width, height));
    if(size > 0){
        const int bps = bytesPerSample(format);
        const int64_t minLength = static_cast<int64_t>(bps) * dataChannels * 2;
        const float rx0 = p0.x + width * float(double(offset.get()) / size);
        const float rx1 = std::max(rx0 + 2, p0.x + width * float(double(offset.get() + length.get()) / size));
        dl->AddRectFilled(ImVec2(rx0, p0.y), ImVec2(rx1, p1.y), IM_COL32(255, 255, 255, 40));
        dl->AddRect(ImVec2(rx0, p0.y), ImVec2(rx1, p1.y), IM_COL32(255, 255, 255, 200), 0, 0, 1.5f);

        const float mx = ImGui::GetIO().MousePos.x;
        const auto toBytes = [&](float x){ return std::clamp<int64_t>(int64_t(double(x - p0.x) / width * size), 0, size); };
        const auto align = [&](int64_t b){ return b - b % bps; };
        const float grip = 5.0f;

        if(ImGui::IsItemActivated()){
            if(std::abs(mx - rx0) <= grip) drag = Drag::Start;
            else if(std::abs(mx - rx1) <= grip) drag = Drag::End;
            else {
                if(mx < rx0 || mx > rx1){
                    // Click outside the region: centre it there
                    offset.setWithoutEventNotifications(static_cast<int>(align(std::clamp<int64_t>(toBytes(mx) - length.get() / 2, 0, std::max<int64_t>(0, size - length.get())))));
                }
                drag = Drag::Move;
                dragAnchorOffset = offset.get();
                dragAnchorX = mx;
            }
        }
        if(ImGui::IsItemActive() && drag != Drag::None){
            const int64_t end = int64_t(offset.get()) + length.get();
            if(drag == Drag::Move){
                const int64_t moved = dragAnchorOffset + int64_t(double(mx - dragAnchorX) / width * size);
                offset.setWithoutEventNotifications(static_cast<int>(align(std::clamp<int64_t>(moved, 0, std::max<int64_t>(0, size - length.get())))));
            }else if(drag == Drag::Start){
                const int64_t start = align(std::clamp<int64_t>(toBytes(mx), 0, end - minLength));
                offset.setWithoutEventNotifications(static_cast<int>(start));
                length.setWithoutEventNotifications(static_cast<int>(std::min<int64_t>(end - start, INT_MAX)));
            }else{
                const int64_t newEnd = std::clamp<int64_t>(toBytes(mx), offset.get() + minLength, size);
                length.setWithoutEventNotifications(static_cast<int>(std::min<int64_t>(newEnd - offset.get(), INT_MAX)));
            }
        }
        if(ImGui::IsItemDeactivated() && drag != Drag::None){
            drag = Drag::None;
            // Through the parameters, so presets and the inspector follow
            offset = offset.get();
            length = length.get();
        }
        if(ImGui::IsItemHovered()){
            if(std::abs(mx - rx0) <= grip || std::abs(mx - rx1) <= grip) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            ImGui::SetTooltip("byte %s", ofToString(toBytes(mx)).c_str());
        }
    }
    ImGui::TextDisabled("Region: offset %s, length %s", humanBytes(offset.get()).c_str(), humanBytes(length.get()).c_str());
}

void scDataBox::drawRegion(float width, float height){
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + width, p0.y + height);
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg));
    ImGui::InvisibleButton("##region", ImVec2(width, height));

    if(preview.empty()){
        dl->AddText(ImVec2(p0.x + 6, p0.y + 6), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    path.get().empty() ? "Open a file" : status.c_str());
        return;
    }

    const int lanes = static_cast<int>(preview.size());
    const float laneH = height / lanes;
    const ImU32 wave = ImGui::GetColorU32(ImGuiCol_PlotLines);
    for(int c = 0; c < lanes; c++){
        const float mid = p0.y + laneH * (c + 0.5f);
        const auto& cols = preview[c];
        for(int x = 0; x < (int)cols.size(); x++){
            const float px = p0.x + width * x / cols.size();
            dl->AddLine(ImVec2(px, mid - cols[x].second * laneH * 0.45f),
                        ImVec2(px, mid - cols[x].first * laneH * 0.45f + 1), wave);
        }
        if(c > 0) dl->AddLine(ImVec2(p0.x, p0.y + laneH * c), ImVec2(p1.x, p0.y + laneH * c), ImGui::GetColorU32(ImGuiCol_Border));
    }

    // Windows from the parameters, playheads from the server
    std::vector<float> heads;
    for(auto& [server, bus] : positionBuses){ if(bus){ heads = bus->readValues; break; } }
    for(int h = 0; h < numHeads; h++){
        const int lane = h % lanes;
        const float y0 = p0.y + laneH * lane, y1 = y0 + laneH;
        const float start = valueFor(position.get(), h, 0.0f);
        const float span = valueFor(window.get(), h, 0.05f);
        const float x0 = p0.x + width * start;
        const float x1 = p0.x + width * std::min(1.0f, start + span);
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(std::max(x1, x0 + 1), y1), headColor(h, 0.18f));
        if(start + span > 1.0f){
            // The window wraps around the end of the region
            dl->AddRectFilled(ImVec2(p0.x, y0), ImVec2(p0.x + width * (start + span - 1.0f), y1), headColor(h, 0.18f));
        }
        if(h < (int)heads.size()){
            const float hx = p0.x + width * std::clamp(heads[h], 0.0f, 1.0f);
            dl->AddLine(ImVec2(hx, y0), ImVec2(hx, y1), headColor(h), 2.0f);
        }
    }

    // Click or drag to move every playhead's window there
    if(ImGui::IsItemActive()){
        const float x = std::clamp((ImGui::GetIO().MousePos.x - p0.x) / width, 0.0f, 1.0f);
        position = std::vector<float>{x};
    }
}

void scDataBox::drawSettings(){
    const float zoom = ofxOceanodeShared::getZoomLevel();
    const float field = 140 * zoom;

    int fmt = format;
    ImGui::SetNextItemWidth(field);
    if(ImGui::Combo("Format", &fmt, formatNames, NumFormats)) format = fmt;
    ImGui::SameLine();
    int chans = dataChannels;
    ImGui::SetNextItemWidth(field);
    if(ImGui::SliderInt("Data channels", &chans, 1, 16)) dataChannels = chans;
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Bytes are interleaved: 3 reads an RGB image as three channels");
    ImGui::SameLine();
    int sr = sampleRate;
    ImGui::SetNextItemWidth(field);
    if(ImGui::DragInt("Rate (Hz)", &sr, 100, 1000, 192000)) sampleRate = sr;

    bool norm = normalize;
    if(ImGui::Checkbox("Normalize", &norm)) normalize = norm;
    ImGui::SameLine();
    bool dc = dcRemove;
    if(ImGui::Checkbox("DC remove", &dc)) dcRemove = dc;
    ImGui::SameLine();
    float l = lag;
    ImGui::SetNextItemWidth(field);
    if(ImGui::SliderFloat("Lag", &l, 0, 2, "%.2f s")) lag = l;
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Position smoothing: higher values scrub through the data");
    ImGui::SameLine();
    float f = fade;
    ImGui::SetNextItemWidth(field);
    if(ImGui::SliderFloat("Fade", &f, 0, 0.5f, "%.2f")) fade = f;
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Fade at the loop point, as a fraction of the window");
}
