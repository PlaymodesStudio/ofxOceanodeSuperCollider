//
//  scMultiTrackRecorder.h
//
//  N-track recorder.
//
//  Uses dedicated "multirecbuf{nCh}" synthdefs which use InFeedback.ar
//  instead of In.ar. This makes execution order in SC's node tree
//  irrelevant — the recorder synths always read the previous block's
//  bus data, so they work regardless of when they were created relative
//  to the source synths. This solves the root cause of why multiple
//  simultaneous recbuf instances fail.
//
//  Bus indices are updated via graphComputed listener, which fires after
//  serverManager::recomputeGraph() has freed old buses and assigned new ones.
//

#pragma once

#include "ofxOceanodeNodeModel.h"
#include "serverManager.h"
#include "scNode.h"
#include "ofxSCBuffer.h"
#include "ofxSCSynth.h"
#include "ofxOscMessage.h"
#include "ofxOceanodeShared.h"
#include "ofMain.h"
#include <filesystem>
namespace fs = std::filesystem;

class scMultiTrackRecorder : public ofxOceanodeNodeModel {
public:
    static constexpr int MAX_TRACKS = 16;

    explicit scMultiTrackRecorder(std::vector<serverManager*> outputServers)
        : ofxOceanodeNodeModel("SC MultiTrack Rec"),
          servers(std::move(outputServers)) {}

    ~scMultiTrackRecorder() override { cleanupAll(); }

    void setup() override {
        description = "Records N audio inputs simultaneously to separate WAV files";

        addInspectorParameter(numTracks.set("Num Tracks", 4, 1, MAX_TRACKS));
        addParameter(numChannels.set("N Chan", 1, 1, 24));
        addParameter(lengthSec.set("Length(sec)", 20.0f, 0.01f, 3600.0f));
        addParameter(serverIndex.set("Server", 0, 0, MAX(0, (int)servers.size()-1)));
        addParameter(record.set("Record", false));
        addParameter(saveDir.set("Folder", ofToDataPath("recordings/", true)));
        addParameter(chooseFolder.set("ChooseFolder", false));
        addParameter(recLED.set("Rec LED", ofColor(255, 0, 0)),
                     ofxOceanodeParameterFlags_DisableInConnection |
                     ofxOceanodeParameterFlags_ReadOnly);
        addParameter(lastSession.set("Last Session", ""),
                     ofxOceanodeParameterFlags_DisableInConnection);

        trackBufs.resize(MAX_TRACKS, nullptr);
        trackSynths.resize(MAX_TRACKS, nullptr);

        // Reserve so emplace_back never invalidates addParameter references
        trackInputs.reserve(MAX_TRACKS);
        for(int t = 0; t < numTracks.get(); t++) pushTrackInput(t);

        listeners.push(numTracks.newListener([this](int& n){ onNumTracksChanged(); }));
        listeners.push(numChannels.newListener([this](int&){ recreateAll(); }));
        listeners.push(lengthSec.newListener([this](float&){ recreateAll(); }));
        listeners.push(serverIndex.newListener([this](int&){ recreateAll(); }));
        listeners.push(record.newListener([this](bool& r){ handleRecordToggle(r); }));
        listeners.push(chooseFolder.newListener([this](bool& t){
            if(t){ openFolderDialog(); chooseFolder = false; }
        }));

        // After every graph recompute, buses have been reassigned.
        // Update all synth "in" params to the new bus indices.
        for(int si = 0; si < (int)servers.size(); si++){
            listeners.push(servers[si]->graphComputed.newListener([this](){
                syncBuses();
            }));
        }
    }

    void update(ofEventArgs&) override {
        if(!isRecording) return;
        uint64_t elapsedMs = ofGetElapsedTimeMillis() - recStartTimeMs;
        if(elapsedMs >= static_cast<uint64_t>(lengthSec.get() * 1000.0f)){
            recStopTimeMs = ofGetElapsedTimeMillis();
            record = false;
        }
    }

    void activate()   override { for(auto* s : trackSynths) if(s) s->run(true); }
    void deactivate() override { for(auto* s : trackSynths) if(s) s->run(false); }

