//
//  scGrainBox.h
//  ofxOceanodeSuperCollider
//
//  Multichannel granular sampler with interactive waveform/envelope GUI.
//
//  Architecture:
//    • Inherits scNode — pure audio source (no In port).
//    • Playheads: tab per playhead in the window; each has a complete set of
//      grain/trigger/jitter/LFO/envelope/region settings and its own synth.
//    • numChannels controls both grain-voice count and PanAz speaker count.
//    • One GrainBox_K_N SynthDef per sample-channel count K (1..2) and channel
//      count N (1..16); compiled by SPECIAL_SYNTHDEFS/KR/scGrainBox.scd.
//    • Per SC server: one mono sample buffer per sample channel of every
//      loaded sample (playhead 1's sample is shared by every playhead in
//      "Shared" mode; a playhead in "Own" mode has its own), and per playhead
//      one synth + one envelope buffer.
//    • Per playhead channels/offset: its synth is GrainBox_K_Np writing to
//      output bus + offset (Channels 0 = follow N Chan).
//    • One output port (N-channel wide bus, managed by serverManager).
//
//  Custom GUI (floating ImGui window):
//    • File browser with audio preview (scSampleBrowser, shared with
//      scRhythmBox): click previews, double-click / Enter / drag onto the
//      waveform loads into the selected playhead's sample.
//    • Waveform display: shows loaded sample, per-channel grain highlights
//      (duration × amp intensity, color per channel), draggable inPoint/outPoint.
//    • Envelope editor: attack / release / shape parameters rendered as a
//      512-sample buffer uploaded to SC via /b_setn.
//

#pragma once

#include "ofxOceanodeNodeModel.h"
#include "ofxOceanodeShared.h"
#include "scNode.h"
#include "serverManager.h"
#include "ofxSuperCollider.h"
#include "ofxSCSynth.h"
#include "ofxSCBuffer.h"
#include "imgui.h"
#include "ofxOceanodeSuperColliderConfig.h"
#include "scTransportSync.h"
#include "scSampleBrowser.h"
#include "scEQEditor.h"
#include <vector>
#include <string>
#include <map>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>

class scGrainBox : public scNode {
public:
    // ── Constants ─────────────────────────────────────────────────────────────
    static constexpr int   MAX_CHANNELS        = 16;
    static constexpr int   MAX_SAMPLE_CHANNELS = 2;   // GrainBox_K_N exists for K = 1..2
    static constexpr int   ENV_BUFFER_SIZE     = 512;
    static constexpr int   WAVEFORM_BINS       = 4096;
    static constexpr int   MAX_PLAYHEADS       = 8;
    static constexpr int   NUM_LFO             = 7;   // Pos Dur Pitch Amp Pan TrRt Cut
    static constexpr int   LFO_AMP             = 3;
    static constexpr int   LFO_TRRT            = 5;
    static constexpr int   LFO_CUT             = 6;   // filtered SynthDefs only
    static const char* LFO_TARGET_NAMES[NUM_LFO];   // defined in .cpp

    // ── Constructor / Destructor ──────────────────────────────────────────────
    scGrainBox(vector<serverManager*> servers);
    ~scGrainBox();

    // ── ofxOceanodeNodeModel overrides ────────────────────────────────────────
    void setup()              override;
    void update(ofEventArgs&) override;
    void draw(ofEventArgs&)   override;
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
    // No audio inputs:
    void setInputBus(ofxSCServer*, scNode*, int)        override {}
    void resetInputBusses(ofxSCServer*, int)            override {}

    // ── Preset serialization ──────────────────────────────────────────────────
    void presetSave(ofJson& j)                          override;
    void presetRecallBeforeSettingParameters(ofJson& j) override;
    void presetRecallAfterSettingParameters(ofJson& j)  override;
    void loadBeforeConnections(ofJson& j)               override;
    void presetHasLoaded()                              override;

private:
    // ── Servers ───────────────────────────────────────────────────────────────
    vector<serverManager*> allServers;

    // ── A loaded sample: waveform data + one mono SC buffer per sample channel
    //    per server ──────────────────────────────────────────────────────────
    struct SampleData {
        std::string        path;
        std::vector<float> peaks;             // WAVEFORM_BINS peak values 0..1
        float              durationSecs = 0.0f;
        int                numChannels  = 1;  // from the WAV header, 1..MAX_SAMPLE_CHANNELS
        std::map<ofxSCServer*, std::vector<ofxSCBuffer*>> bufs;
    };
    SampleData mainSample;   // playhead 1's sample, shared by "Shared" playheads

