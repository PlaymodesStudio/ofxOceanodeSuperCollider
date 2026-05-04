//
//  scBufferBrowser.cpp
//  ofxOceanodeSuperCollider
//

#include "scBufferBrowser.h"
#include <algorithm>
#include <fstream>
#include <cstring>

// ════════════════════════════════════════════════════════════════════════════
// Constructor / Destructor
// ════════════════════════════════════════════════════════════════════════════

scBufferBrowser::scBufferBrowser(vector<serverManager*> servers)
    : ofxOceanodeNodeModel("SC Buffer Browser"), allServers(servers)
{
    for(auto* sm : allServers) {
        if(sm && sm->getServer()) {
            previewServer = sm->getServer();
            break;
        }
    }
}

scBufferBrowser::~scBufferBrowser() {
    stopPreview();
    freeAllBuffers();
}

// ════════════════════════════════════════════════════════════════════════════
// Setup
// ════════════════════════════════════════════════════════════════════════════

void scBufferBrowser::setup() {
    addParameter(showWindow.set("Show", false));
    addOutputParameter(bufferOutput.set("Buffer", {-1}, {-1}, {INT_MAX}));

    auto browserRegionRef = addCustomRegion(browserRegion.set("Buffer Browser", [this](){
        drawBrowserContents();
    }), [this](){
        drawBrowserContents();
    });
    browserRegionRef->setFlags(browserRegionRef->getFlags() | ofxOceanodeParameterFlags_NoGuiWidget);

    // Default browse location
    currentBrowseDir = ofToDataPath("Supercollider/Samples", true);
    if(!std::filesystem::exists(currentBrowseDir))
        currentBrowseDir = ofFilePath::getUserHomeDir();
    refreshBrowseDir(currentBrowseDir);
}

// ════════════════════════════════════════════════════════════════════════════
// Draw (floating ImGui window)
// ════════════════════════════════════════════════════════════════════════════

void scBufferBrowser::draw(ofEventArgs&) {
    if(!showWindow) return;

    string title = "Buffer Browser " + ofToString(getNumIdentifier());
    ImGui::SetNextWindowSize(ImVec2(320, 500), ImGuiCond_FirstUseEver);
    if(ImGui::Begin(title.c_str(), (bool*)&showWindow.get(),
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        drawBrowserContents();
    }
    ImGui::End();
}

void scBufferBrowser::drawBrowserContents() {
    // ── Top bar ──────────────────────────────────────────────────────
    if(ImGui::Button("...")) {
        auto res = ofSystemLoadDialog("Select Samples Folder", true, currentBrowseDir);
        if(res.bSuccess) refreshBrowseDir(res.getPath());
    }
    ImGui::SameLine();
    std::string dirLabel = std::filesystem::path(currentBrowseDir).filename().string();
    ImGui::TextUnformatted(dirLabel.c_str());

    // Show loaded file count
    if(!loadedFiles.empty()) {
        if(loadedFiles.size() == 1) {
            std::string fname = std::filesystem::path(loadedFiles[0]).filename().string();
            ImGui::TextDisabled("Loaded: %s", fname.c_str());
        } else {
            ImGui::TextDisabled("Loaded: %d files", (int)loadedFiles.size());
        }
    } else {
        ImGui::TextDisabled("(no files loaded)");
    }
    ImGui::TextDisabled("click=preview, dbl=toggle select, arrows=navigate");
    ImGui::Separator();

    // ── File list ────────────────────────────────────────────────────
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##blist", ImVec2(0, avail.y), false);

    // Up one level
    if(ImGui::Selectable("^ ..")) {
        auto parent = std::filesystem::path(currentBrowseDir).parent_path().string();
        if(!parent.empty() && parent != currentBrowseDir) {
            refreshBrowseDir(parent);
        }
    }

    // ── Keyboard navigation ──────────────────────────────────────────
    if(ImGui::IsWindowFocused()) {
        bool moved = false;
        if(ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            focusIndex = std::max(0, focusIndex - 1);
            moved = true;
        }
        if(ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            focusIndex = std::min((int)browseEntries.size() - 1, focusIndex + 1);
            moved = true;
        }
        if(ImGui::IsKeyPressed(ImGuiKey_Enter) && focusIndex >= 0 && focusIndex < (int)browseEntries.size()) {
            auto& entry = browseEntries[focusIndex];
            if(entry.isDir) {
                refreshBrowseDir(entry.fullPath);
            } else {
                if(selectedIndices.count(focusIndex)) selectedIndices.erase(focusIndex);
                else selectedIndices.insert(focusIndex);
                rebuildBuffersFromSelection();
            }
        }
        if(moved && focusIndex >= 0 && focusIndex < (int)browseEntries.size()) {
            if(!browseEntries[focusIndex].isDir) triggerPreview(browseEntries[focusIndex].fullPath);
        }
    }

    for(int i = 0; i < (int)browseEntries.size(); i++) {
        auto& entry = browseEntries[i];

        std::string prefix;
        if(entry.isDir) prefix = "[D] ";
        else if(selectedIndices.count(i)) prefix = " *  ";
        else prefix = "    ";
        std::string label = prefix + entry.name;

        bool isSelected = selectedIndices.count(i) > 0;
        bool isFocused  = (i == focusIndex);

        if(isFocused && isSelected) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.15f, 0.55f, 0.25f, 1.0f));
        } else if(isFocused) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.25f, 0.35f, 0.55f, 1.0f));
        } else if(isSelected) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.10f, 0.40f, 0.18f, 1.0f));
        }

        bool highlight = isFocused || isSelected;

        if(ImGui::Selectable(label.c_str(), highlight, ImGuiSelectableFlags_AllowDoubleClick)) {
            if(entry.isDir) {
                refreshBrowseDir(entry.fullPath);
                if(highlight) ImGui::PopStyleColor();
                break;
            } else {
                focusIndex = i;
                if(ImGui::IsMouseDoubleClicked(0)) {
                    if(selectedIndices.count(i)) selectedIndices.erase(i);
                    else selectedIndices.insert(i);
                    rebuildBuffersFromSelection();
                } else {
                    triggerPreview(entry.fullPath);
                }
            }
        }

        if(highlight) ImGui::PopStyleColor();
        if(isFocused && ImGui::IsWindowFocused()) ImGui::SetScrollHereY(0.5f);
        if(!entry.isDir && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", entry.name.c_str());
    }

    ImGui::EndChild();
}

