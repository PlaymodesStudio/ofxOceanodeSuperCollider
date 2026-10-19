//
//  scSlicerBox.h
//  ofxOceanodeSuperCollider
//
//  SlicerBox: cut one audio file into slices and play each slice from its own
//  lane of a Gate vector, like RhythmBox's slicer as a node of its own.
//
//  • Slicing: a regular grid (N equal slices) or the file's transients
//    (spectral-flux onsets with an adaptive threshold, Sensitivity and a
//    minimum slice length, each onset pulled back to the start of its
//    attack), inside a Start..End region. Markers can then be dragged,
//    added (double-click) or deleted (right-click).
//  • Playing: lane i of Gate plays slice i. Gate takes Oceanode vectors (int
//    or float) or an audio cable, read sample accurately (the _sel / _ar
//    switch of OceanodeParameter.ar). Play Mode (all slices): One-shot
//    (plays to its end on the rising edge) or Gated (stops when its gate
//    falls); Pitch (all slices, resampled, in semitones); Reverse per
//    slice. A rising edge restarts its slice.
//  • Sound: one synth plays every slice (SlicerBoxPlayer<S>_<C>, S = 16 / 32 /
//    64 slices, C = the file's 1 or 2 channels, synthdefs/Defaults/SlicerBox),
//    stereo out, then the EQ GrainBox uses (graphiceq2).
//  • GUI: the node shows Show, Gate and whatever is published. The dockable
//    window has the waveform with the slices and a playhead per playing
//    slice, the slice pads (hold one to hear it), the selected slice's
//    settings, the slicing settings and the EQ curve. Right-click a control
//    there to publish it to the node (or type a value / vector).
//  • Files: from the window's sample browser (double-click, or drag onto
//    the waveform), Load..., or a Finder drop; anything macOS can decode (WAV, AIFF, MP3, M4A, CAF...); a file
//    scsynth cannot read itself goes to it as a temporary WAV.
//

#pragma once

#include "scNode.h"
#include "scTimelineMenu.h"
#include "scSnapshots.h"
#include "scDropdowns.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "ofxSCBuffer.h"
#include "scEQEditor.h"
#include "scSampleBrowser.h"
#include "imgui.h"
#include <array>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

class serverManager;

class scSlicerBox : public scNode {
public:
    // its window (SC Mixer Pro's "Go to origin synth window" opens and focuses it)
    std::string getWindowTitle() override { return (canvasID == "Canvas" ? "" : canvasID + "/") + "SlicerBox " + ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return &showWindow; }
private:
public:
    static constexpr int MAX_SLICES = 64;
    static constexpr int OVERVIEW_BINS = 4096;     // waveform peaks of the whole file

    enum SliceMode { SliceGrid, SliceTransients, NumSliceModes };
    enum GateMode { GateOneShot, GateGated, NumGateModes };
    // The effects after the player, in this order. Filter is SlicerBox's own;
    // Comp / Sat / Echo / Reverb are MixerPro's inserts (mixerPro<Stage>2).
    enum Stage { StEq, StFilter, StComp, StSat, StEcho, StRvb, NumStages };

    explicit scSlicerBox(std::vector<serverManager*> servers);
    ~scSlicerBox();

    void setup() override;
    void update(ofEventArgs&) override;
    void draw(ofEventArgs&) override;
    void activate() override;
    void deactivate() override;

    void buildSynth(ofxSCServer* server) override;
    void createSynth(ofxSCServer* server) override;
    void free(ofxSCServer* server) override;
    void setOutputBus(ofxSCServer* server, int index, int bus) override;
    int  getOutputBusIndex(ofxSCServer* server, int index) override;
    void moveSynthBefore(ofxSCServer* server, int nodeID) override;
    int  getLastSynthID(ofxSCServer* server) override;
    void setInputBus(ofxSCServer* server, scNode* node, int bus) override;
    void resetInputBusses(ofxSCServer* server, int targetBus) override;
    void resendParametersForNRT() override;