    // ── Shared SC resources (per server) ──────────────────────────────────────
    std::map<ofxSCServer*, int>          outputBuses;
    std::map<ofxSCServer*, ofxSCBus*>    privateBuses;

    void reloadSampleBuffersForServer(SampleData& sd, ofxSCServer* srv);
    void releaseSampleBuffers(SampleData& sd, ofxSCServer* srv);
    static bool bufferIsLive(ofxSCServer* srv, ofxSCBuffer* b);   // false after a server reboot

    // ── Global node parameters ────────────────────────────────────────────────
    ofParameter<bool>          showWindow;
    ofParameter<int>           numChannelsP;   // 1..MAX_CHANNELS
    ofParameter<float>         sampleMsP;      // output: playhead 1 region length in ms

    // ── Playheads ─────────────────────────────────────────────────────────────
    // Playhead 1 (index 0) owns the original parameters with their original
    // names, so old presets load unchanged. Every extra playhead has a full,
    // independent set (names suffixed " 2", " 3"...) and its own synth per
    // server (same GrainBox_K_N def and sample buffers, own envelope buffer),
    // all writing (Out.ar adds) to the node's output bus.
    struct LfoGroup {
        ofParameter<vector<float>> shape;    // 0=sin 1=tri 2=saw 3=invSaw 4=randStep 5=randCurve
        ofParameter<vector<float>> speed;    // bar divisions (0.125..128)
        ofParameter<vector<float>> phase;    // initial phase / reset target (0..1)
        ofParameter<vector<float>> quant;    // quantization steps (0=off, 2..32)
        ofParameter<vector<float>> strength; // 0..1 (0..48 semitones for pitch, 0..2 for pan)
        ofParameter<vector<float>> pow;      // curve exponent 0.1..10 on the 0..1 shape (1 = off)
    };
    struct GrainHighlight {
        float position;   // absolute buffer position (0..1)
        float duration;   // seconds (used for lifetime and width)
        float amp;        // 0..1, used for highlight intensity
        int   channel;    // output channel (voice + the playhead's offset), 0-based
        int   playhead;   // playhead index, for color
        float lifeTime;   // remaining life in seconds
        float maxLife;    // total life = duration
    };
    struct Playhead {
        int         index = 0;
        std::string sfx;              // "" for playhead 1, " 2", " 3"...

        // Trigger
        ofParameter<vector<int>>   trigger;
        ofParameter<bool>          autoTrig;
        ofParameter<vector<float>> autoTrigBeatDiv;
        ofParameter<vector<float>> chance;
        ofParameter<bool>          uniqueTrig;
        ofParameter<int>           syncGate;
        // Grain
        ofParameter<vector<float>> amp, pitch, duration, position, panAz;
        ofParameter<bool>          dynamicDur, trigDur;
        // Jitter
        ofParameter<bool>          uniqueJit;
        ofParameter<vector<float>> posJit, pitchJit, durJit, ampJit, panAzJit;
        // Region / envelope / levels
        ofParameter<float>         inPoint, outPoint;
        ofParameter<float>         envAttack, envRelease, envTension;
        ofParameter<vector<float>> levels;
        // Output channels: Channels 0 = follow the node's N Chan; Offset = first
        // output channel (clamped so offset + channels <= N Chan)
        ofParameter<int>           channels, chanOffset;
        // Mixing (global tab): phgain = 0 when muted, or when another playhead is soloed
        ofParameter<bool>          mute, solo;
        // Filter: 0 = Off (GrainBox_K_N), 1 LPF2, 2 BPF, 3 Notch, 4 HPF2 (GrainBox_F_K_N)
        ofParameter<int>           filterType;
        ofParameter<float>         cutoff, filterQ;
        // Latch: Amp / Cut LFO (+ base + jitter) sampled at each grain's trigger
        ofParameter<bool>          latchAmp, latchCut;
        // LFOs ("Unique" = Link voices: every voice uses voice 1's value)
        ofParameter<bool>          uniqueLfo[NUM_LFO];
        LfoGroup                   lfo[NUM_LFO];

        // Sample: playhead 1 always uses mainSample; others share it unless
        // ownSample (then `own`, loaded from the browser while its tab is active)
        bool        ownSample = false;
        SampleData  own;

        // SC state
        std::map<ofxSCServer*, ofxSCSynth*>     synths;
        std::map<ofxSCServer*, ofxSCBuffer*>    envBufs;
        std::map<ofxSCServer*, ofEventListener> grainListeners;
        std::vector<float> envData;
        bool               envNeedsUpdate = false;
        float              envResendAt    = -1.0f;  // re-send once after a fresh allocation
        ofEventListeners   listeners;

