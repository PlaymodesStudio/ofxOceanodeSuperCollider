//
//  scPolyComb.h
//  ofxOceanodeSuperCollider
//
//  PolyComb: a polyphonic comb-resonator synth, N voices -> N channels.
//
//  • The node shows Show, Voices, Gate (its level: the velocity), Pitch, Master, Reset and
//    whatever is published. Gate takes Oceanode values (a vector, one per
//    voice) and an audio cable at the same time (they add; read sample
//    accurately). The dockable window has everything else, in tabs laid out
//    in columns; right-click any control there to publish it to the node.
//  • Per voice (PolyCombVoice<N>, synthdefs/Defaults/PolyComb): amp and mod
//    envelopes (ADSR, Alt+drag a segment to bend it; Drone mode holds the
//    gate) -> oscillator bank with FM (sine modulator with feedback) ->
//    filter -> comb resonator (follows the note or a fixed pitch) -> ring
//    modulator -> post filter -> sine shaper.
//  • Modulation: the shared system (scModulation): LFOs (per-voice phase
//    offsets from an indexer, as the original PolyComb), envelopes, velocity,
//    a random value per note, external inputs and the voice index, routed to
//    14 targets by a matrix (Routing tab).
//  • Effects after the voices: MixerPro's EQ, Blur's allpass diffusion smear
//    (PCBlur<N>), echo and SpaceMaster reverb
//    (mixerProEQ<N>, mixerProEcho<N>, boxMicroSpace<N>), each created the first
//    time it is switched on and paused once its tail is over.
//

#pragma once

