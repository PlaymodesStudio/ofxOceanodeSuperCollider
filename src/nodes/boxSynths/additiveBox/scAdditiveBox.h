//
//  scAdditiveBox.h
//  ofxOceanodeSuperCollider
//
//  AdditiveBox: a polyphonic additive synth in the Harmor / Razor family,
//  grown out of the ADDITIVE_ASTRES macro.
//
//  • Voices and outputs are separate: each voice is a series of up to 256
//    partials, and every voice can spread its partials over all the outputs
//    (a ring when Outputs >= 3, left..right in stereo). Unison adds detuned /
//    transposed copies of every voice (a harmonizer with an interval).
//  • Source: two spectra A and B (Saw, Square, Triangle, Pulse, Organ...,
//    Random, or 64 harmonics drawn by hand) morphed by Morph, with tilt,
//    stretch / inharmonicity, even / odd and harmonic blur.
//  • Envelopes: the shared ones (Envelopes tab); one routed to Level is the
//    voices' VCA, else they drone.
//  • Per partial: filter 1 gain (LP / BP / HP / Notch / Comb / drawn curve
//    around a keytracked, modulated cutoff, the fundamental optionally kept),
//    filter 2 (vowel formants or a drawn EQ), moving sweep lobes, random mask
//    and level (new draws on every gate), vibrato, noise, shimmer, time blur
//    and drift.
//  • Modulation: the shared system (scModulation: LFOs, envelopes, external
//    sources added one by one, a routing matrix). Per voice targets (Cutoff,
//    Morph, Pitch, Level, Sweep, Pan, Noise): the modulation synths write a
//    bus per target, each voice reads its channel. Per partial targets
//    (P.Level, P.Pan, P.Pitch): an LFO routed there runs inside the voices,
//    one phase per partial (a "lane", AdditiveBoxLanes, at most 2 LFOs);
//    envelopes and the other sources move every partial alike.
//  • FX: EQ -> Echo -> Reverb, the same synths and controls as GrainBox.
//  • Synths: per voice a source and the parts in use for P partials (P = the
//    smallest cap >= Partials: 8 .. 256), the lanes and the sine banks in
//    chunks of C = min(P, 64) partials (32 for noise lanes; AdditiveBoxLanes<C>, one
//    AdditiveBoxCore<C>_<O> per unison copy and chunk: small defs compile in
//    seconds), a tail (display, pause); all summing into a private bus, then the
//    FX and AdditiveBoxOut<O> (synthdefs/Defaults/AdditiveBox). Everything that
//    only changes with the controls is computed here and sent as arrays; the
//    synth runs only what moves. Filter 1's curve is written into a buffer per
//    voice. A voice pauses itself once its gate is off and it is silent; the
//    node resumes it on the next gate.
//  • GUI: the node shows Show / Voices / Outputs and whatever is published
//    (Pitch, Gate and Levels by default). Everything else is in the dockable
//    window: the live spectrum of every voice with the filter curves (drag
//    filter 1), the per-partial envelopes, the voices x partials matrix, and
//    the controls in tabs. Right-click a control there to publish it to the
//    node (or type a value / vector).
//  • Preset bank in the window (the sound: Gate, Trig, Transpose and Outputs
//    are left alone, everything else is reset to its default first).
//

#pragma once

#include "../../../scNode.h"
#include "../shared/scKeyboardPiano.h"
#include "../../../ui/scTimelineMenu.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "ofxSCBuffer.h"
#include "../../../ui/scEQEditor.h"
#include "../shared/scModulation.h"
#include "../shared/scAudioParam.h"
#include "../../../timing/scScheduledInputs.h"
#include "../shared/scSnapshots.h"
#include "../shared/scVoiceCountMorph.h"
#include "../../../ui/scDropdowns.h"
#include "ofxSCGroup.h"
#include "imgui.h"
#include <array>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

class serverManager;