// ════════════════════════════════════════════════════════════════════════════
// Preset serialization
// ════════════════════════════════════════════════════════════════════════════

void scBufferBrowser::presetSave(ofJson& json) {
    json["browseDir"] = currentBrowseDir;
    ofJson filesArr = ofJson::array();
    for(auto& f : loadedFiles) {
        filesArr.push_back(f);
    }
    json["loadedFiles"] = filesArr;
}

void scBufferBrowser::loadBeforeConnections(ofJson& json) {
    // Restore browse dir early so it's visible immediately
    if(json.contains("browseDir")) {
        std::string dir = json["browseDir"].get<std::string>();
        if(std::filesystem::exists(dir)) {
            refreshBrowseDir(dir);
        }
    }
    // Restore loadedFiles list so presetRecallAfterSettingParameters can use it
    if(json.contains("loadedFiles")) {
        loadedFiles.clear();
        for(auto& f : json["loadedFiles"]) {
            std::string path = f.get<std::string>();
            if(std::filesystem::exists(path))
                loadedFiles.push_back(path);
        }
    }
}

void scBufferBrowser::macroSave(ofJson& json, string /*path*/) {
    json["browseDir"] = currentBrowseDir;
    ofJson filesArr = ofJson::array();
    for(auto& f : loadedFiles) {
        filesArr.push_back(f);
    }
    json["loadedFiles"] = filesArr;
}

void scBufferBrowser::macroLoad(ofJson& json, string /*path*/) {
    if(json.contains("browseDir")) {
        std::string dir = json["browseDir"].get<std::string>();
        if(std::filesystem::exists(dir)) {
            refreshBrowseDir(dir);
        }
    }
    if(json.contains("loadedFiles")) {
        loadedFiles.clear();
        for(auto& f : json["loadedFiles"]) {
            std::string path = f.get<std::string>();
            if(std::filesystem::exists(path))
                loadedFiles.push_back(path);
        }
        // Reload buffers from restored file list
        if(!loadedFiles.empty()) {
            freeAllBuffers();
            std::vector<int> allIndices;
            for(auto& lf : loadedFiles) {
                loadFileIntoBuffers(lf, allIndices);
            }
            if(allIndices.empty()) allIndices.push_back(-1);
            bufferOutput = allIndices;

            // Rebuild selectedIndices
            selectedIndices.clear();
            for(int i = 0; i < (int)browseEntries.size(); i++) {
                if(!browseEntries[i].isDir) {
                    for(auto& lf : loadedFiles) {
                        if(browseEntries[i].fullPath == lf) {
                            selectedIndices.insert(i);
                            break;
                        }
                    }
                }
            }
        }
    }
}