#include "../../../scNode.h"
#include "../shared/scKeyboardPiano.h"
#include "../../../serverManager.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "ofxSCGroup.h"
#include "../../../ui/scEQEditor.h"
#include "../shared/scModulation.h"
#include "../../../ui/scTimelineMenu.h"
#include "../shared/scSnapshots.h"
#include "../shared/scVoiceCountMorph.h"
#include "../../../ui/scDropdowns.h"
#include "../shared/scAudioParam.h"
#include "../../../timing/scScheduledInputs.h"
#include "scPolyCombPitchMap.h"
#include "scPolyCombPitchMorph.h"
#include "../chordBox/scChordBoxEngine.h"
#include "imgui.h"
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class scPolyComb : public scNode {
    scKeyboardPiano keyboardPiano;
public:
    // its window (SC Mixer Pro's "Go to origin synth window" opens and focuses it)
    std::string getWindowTitle() override { return "PolyCombBox " + ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return &showWindow; }
private:
public:
    static constexpr int MAX_VOICES = 32;
    static constexpr int MAX_CHANNELS = 16;                // PolyCombSpread<1..16>
    static constexpr int LEGACY_VOICES = 16;               // up to here: the original 16-lane spreader
    static constexpr int NUM_LFOS = scModulation::MAX_LFOS;
    static constexpr int NUM_ENVS = scModulation::MAX_ENVS;
    static constexpr int NUM_EXT = scModulation::MAX_EXT;
    // sources: LFO 1-8, Env 1-4, Velocity, Random, Ext 1-8, Voice
    static constexpr int NUM_SOURCES = scModulation::NUM_SOURCES;
    static constexpr int SRC_ENV = scModulation::SRC_ENV, SRC_VEL = scModulation::SRC_VEL, SRC_RND = scModulation::SRC_RND,
                         SRC_EXT = scModulation::SRC_EXT, SRC_VOICE = scModulation::SRC_VOICE;
    // the node's own source after the shared ones (a matrix row of its own)
    static constexpr int SRC_DELTA = scModulation::NUM_SOURCES, ALL_SOURCES = SRC_DELTA + 1;
    static constexpr int NUM_TARGETS = 14;
    enum Stage { StComp, StEq, StBlur, StEcho, StRvb, NumStages };

    explicit scPolyComb(std::vector<serverManager*> servers, bool voiceRenderer = false);
    ~scPolyComb();

    void setup() override;
    void update(ofEventArgs&) override;
    void draw(ofEventArgs&) override;
    void setBpm(float bpm) override;
    void activate() override;
    void deactivate() override;

    void buildSynth(ofxSCServer* s) override;
    void createSynth(ofxSCServer* s) override;
    void free(ofxSCServer* s) override;
    void moveSynthBefore(ofxSCServer* s, int nodeID) override;
    int  getLastSynthID(ofxSCServer* s) override;
    void setOutputBus(ofxSCServer* s, int idx, int bus) override;
    int  getOutputBusIndex(ofxSCServer* s, int idx) override;
    void setInputBus(ofxSCServer* s, scNode* node, int bus) override;
    void resetInputBusses(ofxSCServer* s, int targetBus) override;
    void resendParametersForNRT() override;

    void presetSave(ofJson& j) override;
    void loadBeforeConnections(ofJson& j) override;
    void presetRecallAfterSettingParameters(ofJson& j) override;
    // MacroMixer: the sound saved in "polyComb*" blends on the running synths
    // through the snapshot morph (applyState). Voices and Channels need other
    // synths and buses, so a change there replaces the node instead.
    MorphKeys morphKeys() const override;
    MorphHint morphHint(const ofxOceanodeAbstractParameter& p) const override;
    bool morphCheck(const ofJson& target, std::string& reason) const override;
    bool morphBegin(const ofJson& target, std::string& reason) override;
    void morphApply(float t, bool switched) override;
    void morphEnd(bool completed) override;
#ifdef OFXOCEANODE_HAS_TIMELINE_INSTRUMENT
    // Gate (its level is the velocity) and Pitch, one note per voice (timeline piano rolls)
    bool getTimelineInstrument(ofxOceanodeTimelineInstrument& out) override;
#endif

private:
    // ── Per server: a root group, one subgroup per slot (in this order, so
    //    any part can come and go without breaking the order), small synths ──
    static constexpr int W = MAX_VOICES;               // every voice bus is as wide as the most voices
    static constexpr int VIS_AMP = 0;                  // the display bus: amp env levels
    static constexpr int VIS_PAN = W;                  //   and the voices' positions
    static constexpr int VIS_GR = 2 * W;
    static constexpr int VIS_SUB = 2 * W + 1;
    static constexpr int ZERO_TARGET = NUM_TARGETS;    // a modulation bus nobody writes
    enum Slot { SlGate, SlSrc, SlRoute, SlPitch, SlFm, SlOsc, SlFilter, SlComb, SlRing, SlPost, SlShaper,
                SlAmp, SlSub, SlSpread, SlComp, SlEq, SlBlur, SlEcho, SlRvb, NumSlots };
    enum Osc { OSaw, OTri, OPulse, OSine, OSSaw, OWhite, OPink, ODust, OImp, NumOsc };
    enum Ins { IFilter, IComb, IRing, IPost, IShaper, NumIns };
    struct Part { ofxSCSynth* synth = nullptr; bool running = false; float offAt = -1.0f; std::string definition; };
    struct Fx { ofxSCSynth* synth = nullptr; ofxSCBus* in = nullptr; bool running = false; float offAt = -1.0f; };
    struct ServerState {
        ofxSCGroup* root = nullptr;
        std::array<ofxSCGroup*, NumSlots> groups{};
        bool created = false;
        ofxSCSynth* gateS = nullptr;                    // ModGate: gates (their levels), random
        ofxSCSynth* ampEnv = nullptr;                   // ModEnvAr: the amp envelope
        ofxSCServer* server = nullptr;
        bool modCreated = false;                        // the shared modulation's synths and buses
        ofxSCSynth* pitchS = nullptr;                   // PCPitch
        Part fm;                                        // PCFm
        std::array<Part, NumOsc> osc;                   // PCOsc<Type>, the ones audible
        std::array<Part, NumIns> ins;                   // PCFilter / Comb / Ring / Filter (post) / Shaper
        Part sub;                                      // independent sine layer
        ofxSCSynth* amp = nullptr;                      // PCAmp
        ofxSCSynth* spread = nullptr;                   // PolyCombSpread<C> (PCSpreadWide<C> over 16 voices)
        int spreadLanes = LEGACY_VOICES;                // the spreader's voices: 16 or 32
        ofxSCSynth* delta = nullptr;                    // PCDelta: only while Delta is routed
        ofxSCBus* scratch = nullptr;                    // audio, nobody listens: a new part's in / out until routed
        std::array<Fx, NumStages> fx;                   // mixerProEQ / Echo / Rvb
        // buses (all W wide per voice set)
        ofxSCBus* gateBus = nullptr;                    // audio: the gates
        ofxSCBus* envBus = nullptr;                     // audio: the amp envelope
        ofxSCBus* noteBus = nullptr;                    // control, 2W: the note (modulated), the note before
        ofxSCBus* fmBus = nullptr;                      // audio: FM, Hz to add
        ofxSCBus* oscBus = nullptr;                     // audio: the oscillators' sum
        std::array<ofxSCBus*, NumIns> insBus{};         // audio: each insert's output
        ofxSCBus* voiceBus = nullptr;                   // audio: the voices, to the spreader
        ofxSCBus* visBlock = nullptr;                   // control: amp levels, positions (the window)
        int outBus = -1;
        int builtVoices = 0, builtChannels = 0;
        // Allocated only during a voice-count transition, before shared FX.
        ofxSCSynth* voiceMix = nullptr;
        ofxSCBus* voiceMainBus = nullptr;
        std::array<ofxSCBus*, MAX_VOICES> voicePeerBuses{};
        float voiceMixReady = 0;
        std::vector<float> voiceMixInputs, voiceMixWeights;

    };
    std::vector<serverManager*> servers;
    std::map<ofxSCServer*, ServerState> states;
    // Gate, Pitch: Oceanode values or audio cables
    scAudioParam gateIn, pitchIn;
    void refreshOutputs() { for(auto& output : outputs) output = output; }

    // ── Publishable controls (the window's) ──────────────────────────────────
    struct Control {
        std::string key, name;
        ofAbstractParameter* param = nullptr;
        std::function<std::shared_ptr<ofxOceanodeAbstractParameter>()> add;
        std::function<ofJson()> save;
        std::function<void(const ofJson&)> load;
    };
    std::vector<Control> controls;
    std::map<std::string, std::shared_ptr<ofxOceanodeAbstractParameter>> published;
    std::vector<std::string> publishedKeys;
    scTimelineMenu timeline;              // "Add to Timeline" from the window
    bool publishedSeparator = false;
    template<typename T> void reg(ofParameter<T>& p, std::function<void()> send);
    void publish(const std::string& key);
    void unpublish(const std::string& key);
    bool isConnected(const std::string& key) const;
    void publishMenu(const std::string& key);
    void publishedUnderline(const std::string& key) const;

    // ── Engine ───────────────────────────────────────────────────────────────
    int n() const { return morphRenderVoices > 0 ? morphRenderVoices : voices.get(); }
    std::string def(const std::string& name) const { return name + ofToString(n()); }
    int src(const ServerState& st, int s) const;
    int mod(const ServerState& st, int t) const;
    void forStates(const std::function<void(ofxSCServer*, ServerState&)>& f);
    void sendVec(ofxSCSynth* s, const std::string& name, const std::vector<float>& v);
    // what each part needs, sent to it (on creation and when its controls change)
    void sendGate(ServerState& st, bool notesOnly = false);
    void sendAmpEnv(ServerState& st);
    void sendPitch(ServerState& st, bool notesOnly = false);
    void sendFm(ServerState& st);
    void sendOsc(ServerState& st, int o);
    void sendIns(ServerState& st, int i);
    void sendAmp(ServerState& st);
    void sendSub(ServerState& st, bool notesOnly = false);
    void sendSpread(ServerState& st);
    // parts: created when wanted, paused (after a fade) when not
    bool oscWanted(int o) const;
    bool insWanted(int i) const;
    ofxSCSynth* make(ofxSCServer* server, ServerState& st, const std::string& defName, int slot);
    void updateParts(ofxSCServer* server, ServerState& st);
    void routeVoice(ServerState& st);
    void createAll(ofxSCServer* server, ServerState& st);
    void freeSynths(ServerState& st);
    void freeAll(ofxSCServer* server, ServerState& st);
    void ensureBuses(ofxSCServer* server, ServerState& st);
    void rebuildAll();
    // a new voice count: the voices fade out, are rebuilt, fade back in
    float rebuildAt = -1.0f;
    bool fadingIn = false;
    void requestRebuild();
    void rebuildOutput();
    std::vector<float> processedPitch() const;
    std::vector<float> spreadPitch(const std::vector<float>& v) const;
    // ── Pitch & Harmony: the internal pitch maps (External off) ──────────────
    struct MapParams {
        ofParameter<int> scaleSelection, shape, mode, quantize, indexSymmetry, indexSeed, indexCycleShift, version;
        ofParameter<bool> indexInvert, indexSeedOnGate;
        ofParameter<float> indexSkew, indexRoundness, indexPW, indexPow;
        ofParameter<float> degreeOffset, octaveJump, indexComb, indexQuantize, indexRandom, indexWaves;
        ofParameter<float> first, interval, transpose, bend, offset, root, harmonicBase, span;
        ofParameter<std::vector<float>> tones, table;
    };
    MapParams oscMap, combMap;
    ofParameter<bool> pitchExternal;      // on: the Pitch values as they are (as before)
    ofParameter<int> pitchShiftMode;      // External off, Pitch around 60: 0 after the map, 1 in the scale
    ofParameter<bool> combMapOn;          // External off: the resonators follow their own map
    ofJson pitchDefaults = ofJson::object();
    scPolyCombPitchMorph pitchMorph;
    bool internalPitch() const { return !pitchExternal.get(); }
    float combMapAmount() const;
    bool combMapped() const { return internalPitch() && combMapAmount()>0.f; }
    ofJson pitchMorphOverrides() const;
    std::vector<float> combMapPitches() const;
    std::vector<float> previousMapGate;
    void randomizeMapSeedsOnGate(const std::vector<float>& values);
    void updatePitchIndexLimits();
    pcmap::Map mapOf(const MapParams& m) const;
    std::vector<float> oscillatorPitches() const;
    std::vector<float> resonatorPitches() const;
    std::vector<float> ringSourcePitches() const;
    std::vector<float> mapPitches(const MapParams& m) const;   // n, with the played Pitch
    std::vector<chordbox::LibItem> scaleLibrary, chordLibrary;
    void loadPitchLibraries();
    void selectMapScale(MapParams& m);
    void syncMapScale(MapParams& m);
    const chordbox::LibItem* mapScaleAt(int index) const;
    // ── Delta: how much two LFOs moved (the original's DELTA), a source ─────
    ofParameter<int> deltaSrcA, deltaSrcB;
    ofParameter<float> deltaMixA, deltaMixB, deltaWindow, deltaGain;
    ofJson deltaDefaults = ofJson::object();
    bool deltaRouted() const;
    void updateDelta(ofxSCServer* server, ServerState& st);
    void sendDelta(ServerState& st);
    void updateScramble();
    std::vector<float> panPositions() const;

    // ── Effects (as SlicerBox's chain) ───────────────────────────────────────
    bool stageOn(int s) const;
    float stageAmount(int s) const;   // a stage's On, fractional while a snapshot morph fades it
    std::string stageDef(int s) const;
    float stageTail(int s) const;
    float echoDelaySec() const;
    void sendStage(int s, ofxSCSynth* synth, ofxSCServer* server);
    void sendStageAll(int s);
    void updateStages(ofxSCServer* server, ServerState& st);
    void routeChain(ServerState& st);
    void requireSynthdefs();

    // ── Node parameters ──────────────────────────────────────────────────────
    ofParameter<bool> showWindow;
    ofParameter<int> voices, outChannels;
    ofParameter<int> spreadMode;          // placement: 0 Spread, 1 Channels, 2 Manual (Pan)
    ofParameter<int> spreadLayout;        // more than 2 speakers: 0 ring, 1 line
    ofParameter<float> spreadWidth, spreadCenter;
    ofParameter<std::vector<float>> panP; // Manual: each voice's position, 0..1 across the speakers
    ofParameter<std::vector<float>> gate, pitch;   // a gate's level is the note's velocity
    scScheduledInputs noteInputs;    // the timeline's notes, timetagged
    ofParameter<float> master;
    ofParameter<void> resetLFOs;

    // ── Voice ────────────────────────────────────────────────────────────────
    ofParameter<bool> drone;
    int sentDrone = -1;
    bool droning() const;          // Whether continuous playback is enabled
    void sendEnvMode(bool force = false);
    ofParameter<float> ampA, ampD, ampS, ampR, ampCA, ampCD, ampCR, velSens, glide, vibRate, vibRand, vibAmt;
    ofParameter<float> tremRate, tremRand, tremDepth, tremRound;
    ofJson tremDefaults = ofJson::object();
    ofParameter<std::vector<float>> transpose, detune;
    ofParameter<bool> scramble;
    ofParameter<bool> expandPitch;        // more voices than pitches: repeat them an octave higher each time (off: the last repeats)
    int pitchCableChannels = -1;          // the audio Pitch cable's values (sent to PCPitch as \pitch_k)
    // ── Oscillators and FM ───────────────────────────────────────────────────
    ofParameter<std::vector<float>> oscSaw, oscTri, oscPulse, oscPw, oscSine, oscSSaw, oscSSDetune,
                                    oscWhite, oscPink, oscDust, oscImpulse, oscAmp;
    ofParameter<float> oscDustDensity;    // octaves (0 .. -10) below the note: the dust's grain rate
    ofParameter<std::vector<float>> fmAmt, fmRatio;
    ofParameter<float> fmFb;
    ofParameter<bool> fmSnap;
    // ── Filters ──────────────────────────────────────────────────────────────
    ofParameter<int> fType, postType;
    ofParameter<int> filterModel, postFilterModel; // 0 classic, 1 the original PolyComb's ladder (PCFilterR)
    ofParameter<bool> combSaturation;
    ofParameter<float> combSatLevel;
    ofParameter<int> ringPitchSource;    // external / oscillator note bus / resonator map
    ofParameter<int> ringModel;          // 0 classic (a note), 1 the original PolyComb's (PCRingR: a ratio of the voice's note)
    ofParameter<float> ringWidth;
    ofParameter<float> ringRatio;          // one ratio for every voice (per-voice offsets: Ring Transpose)
    ofParameter<int> combModel;          // legacy snapshot field, fixed to 1 (PCCombR)
    ofParameter<std::vector<float>> cutoff, res, postCut, postRes;
    ofParameter<float> keyTrack;
    ofParameter<float> postKeyTrack;
    // ── Resonator, ring, shaper ──────────────────────────────────────────────
    ofParameter<int> combMode;                                 // 0 follow the note, 1 fixed
    ofParameter<std::vector<float>> combPitch, combTranspose, combDetune, combFeed, combMix, ringPitch, ringTranspose, ringDetune,
                                    ringMix, drive, shaperMix;
    ofParameter<float> combTone;
    ofParameter<float> combGlide, combVibRate, combVibRand, combVibAmt;
    // parts switched on / off (the oscillators: by their level)
    ofParameter<bool> fmOn, filterOn, combOn, ringOn, postOn, shaperOn;
    // ── Modulation: LFOs, envelopes, external sources, the matrix ──────────
    scModulation modulation;
    // ── Effects ──────────────────────────────────────────────────────────────
    ofParameter<bool> compOn, compAuto;
    ofParameter<float> compThreshold, compRatio, compKnee, compAttack, compRelease,
                       compMakeup, compRms, compHpf, compMix;
    ofJson compDefaults = ofJson::object();
    ofJson blurDefaults = ofJson::object();   // presets / snapshots from before Blur: off
    ofJson subDefaults = ofJson::object();
    ofParameter<bool> subOn;
    ofParameter<float> subLevel, subTranspose, subFineTune, subVibRate, subVibAmt, subVibRand, subA, subD, subS, subR, subCA, subCD, subCR;
    ofParameter<bool> eqOn;
    ofParameter<float> eqGain[5], eqFreq[5], eqShape[5];
    scEQEditor eqEditor;
    ofParameter<bool> echoOn, echoBeats;
    ofParameter<int> echoFilter;
    ofParameter<float> echoDelay, echoBeatVal, echoFeed, echoCutoff, echoResonance, echoPingPong, echoMix;
    // Echo Model 1: the original PolyComb's delay (PCEchoR): its lowpass (Hz), resonance,
    // highpass (Hz) in the loop, the lines' L / R time spread
    ofParameter<int> echoModel;
    ofParameter<float> echoHi, echoLo, echoRes, echoSpread;
    ofParameter<bool> blurOn;
    ofParameter<float> blurTime, blurAmount, blurDensity, blurSpread, blurLowCut, blurHighCut, blurMix;
    ofParameter<bool> rvbOn;
    ofParameter<float> rvbMix, rvbDecay, rvbSize, rvbPredelay, rvbLate, rvbSpread, rvbLowpass,
                       rvbHiDamp, rvbLoDamp, rvbModRate, rvbSpin, rvbWander;

    // ── State ────────────────────────────────────────────────────────────────
    float currentBpm = 120.0f;
    std::vector<int> scrambleOrder;
    std::vector<float> visValues;        // read back: amp levels and positions
    float nextVisRead = 0.0f;
    bool restoring = false;
    ofEventListeners listeners;

    // ── Window ───────────────────────────────────────────────────────────────
    void drawWindow();
    void drawEnvTab(float w);
    void drawOscTab(float w);
    void drawFilterTab(float w);
    void drawResonatorTab(float w);
    void drawRingPitchVector(float w);
    void drawFxTab(float w);
    void drawCompPanel(float w);
    void drawPanTab(float w);
    void drawPitchTab(float w);
    void drawMapColumn(MapParams& m, const char* id, float w, bool comb);
    void drawDeltaCard(float w);
    int panDragVoice = -1;
    void sectionHeader(const char* title, float w, ofParameter<bool>* on = nullptr, ImU32 accent = 0);
    void slider(ofParameter<float>& p, const char* label, float w, const char* fmt = "%.2f", bool log = false);
    void sliderVec(ofParameter<std::vector<float>>& p, const char* label, float w, const char* fmt = "%.2f", bool log = false);
    void sliderInt(ofParameter<int>& p, const char* label, float w);
    void combo(ofParameter<int>& p, const char* label, float w);   // its names: dropdowns
    void toggle(ofParameter<bool>& p, const char* label);
    void envelopeEditor(const char* id, ofParameter<float>& a, ofParameter<float>& d, ofParameter<float>& s,
                        ofParameter<float>& r, ofParameter<float>& ca, ofParameter<float>& cd, ofParameter<float>& cr,
                        float w, float h, ImU32 color, int visOffset);
    float labelW = 70.0f;
    std::shared_ptr<ofxOceanodeAbstractParameter> combPitchHandle;
    std::shared_ptr<ofxOceanodeAbstractParameter> voicesHandle;   // a connection drives Voices: snapshots leave it
    int userVoices = -1;              // the patch's own voice count (a preset, by hand): not a snapshot's
    bool voicesFromSnapshot = false;
    void drawAmpCard(float w);

    // Temporary renderers have their own count-dependent pitch maps, envelopes
    // and modulation, but no UI/library work or post-spreader effects.
    bool voiceRenderer = false, applyingSnapshot = false, syncingVoiceMorph = false;
    scPolyComb* transposeOwner = this;   // Global Transpose: a voice-morph renderer reads its parent's choice
    std::map<std::string, std::function<void()>> morphSends;   // queued while a snapshot applies, one per part
    int morphRenderVoices = 0, voiceAliasCount = 0, morphDomainVoices = 0;
    struct VoicePeer {
        std::unique_ptr<scPolyComb> node;
        ofJson lastSound;
        float unusedSince = -1;
    };
    std::map<int, VoicePeer> voicePeers;
    std::map<ofxSCServer*, int> rendererBefore; // peer root belongs before the parent's mixer
    std::vector<float> voiceMorphWeights;
    int voiceHandoff = 0; // 0: morph, 2: remove mixer after adopting the endpoint bank
    float voiceEndpointSince = -1, voiceHandoffAt = -1;
    void prepareVoiceMorph(const ofJson& a, const ofJson& b, float t);
    void syncVoiceMorph();
    void updateVoiceMorph(ofEventArgs&);
    void releaseVoiceMorphServer(ofxSCServer*, ServerState&);
    void releaseVoiceMorph();
    bool adoptVoiceBank(int count);
    void configureVoiceMix(ServerState&);


    // ── Snapshots (scSnapshots, shared by the synths): every window control,
    //    the routing matrix, the LFO / envelope / ext counts, voice count; not
    //    Channels, Master, the speaker layout, the Ext values, nor controls
    //    driven by a connection. ──
    scSnapshots snaps;
    scDropdowns dropdowns;   // the combos' names (a published combo is a dropdown)
    bool snapExcluded(const std::string& key) const;
    bool drivenByConnection(const std::string& key) const;
    ofJson captureSnapshot() const;
    void applyState(const ofJson& a, const ofJson& b, float t, bool sw);   // t 0: a, 1: b; sw: switches take b's
    ofJson snapshotOf(const ofJson& saved) const;   // a saved preset's sound, as a snapshot
    ofJson morphFrom, morphTo;                      // a MacroMixer morph: the start, the arrival
};
