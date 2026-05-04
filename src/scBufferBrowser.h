//
//  scBufferBrowser.h
//  ofxOceanodeSuperCollider
//
//  Standalone file-browser node with preview and vector<int> buffer output.
//  Follows the scBuffer pattern: per-channel readChannel, one buffer index per channel.
//  Supports multi-select: selecting 2 stereo files → vector<int> of size 4.
//

#pragma once

#include "ofxOceanodeNodeModel.h"
#include "serverManager.h"
#include "ofxSuperCollider.h"
#include "ofxSCSynth.h"
#include "ofxSCBuffer.h"
#include "imgui.h"
#include <vector>
#include <string>
#include <set>
#include <filesystem>

class scBufferBrowser : public ofxOceanodeNodeModel {
public:
    scBufferBrowser(vector<serverManager*> servers);
    ~scBufferBrowser();

    void setup() override;
    void draw(ofEventArgs&) override;
    void drawBrowserContents();

    // ── Preset serialization ────────────────────────────────────────────────
    void presetSave(ofJson &json) override;
    void presetRecallAfterSettingParameters(ofJson &json) override;
    void loadBeforeConnections(ofJson &json) override;
    void macroSave(ofJson &json, string path) override;
    void macroLoad(ofJson &json, string path) override;

private:
    // ── File browser state ──────────────────────────────────────────────────
    struct BrowseEntry {
        bool        isDir;
        std::string name;
        std::string fullPath;
    };
    std::string              currentBrowseDir;
    std::vector<BrowseEntry> browseEntries;
    int                      focusIndex = -1;  // keyboard cursor in browseEntries

    void refreshBrowseDir(const std::string& dir);

    // ── Multi-selection ─────────────────────────────────────────────────────
    // Tracks which file entries are selected (indices into browseEntries).
    std::set<int>             selectedIndices;
    std::vector<std::string>  loadedFiles;   // absolute paths of loaded files, in order

    void rebuildBuffersFromSelection();
    void loadFileIntoBuffers(const std::string& absPath,
                             std::vector<int>& outIndices);
    void freeAllBuffers();

    int getNumChannelsFromFile(const std::string& absPath);

    // ── Preview ─────────────────────────────────────────────────────────────
    void triggerPreview(const std::string& absPath);
    void stopPreview();

    // ── Servers ─────────────────────────────────────────────────────────────
    vector<serverManager*> allServers;
    ofxSCServer* previewServer = nullptr;

    // ── Buffers (per-channel, same pattern as scBuffer) ─────────────────────
    std::vector<ofxSCBuffer*> buffers;        // primary server buffers
    std::vector<ofxSCBuffer*> mirrorBuffers;  // extra servers

    // ── Preview resources ───────────────────────────────────────────────────
    ofxSCBuffer* previewBuf   = nullptr;
    ofxSCSynth*  previewSynth = nullptr;

    // ── Parameters ──────────────────────────────────────────────────────────
    ofParameter<bool>         showWindow;
    ofParameter<vector<int>>  bufferOutput;
    customGuiRegion           browserRegion;

    ofEventListeners nodeListeners;
};
