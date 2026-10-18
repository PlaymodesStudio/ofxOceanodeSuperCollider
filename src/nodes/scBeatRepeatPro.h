//
//  scBeatRepeatPro.h
//  ofxOceanodeSuperCollider
//
//  Multichannel beat repeater with a dockable waveform editor.  All channels
//  share one transport/read head so stereo and surround images stay coherent.
//

#pragma once

#include "ofxOceanodeNodeModel.h"
#include "ofxOceanodeShared.h"
#include "scNode.h"
#include "serverManager.h"
#include "ofxSCSynth.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

class scBeatRepeatPro : public scNode {
public:
    // its window (SC Mixer Pro's "Go to origin synth window" opens and focuses it)
    std::string getWindowTitle() override { return (canvasID == "Canvas" ? "" : canvasID + "/") + "BeatRepeat Pro " + ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return &showWindow; }
private:
public:
    static constexpr int MAX_CHANNELS = 16;
    static constexpr int WAVEFORM_BINS = 1536;
    static constexpr int SNAPSHOT_COUNT = 24;

    explicit scBeatRepeatPro(std::vector<serverManager*> servers);
    ~scBeatRepeatPro() override;

    void setup() override;
    void update(ofEventArgs&) override;
    void draw(ofEventArgs&) override;
    void activate() override;
    void deactivate() override;

    void buildSynth(ofxSCServer* server) override;
    void createSynth(ofxSCServer* server) override;
    void free(ofxSCServer* server) override;
    void moveSynthBefore(ofxSCServer* server, int nodeID) override;
    void setOutputBus(ofxSCServer* server, int index, int bus) override;
    void setInputBus(ofxSCServer* server, scNode* node, int bus) override;
    void resetInputBusses(ofxSCServer* server, int targetBus = 0) override;
    int getOutputBusIndex(ofxSCServer* server, int index) override;
    int getLastSynthID(ofxSCServer* server) override;
    void resendParametersForNRT() override;
    void setBpm(float bpm) override;

    void presetSave(ofJson& json) override;
    void loadBeforeConnections(ofJson& json) override;
    void presetRecallBeforeSettingParameters(ofJson& json) override;
    void presetRecallAfterSettingParameters(ofJson& json) override;

private:
    std::vector<serverManager*> allServers;
    std::map<ofxSCServer*, ofxSCSynth*> synths;
    std::map<ofxSCServer*, ofEventListener> feedbackListeners;
    std::map<ofxSCServer*, int> inputBuses;
    std::map<ofxSCServer*, int> outputBuses;
    ofEventListeners nodeListeners;

    // Always-visible, basic node controls.
    ofParameter<bool>  showWindow;
    ofParameter<int>   numChannels;
    ofParameter<bool>  go;
    // Effective musical denominator: 16 = a sixteenth note. Dotted and
    // triplet choices use their duration-equivalent denominators.
    ofParameter<float> repeatDivision;
    ofParameter<float> mix;
    ofParameter<float> level;
    ofParameter<int>   reset;

    // Full editor controls; any of these can be published into the node.
    ofParameter<bool>  adaptLength;
    ofParameter<bool>  pitchMode;
    ofParameter<float> repeatPitch;
    ofParameter<float> offsetMs;
    ofParameter<float> gate;
    ofParameter<float> declickMs;
    ofParameter<float> rate;
    ofParameter<bool>  reverse;
    ofParameter<float> pitchStep;
    ofParameter<int>   numPitchSteps;
    ofParameter<float> loopAmp;
    ofParameter<float> inputGain;
    ofParameter<int>   filterType;
    ofParameter<float> cutoffPitch;
    ofParameter<float> cutoffStep;
    ofParameter<int>   numCutoffSteps;
    ofParameter<float> resonance;
    ofParameter<bool>  limiter;
    ofParameter<float> ceiling;
    ofParameter<bool>  autoReset;
    // 0 = count repeat occurrences, 1 = elapsed quarter-note beats.
    ofParameter<int>   autoResetMode;
    ofParameter<float> autoResetLoops;
    ofParameter<float> autoResetBeats;
    ofParameter<bool>  snapshotActivates;
    ofParameter<bool>  momentary;