    void setBpm(float bpm) override;
    void presetSave(ofJson& json) override;
    void loadBeforeConnections(ofJson& json) override;
    void presetRecallBeforeSettingParameters(ofJson& json) override;
    void presetRecallAfterSettingParameters(ofJson& json) override;

    // The audio of a file, decoded on a worker thread
    struct Audio {
        std::string path;           // as chosen
        std::string playPath;       // what scsynth reads (the file, or a temporary WAV)
        std::vector<float> mono;    // mixdown, for the waveform and the transients
        int channels = 0;           // of the played file: 1 or 2
        int frames = 0;
        float sampleRate = 44100;
        std::vector<float> flux;    // spectral flux per hop (onset strength), 0..1
        int fluxHop = 256, fluxSize = 1024;
        std::string error;
    };
    // analyse = false skips the spectral flux (no transients wanted)
    static Audio decodeFile(std::string path, std::string tmpDir, bool analyse = true);
    // Onsets (in frames) inside from..to: peaks of the flux over an adaptive
    // threshold set by sensitivity 0..1, at least minFrames apart (the
    // stronger kept), each pulled back to the start of its attack
    static std::vector<int> detectOnsets(const Audio& audio, float sensitivity, int minFrames, int from, int to);

private:
    // ── Slices ───────────────────────────────────────────────────────────────
    int numSlices() const { return std::max(0, (int)points.size() - 1); }
    void reslice();                                   // from the slicing settings
    void sliceBounds(int slice, int& startFrame, int& endFrame) const;
    float sliceValue(const std::vector<float>& values, int slice, float fallback) const;
    void setSliceValue(ofParameter<std::vector<float>>& p, int slice, float value);
    int sliceCap() const;

    // ── Synths ───────────────────────────────────────────────────────────────
    struct ServerState {
        ofxSCBuffer* buffer = nullptr;
        std::string bufferPath;
        int bufferChannels = 0;
        ofxSCSynth* player = nullptr;
        // Each stage: created the first time it is switched on; switched off,
        // it fades (Echo / Reverb: its tail rings out), then it is paused and
        // the chain goes around it. `in` is the stage's input bus.
        struct Fx { ofxSCSynth* synth = nullptr; ofxSCBus* in = nullptr; bool running = false; float offAt = -1.0f; };
        std::array<Fx, NumStages> fx;
        // The step sequencer (SlicerBoxSeq, before the player, while Sequence
        // is on): its gates (64 audio channels) and its position (control)
        ofxSCSynth* seq = nullptr;
        ofxSCBus* seqBus = nullptr;
        ofxSCBus* seqPos = nullptr;
        ofxSCBus* vis = nullptr;       // playheads (control, MAX_SLICES channels)
        int outBus = -1;
        int gateBus = -1;
        int resetBus = -1;             // audio cable plugged into Reset, -1 if none              // audio cable plugged into Gate, -1 if none
        int builtCap = 0, builtChannels = 0;
        bool chainCreated = false;     // the graph created this server's chain
    };
    std::string playerDefName() const;
    void ensureBuffer(ofxSCServer* server, ServerState& st);
    void rebuildChain(ofxSCServer* server, ServerState& st);
    void freeChain(ServerState& st);
    void sendSlices(ofxSCSynth* synth, bool force);
    void sendSlicesAll();
    void sendGate(ofxSCSynth* synth);
    void sendGateRouting(ofxSCSynth* synth, const ServerState& st);
    void sendEq(ofxSCSynth* synth);
    void sendEqAll();
    void sendAmp(ofxSCSynth* synth);
    bool stageOn(int stage) const;
    static const char* stageDef(int stage);
    float stageTail(int stage) const;
    float echoDelaySec() const;
    void sendStage(int stage, ofxSCSynth* synth);
    void sendStageAll(int stage);
    void updateStages(ofxSCServer* server, ServerState& st);
    void routeChain(ServerState& st);
    void ensureSeq(ofxSCServer* server, ServerState& st);
    void sendSeq(ofxSCSynth* synth, bool notes = true);
    int seqPerBeat() const;
    void audition(int slice, bool on);