class scAdditiveBox : public scNode {
    scKeyboardPiano keyboardPiano;
public:
    // its window (SC Mixer Pro's "Go to origin synth window" opens and focuses it)
    std::string getWindowTitle() override { return (canvasID == "Canvas" ? "" : canvasID + "/") + "AdditiveBox " + ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return &showWindow; }
private:
public:
    static constexpr int MAX_VOICES = 16;
    static constexpr int MAX_UNISON = 8;
    static constexpr int MAX_OUTPUTS = 16;       // AdditiveBoxCore<C>_<O> exists for O = 1..16
    static constexpr int MAX_PARTIALS = 256;
    static constexpr int DISPLAY_MAX = 64;       // partials per voice on the display bus
    static constexpr int NUM_CAPS = 6;
    static constexpr std::array<int, NUM_CAPS> CAPS = {8, 16, 32, 64, 128, 256};
    static constexpr int CHUNK = 64;             // lanes and cores: chunks of min(P, CHUNK) partials
    static constexpr int NOISE_CHUNK = 32;       // ... the noise lanes (LanesN) of min(P, NOISE_CHUNK)
    static constexpr int STREAM_EXTRA = 9;       // after the 3 x P streams: silence, vibrato (sine, depth), 2 x 3 lane clock
    static constexpr int CURVE_SIZE = 512;       // filter 1 curve: -128..+128 semitones
    static constexpr int TABLE_SIZE = 64;        // drawn harmonics
    static constexpr int SHAPE_POINTS = 33;      // drawn filter curves
    static constexpr int NUM_TARGETS = 10, FIRST_PARTIAL_TARGET = 7, MAX_LANES = 2, LANE_SIZE = 257;
    static constexpr int SWEEP_SIZE = 257;       // sweep shape: one cycle, last point = first
    static constexpr int SWEEP_POINTS = 16;      // drawn sweep shape

    enum FilterMode { FilterOff, FilterLP, FilterBP, FilterHP, FilterNotch, FilterComb, FilterDrawn, NumFilterModes };
    enum Filter2Mode { Filter2Off, Filter2Vowel, Filter2Drawn, NumFilter2Modes };
    enum Wave { WaveFlat, WaveSaw, WaveSquare, WaveTriangle, WaveSine, WavePulse, WaveOrgan, WaveHollow, WaveRandom, WaveDrawn, NumWaves };
    enum PanMode { PanRandom, PanSpectrum, PanAlternate, PanChannels, NumPanModes };
    enum SweepShape { SweepLobes, SweepTriangle, SweepSawUp, SweepSawDown, SweepPulse, SweepGauss, SweepSteps, SweepDrawn, NumSweepShapes };
    enum FxStage { FX_EQ, FX_ECHO, FX_REVERB, FX_COUNT };
    // a voice's switchable parts, in their order in the voice (additiveBox.scd);
    // the lanes run as a clock and chunks (VoiceSC::laneClock, lanes)
    enum Part { PartMotion, PartLane, PartFilt, PartSweep, PartShim, PartBlur, PartDrift, NumParts };

    explicit scAdditiveBox(std::vector<serverManager*> servers);
    ~scAdditiveBox();

    void setup() override;
    void update(ofEventArgs&) override;
    void draw(ofEventArgs&) override;
    void activate() override;
    void deactivate() override;
    void setBpm(float bpm) override { mod.setBpm(bpm); currentBpm = bpm; }

    // scNode interface
    void buildSynth(ofxSCServer* server) override;
    void createSynth(ofxSCServer* server) override;
    void free(ofxSCServer* server) override;
    void setOutputBus(ofxSCServer* server, int index, int bus) override;
    int  getOutputBusIndex(ofxSCServer* server, int index) override;
    void moveSynthBefore(ofxSCServer* server, int nodeID) override;
    int  getLastSynthID(ofxSCServer* server) override;
    void setInputBus(ofxSCServer* server, scNode* node, int bus) override;
    void resetInputBusses(ofxSCServer* server, int) override;
    void resendParametersForNRT() override;