    // Called by Oceanode BEFORE restoring connections from the preset file.
    // We must ensure the correct number of "In N" ports exist so the
    // connection-restore pass can find them by name.
    void loadBeforeConnections(ofJson& json) override {
        isLoadingPreset = true;

        // deserializeParameter handles all JSON type coercions safely,
        // and fires the numTracks listener — but isLoadingPreset guards it.
        deserializeParameter(json, numTracks);

        int savedN = std::max(1, std::min(numTracks.get(), MAX_TRACKS));

        // Directly adjust ports to match savedN without going through listener
        int curN = (int)trackInputs.size();
        if(savedN > curN){
            for(int t = curN; t < savedN; t++) pushTrackInput(t);
        } else if(savedN < curN){
            for(int t = curN - 1; t >= savedN; t--){
                cleanupTrack(t);
                popTrackInput();
            }
        }

        isLoadingPreset = false;
    }

private:

    // ── input port management ──────────────────────────────────────────────────

    void pushTrackInput(int t){
        trackInputs.emplace_back();
        trackInputs.back().set("In " + ofToString(t + 1), nodePort());
        addParameter(trackInputs.back(), ofxOceanodeParameterFlags_DisableOutConnection);
        listeners.push(trackInputs.back().newListener([this, t](nodePort&){
            recreateTrack(t);
        }));
    }

    void popTrackInput(){
        if(trackInputs.empty()) return;
        removeParameter(trackInputs.back().getName());
        trackInputs.pop_back();
    }

    void onNumTracksChanged(){
        if(isLoadingPreset) return;  // loadBeforeConnections handles this
        int newN = numTracks.get();
        int curN = (int)trackInputs.size();
        if(newN > curN){
            for(int t = curN; t < newN; t++) pushTrackInput(t);
        } else {
            for(int t = curN - 1; t >= newN; t--){
                cleanupTrack(t);
                popTrackInput();
            }
        }
    }

    // ── per-track creation (only buf+synth for one track) ─────────────────────

    void recreateTrack(int t){
        if(numChannels < 1 || numChannels > 24) return;
        if(serverIndex < 0 || serverIndex >= (int)servers.size()) return;
        if(t < 0 || t >= MAX_TRACKS) return;

        cleanupTrack(t);

        if(t >= (int)trackInputs.size()) return;
        if(trackInputs[t]->getNodeRef() == nullptr) return;

        ofxSCServer* server = servers[serverIndex]->getServer();
        int nCh  = numChannels.get();
        int nFrm = static_cast<int>(std::ceil(lengthSec.get() * 44100.0f));
        std::string defName = "multirecbuf" + ofToString(nCh);

        trackBufs[t] = new ofxSCBuffer(nFrm, nCh, server);
        trackBufs[t]->alloc();

        trackSynths[t] = new ofxSCSynth(defName, server);
        trackSynths[t]->createAndRun(1, 1, getActive());  // addToTail
        trackSynths[t]->set("buf", trackBufs[t]->index);
        trackSynths[t]->set("record", 0);

        // Set bus now — corrected again in syncBuses() after graphComputed
        trackSynths[t]->set("in", trackInputs[t]->getBusIndex(server));
    }

    // ── full recreation (numChannels / lengthSec / server changed) ────────────

    void recreateAll(){
        if(numChannels < 1 || numChannels > 24) return;
        if(isRecording){ record = false; handleRecordToggle(false); }
        for(int t = 0; t < (int)trackInputs.size(); t++){
            recreateTrack(t);
        }
    }

    // ── bus sync: called after every graphComputed ────────────────────────────
    // serverManager has freed old buses and assigned new ones;
    // getBusIndex() now returns the new valid index.
    // Also creates synths for tracks whose input was wired but whose
    // recreateTrack() fired before getNodeRef() was non-null.

    void syncBuses(){
        if(serverIndex < 0 || serverIndex >= (int)servers.size()) return;
        ofxSCServer* server = servers[serverIndex]->getServer();

        for(int t = 0; t < MAX_TRACKS; t++){
            if(t >= (int)trackInputs.size()) continue;
            if(trackInputs[t]->getNodeRef() == nullptr) continue;

            // If synth doesn't exist yet (input was wired after recreateTrack
            // returned early because getNodeRef was null), create it now.
            if(!trackSynths[t]){
                recreateTrack(t);
                continue;  // recreateTrack already calls getBusIndex
            }

            trackSynths[t]->set("in", trackInputs[t]->getBusIndex(server));
        }
    }

    // ── record toggle ─────────────────────────────────────────────────────────