        // Robust pulses: SC sees ever-increasing counters (a rising edge per
        // pulse), so a 1-then-0 pair landing in one control block is not lost.
        std::vector<int> lastTrigger;
        std::vector<int> triggerCounts;
        int   syncGateCount = 0;
        bool  syncGateReset = false;
        bool  triggerReset  = false;   // window Trig button: back to 0 next frame
#if OFXOCEANODESC_HAS_TRANSPORT
        double hardCounter = 0.0;
        bool   needHard    = true;
#endif
    };
    std::vector<std::unique_ptr<Playhead>> playheads;
    int  activePlayhead = 0;       // GUI tab
    int  pendingTabSelect = -1;    // select this tab on the next frame
    Playhead& ph0() { return *playheads[0]; }

    void createPlayheadParameters(Playhead& ph);
    void setupPlayheadListeners(Playhead& ph);
    void allocEnvBuffers(Playhead& ph);
    void ensureEnvBuffer(Playhead& ph, ofxSCServer* srv);    // alloc + data atomically
    // NRT render: the capture can only recreate the envelope buffers EMPTY
    // (their data is generated here, not read from a file), which would make
    // every grain silent. Re-send each one with its data at score time zero,
    // and push every playhead's parameters (scNode contract).
public:
    void resendParametersForNRT() override;
private:
    void freePlayheadSC(Playhead& ph);
    bool addPlayhead();                 // appends one; false at MAX_PLAYHEADS
    bool removeLastPlayhead();          // never removes playhead 1
    void setNumPlayheads(int n);
    void configureSynth(Playhead& ph, ofxSCSynth* s, ofxSCServer* srv);
    void createPlayheadSynth(Playhead& ph, ofxSCServer* srv); // runtime add
    void replaceSynths(ofxSCServer* srv);                     // def variant / bus changed
    void refreshPlayheadSynth(Playhead& ph, ofxSCServer* srv); // one playhead
    void refreshPlayheadSynths(Playhead& ph);                 // on every server
    ofxSCSynth* earliestSynth(ofxSCServer* srv, int belowIndex);
    std::vector<float> expandF(const vector<float>& v, int n) const;  // exactly n values
    std::vector<int>   expandI(const vector<int>& v, int n) const;
    std::vector<int>   triggerCountsFor(Playhead& ph);
    int  phChannels(const Playhead& ph) const;   // effective voice / speaker count
    int  phOffset(const Playhead& ph) const;     // effective first output channel
    int  outBusFor(const Playhead& ph, ofxSCServer* srv) const;
    SampleData&       sampleOf(Playhead& ph);
    const SampleData& sampleOf(const Playhead& ph) const;
    std::string defNameFor(const Playhead& ph) const;
    void setBufnums(Playhead& ph, ofxSCSynth* s, ofxSCServer* srv);
    void setOwnSample(Playhead& ph, bool own);

    // ── Waveform display ──────────────────────────────────────────────────────
    void loadWaveformData(SampleData& sd, const std::string& path);

    float waveZoom   = 1.0f;   // 1=full sample visible; 64=1/64th
    float waveScroll = 0.0f;   // 0=left edge, 1=right edge
    float waveH      = 0.0f;   // waveform panel height in screen px (0=init on first draw)

    // ── Envelope buffer ───────────────────────────────────────────────────────
    void computeEnvData(Playhead& ph);
    void uploadEnvBuffers(Playhead& ph);

    // ── Grain highlight system ────────────────────────────────────────────────
    std::deque<GrainHighlight> grainHighlights;

    // ── Global controls (global tab; publishable, not in the node by default) ─
    ofParameter<float> transposeP, volumeP, speedP;
    // Global offsets added (in C++, unclipped: the SynthDef clips pos / dur
    // after jitter + LFO; chance is a threshold) to every playhead's values
    ofParameter<float> gPosP, gDurP, gChanceP;
    std::vector<float> withOffset(const vector<float>& v, int n, float off) const;
    void sendOffsetParams(Playhead& ph);
    void sendMixParams();                          // transpose/volume/speed/phgain → every synth
    float phGainFor(const Playhead& ph) const;     // mute / solo
    void  setMixArgs(Playhead& ph, ofxSCSynth* s);
    void  setFilterArgs(Playhead& ph, ofxSCSynth* s);
    static bool synthIsFiltered(ofxSCSynth* s);

