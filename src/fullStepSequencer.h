//
//  fullStepSequencer.h
//  ofxOceanodeSuperCollider
//
//  Fruity-Loops-style multitrack step sequencer with integrated file browser.
//  Phase 1: sample browser, dynamic track count, VOL and PROB parameter tabs,
//           slot system, BPM awareness, per-track stereo audio outputs.
//
//  Architecture:
//    • Inherits scNode for proper SC signal-graph integration.
//    • Pre-allocates MAX_TRACKS output ports at setup(); only numTracks are active.
//    • One "FullStepSeqTrack" synth instance per track per SC server.
//    • One mono sample buffer per track per server (via ofxSCBuffer::readChannel).
//    • Slot system: MAX_SLOTS independent snapshots of all per-track data.
//    • Floating ImGui window: left = file browser, right = scrollable track rows.
//

#pragma once

#include "ofxOceanodeNodeModel.h"
#include "ofxOceanodeShared.h"
#include "scNode.h"
#include "serverManager.h"
#include "ofxSuperCollider.h"
#include "ofxSCSynth.h"
#include "ofxSCBuffer.h"
#include "ofxSCBus.h"
#include "imgui.h"
#include <vector>
#include <string>
#include <map>
#include <filesystem>
#include <fstream>
#include <cstring>

class fullStepSequencer : public scNode {
public:
    // ── Constants ─────────────────────────────────────────────────────────────
    static constexpr int   MAX_TRACKS  = 8;
    static constexpr int   MAX_STEPS   = 64;
    static constexpr int   MAX_SLOTS   = 8;
    static constexpr float BEAT_W      = 128.0f; // pixels per beat → square steps at stepsPerBeat=4
    static constexpr float STEP_H      = 32.0f;  // step button height
    static constexpr float PARAM_H     = 64.0f;  // VOL/PROB slider height
    static constexpr float STEP_ROUND  = 4.0f;   // step button corner radius
    static constexpr float STEP_GAP    = 2.0f;   // uniform gap between every step button

    // ── Data types ────────────────────────────────────────────────────────────

    /// Per-track global configuration — identical across all slots for a given track.
    struct TrackConfig {
        std::string name          = "Track";
        int         numBeats      = 4;
        int         stepsPerBeat  = 4;

        bool        monoMode      = true;   // true=mono (restart), false=poly (8 voices)
        bool        volLatch       = true;   // true=latch per-step values at trigger

        // WAV tab: playback region and duration control
        float       inPoint        = 0.0f;
        float       outPoint       = 1.0f;
        bool        loopEnabled    = false;
        bool        fixDuration    = false;
        float       durationBeats  = 1.0f;
        int         wavGrid        = 0;     // 0=off, N>0: grid snap divisions

        // ENV tab: global amplitude envelope
        bool        envEnabled     = false;
        float       envAttack      = 0.01f;
        float       envHold        = 0.0f;
        float       envDecay       = 0.3f;
        float       envCurveA      = 0.0f;
        float       envCurveD      = 0.0f;

        // EQ tab: 3-band insert EQ (HP → Peak → LP)
        bool        eqEnabled      = false;
        float       eqHPFreq       = 80.0f;    // HP cutoff Hz
        float       eqHPQ          = 0.71f;    // HP Q (sends 1/Q to SC)
        float       eqPeakFreq     = 1000.0f;  // Peak center Hz
        float       eqPeakGain     = 0.0f;     // Peak gain dB  (-24..+24)
        float       eqPeakQ        = 1.0f;     // Peak Q (sends 1/Q to SC)
        float       eqLPFreq       = 12000.0f; // LP cutoff Hz
        float       eqLPQ          = 0.71f;    // LP Q (sends 1/Q to SC)

        // EUC tab: euclidean tool settings (not per-slot data)
        int         euclPulses     = 4;
        int         euclSteps      = 16;  // pattern length for euclidean fill (cut/repeat to fit ns)
        int         euclOffset     = 0;
        int         accentPulses   = 4;
        int         accentSteps    = 16;  // pattern length for euclidean accents (cut/repeat to fit ns)
        int         accentOffset   = 0;   // rotation offset for euclidean accent pattern
        float       accentHi       = 1.0f;
        float       accentLo       = 0.5f;
        float       accentBeatVar  = 0.05f;