    // Presets: published controls and window-only controls
    void presetSave(ofJson& json) override;
    void loadBeforeConnections(ofJson& json) override;
    void presetRecallBeforeSettingParameters(ofJson& json) override;
    void presetRecallAfterSettingParameters(ofJson& json) override;
#ifdef OFXOCEANODE_HAS_TIMELINE_INSTRUMENT
    // Gate (its level is the velocity) and Pitch, one note per voice
    bool getTimelineInstrument(ofxOceanodeTimelineInstrument& out) override;
    bool getMidiKeyboardInstrument(ofxOceanodeTimelineInstrument& out, bool bind = false) override {
        if(bind) {
            publishByKey(pitch.getEscapedName());
            publishByKey(gate.getEscapedName());
        }
        getTimelineInstrument(out);
        return true;
    }
#endif

private:
    // ── One voice (or unison copy), as sent to its synth ─────────────────────
    struct VoiceData {
        std::map<std::string, std::vector<float>> arrays;   // fr po pa pd ta td tr vf vd nr nd shr sph bl pb drr mm
        std::map<std::string, float> scalars;
        std::vector<float> ratio;                            // P frequency ratios (display)
        std::vector<float> view;                             // P static levels with filter 1 (display)
        float pitch = 48;
        float peak = 1;                                      // loudest unfiltered partial (display)
    };
    VoiceData computeVoice(int voice, int copy) const;
    float random01(int voice, int salt, int partial) const;
    float waveLevel(int voice, int wave, const std::vector<float>& table, int k) const;
    float filter1Gain(int voice, float semitonesFromCutoff, int mode = -1) const;
    float filter2Gain(int voice, float hz, int mode = -1) const;
    std::vector<float> filter1Curve(int voice) const;
    float voiceCutoff(int voice) const;                      // filter 1 cutoff, keytracked (MIDI)
    float sweepShapeAt(int voice, float x, int mode = -1) const;            // the sweep shape, x = 0..1 (one cycle)
    std::vector<float> sweepTable(int voice) const;
    float sweepRangeWeight(int voice, float x) const;        // share of the depth at x = 0..1 along the sweep
    float sampleRate() const;