    void handleRecordToggle(bool rec){
        isRecording = rec;
        if(rec){
            // Recreate any synth that is missing (e.g. input connected before
            // first graphComputed), then sync all bus indices right now.
            // This is the last possible moment — guaranteed to be correct.
            syncBuses();
        }
        for(auto* s : trackSynths) if(s) s->set("record", rec ? 1 : 0);
        if(rec){
            recStartTimeMs = ofGetElapsedTimeMillis();
            recLED = ofColor(0, 255, 0);
        } else {
            recStopTimeMs = ofGetElapsedTimeMillis();
            recLED = ofColor(255, 0, 0);
            saveAllTracks();
        }
    }

    // ── save ──────────────────────────────────────────────────────────────────

    void saveAllTracks(){
        if(serverIndex < 0 || serverIndex >= (int)servers.size()) return;
        ofxSCServer* server = servers[serverIndex]->getServer();

        try { fs::create_directories(saveDir.get()); }
        catch(const std::exception& e){
            ofLogError("scMultiTrackRecorder") << "Cannot create dir: " << e.what();
            return;
        }

        std::string preset = ofxOceanodeShared::getCurrentPresetName();
        if(preset.empty()) preset = "NoPreset";
        ofStringReplace(preset, " ", "_");
        ofStringReplace(preset, "/", "_");
        ofStringReplace(preset, "\\", "_");
        ofStringReplace(preset, ":", "_");

        std::string ts = ofGetTimestampString("%Y%m%d_%H%M%S");
        // Include server index so two instances recording simultaneously
        // never write to the same filename.
        std::string srvTag = "_srv" + ofToString(serverIndex.get());
        std::string firstPath;

        for(int t = 0; t < MAX_TRACKS; t++){
            if(!trackBufs[t]) continue;
            // Skip buffers that failed to allocate (index 0 = SC default/uninitialized)
            if(trackBufs[t]->index <= 0) continue;

            std::string fname   = preset + "_track" + ofToString(t + 1) + srvTag + "_" + ts + ".wav";
            std::string absPath = (fs::path(saveDir.get()) / fname).string();

            ofxOscMessage m;
            m.setAddress("/b_write");
            m.addIntArg(trackBufs[t]->index);
            m.addStringArg(absPath);
            m.addStringArg("wav");
            m.addStringArg("float");
            m.addIntArg(-1);  // all frames
            m.addIntArg(0);
            m.addIntArg(0);
            server->sendMsg(m);

            if(firstPath.empty()) firstPath = absPath;
        }
        lastSession = firstPath;
    }

    // ── cleanup ───────────────────────────────────────────────────────────────

    void cleanupTrack(int t){
        if(t < 0 || t >= (int)trackBufs.size()) return;
        if(trackSynths[t]){ trackSynths[t]->free(); delete trackSynths[t]; trackSynths[t] = nullptr; }
        if(trackBufs[t]){   trackBufs[t]->free();   delete trackBufs[t];   trackBufs[t]   = nullptr; }
    }

    void cleanupAll(){
        isRecording = false;
        for(int t = 0; t < (int)trackBufs.size(); t++) cleanupTrack(t);
        recLED = ofColor(255, 0, 0);
    }

    void openFolderDialog(){
        ofFileDialogResult res = ofSystemLoadDialog("Choose folder", true);
        if(res.bSuccess) saveDir = res.getPath();
    }

    // ── members ───────────────────────────────────────────────────────────────
    ofEventListeners listeners;

    ofParameter<int>         numTracks;
    ofParameter<int>         numChannels;
    ofParameter<float>       lengthSec;
    ofParameter<int>         serverIndex;
    ofParameter<bool>        record;
    ofParameter<std::string> saveDir;
    ofParameter<bool>        chooseFolder;
    ofParameter<ofColor>     recLED;
    ofParameter<std::string> lastSession;

    std::vector<ofParameter<nodePort>> trackInputs;

    std::vector<ofxSCBuffer*> trackBufs;    // MAX_TRACKS, null = inactive
    std::vector<ofxSCSynth*>  trackSynths;  // MAX_TRACKS, null = inactive

    bool     isRecording    = false;
    bool     isLoadingPreset = false;
    uint64_t recStartTimeMs = 0;
    uint64_t recStopTimeMs  = 0;

    std::vector<serverManager*> servers;
};