    // Auto Trigger: a transport-locked machine that, on each grid step,
    // decides whether to fire a repeat and which snapshot it uses.
    static constexpr int AUTO_MAX_LANE_STEPS = 32;
    ofParameter<bool>  autoEnabled;
    ofParameter<int>   autoClock;     // 0 = transport, 1 = free (node BPM), 2 = follow transport when playing
    ofParameter<int>   autoMode;      // 0 = every Interval steps, 1 = probability lane
    ofParameter<int>   autoGrid;      // index into brAutoGrids() (steps per beat)
    ofParameter<int>   autoInterval;
    ofParameter<int>   autoOffset;
    ofParameter<int>   autoLaneSteps;
    ofParameter<float> autoChance;
    ofParameter<float> autoLength;    // quarter-note beats a fired repeat lasts
    ofParameter<int>   autoSeed;
    ofParameter<bool>  autoEvolve;    // transport loop passes roll different dice
    ofParameter<void>  autoTrigOut;
    ofParameter<int>   autoSlotOut;
    // Saved with the preset (not the shared snapshot bank).
    std::array<float, SNAPSHOT_COUNT> autoWeights;
    std::array<float, AUTO_MAX_LANE_STEPS> autoLane;

    // Scheduler state. Decisions are made a little ahead of the transport and
    // sent timetagged, so each repeat starts exactly on its grid step.
    struct AutoEvent {
        uint64_t steadyTimeUs = 0;
        int slot = -1;          // -1 = release (back to dry)
        int64_t step = 0;
    };
    bool autoScheduling = false;
    uint64_t autoGeneration = 0;
    uint64_t autoFresh = 1;          // Auto Seed 0 (no seed): renewed on every start and seek
    uint64_t autoKey(uint64_t pass) const;
    uint64_t autoFrontierUs = 0;     // steps up to this instant are decided
    bool autoReleasePending = false;
    uint64_t autoReleaseUs = 0;
    bool autoOwnsGo = false;
    bool autoUsingTransport = false;
    bool autoFreeRunning = false;
    double autoFreeBeat = 0.0;
    uint64_t autoFreeLastUs = 0;
    uint64_t autoFreeGeneration = 0;
    bool suppressParamSend = false;
    std::vector<AutoEvent> autoDisplayQueue; // shown in the GUI when they play
    int64_t autoLastFiredStep = std::numeric_limits<int64_t>::min();
    int64_t autoPlayheadStep = -1;
    int autoLastFiredSlot = -1;
    std::vector<int> autoHistory;

    void updateAutoTrigger();
    void stopAutoTrigger();
    void scheduleAutoFire(uint64_t steadyTimeUs, int slot, int64_t step);
    void scheduleAutoRelease(uint64_t steadyTimeUs);
    int autoPickSlot(int64_t step, uint64_t pass) const;
    float autoStepProbability(int64_t step) const;
    bool isAutoKey(const std::string& key) const;
    void drawAutoCard();
    bool drawMultiSlider(const char* id, float* values, int count, float height,
                         int highlight, const std::function<bool(int)>& enabled,
                         const std::function<std::string(int)>& label);

    float currentBpm = 120.0f;

    int oldNumChannels = 2;
    int resetCounter = 0;
    bool resetParameterPending = false;
    int visualizationHoldFrames = 0;
    bool visOn = false;
    float waveformHeight = 280.0f;

    // Visualization state, filled by /beatRepeatProVis SendReply messages.
    std::vector<float> waveHigh;
    std::vector<float> waveLow;
    float writeHead = 0.0f;
    float readHead = 0.0f;
    float loopStart = 0.0f;
    float loopEnd = 0.0f;
    float cyclePhase = 0.0f;
    bool wetActive = false;
    bool captureActive = false;