    // ── Publishing window controls into the node ─────────────────────────────
    struct PublishAction {
        std::string key;
        bool isVector = false;
        std::vector<std::string> dropdown;
        std::function<std::shared_ptr<ofxOceanodeAbstractParameter>()> publish;
        std::function<void(ofJson&)> save;
        std::function<void(const ofJson&)> load;
        std::function<std::string()> valueText;
        std::function<void(const std::string&)> applyText;
    };
    template<typename T>
    void addPublishAction(ofParameter<T>& parameter, std::vector<std::string> dropdown = {});
    void initializePublishActions();
    const PublishAction* findPublishAction(const std::string& key) const;
    bool isPublished(const std::string& key) const;
    bool hasPublishedConnection(const std::string& key) const;
    bool publishByKey(const std::string& key);
    bool unpublishByKey(const std::string& key);
    void syncPublished(const std::vector<std::string>& keys);
    void controlMenu(const std::string& key);
    void controlMenuItems(const std::string& key, const char* title = nullptr);

    // ── Window ───────────────────────────────────────────────────────────────
    void drawWindow();
    void drawHeader();
    void drawWaveform(ImVec2 size);
    void drawPads(float width);
    void drawSliceSettings();
    void drawSlicing();
    void drawEq(float width, float height);
    void drawAmp(float w);
    void drawFilterEq(float w);
    void drawDynamics(float w);
    void drawSpace(float w);
    void drawSequence(float w);
    void sectionHeader(const char* title, float w, ofParameter<bool>* on = nullptr);
    bool sliderFloat(ofParameter<float>& p, const char* format = "%.3f", bool logarithmic = false,
                     float width = -FLT_MIN);   // -FLT_MIN: the whole row
    bool sliderInt(ofParameter<int>& p, float width = -FLT_MIN);
    bool comboInt(ofParameter<int>& p, const std::vector<std::string>& options, float width = -FLT_MIN);
    bool checkbox(ofParameter<bool>& p);
    ImU32 sliceColor(int slice, float alpha) const;
    float frameToX(double frame, float x0, float width) const;
    double xToFrame(float x, float x0, float width) const;

    // ── Parameters ───────────────────────────────────────────────────────────
    // On the node
    ofParameter<bool> showWindow;
    ofParameter<std::vector<float>> gate;
    ofParameter<void> seqReset;          // restarts the sequence (or an audio cable: its rising edges)
    // Window (publishable)
    ofParameter<float> volume, pitch;
    ofParameter<int> sliceMode, gridSlices;
    ofParameter<float> sensitivity, minLength, regionStart, regionEnd;
    ofParameter<int> playMode;                       // GateOneShot / GateGated, every slice
    ofParameter<std::vector<float>> sliceReverse;    // per slice (one value: all)
    ofParameter<bool> eqOn;
    ofParameter<float> eqGain[5], eqFreq[5], eqShape[5], eqMix;
    // Amplitude envelope (every slice)
    ofParameter<float> ampAttack, ampDecay, ampSustain, ampRelease;
    ofParameter<float> ampCurveA, ampCurveD, ampCurveR;   // segment tension (SC curve values)
    // Filter
    ofParameter<bool> filterOn;
    ofParameter<int> filterType, filterSlope;
    ofParameter<float> filterCutoff, filterResonance;
    // Compressor, Saturator, Echo, Reverb: MixerPro's controls and ranges
    ofParameter<bool> compOn, compAuto;
    ofParameter<float> compThreshold, compRatio, compKnee, compAttack, compRelease, compMakeup, compRms, compHpf, compMix;
    ofParameter<bool> satOn, satAuto;
    ofParameter<float> satDrive, satWarmth, satTone, satOutput, satMix;
    ofParameter<bool> echoOn, echoBeats;
    ofParameter<int> echoFilter;
    ofParameter<float> echoDelay, echoBeatVal, echoFeed, echoCutoff, echoResonance, echoPingPong, echoMix;
    ofParameter<bool> rvbOn;
    ofParameter<float> rvbMix, rvbDecay, rvbSize, rvbPredelay, rvbLate, rvbSpread, rvbLowpass,
                       rvbHiDamp, rvbLoDamp, rvbModRate, rvbSpin, rvbWander;
    float currentBpm = 120.0f;
    // Step sequencer: step j plays slice seqSlice[j] (-1: nothing) for
    // seqLen[j] steps
    static constexpr int SEQ_STEPS = 64;
    ofParameter<bool> seqOn;
    ofParameter<int> seqSteps, seqDivision;
    ofParameter<float> seqGate;
    ofParameter<float> seqRandom;       // each pass, a step plays a random slice with this chance
    std::vector<float> seqPlaying;      // the slice each step plays now (read back, -1 rest)
    int sentSlices = -1;
    std::vector<float> seqSlice = std::vector<float>(SEQ_STEPS, -1.0f);
    std::vector<float> seqLen = std::vector<float>(SEQ_STEPS, 1.0f);
    bool seqDirty = false;
    float seqPosition = -1.0f;          // steps, read back for the window (-1 stopped)
    float nextSeqRead = 0.0f;
    int seqDragStep = -1, seqDragMode = 0;   // 1 resize, 2 move
    float seqDragGrab = 0.0f;