    // ── Synths: the node is a group [modulation, voices, FX..., output]; each
    //    voice a group [source, parts in order (the lanes: clock, chunks), the
    //    cores (per unison copy, per chunk), the tail] ──
    // Gate and Pitch (published by default) also take audio cables: then the
    // voices' synths read them (mapped), gates as levels
    scAudioParam gateIn, pitchIn;
    std::set<std::string> skipScalars;            // mapped controls: never set (a set unmaps)
    void refreshOutputs() { for(auto& output : outputs) output = output; }
    void resendAllVoices();
    struct VoiceSC {
        ofxSCGroup* grp = nullptr;
        ofxSCBus* bus = nullptr;                          // the streams: levels, ratios, positions (3 x cap), STREAM_EXTRA
        ofxSCSynth* src = nullptr;                        // AdditiveBoxSrc<P>
        std::array<ofxSCSynth*, NumParts> parts{};        // the ones in use (not PartLane: below)
        ofxSCSynth* laneClock = nullptr;                  // AdditiveBoxLaneClock, with the lanes
        std::vector<ofxSCSynth*> lanes;                   // AdditiveBoxLanes(N)<C>, one per chunk
        std::vector<std::vector<ofxSCSynth*>> cores;      // AdditiveBoxCore<C>_<O>, per unison copy, per chunk
        ofxSCSynth* tail = nullptr;                       // AdditiveBoxTail<P>: display, pause
        // last values sent to each synth (a re-notified parameter does not resend)
        std::map<ofxSCSynth*, std::map<std::string, std::vector<float>>> sentA;
        std::map<ofxSCSynth*, std::map<std::string, float>> sentS;
    };
    struct ServerState {
        ofxSCGroup* root = nullptr;
        ofxSCGroup* voicesGroup = nullptr;
        bool created = false;
        std::vector<VoiceSC> voices;
        std::map<int,std::vector<VoiceSC>> partialBanks;
        std::vector<ofxSCSynth*> noteWriters;
        std::vector<std::map<std::string,float>> noteSent;
        std::vector<int> noteMapped;
        ofxSCSynth* out = nullptr;
        ofxSCBus* mix = nullptr;          // audio, MAX_OUTPUTS channels, the voices sum here
        ofxSCBus* vis = nullptr;          // control, voices * 2 * DISPLAY_MAX
        ofxSCBus* visJunk = nullptr;      // where unison copies write their display
        int outBus = -1;
        // Filter 1 curve of each voice
        ofxSCBuffer* curves[MAX_VOICES] = {};
        bool curvesPending = false;       // /b_alloc sent, waiting for /synced
        float curvesAllocTime = 0;
        std::vector<float> sentCurves[MAX_VOICES];
        // Sweep shape of each voice (allocated with the curves)
        ofxSCBuffer* sweeps[MAX_VOICES] = {};
        std::vector<float> sentSweeps[MAX_VOICES];
        // FX: the enabled stages, in order; stage i reads the previous bus
        // (the mix bus first) and writes fxBuses[i]; the output synth reads
        // the last one
        std::vector<int> fxPlan;
        int fxChannels = 0;
        ofxSCSynth* fx[FX_COUNT] = {nullptr, nullptr, nullptr};
        std::vector<ofxSCBus*> fxBuses;
        int outIn = -1;
        // Modulation: a group at the head (sources, then routes); each voice's
        // source writes its gate (audio, 16)
        ofxSCGroup* modGroup = nullptr;
        ofxSCGroup* modSources = nullptr;
        ofxSCGroup* modRoutes = nullptr;
        bool modCreated = false;
        ofxSCBus* gateBus = nullptr;
        // LFO lanes: each lane's LFO cycle (allocated with the curves)
        ofxSCBuffer* laneBufs[2 * MAX_LANES] = {};        // the lanes' cycles, then their noise curves
        std::vector<float> sentLanes[2 * MAX_LANES];
    };
    int numVoicesClamped() const;
    int numOutputsClamped() const;
    int unisonClamped() const;
    int partialCap() const;
    static int capForCount(int count);
    std::vector<float> partialMorphWeights;
    float partialMorphRetireAt=-1;
    bool partialBankMode=false;
    int renderPartialCap=0;
    float renderPartialGain=1;
    std::map<int,float> partialCapWeights() const;
    void preparePartialMorph(const ofJson&,const ofJson&,float);
    void finishPartialMorph();
    void forPartialBanks(ofxSCServer*,const std::function<void()>&);
    void syncNoteWriters(ofxSCServer*,bool force=false);
    float partialPresence(int index) const;
    float partialCountValue(const std::function<float(int)>&) const;
    int chunkSize() const { return std::min(partialCap(), int(CHUNK)); }
    int laneChunkSize() const { return std::min(partialCap(), lanesHaveNoise() ? int(NOISE_CHUNK) : int(CHUNK)); }
    std::string coreDefName() const;
    std::string outDefName() const;
    void ensureBuses(ofxSCServer* server);
    void releaseBuses(ofxSCServer* server);
    void uploadCurves(ofxSCServer* server, bool force);
    void computeAll();
    void sendAll(ofxSCServer* server, bool force);
    // a voice's synths and what each gets
    float unisonSide(int u) const;                                  // -1..1: where copy u sits
    std::map<std::string, float> coreScalars(int v, int u) const;   // pitch, amp, pan offset of a copy
    bool partWanted(int part, int v) const;
    void sendVoice(ofxSCServer* server, int v, const VoiceData& data, bool force);
    void sendTo(VoiceSC& vs, ofxSCSynth* s, unsigned role, const VoiceData& data,
                const std::map<std::string, float>& extra, bool force);
    void syncVoice(ofxSCServer* server, int v, bool force);         // groups, bus, synths (parts on / off)
    void syncVoices(ofxSCServer* server);                           // voice / unison count, parts
    void freeVoice(ofxSCServer* server, VoiceSC& vs, bool sendFree);
    void rebuildDefs();
    void applyStructure();
    void freeSynths(ofxSCServer* server, bool sendFree = true);
    void onGate(const std::vector<float>& values);
    void onTrig(const std::vector<int>& values);
    void markDirty() { dirty = true; }
    void flushDirty();                              // every voice recomputed, what changed sent
    void laneData(int v, VoiceData& d) const;      // the modulation's share of a voice's data
    void computeLaneTables();
    bool lanesHaveNoise() const;                    // the lane part runs the noise variant
    bool modDirty = false;                          // only the modulation's data changed
    int dataGen = 0;                                // voiceData's generation (the views' caches)
    bool windowDrawn = false;                       // drawn last frame: read the server's data
    float liveReadAt = 0.0f;
    struct SpecCache { int gen = -1; float w = 0, h = 0; std::vector<std::vector<ImVec2>> stat; std::vector<ImVec2> f1, f2; };
    SpecCache specCache;