    std::array<ofJson, SNAPSHOT_COUNT> snapshotSlots;
    std::array<bool, SNAPSHOT_COUNT> snapshotUsed{};
    std::array<std::string, SNAPSHOT_COUNT> snapshotNames{};
    std::array<std::array<char, 64>, SNAPSHOT_COUNT> snapshotNameBuffers{};
    int activeSnapshot = -1;
    bool sharedSnapshotsLoaded = false;
    bool snapshotSaveDialog = false;
    bool snapshotLoadDialog = false;
    std::string snapshotBankMessage;
    bool momentaryHoldActive = false;
    int momentarySource = -1; // -2 = Go, 0..23 = snapshot slot

    struct PublishAction {
        std::string key;
        std::function<bool()> publish;
        std::function<void(ofJson&)> save;
        std::function<void(const ofJson&)> load;
        std::function<std::string()> valueText;
        std::function<void(const std::string&)> applyText;
    };
    std::vector<PublishAction> publishActions;
    std::vector<std::string> publishedKeys;
    std::map<std::string, std::shared_ptr<ofxOceanodeAbstractParameter>> publishedHandles;
    bool publishedSeparatorAdded = false;

    template<typename T>
    bool publishParameter(const std::string& key, ofParameter<T>& parameter) {
        if(getParameterGroup().contains(parameter.getEscapedName())) return false;
        if(!publishedSeparatorAdded) {
            addSeparator("Published", ofColor(200));
            publishedSeparatorAdded = true;
        }
        publishedHandles[key] = addParameter(parameter);
        if(std::find(publishedKeys.begin(), publishedKeys.end(), key) == publishedKeys.end())
            publishedKeys.push_back(key);
        parameterGroupChanged.notify(this);
        return true;
    }

    void initializePublishActions();
    const PublishAction* findPublishAction(const std::string& key) const;
    bool isPublished(const std::string& key) const;
    bool hasPublishedConnection(const std::string& key) const;
    bool publishByKey(const std::string& key);
    bool unpublishByKey(const std::string& key);
    void syncPublished(const std::vector<std::string>& keys);
    void drawPublishMenu(const std::string& key);
    void drawValuePopup(const std::string& popupKey,
                        const std::function<std::string()>& valueText,
                        const std::function<void(const std::string&)>& applyText,
                        bool showPublishItems);
    void markPublishedItem(const std::string& key) const;

    void configureSynth(ofxSCServer* server, ofxSCSynth* synth);
    void attachFeedback(ofxSCServer* server, ofxSCSynth* synth);
    void sendAllParams(ofxSCSynth* synth, bool includeTransport = true);
    void sendAllParams();
    void requireSynthdefs();
    void recreateSynths();
    std::string synthDefName() const;
    void setVisualizationEnabled(bool enabled);

    void drawWindow();
    void drawWaveform(float width, float height);
    void drawControls(float width);
    void drawSectionHeader(const char* title);
    void drawSnapshotMatrix();
    void storeSnapshot(int slot);
    void recallSnapshot(int slot, bool forceActivate = false, bool suppressActivation = false);
    std::string sharedSnapshotBankPath() const;
    bool saveSnapshotBank(const std::string& path, bool report = true);
    bool loadSnapshotBank(const std::string& path, bool installAsShared = true, bool report = true);
    void saveSharedSnapshotBank();
    void beginMomentaryHold(int source);
    void endMomentaryHold(int source);
    ofJson captureSnapshot() const;
    void applySnapshot(const ofJson& snapshot);
    bool musicalTimeSlider(const char* label);
    std::string musicalTimeLabel(float division) const;
    void drawLoopRegion(ImDrawList* drawList, ImVec2 min, ImVec2 max,
                        float start, float end, ImU32 color) const;
    void drawGateMask(ImDrawList* drawList, ImVec2 min, ImVec2 max) const;

    bool sliderFloat(const char* label, ofParameter<float>& parameter,
                     const char* format = "%.3f");
    bool sliderInt(const char* label, ofParameter<int>& parameter);
    bool checkbox(const char* label, ofParameter<bool>& parameter);
};