void scBufferBrowser::presetRecallAfterSettingParameters(ofJson& json) {
    // If loadedFiles wasn't restored yet (e.g. loadBeforeConnections not called),
    // restore from json now
    if(loadedFiles.empty() && json.contains("loadedFiles")) {
        for(auto& f : json["loadedFiles"]) {
            std::string path = f.get<std::string>();
            if(std::filesystem::exists(path))
                loadedFiles.push_back(path);
        }
    }
    // Reload all buffers from the saved file list
    if(!loadedFiles.empty()) {
        freeAllBuffers();
        std::vector<int> allIndices;
        for(auto& f : loadedFiles) {
            loadFileIntoBuffers(f, allIndices);
        }
        if(allIndices.empty()) allIndices.push_back(-1);
        bufferOutput = allIndices;

        // Rebuild selectedIndices to match browseEntries
        selectedIndices.clear();
        for(int i = 0; i < (int)browseEntries.size(); i++) {
            if(!browseEntries[i].isDir) {
                for(auto& lf : loadedFiles) {
                    if(browseEntries[i].fullPath == lf) {
                        selectedIndices.insert(i);
                        break;
                    }
                }
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// File browser
// ════════════════════════════════════════════════════════════════════════════

void scBufferBrowser::refreshBrowseDir(const std::string& dir) {
    browseEntries.clear();
    selectedIndices.clear();
    focusIndex = -1;
    currentBrowseDir = dir;
    if(!std::filesystem::exists(dir)) return;

    try {
        std::vector<BrowseEntry> dirs, files;
        for(auto& e : std::filesystem::directory_iterator(dir)) {
            std::string fname = e.path().filename().string();
            if(fname.empty() || fname.front() == '.') continue;

            if(e.is_directory()) {
                dirs.push_back({true, fname, e.path().string()});
            } else {
                std::string ext = ofToLower(e.path().extension().string());
                if(ext == ".wav" || ext == ".aif" || ext == ".aiff") {
                    files.push_back({false, fname, e.path().string()});
                }
            }
        }
        std::sort(dirs.begin(),  dirs.end(),  [](auto& a, auto& b){ return a.name < b.name; });
        std::sort(files.begin(), files.end(), [](auto& a, auto& b){ return a.name < b.name; });
        browseEntries.insert(browseEntries.end(), dirs.begin(),  dirs.end());
        browseEntries.insert(browseEntries.end(), files.begin(), files.end());
    } catch(const std::exception& e) {
        ofLogWarning("scBufferBrowser") << "refreshBrowseDir: " << e.what();
    }

    // Re-highlight any previously loaded files that are in this directory
    for(int i = 0; i < (int)browseEntries.size(); i++) {
        if(!browseEntries[i].isDir) {
            for(auto& lf : loadedFiles) {
                if(browseEntries[i].fullPath == lf) {
                    selectedIndices.insert(i);
                    break;
                }
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Multi-select buffer management
// ════════════════════════════════════════════════════════════════════════════

void scBufferBrowser::rebuildBuffersFromSelection() {
    freeAllBuffers();
    loadedFiles.clear();

    // Collect selected file paths in browse order
    std::vector<std::string> selectedPaths;
    for(int i = 0; i < (int)browseEntries.size(); i++) {
        if(selectedIndices.count(i) && !browseEntries[i].isDir) {
            selectedPaths.push_back(browseEntries[i].fullPath);
        }
    }

    std::vector<int> allIndices;
    for(auto& path : selectedPaths) {
        loadFileIntoBuffers(path, allIndices);
        loadedFiles.push_back(path);
    }

    if(allIndices.empty()) allIndices.push_back(-1);
    bufferOutput = allIndices;
}

void scBufferBrowser::loadFileIntoBuffers(const std::string& absPath,
                                           std::vector<int>& outIndices) {
    int numCh = getNumChannelsFromFile(absPath);
    if(numCh < 1) numCh = 1;

    ofxSCServer* primaryServer = nullptr;
    for(auto* sm : allServers) {
        if(sm && sm->getServer()) { primaryServer = sm->getServer(); break; }
    }
    if(!primaryServer) return;

    for(int ch = 0; ch < numCh; ch++) {
        try {
            auto* buf = new ofxSCBuffer(0, 0, primaryServer);
            buf->readChannel(absPath, {ch});
            buffers.push_back(buf);
            outIndices.push_back(buf->index);
            ofLogNotice("scBufferBrowser") << "Loaded " << absPath
                << " ch" << ch << " bufnum=" << buf->index;
        } catch(const std::exception& e) {
            ofLogError("scBufferBrowser") << "loadFile ch" << ch << ": " << e.what();
        }
    }

    // Mirror to other servers
    for(size_t j = 1; j < allServers.size(); j++) {
        if(!allServers[j] || !allServers[j]->getServer()) continue;
        ofxSCServer* srv = allServers[j]->getServer();
        for(int ch = 0; ch < numCh; ch++) {
            try {
                auto* buf = new ofxSCBuffer(0, 0, srv);
                buf->readChannel(absPath, {ch});
                mirrorBuffers.push_back(buf);
            } catch(const std::exception& e) {
                ofLogError("scBufferBrowser") << "mirror ch" << ch << ": " << e.what();
            }
        }
    }
}

void scBufferBrowser::freeAllBuffers() {
    for(auto* b : buffers)       if(b) { b->free(); delete b; }
    for(auto* b : mirrorBuffers) if(b) { b->free(); delete b; }
    buffers.clear();
    mirrorBuffers.clear();
}

// ════════════════════════════════════════════════════════════════════════════
// WAV channel count reader
// ════════════════════════════════════════════════════════════════════════════

int scBufferBrowser::getNumChannelsFromFile(const std::string& absPath) {
    std::ifstream file(absPath, std::ios::binary);
    if(!file.is_open()) return 0;

    char riff[4], wave[4];
    uint32_t riffSize;
    file.read(riff, 4);
    file.read((char*)&riffSize, 4);
    file.read(wave, 4);
    if(std::strncmp(riff, "RIFF", 4) != 0 || std::strncmp(wave, "WAVE", 4) != 0) {
        return 1;  // Not WAV (AIFF etc.) — assume mono
    }

    while(file.good()) {
        char chunkID[4];
        uint32_t chunkSize;
        file.read(chunkID, 4);
        file.read((char*)&chunkSize, 4);
        if(!file.good()) break;

        if(std::strncmp(chunkID, "fmt ", 4) == 0 && chunkSize >= 16) {
            uint16_t formatType, channels;
            file.read((char*)&formatType, 2);
            file.read((char*)&channels, 2);
            return (int)channels;
        }
        file.seekg(chunkSize + (chunkSize & 1), std::ios::cur);
    }
    return 1;
}

// ════════════════════════════════════════════════════════════════════════════
// Preview (single-click audition)
// ════════════════════════════════════════════════════════════════════════════

void scBufferBrowser::triggerPreview(const std::string& absPath) {
    stopPreview();
    if(absPath.empty()) return;

    if(!previewServer) {
        for(auto* sm : allServers) {
            if(sm && sm->getServer()) { previewServer = sm->getServer(); break; }
        }
    }
    if(!previewServer) return;

    try {
        previewBuf = new ofxSCBuffer(0, 0, previewServer);
        previewBuf->read(absPath);

        previewSynth = new ofxSCSynth("BufferBrowserPreview", previewServer);
        previewSynth->set("bufnum", previewBuf->index);
        previewSynth->set("out",    0);
        previewSynth->set("gain",   0.7f);
        previewSynth->addToTail();
    } catch(const std::exception& e) {
        ofLogError("scBufferBrowser") << "triggerPreview: " << e.what();
        stopPreview();
    }
}

void scBufferBrowser::stopPreview() {
    if(previewSynth) { previewSynth->free(); delete previewSynth; previewSynth = nullptr; }
    if(previewBuf)   { previewBuf->free();   delete previewBuf;   previewBuf   = nullptr; }
}