    // ── Modulation (scModulation) ────────────────────────────────────────────
    scModulation mod;
    template<typename T> void regMod(ofParameter<T>& p, std::function<void()> send);
    struct Lane { int lfo = -1; float amt[3] = {0, 0, 0}; };   // level, pan, pitch (scaled)
    std::vector<Lane> activeLanes() const;                    // LFOs routed to per-partial targets (2 at most)
    std::vector<std::vector<float>> laneTables;
    void createMod(ofxSCServer* server);
    void freeMod(ofxSCServer* server, bool sendFree);
    void migrateOldModulation(const ofJson& json);
    void migrateOldEnvelope(const ofJson& json);
    bool levelEnvelope() const;   // an envelope routed to Level: the voices' VCA

    // ── Publishing window controls into the node ─────────────────────────────
    struct PublishAction {
        std::string key;
        ofAbstractParameter* param = nullptr;
        bool isVector = false;
        std::vector<std::string> dropdown;
        std::function<std::shared_ptr<ofxOceanodeAbstractParameter>()> publish;
        std::function<void(ofJson&)> save;
        std::function<void(const ofJson&)> load;
        std::function<void()> reset;                 // back to the value at setup
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
    bool notInPresets(const std::string& key) const;

    // ── FX: EQ -> Echo -> Reverb (graphiceq<O>, Echo<O>, boxMicroSpace<O>, the
    // same synths and controls as GrainBox) between the voices and the output
    // synth; a stage that is off runs no synth ──────────────────────────────
    std::string fxDefName(int stage) const;
    bool fxStageReady(ofxSCServer* server) const;
    void requireFxSynthdefs();
    void rebuildFx(ofxSCServer* server);
    void rebuildFxAll();
    void freeFxSynths(ofxSCServer* server);
    void sendFxParams(int stage, ofxSCSynth* synth);
    void sendFxParamsAll(int stage);
    void drawFxPanel();
    void drawEqEditor(float w, float h);
    bool fxPending = false;
    scEQEditor eqEditor;
    bool eqCurveDirty = true;

    // ── Snapshots and named presets (data/nodeSnapshots/AdditiveBox/Presets):
    //    scSnapshots; a sound = Voices + every control a preset holds + the modulation ──
    scSnapshots snaps;
    std::shared_ptr<ofxOceanodeAbstractParameter> voiceCountHandle;
    bool soundDriven(const std::string& key) const;
    std::vector<float> voiceMorphWeights;
    int voiceMorphCapacity = 0;
    float voiceMorphRetireAt = -1;
    bool applyingVoiceMorph = false;
    float voiceMorphGain(int voice) const;
    void finishVoiceMorph();
    ofJson captureSound() const;
    void loadSound(const ofJson& json);                                     // a whole sound
    void applySound(const ofJson& a, const ofJson& b, float t, bool sw);    // a blend
    const ofJson* modSource = nullptr;   // the routing a blend switched to
    float currentBpm = 120.0f;
    bool loadingBankPreset = false;   // structure changes wait for applyStructure()

    // ── Window ───────────────────────────────────────────────────────────────
    void drawWindow();
    void drawHeader();
    void drawSpectrum(ImVec2 size);
    void drawMatrix(ImVec2 size);
    void drawSoundTab();
    void drawSourceTab();
    void drawFiltersTab();
    void drawSpeakers(ImVec2 size);
    void drawSweepTab();
    void drawSweepShape(ImVec2 size);
    void drawSweepMap(ImVec2 size);
    bool drawPointEditor(const char* id, ofParameter<std::vector<float>>& parameter, float lo, float hi,
                         ImVec2 size, bool bars, const char* title, int points = 0);
    bool sliderVector(ofParameter<std::vector<float>>& parameter, const char* format = "%.3f", bool logarithmic = false);
    bool comboVector(ofParameter<std::vector<int>>& parameter, const std::vector<std::string>& options);
    bool comboInt(ofParameter<int>& parameter, const std::vector<std::string>& options);
    bool sliderInt(ofParameter<int>& parameter);
    bool sliderFloat(ofParameter<float>& parameter, const char* format = "%.3f", bool logarithmic = false);
    bool checkbox(ofParameter<bool>& parameter);
    void group(const char* title);
    float liveLevel(int voice, int slot) const;        // display slot of a voice, -1 if no data
    float livePos(int voice, int slot) const;          // its position (0..1 around the outputs), NAN if no data
    int displaySlots() const;
    int displayPartial(int slot) const;                // partial index (0-based) of a display slot