        // Volume / probability / transpose (mirrors node vector params)
        float       trackPitch     = 0.0f;
        float       globalVol      = 1.0f;
        float       globalProb     = 1.0f;

        bool        muted          = false;  // per-track mute (mirrors muteP node param)
        bool        solo           = false;  // per-track solo (isolates this track)

        int getNumSteps() const { return std::min(numBeats * stepsPerBeat, MAX_STEPS); }
    };

    /// Per-track per-slot data — shift, step patterns, and per-slot FX settings.
    struct TrackData {
        int                shift     = 0;
        int                activeTab = -1;  // -1=none  0=VOL … 8=ENV

        // FX: Reverb (FreeVerb2) — per-slot
        float revRoom      = 0.7f;
        float revDamp      = 0.5f;
        // FX: Echo (delay with resonant HP/LP filter) — per-slot
        int   echoMode      = 0;      // 0=beats, 1=pitch (1/Hz)
        float echoBeats     = 1.0f;
        float echoPitchNote = 69.0f; // MIDI note → converted to Hz before sending to SC
        float echoFeedback = 0.40f;
        float echoRes      = 0.0f;
        float echoHPF      = 200.0f;
        float echoLPF      = 8000.0f;

        std::vector<bool>  stepOn;
        std::vector<float> stepVol;
        std::vector<float> stepProb;
        std::vector<float> stepPan;     // -1..1 stereo balance
        std::vector<float> stepCut;     // -1..0 LP, 0..1 HP
        std::vector<float> stepRes;     // 0..1 resonance
        std::vector<int>   stepPitch;   // -12..12 semitones per step
        std::vector<bool>  stepReverse; // true = play sample backwards for this step
        std::vector<float> stepRevSend;  // 0..1 reverb send amount per step
        std::vector<float> stepEchoSend; // 0..1 echo send amount per step

        void resizeSteps() {
            // Always grow to MAX_STEPS — never shrink.
            stepOn      .resize(MAX_STEPS, false);
            stepVol     .resize(MAX_STEPS, 1.0f);
            stepProb    .resize(MAX_STEPS, 1.0f);
            stepPan     .resize(MAX_STEPS, 0.0f);
            stepCut     .resize(MAX_STEPS, 0.0f);
            stepRes     .resize(MAX_STEPS, 0.0f);
            stepPitch   .resize(MAX_STEPS, 0);
            stepReverse .resize(MAX_STEPS, false);
            stepRevSend .resize(MAX_STEPS, 0.0f);
            stepEchoSend.resize(MAX_STEPS, 0.0f);
        }
    };

    /// One slot holds per-slot step/shift data for all tracks.
    struct SlotData {
        std::vector<TrackData> tracks; // size == numTracks at time of use
    };

    // ── Constructor / Destructor ──────────────────────────────────────────────
    fullStepSequencer(vector<serverManager*> servers);
    ~fullStepSequencer();

    // ── ofxOceanodeNodeModel overrides ────────────────────────────────────────
    void setup()              override;
    void update(ofEventArgs&) override;
    void draw(ofEventArgs&)   override;
    void setBpm(float bpm)    override;
    void activate()           override;
    void deactivate()         override;

    // ── scNode interface ──────────────────────────────────────────────────────
    void buildSynth(ofxSCServer* s)                     override;
    void createSynth(ofxSCServer* s)                    override;
    void free(ofxSCServer* s)                           override;
    void setOutputBus(ofxSCServer* s, int idx, int bus) override;
    int  getOutputBusIndex(ofxSCServer* s, int idx)     override;
    void moveSynthBefore(ofxSCServer* s, int nodeID)    override;
    int  getLastSynthID(ofxSCServer* s)                 override;
    // No audio input:
    void setInputBus(ofxSCServer*, scNode*, int)        override {}
    void resetInputBusses(ofxSCServer*, int)            override {}

    // ── Preset serialization ──────────────────────────────────────────────────
    void presetSave(ofJson& j)                         override;
    void presetRecallAfterSettingParameters(ofJson& j) override;
    void loadBeforeConnections(ofJson& j)              override;
    void macroSave(ofJson& j, string path)             override;
    void macroLoad(ofJson& j, string path)             override;

private:
    // ── Servers ───────────────────────────────────────────────────────────────
    vector<serverManager*> allServers;