    // ── Global FX chain: EQ → Echo → Reverb (the user's SynthDefs) ────────────
    // All off: playheads write straight to the output bus (no extra synths).
    // Otherwise playheads write to a private bus and the enabled stages chain
    // private buses → output bus, right after playhead 1's synth.
    enum FxStage { FX_EQ = 0, FX_ECHO = 1, FX_REVERB = 2, FX_COUNT = 3 };
    ofParameter<bool>  fxOnP[FX_COUNT];
    ofParameter<float> eqGainP[5], eqMixP;
    ofParameter<float> eqFreqP[5];    // Hz (graphiceqN defaults 80 / 250 / 1k / 4k / 12k)
    ofParameter<float> eqShapeP[5];   // slope for bands 1 and 5 (shelves), Q for 2..4
    scEQEditor eqEditor;              // curve editor shared with scGraphicEQ
    bool       eqCurveDirty = true;
    void       drawEqSection(float w, float h);
    ofParameter<float> echoDelayP, echoFeedP, echoCutoffP, echoMixP;
    ofParameter<float> revSizeP, revDecayP, revPredelayP, revLowpassP, revMixP;
    struct FxState {
        std::vector<int>       plan;                 // enabled + loaded stages, in order
        ofxSCSynth*            synths[FX_COUNT] = {nullptr, nullptr, nullptr};
        std::vector<ofxSCBus*> buses;                // input bus of each planned stage
    };
    std::map<ofxSCServer*, FxState> fx;
    std::map<ofxSCServer*, float>   fxDefsRequestedAt;   // /d_loadDir sent (s since start)
    bool fxPending = false;                              // a stage waits for its defs
    std::string fxDefName(int stage) const;
    bool fxStageReady(int stage, ofxSCServer* srv) const;
    void requestFxDefs(ofxSCServer* srv, bool force);
    std::vector<int> computeFxPlan(ofxSCServer* srv) const;
    void planFx(ofxSCServer* srv);                       // plan + buses (no synths)
    void createFxSynths(ofxSCServer* srv);               // after playhead 1's synth
    void freeFxSynths(ofxSCServer* srv);
    void freeFx(ofxSCServer* srv);                       // synths + buses + plan
    void rebuildFx(ofxSCServer* srv);                    // runtime: re-plan, re-route, re-create
    void rebuildFxAll();
    void sendFxParams(int stage, ofxSCSynth* s);
    void sendFxParamsAll();
    int  fxOutBus(ofxSCServer* srv, int planPos) const;
    std::vector<std::string> fxParamKeys;                // for the publish registry

    // ── File browser + audio preview (shared with scRhythmBox) ────────────────
    scSampleBrowser browser{"scGrainBox", {".wav", ".aif", ".aiff", ".flac"}};
    float           browserW = 220.0f;   // unscaled (x zoom)

    // ── GUI ───────────────────────────────────────────────────────────────────
    void drawGrainBoxWindow();
    void drawBrowser(float w, float h);
    void drawPlayheadTabs();
    bool globalTab = false;           // the Global tab is selected
    bool pendingGlobalSelect = false;
    bool tabsInitialized = false;
    void drawGlobalPanel(float w);
    void drawGlobalWaveform(ImDrawList* dl, ImVec2 pos, float w, float h);
    void drawFxPanel(float w);
    void drawPlayheadHeader(Playhead& ph, float w);   // sample mode + channels row
    void drawWaveformPanel(ImDrawList* dl, ImVec2 pos, float w, float h);
    void drawEnvelopePanel(Playhead& ph, float w, float h);   // curve + sliders in one cell
    void drawControlsPanel(Playhead& ph, float w, float h);
    void drawModRow(Playhead& ph, float w, float h);
    void drawLFOPreview(ImDrawList* dl, ImVec2 pos, float w, float h,
                        int shape, float phase, float quant, float strength, float barDiv,
                        int playhead, float powExp = 1.0f);

    // In/out point drag state
    enum class WavDrag { None, InPoint, OutPoint };
    WavDrag wavDragMode = WavDrag::None;
    int     envDragHandle = -1;

    // ── Helpers ───────────────────────────────────────────────────────────────
    void        loadSample(const std::string& path);             // playhead 1's (shared) sample
    void        loadSampleInto(SampleData& sd, const std::string& path);
    void        loadSampleForPlayhead(Playhead& ph, const std::string& path);  // browser → tab
    void        sendAllParams(Playhead& ph);   // push all params to the playhead's synths
    void        validateAndReshapeVectorParams();  // ensure vector params match numChannels

