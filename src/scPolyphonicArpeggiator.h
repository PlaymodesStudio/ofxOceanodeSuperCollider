#ifndef scPolyphonicArpeggiator_h
#define scPolyphonicArpeggiator_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"
#include "ofxSCSynth.h"
#include "imgui.h"
#include <random>
#include <algorithm>

// Snapshot state — identical to polyphonicArpeggiator
struct ScArpeggiatorSnapshot {
    int seqSize;

    vector<float> scale;
    int patternMode;
    vector<int> idxPattern;
    int degStart;
    int stepInterval;
    int transpose;

    int polyphony;
    int polyInterval;
    int skipSteps;
    float strum;
    float strumRndm;
    int strumDir;

    float octaveDev;
    int octaveDevRng;
    float idxDev;
    int idxDevRng;
    float pitchDev;
    int pitchDevRng;

    float velBase;
    float velRndm;
    float eucAccStrength;

    int durBase;
    int durRndm;
    int durEucStrength;

    int eucLen;
    int eucHits;
    int eucOff;
    int eucAccLen;
    int eucAccHits;
    int eucAccOff;
    int eucDurLen;
    int eucDurHits;
    int eucDurOff;

    float stepChance;
    float noteChance;

    bool hasData = false;
};

class scPolyphonicArpeggiator : public scNode {
public:
    scPolyphonicArpeggiator();
    ~scPolyphonicArpeggiator();

    void setup() override;
    void update(ofEventArgs &e) override;

    // scNode overrides — full canonical graph-optimization interface
    void buildSynth(ofxSCServer* server) override;
    void createSynth(ofxSCServer* server) override;
    void moveSynthBefore(ofxSCServer* server, int nodeID) override;
    void free(ofxSCServer* server) override;
    void setOutputBus(ofxSCServer* server, int index, int bus) override;
    void setInputBus(ofxSCServer* server, scNode* node, int bus) override;
    void resetInputBusses(ofxSCServer* server, int targetBus = 0) override;
    int  getOutputBusIndex(ofxSCServer* server, int index) override;
    int  getLastSynthID(ofxSCServer* server) override;
    void activate() override;
    void deactivate() override;

    ofEvent<void> resendParams;

    void presetSave(ofJson &json) override;
    void presetRecallAfterSettingParameters(ofJson &json) override;

private:
    // ── SC synth management ──
    std::map<ofxSCServer*, ofxSCSynth*> synthInstances;
    std::map<ofxSCServer*, std::map<int, int>> outputBuses;
    std::map<ofxSCServer*, std::map<scNode*, int>> inputBuses;

    // ── Parameters: Trigger & Control ──
    ofParameter<void> reset;
    ofParameter<int> eucLen;
    ofParameter<int> eucHits;
    ofParameter<int> eucOff;
    ofParameter<float> stepChance;
    ofParameter<float> noteChance;

    // ── Parameters: Pitch ──
    ofParameter<vector<float>> scale;
    ofParameter<int> patternMode;
    ofParameter<vector<int>> idxPattern;
    ofParameter<int> seqSize;
    ofParameter<int> degStart;
    ofParameter<int> stepInterval;
    ofParameter<int> transpose;

    // ── Parameters: Polyphony ──
    ofParameter<int> polyphony;
    ofParameter<int> polyInterval;
    ofParameter<int> skipSteps;
    ofParameter<float> strum;
    ofParameter<float> strumRndm;
    ofParameter<int> strumDir;

    // ── Parameters: Deviation ──
    ofParameter<float> octaveDev;
    ofParameter<int> octaveDevRng;
    ofParameter<float> idxDev;
    ofParameter<int> idxDevRng;
    ofParameter<float> pitchDev;
    ofParameter<int> pitchDevRng;

    // ── Parameters: Velocity ──
    ofParameter<float> velBase;
    ofParameter<float> velRndm;
    ofParameter<int> eucAccLen;
    ofParameter<int> eucAccHits;
    ofParameter<int> eucAccOff;
    ofParameter<float> eucAccStrength;

    // ── Parameters: Duration ──
    ofParameter<int> durBase;
    ofParameter<int> durRndm;
    ofParameter<int> eucDurLen;
    ofParameter<int> eucDurHits;
    ofParameter<int> eucDurOff;
    ofParameter<int> durEucStrength;

    // ── Parameters: SC Scalar ──
    ofParameter<int> seed;

    // ── Visualization outputs (C++-side, not audio) ──
    ofParameter<vector<float>> pitchOut;       // full seqSize sequence
    ofParameter<vector<float>> activePitchOut; // polyphony-sized active voices

    // ── GUI Parameters ──
    ofParameter<float> guiWidth;
    ofParameter<float> patternHeight;
    ofParameter<float> euclideanHeight;

    // ── Custom GUI Regions ──
    customGuiRegion uiPattern;
    customGuiRegion uiEuclidean;
    customGuiRegion uiSnapshots;

    // ── Snapshot ──
    ofParameter<float> morphTime;
    vector<ScArpeggiatorSnapshot> snapshotSlots;
    int activeSnapshotSlot;
    bool isMorphing;
    float morphStartTime;
    ScArpeggiatorSnapshot startSnapshot;
    ScArpeggiatorSnapshot targetSnapshot;

    // ── Internal state: sequence data ──
    vector<float> expandedScale;
    vector<float> currentPitches;     // seqSize floats, sent as \pitchvalues to SC
    vector<bool>  euclideanPattern;   // gate pattern
    vector<bool>  euclideanAccents;   // accent pattern
    vector<bool>  euclideanDurations; // duration pattern
    vector<float> deviationValues;

    // ── Random ──
    std::mt19937 rng;
    std::uniform_real_distribution<float> dist01;

    // ── Helpers ──
    void generateEuclideanPattern(vector<bool>& pattern, int length, int hits, int offset);
    void rebuildExpandedScale();
    float getScaleDegree(int index);
    void rebuildDeviations();
    void rebuildPitchSequence();
    void uploadArraysToSynths();
    void uploadScalarParamsToSynths();
    string getSynthDefName() const;

    // ── GUI ──
    void drawPatternDisplay();
    void drawEuclideanDisplay();
    void drawSnapshotSlots();

    // ── Snapshot ──
    void storeToSlot(int slot);
    void recallSlot(int slot);
    void updateMorph();
    void saveSnapshotToDisk(int slot);
    void loadSnapshotFromDisk(int slot);
    void loadAllSnapshotsFromDisk();
    void deleteSnapshotFromDisk(int slot);
    string getSnapshotsFolderPath();
    string getSnapshotFilePath(int slot);

    ofEventListeners listeners;

    static constexpr int MAX_SEQUENCE_SIZE = 64; // matches SynthDef maxSteps
    static constexpr int MAX_POLYPHONY = 16;
};

#endif /* scPolyphonicArpeggiator_h */