    // ── BPM (pushed by framework via setBpm()) ────────────────────────────────
    float currentBpm = 120.0f;

    // ── Node-GUI parameters ───────────────────────────────────────────────────
    ofParameter<bool>          showWindow;
    ofParameter<int>           resetSeq;      // 0/1 — trigger fires on rising edge 0→1
    ofParameter<int>           numTracksP;   // 1..MAX_TRACKS — number of active tracks
    ofParameter<vector<float>> transposeP;   // per-track transpose in semitones (-24..24)
    ofParameter<vector<float>> globalVolP;   // per-track output volume (0..1)
    ofParameter<vector<float>> globalProbP;  // per-track probability multiplier (0..1)
    ofParameter<vector<int>>   muteP;        // per-track mute state: 0=unmuted, 1=muted
    ofParameter<float>         swingP;       // global swing for all tracks (0=straight, 0.5=max)
    ofParameter<int>           currentSlotP; // 0..MAX_SLOTS-1
    ofParameter<bool>          embedInProject;
    ofParameter<int>           gateOut;      // 1 when current step gate is open

    // ── Multitrack ────────────────────────────────────────────────────────────
    int numTracks = 1;  // current active track count (driven by numTracksP)
    void setNumTracks(int n);  // handles all add/remove logic for tracks

    // ── Track config (global per-track, shared across all slots) ─────────────
    std::vector<TrackConfig> trackConfigs;  // size MAX_TRACKS

    TrackConfig&       trackConfig(int ti);
    const TrackConfig& trackConfig(int ti) const;

    // ── Slot / track data (per-slot: shift + step patterns) ──────────────────
    std::vector<SlotData> slots;           // fixed size MAX_SLOTS

    TrackData&       track(int ti);
    const TrackData& track(int ti) const;
    void initSlots();                      // allocate slots with 1 track each
    void reloadCurrentSlot();             // update step/timing params on running synth (no sample reload)

    // ── SC synth resources ────────────────────────────────────────────────────
    // trackSynths[server][ti] → synth (nullptr = not created)
    std::map<ofxSCServer*, std::vector<ofxSCSynth*>> trackSynths;
    // outputBuses[server][ti] → audio bus index assigned by graph manager
    std::map<ofxSCServer*, std::map<int, int>>       outputBuses;

    // ── Audio bus resources ───────────────────────────────────────────────────
    // privateBuses[server][ti]: internal audio bus used as default out (freed when
    //   setOutputBus() is called with a real graph-allocated bus)
    std::map<ofxSCServer*, std::vector<ofxSCBus*>> privateBuses;
    // stepBuses[server][ti]: 1-channel KR bus; synth writes current step index
    std::map<ofxSCServer*, std::vector<ofxSCBus*>> stepBuses;
    // gateBuses[server][ti]: 1-channel KR bus; synth writes current gate (0 or 1)
    std::map<ofxSCServer*, std::vector<ofxSCBus*>> gateBuses;
    // currentStep[ti]: last step index read back from SC (for playhead drawing)
    std::vector<int> currentStep;

    // ── Sample buffer resources ───────────────────────────────────────────────
    // Each track has its own sample (not per-slot — same sample across all slots for that track).
    // trackBufs[ti][server] → the sample buffer for track ti.
    std::vector<std::map<ofxSCServer*, ofxSCBuffer*>> trackBufs; // size MAX_TRACKS
    std::vector<std::string> samplePaths;  // per-track sample path (not per-slot)

    int  getBufnum(int ti, ofxSCServer* srv) const;
    void loadSampleForTrack(int ti, const std::string& path);
    void freeSampleForTrack(int ti);
    void freeAllSamples();

    // ── Synth helpers ─────────────────────────────────────────────────────────
    void createTrackSynth(ofxSCServer* srv, int ti);
    void freeServerSynths(ofxSCServer* srv);
    void sendStepData(ofxSCSynth* s, const TrackData& td);
    void sendStepDataDirect(ofxSCSynth* s, const TrackData& td, ofxSCServer* srv);
    void sendStepDataToAll(int ti);
    void fireStepParams(int ti);
    void sendBpmToAll();
    void updateActiveStates(); // recompute SC 'active' for all tracks (mute + solo)