    // ── State ────────────────────────────────────────────────────────────────
    std::vector<serverManager*> servers;
    std::map<ofxSCServer*, ServerState> states;
    std::shared_ptr<nodePort> gateInput = std::make_shared<nodePort>();   // audio into Gate
    std::shared_ptr<nodePort> resetInput = std::make_shared<nodePort>();  // audio into Reset
    int silentBus = -1;

    Audio audio;                         // the file playing
    std::future<Audio> decodeTask;
    std::string pendingPath;             // decoding
    std::string loadError;
    std::vector<float> overview;         // OVERVIEW_BINS peaks
    std::vector<float> points;           // slice boundaries, 0..1 of the file, ascending
    bool pointsFromPreset = false;       // keep them when the file arrives
    bool slicesDirty = true;             // resend start / end / rate / rev / mode
    std::vector<float> lastGate;         // for the window's lane lights

    // Window state
    int selectedSlice = 0;
    int auditioning = -1;
    bool editAll = false;
    float viewStart = 0.0f, viewEnd = 1.0f;     // zoom, 0..1 of the file
    int dragMarker = -1;                        // interior boundary being dragged
    bool windowHovered = false;                 // for file drops
    bool pendingLoadDialog = false;
    std::string pendingDroppedFile;
    scEQEditor eqEditor;
    // The shared sample browser (RhythmBox / GrainBox / SampleBox): double-click
    // a file or drag it onto the waveform to load it
    scSampleBrowser browser{"scSlicerBox", {".wav", ".aif", ".aiff", ".flac"}};
    float browserWidth = 220.0f;
    float waveHeight = 0.0f;            // screen pixels, 0 = the default; dragged by its grip
    void drawWaveformGrip(float w);
    bool eqCurveDirty = true;
    std::vector<float> playheads;               // progress of each slice, 0 = silent
    float nextPlayheadRead = 0.0f;

    std::vector<PublishAction> publishActions;
    std::vector<std::string> publishedKeys;
    scTimelineMenu timeline;              // "Add to Timeline" from the window
    std::map<std::string, std::shared_ptr<ofxOceanodeAbstractParameter>> publishedHandles;
    scSnapshots snaps;                    // the shared snapshots: library, morph, presets
    ofJson initial;                       // the sound at setup (the factory's base)
    ofJson captureSound() const;
    void applySound(const ofJson& a, const ofJson& b, float t, bool sw);
    std::vector<ofJson> factorySounds() const;
    bool publishedSeparatorAdded = false;
    ofEventListeners listeners;
    ofEventListener dragListener;
};
