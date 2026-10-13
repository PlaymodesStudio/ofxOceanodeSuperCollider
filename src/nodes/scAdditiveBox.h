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
//  • Per partial: its own ADSR (times shortened at random and / or for high
//    partials), filter 1 gain (LP / BP / HP / Notch / Comb / drawn curve
//    around a keytracked, modulated cutoff, the fundamental optionally kept),
//    filter 2 (vowel formants or a drawn EQ), moving sweep lobes, random mask
//    and level (new draws on every gate), vibrato, noise, shimmer, time blur
//    and drift.
//  • Modulation: an envelope and two LFOs (sine, triangle, saw, square, S&H)
//    to cutoff, morph, pitch, level, sweep, pan and noise.
//  • FX: EQ -> Echo -> Reverb, the same synths and controls as GrainBox.
//  • Synths: one AdditiveBoxV<P>_<O> per voice and unison copy (P = the
//    smallest cap >= Partials: 8 .. 256), summing into a private bus, then the
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

#include "scNode.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "ofxSCBuffer.h"
#include "scEQEditor.h"
#include "imgui.h"
#include <array>
#include <functional>
#include <map>
#include <string>
#include <vector>

class serverManager;

class scAdditiveBox : public scNode {
public:
    static constexpr int MAX_VOICES = 16;
    static constexpr int MAX_UNISON = 8;
    static constexpr int MAX_OUTPUTS = 16;       // AdditiveBoxV<P>_<O> exists for O = 1..16
    static constexpr int MAX_PARTIALS = 256;
    static constexpr int DISPLAY_MAX = 64;       // partials per voice on the display bus
    static constexpr int NUM_CAPS = 6;
    static constexpr std::array<int, NUM_CAPS> CAPS = {8, 16, 32, 64, 128, 256};
    static constexpr int CURVE_SIZE = 512;       // filter 1 curve: -128..+128 semitones
    static constexpr int TABLE_SIZE = 64;        // drawn harmonics
    static constexpr int SHAPE_POINTS = 33;      // drawn filter curves
    static constexpr int MOD_SOURCES = 3, MOD_DESTS = 7;
    static constexpr int SWEEP_SIZE = 257;       // sweep shape: one cycle, last point = first
    static constexpr int SWEEP_POINTS = 16;      // drawn sweep shape

    enum FilterMode { FilterOff, FilterLP, FilterBP, FilterHP, FilterNotch, FilterComb, FilterDrawn, NumFilterModes };
    enum Filter2Mode { Filter2Off, Filter2Vowel, Filter2Drawn, NumFilter2Modes };
    enum Wave { WaveFlat, WaveSaw, WaveSquare, WaveTriangle, WaveSine, WavePulse, WaveOrgan, WaveHollow, WaveRandom, WaveDrawn, NumWaves };
    enum PanMode { PanRandom, PanSpectrum, PanAlternate, NumPanModes };
    enum SweepShape { SweepLobes, SweepTriangle, SweepSawUp, SweepSawDown, SweepPulse, SweepGauss, SweepSteps, SweepDrawn, NumSweepShapes };
    enum FxStage { FX_EQ, FX_ECHO, FX_REVERB, FX_COUNT };

    explicit scAdditiveBox(std::vector<serverManager*> servers);
    ~scAdditiveBox();

    void setup() override;
    void update(ofEventArgs&) override;
    void draw(ofEventArgs&) override;
    void activate() override;
    void deactivate() override;

    // scNode interface
    void buildSynth(ofxSCServer* server) override;
    void createSynth(ofxSCServer* server) override;
    void free(ofxSCServer* server) override;
    void setOutputBus(ofxSCServer* server, int index, int bus) override;
    int  getOutputBusIndex(ofxSCServer* server, int index) override;
    void moveSynthBefore(ofxSCServer* server, int nodeID) override;
    int  getLastSynthID(ofxSCServer* server) override;
    void setInputBus(ofxSCServer*, scNode*, int) override {}
    void resetInputBusses(ofxSCServer*, int) override {}
    void resendParametersForNRT() override;

    // Presets: published controls and window-only controls
    void presetSave(ofJson& json) override;
    void loadBeforeConnections(ofJson& json) override;
    void presetRecallBeforeSettingParameters(ofJson& json) override;
    void presetRecallAfterSettingParameters(ofJson& json) override;

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
    float filter1Gain(int voice, float semitonesFromCutoff) const;
    float filter2Gain(int voice, float hz) const;
    std::vector<float> filter1Curve(int voice) const;
    float voiceCutoff(int voice) const;                      // filter 1 cutoff, keytracked (MIDI)
    float sweepShapeAt(int voice, float x) const;            // the sweep shape, x = 0..1 (one cycle)
    std::vector<float> sweepTable(int voice) const;
    float sweepRangeWeight(int voice, float x) const;        // share of the depth at x = 0..1 along the sweep
    float sampleRate() const;