    // ── File browser ──────────────────────────────────────────────────────────
    struct BrowseEntry { bool isDir; std::string name, fullPath; };
    std::string              browseDir;
    std::vector<BrowseEntry> browseEntries;

    void refreshBrowse(const std::string& dir);

    // ── Preview playback ──────────────────────────────────────────────────────
    ofxSCServer* previewServer = nullptr;
    ofxSCBuffer* previewBuf    = nullptr;
    ofxSCSynth*  previewSynth  = nullptr;

    void triggerPreview(const std::string& path);
    void stopPreview();

    // ── ImGui window rendering ────────────────────────────────────────────────
    void drawSequencerWindow();
    void drawBrowser(float w, float h);
    void drawTrack(int ti);

    // ImGui InputText edit buffers (rebuilt from TrackData names, not serialized)
    char nameEditBuf[MAX_TRACKS][64];

    // ── Preset helpers ────────────────────────────────────────────────────────
    void serializeSlots(ofJson& j) const;
    void deserializeSlots(const ofJson& j);

    // ── Sample embed helpers (mirrors scBuffer pattern) ───────────────────────
    // Copy all slot samples into <folderRootAbs>/samples/ and rewrite the JSON paths.
    void        embedSamplesIntoJson(ofJson& j, const std::string& folderRootAbs);
    std::string resolveToAbsolutePath(const std::string& inputPath) const;
    std::string computeDataRelativePath(const std::string& absPath) const;

    // ── Waveform display ─────────────────────────────────────────────────────
    static constexpr int WAVEFORM_BINS = 512;
    std::vector<std::vector<float>> waveformPeaks;  // [track][bin] peak (0..1)
    void                 loadWaveformData(int ti, const std::string& path);

    // WAV tab drag state
    enum class WavDrag { None, In, Out };
    WavDrag wavDragMode = WavDrag::None;

    // ── Euclidean rhythm helper ───────────────────────────────────────────────
    /// Returns a boolean pattern of length n with k evenly-distributed pulses.
    static std::vector<bool> euclideanRhythm(int k, int n);

    // ── Internal flags ────────────────────────────────────────────────────────
    float    fxColW          = 210.0f; // FX column width (right panel)

    int      lastResetVal    = 0;   // previous value of resetSeq — detect rising edge 0→1
    int  sliderPaintTrack    = -1;   // track index owning current slider paint gesture (-1 = none)
    int  stepPaintTrack      = -1;   // track index owning current step-on paint gesture (-1 = none)
    bool stepPaintValue      = false; // value being stamped during a step-on paint gesture
    int  revPaintTrack       = -1;   // track index owning current REV tab paint gesture (-1 = none)
    bool revPaintValue       = false; // value being stamped during a REV paint gesture
    int  browserSel          = -1;   // keyboard-selected entry index in file browser (-1 = none)
    float browserW           = 220.0f; // file browser panel width (resizable)

    // ── Per-track step-data ofParameters (FM7Drone listener pattern) ─────────
    // Storing step arrays as ofParameters means any .set() call — from preset
    // load, slot change, or UI edit — fires a listener that calls synth->set(),
    // which sends /n_setn immediately when the synth is confirmed live.
    std::vector<ofParameter<vector<float>>> pStepOn;    // size MAX_TRACKS
    std::vector<ofParameter<vector<float>>> pStepVol;
    std::vector<ofParameter<vector<float>>> pStepProb;
    std::vector<ofParameter<vector<float>>> pStepPan;
    std::vector<ofParameter<vector<float>>> pStepCut;
    std::vector<ofParameter<vector<float>>> pStepRes;
    std::vector<ofParameter<vector<int>>>   pStepPitch;
    std::vector<ofParameter<vector<float>>> pStepReverse;
    std::vector<ofParameter<vector<float>>> pStepRevSend;
    std::vector<ofParameter<vector<float>>> pStepEchoSend;

    // ── Event listeners ───────────────────────────────────────────────────────
    ofEventListeners nodeListeners;
};
