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

        // AMP tab: beat-synced amplitude LFO
        bool        lfoEnabled     = false;
        float       lfoRate        = 1.0f;   // beats per LFO cycle (1=one cycle/beat)
        float       lfoDepth       = 0.5f;   // 0..1 amplitude modulation depth
        int         lfoShape       = 0;      // 0=sin, 1=square, 2=saw, 3=invsaw
        float       lfoPhase       = 0.0f;   // initial/retrigger phase 0..1
        float       lfoPulseWidth  = 0.5f;   // square wave pulse width 0..1

        // Slicer mode: sample divided into N slices (N=numSteps); each step triggers one slice
        bool        slicerMode     = false;
        bool        sliceFit       = false;  // stretch each slice to exactly one step duration
        int         sliceGrid      = 0;      // 0=off, N>0: grid snap divisions for slice boundaries
        std::vector<float> slicePoints;      // ns+1 boundary positions [0..1] (lazy-inited)

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
        float revTailLP    = 8000.0f;  // 1-pole LP cutoff on reverb tail (Hz)
        float revTailHP    = 20.0f;    // 1-pole HP cutoff on reverb tail (Hz)
        // FX: Echo (delay with resonant HP/LP filter) — per-slot
        int   echoMode      = 0;      // 0=beats, 1=pitch (1/Hz)
        float echoBeats     = 1.0f;
        float echoPitchNote = 69.0f; // MIDI note → converted to Hz before sending to SC
        float echoFeedback = 0.40f;
        float echoRes      = 0.0f;
        float echoHPF      = 200.0f;
        float echoLPF      = 8000.0f;
        // ARP: simple arpeggiation — per-slot
        bool  arpEnabled   = false;
        float arpInterval  = 7.0f;   // semitones per arp step (±24)
        int   arpModulo    = 4;      // steps before arp restarts (1..16)
        float arpGateWidth = 1.0f;   // gate width fraction (0..1); 1=full, only in mono+arp mode
        int   arpSpeedMode = 0;      // 0=divisions/beat, 1=MIDI pitch→Hz
        // STUT: multi-tap echo per step — per-slot
        bool  stuttEnabled  = false;
        int   stuttNumTaps  = 3;     // number of echo taps (1..16)
        float stuttFadeVol  = 0.7f;  // per-tap volume multiplier (0..1)
        float stuttFadeCut  = 0.0f;  // per-tap filter shift (-1..1)
        float stuttInterval = 0.0f;  // semitones per tap (-24..24)
        float stuttRes      = 0.0f;  // resonance added to filter on stutter taps (0..1)

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
        std::vector<bool>  stepArp;      // true = arp enabled for this step
        std::vector<float> stepArpSpeed; // arp retrigger speed per step (divisions/beat)
        std::vector<bool>  stepStut;     // true = stutter echo enabled for this step
        std::vector<float> stepStutSpeed;// stutter retrigger speed per step (divisions/beat)
        std::vector<int>   stepSlice;    // which slice index plays at each step (slicer mode)
        std::vector<bool>  stepSliceOn;  // per-step silence flag for slicer mode (true=play, false=silence)

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
            stepArp      .resize(MAX_STEPS, false);
            stepArpSpeed .resize(MAX_STEPS, 4.0f);
            stepStut     .resize(MAX_STEPS, false);
            stepStutSpeed.resize(MAX_STEPS, 4.0f);
            stepSlice    .resize(MAX_STEPS, 0);
            for(int i = 0; i < MAX_STEPS; i++) stepSlice[i] = i;
            stepSliceOn  .resize(MAX_STEPS, true);
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
    ofParameter<vector<int>>   soloP;        // per-track solo state: 0=off, 1=soloed
    ofParameter<float>         swingP;       // global swing for all tracks (0=straight, 0.5=max)
    ofParameter<float>         globalTransposeP; // global semitone offset added to all track transposes
    ofParameter<int>           currentSlotP; // 0..MAX_SLOTS-1
    ofParameter<bool>          embedInProject;
    ofParameter<vector<int>>   gateOut;      // per-track gate state (0 or 1)

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
    // mixBus[server]: single stereo audio bus that ALL track synths also write to.
    // Exposed as the "Mix" output port; the OS graph system sums all writers per cycle.
    std::map<ofxSCServer*, ofxSCBus*> mixBuses;
    ofParameter<nodePort> mixOutParam;
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
    // 8192 bins gives ~1px resolution per bin at 16x zoom with a ~512px display.
    static constexpr int WAVEFORM_BINS = 8192;
    std::vector<std::vector<float>> waveformPeaks;  // [track][bin] peak (0..1)
    void                 loadWaveformData(int ti, const std::string& path);

    // Per-track waveform zoom (1..64) and scroll (0..1) — shared by WAV and SLICE tabs.
    float waveZoom  [MAX_TRACKS];   // 1 = full sample visible; 64 = 1/64 visible
    float waveScroll[MAX_TRACKS];   // 0 = left edge, 1 = right edge

    // WAV tab drag state
    enum class WavDrag { None, In, Out };
    WavDrag wavDragMode = WavDrag::None;

    // SLICE tab drag state (interior slice boundary drag)
    int  sliceDragTrack = -1;
    int  sliceDragIdx   = 0;   // 0=none, ≥1=interior boundary k, -2=inPoint, -3=outPoint

    // Slicer matrix paint state (drag assigns the same slice row across columns)

    // ── Slicer helpers ────────────────────────────────────────────────────────
    /// Initialize (or reinitialize) slicePoints to uniform spacing between inPoint..outPoint.
    void initSlicePoints(int ti);
    /// Compute per-step slice start/end arrays from slicePoints + stepSlice assignment.
    void computeSliceArrays(int ti, std::vector<float>& starts, std::vector<float>& ends) const;

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
    int  arpPaintTrack       = -1;   // track index owning current ARP step paint gesture (-1 = none)
    bool arpPaintValue       = false; // value being stamped during an ARP step paint gesture
    int  stuttPaintTrack     = -1;   // track index owning current STUT step paint gesture (-1 = none)
    bool stuttPaintValue     = false; // value being stamped during a STUT step paint gesture
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
    std::vector<ofParameter<vector<float>>> pStepArp;
    std::vector<ofParameter<vector<float>>> pStepArpSpeed;
    std::vector<ofParameter<vector<float>>> pStepStut;
    std::vector<ofParameter<vector<float>>> pStepStutSpeed;
    std::vector<ofParameter<vector<float>>> pStepSliceStart; // [MAX_TRACKS] slice start (0..1) per step
    std::vector<ofParameter<vector<float>>> pStepSliceEnd;   // [MAX_TRACKS] slice end   (0..1) per step
    std::vector<ofParameter<vector<float>>> pStepSliceOn;    // [MAX_TRACKS] per-step silence flag (1=play)

    // ── Event listeners ───────────────────────────────────────────────────────
    ofEventListeners nodeListeners;
};