    int                oldNumChannels = 1;
    ofEventListeners   nodeListeners;

    // ── BPM ───────────────────────────────────────────────────────────────────
    float currentBpm = 120.0f;
    void  setBpm(float bpm) override;
    float effectiveBpm() const;   // node BPM, or the transport's in sync mode

    // ── Publish to Node (same mechanism as polyphonicArpeggiatorGUI) ─────────
    // Only a minimal set of playhead-1 parameters is always in the node
    // (Show, N Chan, Trigger, AutoTrig, Amp, Pitch, Duration, Position, Levels,
    // SampleMs). Every other window control can be published at runtime with
    // its right-click menu. Keys are the parameters' escaped names, which are
    // also the names old presets stored their values and connections under.
    struct EditorPublishAction {
        std::string key;
        std::function<bool()> publish;
        std::function<bool()> unpublish;
        std::function<bool()> isPublished;
        std::function<bool()> isAvailableInNode;
        std::function<void(ofJson&)> saveValue;          // value -> json[key]
        std::function<void(const ofJson&)> loadValue;    // json[key] -> value
        // Right-click "Value" field: current value as text / apply typed text
        // (clamped to the range; vectors: one value for all voices, or a
        // comma-separated list)
        std::function<std::string()> valueText;
        std::function<void(const std::string&)> applyText;
        bool isVector = false;
        bool legacyNodeParameter = false;  // was a node parameter before publishing existed
        int  playhead = 0;
    };
    std::vector<EditorPublishAction> publishableEditorParameters;
    std::vector<std::string> publishedEditorParameterKeys;
    std::map<std::string, std::shared_ptr<ofxOceanodeAbstractParameter>> publishedEditorParameterHandles;
    bool publishedEditorSeparatorAdded = false;
    std::vector<std::string> legacyAutoPublishedKeys;   // see loadBeforeConnections

    void initializePublishableEditorParameters();
    bool isEditorParameterPublished(const std::string& key) const;
    const EditorPublishAction* findPublishableEditorParameter(const std::string& key) const;
    bool publishEditorParameterToNode(const std::string& key);
    bool unpublishEditorParameterFromNode(const std::string& key);
    void syncPublishedEditorParameters(const std::vector<std::string>& keys);
    void drawPublishedLabelUnderline(const std::string& key,
                                     const char* label,
                                     float frameWidth = -1.0f,
                                     bool checkbox = false) const;
    void drawPublishedCurrentItemUnderline(const std::string& key) const;
    // focusValue: focus the typed-value field when the menu opens; title
    // replaces its "Value" label (EQ band menus show three of them)
    void drawNodePublishMenuItems(const std::string& key, bool focusValue = true,
                                  const char* title = nullptr);
    void drawNodePublishContextMenu(const std::string& key,
                                    const char* label = nullptr,
                                    float frameWidth = -1.0f,
                                    bool checkbox = false);
    bool parameterHasConnection(const std::string& key) const;

    template<typename ParameterType>
    bool publishEditorParameterToNode(const std::string& key,
                                      ofParameter<ParameterType>& parameter,
                                      ofxOceanodeParameterFlags flags = 0) {
        if(getParameterGroup().contains(parameter.getEscapedName())) return false;
        if(!publishedEditorSeparatorAdded) {
            addSeparator("Published", ofColor(200));
            publishedEditorSeparatorAdded = true;
        }
        publishedEditorParameterHandles[key] = addParameter(parameter, flags);
        if(std::find(publishedEditorParameterKeys.begin(), publishedEditorParameterKeys.end(), key) == publishedEditorParameterKeys.end()) {
            publishedEditorParameterKeys.push_back(key);
        }
        return true;
    }

#if OFXOCEANODESC_HAS_TRANSPORT
    // ── Sync To Transport ─────────────────────────────────────────────────────
    ofParameter<bool>  syncToTransportP;
    ofParameter<float> beatOffsetP;
    scTransportSync::Follower  follower;
    scTransportSync::StepClock stepClock;   // unit = beats, grid block = 32 beats
    double   anchorIdCounter = 0.0;
    uint64_t lastAnchorUs    = 0;
    float    syncBpm         = 120.0f;
    void handleSyncChanged(bool on);
    void updateTransportSync();
    void sendAnchor(const scTransportSync::Anchor& a);
    void setSyncHoldArgs(Playhead& ph, ofxSCSynth* s);
    bool isSyncing() const { return syncToTransportP.get(); }
#else
    bool isSyncing() const { return false; }
#endif
};
