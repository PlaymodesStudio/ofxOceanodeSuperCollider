//
//  scPolyComb.h
//  ofxOceanodeSuperCollider
//
//  PolyComb: a polyphonic comb-resonator synth, N voices -> N channels.
//
//  • The node shows Show, Voices, Gate, Pitch, Velocity, Master, Reset and
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
//  • Effects after the voices: MixerPro's EQ, echo and SpaceMaster reverb
//    (mixerProEQ<N>, mixerProEcho<N>, mixerProRvb<N>), each created the first
//    time it is switched on and paused once its tail is over.
//

#pragma once

#include "scNode.h"
#include "serverManager.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "ofxSCGroup.h"
#include "scEQEditor.h"
#include "scModulation.h"
#include "scAudioParam.h"
#include "imgui.h"
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class scPolyComb : public scNode {
public:
    // its window (SC Mixer Pro's "Go to origin synth window" opens and focuses it)
    std::string getWindowTitle() override { return "PolyComb " + ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return &showWindow; }
private:
public:
    static constexpr int MAX_VOICES = 16;
    static constexpr int NUM_LFOS = scModulation::MAX_LFOS;
    static constexpr int NUM_ENVS = scModulation::MAX_ENVS;
    static constexpr int NUM_EXT = scModulation::MAX_EXT;
    // sources: LFO 1-8, Env 1-4, Velocity, Random, Ext 1-8, Voice
    static constexpr int NUM_SOURCES = scModulation::NUM_SOURCES;
    static constexpr int SRC_ENV = scModulation::SRC_ENV, SRC_VEL = scModulation::SRC_VEL, SRC_RND = scModulation::SRC_RND,
                         SRC_EXT = scModulation::SRC_EXT, SRC_VOICE = scModulation::SRC_VOICE;
    static constexpr int NUM_TARGETS = 14;
    enum Stage { StEq, StEcho, StRvb, NumStages };

    explicit scPolyComb(std::vector<serverManager*> servers);
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

private:
    // ── Per server: a root group, one subgroup per slot (in this order, so
    //    any part can come and go without breaking the order), small synths ──
    static constexpr int W = 16;                       // every voice bus is 16 wide
    static constexpr int VIS_AMP = 0;                  // the display bus: amp env levels
    static constexpr int VIS_PAN = W;                  //   and the voices' positions
    static constexpr int ZERO_TARGET = NUM_TARGETS;    // a modulation bus nobody writes
    enum Slot { SlGate, SlSrc, SlRoute, SlPitch, SlFm, SlOsc, SlFilter, SlComb, SlRing, SlPost, SlShaper,
                SlAmp, SlSpread, SlEq, SlEcho, SlRvb, NumSlots };
    enum Osc { OSaw, OTri, OPulse, OSine, OSSaw, OWhite, OPink, ODust, OImp, NumOsc };
    enum Ins { IFilter, IComb, IRing, IPost, IShaper, NumIns };
    struct Part { ofxSCSynth* synth = nullptr; bool running = false; float offAt = -1.0f; };
    struct Fx { ofxSCSynth* synth = nullptr; ofxSCBus* in = nullptr; bool running = false; float offAt = -1.0f; };
    struct ServerState {
        ofxSCGroup* root = nullptr;
        std::array<ofxSCGroup*, NumSlots> groups{};
        bool created = false;
        ofxSCSynth* gateS = nullptr;                    // ModGate: gates, velocity, random
        ofxSCSynth* ampEnv = nullptr;                   // ModEnvAr: the amp envelope
        ofxSCServer* server = nullptr;
        bool modCreated = false;                        // the shared modulation's synths and buses
        ofxSCSynth* pitchS = nullptr;                   // PCPitch
        Part fm;                                        // PCFm
        std::array<Part, NumOsc> osc;                   // PCOsc<Type>, the ones audible
        std::array<Part, NumIns> ins;                   // PCFilter / Comb / Ring / Filter (post) / Shaper
        ofxSCSynth* amp = nullptr;                      // PCAmp
        ofxSCSynth* spread = nullptr;                   // PolyCombSpread<C>
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
    };
    std::vector<serverManager*> servers;
    std::map<ofxSCServer*, ServerState> states;
    // Gate, Pitch, Velocity: Oceanode values or audio cables
    scAudioParam gateIn, pitchIn, velIn;
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
    bool publishedSeparator = false;
    template<typename T> void reg(ofParameter<T>& p, std::function<void()> send);
    void publish(const std::string& key);
    void unpublish(const std::string& key);
    bool isConnected(const std::string& key) const;
    void publishMenu(const std::string& key);
    void publishedUnderline(const std::string& key) const;

    // ── Engine ───────────────────────────────────────────────────────────────
    int n() const { return voices.get(); }
    std::string def(const std::string& name) const { return name + ofToString(n()); }
    int src(const ServerState& st, int s) const;
    int mod(const ServerState& st, int t) const;
    void forStates(const std::function<void(ofxSCServer*, ServerState&)>& f);
    void sendVec(ofxSCSynth* s, const std::string& name, const std::vector<float>& v);
    // what each part needs, sent to it (on creation and when its controls change)
    void sendGate(ServerState& st);
    void sendAmpEnv(ServerState& st);
    void sendPitch(ServerState& st);
    void sendFm(ServerState& st);
    void sendOsc(ServerState& st, int o);
    void sendIns(ServerState& st, int i);
    void sendAmp(ServerState& st);
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
    void rebuildOutput();
    std::vector<float> processedPitch() const;
    void updateScramble();
    std::vector<float> panPositions() const;

    // ── Effects (as SlicerBox's chain) ───────────────────────────────────────
    bool stageOn(int s) const;
    std::string stageDef(int s) const;
    float stageTail(int s) const;
    float echoDelaySec() const;
    void sendStage(int s, ofxSCSynth* synth);
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
    ofParameter<std::vector<float>> gate, pitch, velocity;
    ofParameter<float> master;
    ofParameter<void> resetLFOs;

    // ── Voice ────────────────────────────────────────────────────────────────
    ofParameter<int> envMode;                                  // 0 Auto, 1 Gated, 2 Drone
    int sentDrone = -1;
    bool gateTouched = false;          // Auto: gated once Gate has had a value (or a cable)
    bool droning() const;          // Drone, or Auto with nothing on Gate
    void sendEnvMode(bool force = false);
    ofParameter<float> ampA, ampD, ampS, ampR, ampCA, ampCD, ampCR, velSens, glide, vibRate, vibRand, vibAmt;
    ofParameter<std::vector<float>> transpose, detune;
    ofParameter<bool> scramble;
    // ── Oscillators and FM ───────────────────────────────────────────────────
    ofParameter<std::vector<float>> oscSaw, oscTri, oscPulse, oscPw, oscSine, oscSSaw, oscSSDetune,
                                    oscWhite, oscPink, oscDust, oscImpulse, oscAmp;
    ofParameter<std::vector<float>> fmAmt, fmRatio;
    ofParameter<float> fmFb;
    ofParameter<bool> fmSnap;
    // ── Filters ──────────────────────────────────────────────────────────────
    ofParameter<int> fType, postType;
    ofParameter<std::vector<float>> cutoff, res, postCut, postRes;
    ofParameter<float> keyTrack;
    // ── Resonator, ring, shaper ──────────────────────────────────────────────
    ofParameter<int> combMode;                                 // 0 follow the note, 1 fixed
    ofParameter<std::vector<float>> combPitch, combTranspose, combDetune, combFeed, combMix, ringPitch, ringTranspose, ringDetune,
                                    ringMix, drive, shaperMix;
    ofParameter<float> combTone;
    // parts switched on / off (the oscillators: by their level)
    ofParameter<bool> fmOn, filterOn, combOn, ringOn, postOn, shaperOn;
    // ── Modulation: LFOs, envelopes, external sources, the matrix ──────────
    scModulation modulation;
    // ── Effects ──────────────────────────────────────────────────────────────
    ofParameter<bool> eqOn;
    ofParameter<float> eqGain[5], eqFreq[5], eqShape[5];
    scEQEditor eqEditor;
    ofParameter<bool> echoOn, echoBeats;
    ofParameter<int> echoFilter;
    ofParameter<float> echoDelay, echoBeatVal, echoFeed, echoCutoff, echoResonance, echoPingPong, echoMix;
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
    void drawFxTab(float w);
    void drawPanTab(float w);
    int panDragVoice = -1;
    void sectionHeader(const char* title, float w, ofParameter<bool>* on = nullptr, ImU32 accent = 0);
    void slider(ofParameter<float>& p, const char* label, float w, const char* fmt = "%.2f", bool log = false);
    void sliderVec(ofParameter<std::vector<float>>& p, const char* label, float w, const char* fmt = "%.2f", bool log = false);
    void sliderInt(ofParameter<int>& p, const char* label, float w);
    void combo(ofParameter<int>& p, const char* label, float w, const std::vector<const char*>& items);
    void toggle(ofParameter<bool>& p, const char* label);
    void envelopeEditor(const char* id, ofParameter<float>& a, ofParameter<float>& d, ofParameter<float>& s,
                        ofParameter<float>& r, ofParameter<float>& ca, ofParameter<float>& cd, ofParameter<float>& cr,
                        float w, float h, ImU32 color, int visOffset);
    float labelW = 70.0f;
    std::shared_ptr<ofxOceanodeAbstractParameter> combPitchHandle;
    void drawAmpCard(float w);

    // ── Snapshots: 16 slots of the sound (every window control, the routing
    //    matrix, the LFO / envelope / ext counts; not Voices, Channels, Master,
    //    the speaker layout, the Ext values, nor controls driven by a
    //    connection). A shared library (data/nodeSnapshots/PolyComb/
    //    snapshot_<k>.json, every PolyComb sees it), named banks in its banks/
    //    folder, and a factory collection. Recall jumps, or glides over Morph
    //    Time; Morph blends snapshot A into B. ──
    static constexpr int NUM_SNAPS = 128;
    std::array<ofJson, NUM_SNAPS> snapSlots;
    std::array<bool, NUM_SNAPS> snapUsed{};
    int activeSnap = -1;
    bool snapRecalling = false;
    ofParameter<int> snapshotP, morphAP, morphBP;
    ofParameter<float> morphP, morphTimeP;
    ofParameter<bool> morphBeatsP, morphGlideP;
    bool morphRun = false;
    float morphT0 = 0.0f, morphDur = 1.0f, morphProgress = 0.0f;
    int morphTarget = -1;
    ofJson morphFrom, morphTo, defaultSnap;
    bool snapLoadDialog = false;
    std::string snapMsg;
    char snapNameBuf[48] = "";
    char snapBankName[64] = "my bank";
    std::filesystem::file_time_type snapLibTime{};
    float snapLibCheckAt = 0.0f;
    bool snapExcluded(const std::string& key) const;
    bool drivenByConnection(const std::string& key) const;
    ofJson captureSnapshot() const;
    void applyState(const ofJson& a, const ofJson& b, float t);   // t 0: a, 1: b
    void storeSnapshot(int slot, const std::string& name = "");
    void recallSnapshot(int slot);
    void startGlide(int slot);
    void applyAB();
    std::string snapName(int k) const;
    std::string snapDir() const;
    std::filesystem::file_time_type snapSetTime() const;
    void saveSnapshotLibrary();
    void loadSnapshotLibrary();
    void saveSnapshotBank(const std::string& path);
    bool loadSnapshotBank(const std::string& path);
    std::vector<ofJson> factorySnapshots() const;
    void loadFactory();
    void drawSnapshotBar(float w);
};