    // ── Synths ───────────────────────────────────────────────────────────────
    struct ServerState {
        std::vector<ofxSCSynth*> slots;   // voice v, copy u at v * Unison + u
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
        ofxSCSynth* fx[FX_COUNT] = {nullptr, nullptr, nullptr};
        std::vector<ofxSCBus*> fxBuses;
        int outIn = -1;
        // Last values sent to each slot (a re-notified parameter does not resend)
        std::vector<std::map<std::string, std::vector<float>>> sentArrays;
        std::vector<std::map<std::string, float>> sentScalars;
    };
    int numVoicesClamped() const;
    int numOutputsClamped() const;
    int unisonClamped() const;
    int partialCap() const;
    std::string voiceDefName() const;
    std::string outDefName() const;
    void ensureBuses(ofxSCServer* server);
    void releaseBuses(ofxSCServer* server);
    void uploadCurves(ofxSCServer* server, bool force);
    ofxSCSynth* newSlotSynth(ofxSCServer* server);
    ofxSCSynth* chainHead(ServerState& st) const;            // first FX synth, else the output synth
    void sendSlot(ofxSCServer* server, int slot, const VoiceData& data, bool force);
    void computeAll();
    void sendAll(ofxSCServer* server, bool force);
    void syncSlots(ofxSCServer* server);
    void rebuildDefs();
    void applyStructure();
    void freeSynths(ofxSCServer* server);
    void onGate(const std::vector<float>& values);
    void onTrig(const std::vector<int>& values);
    void markDirty() { dirty = true; }

    // ── Publishing window controls into the node ─────────────────────────────
    struct PublishAction {
        std::string key;
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

    // ── FX: EQ -> Echo -> Reverb (graphiceq<O>, Echo<O>, SpaceMaster<O>, the
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

    // ── Preset bank: data/Supercollider/AdditiveBox/Presets/<name>.json ─────
    std::string presetDir() const;
    void scanPresets();
    void loadBankPreset(const std::string& name);
    bool saveBankPreset(const std::string& name);
    void drawPresetBar();
    std::vector<std::string> bankPresets;
    std::string currentPreset;
    bool presetsScanned = false;
    bool loadingBankPreset = false;   // structure changes wait for applyStructure()

    // ── Window ───────────────────────────────────────────────────────────────
    void drawWindow();
    void drawHeader();
    void drawSpectrum(ImVec2 size);
    void drawEnvelopes(ImVec2 size);
    void drawMatrix(ImVec2 size);
    void drawSoundTab();
    void drawSourceTab();
    void drawFiltersTab();
    void drawModTab();
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
    int displaySlots() const;
    int displayPartial(int slot) const;                // partial index (0-based) of a display slot

    // ── Parameters ───────────────────────────────────────────────────────────
    // On the node
    ofParameter<bool> showWindow;
    ofParameter<int> numVoices, numOutputs;
    // Voice
    ofParameter<std::vector<float>> pitch, gate, levels, glide;
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
    ofParameter<std::vector<float>> attack, decay, sustain, release, curve, envRand, envTilt;
    // Motion
    ofParameter<std::vector<float>> vibrato, vibRate, partialVib, partialVibRate, shimmer, shimmerRate;
    ofParameter<std::vector<float>> noise, noiseRate, noiseTilt, timeBlur;
    // Space
    ofParameter<std::vector<float>> pan, spread, drift, driftRate, width;
    ofParameter<std::vector<int>> panMode;
    // Modulation
    ofParameter<float> modAttack, modDecay, modSustain, modRelease, modCurve;
    ofParameter<int> lfoShape[2];
    ofParameter<float> lfoRate[2];
    ofParameter<bool> lfoRetrig[2];
    ofParameter<float> modAmount[MOD_SOURCES][MOD_DESTS];
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
    std::vector<int> lastTrig;
    std::vector<bool> gateOpen;
    std::vector<float> liveData;               // display bus of the first server
    bool liveValid = false;
    bool dirty = true;
    int builtCap = 0, builtOutputs = 0;
    std::map<std::string, int> dragPoint;      // point editors: last point dragged

    std::vector<PublishAction> publishActions;
    std::vector<std::string> publishedKeys;
    std::map<std::string, std::shared_ptr<ofxOceanodeAbstractParameter>> publishedHandles;
    bool publishedSeparatorAdded = false;

    float viewHeight = 260.0f;
    ofEventListeners nodeListeners;
};