    // ── Parameters ───────────────────────────────────────────────────────────
    // On the node
    ofParameter<bool> showWindow;
    ofParameter<int> numVoices, numOutputs;
    // Voice
    ofParameter<std::vector<float>> pitch, gate, levels, glide;
    scScheduledInputs noteInputs;    // the timeline's notes, timetagged (Gate, Pitch while published)
    ofParameter<std::vector<int>> trig;
    ofParameter<float> volume;
    ofParameter<float> transpose;   // semitones added to every voice's Pitch
    // Source
    ofParameter<std::vector<int>> waveA, waveB;
    ofParameter<std::vector<float>> morph, harmonicsA, harmonicsB, harmBlur;
    // Harmonics
    ofParameter<int> partials;
    ofParameter<std::vector<float>> tilt, stretch, inharm, evenOdd, detune, shift;
    // Filter 1
    ofParameter<std::vector<int>> filterMode;
    ofParameter<std::vector<float>> cutoff, keytrack, slope, resonance, safeBass, filterShape;
    // Filter 2
    ofParameter<std::vector<int>> filter2Mode;
    ofParameter<std::vector<float>> vowel, formantShift, formantWidth, filter2Mix, drawnEq;
    // Sweep
    ofParameter<std::vector<float>> sweepAmt, sweepWaves, sweepRate, sweepSharp, sweepPhase;
    ofParameter<std::vector<int>> sweepMode, sweepShape;
    ofParameter<std::vector<float>> sweepSkew, sweepLow, sweepHigh, sweepDrawn;
    ofParameter<bool> sweepInvert, sweepRetrig;
    ofParameter<float> sweepVoiceOffset;
    // Unison
    ofParameter<int> unison;
    ofParameter<float> unisonDetune, unisonSpread, unisonInterval, unisonBlend;
    // Scatter
    ofParameter<std::vector<float>> density, randAmp, maskGlide;
    ofParameter<int> seed;
    ofParameter<bool> reroll;
    // Envelope
    // Motion
    ofParameter<std::vector<float>> vibrato, vibRate, partialVib, partialVibRate, shimmer, shimmerRate;
    ofParameter<std::vector<float>> noise, noiseRate, noiseTilt, timeBlur;
    // Space
    ofParameter<std::vector<float>> pan, spread, drift, driftRate, width;
    ofParameter<std::vector<int>> panMode;
    ofParameter<int> speakerLayout;   // 3 outputs or more: 0 a ring, 1 a line
    // FX
    ofParameter<bool> fxOn[FX_COUNT];
    ofParameter<float> eqGain[5], eqFreq[5], eqShape[5], eqMix;
    ofParameter<float> echoDelay, echoFeed, echoCutoff, echoMix, echoResonance;
    ofParameter<int> echoFilter;
    ofParameter<float> revSize, revDecay, revPredelay, revLowpass, revMix, revPosition, revSpread;

    // ── State ────────────────────────────────────────────────────────────────
    std::vector<serverManager*> servers;
    std::map<ofxSCServer*, ServerState> states;
    std::vector<VoiceData> voiceData;          // copy 0 of each voice, for the views
    std::vector<std::vector<float>> curveData; // filter 1 curve of each voice
    std::vector<std::vector<float>> sweepData; // sweep shape of each voice
    std::vector<int> rolls;                    // random draw generation of each voice
    uint32_t freshSeed = 1;                    // Seed 0 (no seed): this node's own, drawn at creation
    std::vector<int> lastTrig;
    std::vector<bool> gateOpen;
    std::vector<float> liveData;               // display bus of the first server
    bool liveValid = false;
    bool dirty = true;
    int builtCap = 0, builtOutputs = 0;
    std::map<std::string, int> dragPoint;      // point editors: last point dragged

    std::vector<PublishAction> publishActions;
    std::vector<std::string> publishedKeys;
    scTimelineMenu timeline;              // "Add to Timeline" from the window
    std::map<std::string, std::shared_ptr<ofxOceanodeAbstractParameter>> publishedHandles;
    bool publishedSeparatorAdded = false;

    float viewHeight = 260.0f;
    ofEventListeners nodeListeners;
};
