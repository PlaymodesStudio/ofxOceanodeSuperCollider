//
//  scAnalogBox.h
//  ofxOceanodeSuperCollider
//
//  AnalogBox: a polyphonic subtractive synth, N voices -> N channels.
//
//  • The node shows Show, Voices, Channels, Gate, Pitch, Velocity, Master,
//    Reset and whatever is published. Gate takes Oceanode values and an audio
//    cable at the same time. The dockable window has everything else, in tabs
//    laid out in columns; right-click any control there to type a value,
//    publish it to the node or modulate it.
//  • Per voice (synthdefs/Defaults/AnalogBox): up to 6 oscillators, added at
//    will, each a MorphOsc (ugens/OceanodeAnalog: the LFOs' Width / Skew /
//    Round shape as a band-limited audio oscillator) with octave, semitones,
//    cents, ratio, hard sync to oscillator 1 and FM from any oscillator; noise
//    (with a color), ring 1 x 2 -> up to 4 filters, added at will, in series or
//    in parallel: AnalogFilter (SVF or ladder, 2 or 4 poles, LP / BP / HP /
//    notch, or a comb; drive, saturation, feedback; a high-pass before the
//    first) -> the amp envelope (VCA).
//  • Modulation: the shared system (scModulation): LFOs, envelopes, velocity,
//    a random value per note, external inputs and the voice index, routed to
//    13 targets by a matrix (and any other control: right-click, Modulate).
//  • Effects after the voices, at the speaker count: sine shaper, saturation,
//    ring modulator, frequency shifter, chorus, EQ, echo, MicroSpace reverb (each created the
//    first time it is switched on, paused once its tail is over).
//  • Snapshots and a factory collection, as PolyComb's.
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
#include "../../../ui/scDropdowns.h"
#include "../shared/scAudioParam.h"
#include "../../../timing/scScheduledInputs.h"
#include "imgui.h"
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class scAnalogBox : public scNode {
    scKeyboardPiano keyboardPiano;
public:
    // its window (SC Mixer Pro's "Go to origin synth window" opens and focuses it)
    std::string getWindowTitle() override { return "AnalogBox " + ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return &showWindow; }
private:
public:
    static constexpr int MAX_VOICES = 16;
    static constexpr int MAX_OSCS = 6;
    static constexpr int MAX_FILTERS = 4;
    static constexpr int NUM_LFOS = scModulation::MAX_LFOS;
    static constexpr int NUM_ENVS = scModulation::MAX_ENVS;
    static constexpr int NUM_EXT = scModulation::MAX_EXT;
    static constexpr int NUM_SOURCES = scModulation::NUM_SOURCES;
    static constexpr int SRC_ENV = scModulation::SRC_ENV, SRC_VEL = scModulation::SRC_VEL, SRC_RND = scModulation::SRC_RND,
                         SRC_EXT = scModulation::SRC_EXT, SRC_VOICE = scModulation::SRC_VOICE;
    // (new targets go at the end: presets keep the matrix by index)
    enum Target { TPitch, TSlave, TWidth, TSkew, TRound, TFm, TNoise, TCut, TRes, TDrive, TFb, TAmp, TPan,
                  TCut2, TRes2, TCut3, TRes3, TCut4, TRes4, NUM_TARGETS };
    enum Stage { StShaper, StSat, StRing, StShift, StChorus, StEq, StEcho, StRvb, NumStages };

    explicit scAnalogBox(std::vector<serverManager*> servers);
    ~scAnalogBox();

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
#ifdef OFXOCEANODE_HAS_TIMELINE_INSTRUMENT
    // Gate / Pitch / Velocity, one note per voice (timeline piano rolls)
    bool getTimelineInstrument(ofxOceanodeTimelineInstrument& out) override;
#endif

private:
    // ── Per server: a root group, one subgroup per slot (in this order) ─────
    static constexpr int W = 16;                       // every voice bus is 16 wide
    static constexpr int VIS_PAN = W;                  //   the voices' positions
    static constexpr int VIS_CUT = 2 * W;              //   their cutoffs (notes), then resonances (W each)
    enum Slot { SlGate, SlSrc, SlRoute, SlPitch, SlOsc, SlOscLast = SlOsc + MAX_OSCS - 1, SlNoise, SlFilter, SlAmp,
                SlSpread, SlFx, SlFxLast = SlFx + NumStages - 1, NumSlots };
    struct Fx { ofxSCSynth* synth = nullptr; ofxSCBus* in = nullptr; bool running = false; float offAt = -1.0f; };
    struct ServerState {
        ofxSCGroup* root = nullptr;
        std::array<ofxSCGroup*, NumSlots> groups{};
        bool created = false;
        ofxSCServer* server = nullptr;
        bool modCreated = false;
        ofxSCSynth* gateS = nullptr;                    // ModGate: gates, velocity, random
        ofxSCSynth* pitchS = nullptr;                   // ABPitch
        std::array<ofxSCSynth*, MAX_OSCS> osc{};        // ABOsc, one per oscillator in use
        ofxSCSynth* noise = nullptr;                    // ABNoise (while its level is up)
        ofxSCSynth* ring = nullptr;                     // ABRing (two oscillators, its level up)
        ofxSCSynth* filter = nullptr;                   // ABFilter (filter 1)
        std::array<ofxSCSynth*, MAX_FILTERS - 1> moreFilters{};   // filters 2.. (in use)
        std::array<ofxSCBus*, MAX_FILTERS - 1> chainBus{};        // audio: in series, filter k's output
        ofxSCSynth* amp = nullptr;                      // ABAmp
        ofxSCSynth* spread = nullptr;                   // PolyCombSpread<C>
        std::array<Fx, NumStages> fx;
        ofxSCBus* gateBus = nullptr;                    // audio: the gates
        ofxSCBus* noteBus = nullptr;                    // control, 2W: the note (modulated), the note before
        ofxSCBus* mixBus = nullptr;                     // audio: the oscillators, noise, ring
        ofxSCBus* poolPitch = nullptr;                  // audio: the pool voices' pitches (ABGateAlloc)
        std::array<ofxSCBus*, MAX_OSCS> raw{};          // audio: each oscillator alone (FM, ring)
        std::array<ofxSCBus*, MAX_OSCS> freq{};         // control: each oscillator's frequency (sync)
        ofxSCBus* filtBus = nullptr;                    // audio: the filter's output
        ofxSCBus* voiceBus = nullptr;                   // audio: the voices, to the spreader
        ofxSCBus* visBlock = nullptr;                   // control: positions, cutoffs, resonances (the window)
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
    scTimelineMenu timeline;              // "Add to Timeline" from the window
    bool publishedSeparator = false;
    template<typename T> void reg(ofParameter<T>& p, std::function<void()> send);
    void publish(const std::string& key);
    void unpublish(const std::string& key);
    bool isConnected(const std::string& key) const;
    void publishMenu(const std::string& key);
    void publishedUnderline(const std::string& key) const;

    // ── Engine ───────────────────────────────────────────────────────────────
    // Voices: the gate / pitch / velocity inputs. Gated, with Extra Voices, the
    // synth runs a pool of min(16, 2 x Voices) voices (ABGateAlloc hands each
    // new note the next one): a note never restarts a voice still releasing
    int nIn() const { return voices.get(); }
    bool allocating() const { return extraVoices.get() && !droning() && voices.get() < MAX_VOICES; }
    int n() const { return allocating() ? std::min(MAX_VOICES, 2 * voices.get()) : voices.get(); }
    int oscs() const { return std::clamp(oscCount.get(), 1, MAX_OSCS); }
    std::string def(const std::string& name) const { return name + ofToString(n()); }
    int src(const ServerState& st, int s) const;
    int mod(const ServerState& st, int t) const;
    void forStates(const std::function<void(ofxSCServer*, ServerState&)>& f);
    void sendVec(ofxSCSynth* s, const std::string& name, const std::vector<float>& v);
    void sendGate(ServerState& st);
    void sendPitch(ServerState& st);
    void sendOsc(ServerState& st, int k);
    void sendNoise(ServerState& st);
    void sendRing(ServerState& st);
    void sendFilter(ServerState& st);           // every filter (and its buses: series / parallel)
    void sendFilterK(ServerState& st, int k);
    int filters() const { return std::clamp(filterCount.get(), 1, MAX_FILTERS); }
    void sendAmp(ServerState& st);
    void sendSpread(ServerState& st);
    bool noiseWanted() const;
    bool ringWanted() const;
    void updateParts(ofxSCServer* server, ServerState& st);    // oscillators, noise, ring: as many as wanted
    void createAll(ofxSCServer* server, ServerState& st);
    void freeSynths(ServerState& st);
    void freeAll(ofxSCServer* server, ServerState& st);
    void ensureBuses(ofxSCServer* server, ServerState& st);
    void rebuildAll();
    void rebuildOutput();
    std::vector<float> panPositions() const;

    // ── Effects ──────────────────────────────────────────────────────────────
    float stageOn(int s) const;
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
    ofParameter<int> spreadMode, spreadLayout;
    ofParameter<float> spreadWidth, spreadCenter;
    ofParameter<std::vector<float>> panP;
    ofParameter<std::vector<float>> gate, pitch, velocity;
    scScheduledInputs noteInputs;    // the timeline's notes, timetagged
    ofParameter<float> master;
    ofParameter<void> resetLFOs;

    // ── Voice ────────────────────────────────────────────────────────────────
    ofParameter<int> envMode;                                  // 0 Auto, 1 Gated, 2 Drone
    ofParameter<bool> extraVoices;
    int sentDrone = -1;
    bool droning() const;
    void sendEnvMode(bool force = false);
    // no amp envelope of its own: an envelope routed to Amp is the VCA (else it drones)
    bool vcaOn() const;
    bool ampDirty = false;
    bool gateTouched = false;          // Auto: gated once Gate has had a value (or a cable)
    ofParameter<float> velSens, glide, vibRate, vibRand, vibAmt, drift;
    ofParameter<std::vector<float>> transpose, detune;
    // ── Oscillators ──────────────────────────────────────────────────────────
    ofParameter<int> oscCount;
    struct Osc {
        ofParameter<int> oct, fmFrom;
        ofParameter<float> semi, fine, ratio, skew, round, width, noise;
        ofParameter<bool> centered, sync;
        ofParameter<std::vector<float>> level, fm;
    };
    std::array<Osc, MAX_OSCS> osc;
    ofParameter<std::vector<float>> noiseLevel, ringLevel;
    ofParameter<float> noiseColor;
    // ── Filter ───────────────────────────────────────────────────────────────
    ofParameter<bool> filterOn;
    ofParameter<int> fMode, fType;
    ofParameter<std::vector<float>> cutoff, res;
    ofParameter<float> keyTrack, fDrive, fSat, fFeedback, hpf, fLevel;
    ofParameter<int> filterCount, filterRouting;          // 1..4; 0 series, 1 parallel
    struct MoreFilter {
        ofParameter<bool> on;
        ofParameter<int> mode, type;
        ofParameter<std::vector<float>> cutoff, res;
        ofParameter<float> keyTrack, drive, sat, feedback, level;
    };
    std::array<MoreFilter, MAX_FILTERS - 1> moreF;
    // filter k's settings (0: the first's own parameters)
    struct FilterRef {
        ofParameter<bool>* on; ofParameter<int>* mode; ofParameter<int>* type;
        ofParameter<std::vector<float>>* cutoff; ofParameter<std::vector<float>>* res;
        ofParameter<float>* keyTrack; ofParameter<float>* drive; ofParameter<float>* sat;
        ofParameter<float>* feedback; ofParameter<float>* level;
    };
    FilterRef filterRef(int k);
    void drawFilterCard(int k, float w);
    // ── Modulation ───────────────────────────────────────────────────────────
    scModulation modulation;
    // ── Effects ──────────────────────────────────────────────────────────────
    ofParameter<bool> shaperOn, satOn, ringFxOn, chorusOn, shiftOn, shiftBoth;
    ofParameter<float> shiftHz, shiftSpread, shiftFeed, shiftDelay, shiftTone, shiftMix;
    ofParameter<float> shaperDrive, shaperMix;
    ofParameter<float> satDrive, satBias, satTone, satOutput, satMix;
    ofParameter<float> ringFreq, ringSpread, ringMix;
    ofParameter<float> chorusRate, chorusDepth, chorusDelay, chorusSpread, chorusMix;
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
    std::vector<float> visValues;
    float nextVisRead = 0.0f;
    bool restoring = false;
    ofEventListeners listeners;

    // ── Window ───────────────────────────────────────────────────────────────
    void drawWindow();
    void drawOscTab(float w);
    void drawOscCard(int k, float w);
    void drawFilterTab(float w);
    void drawFxTab(float w);
    void drawPanTab(float w);
    int panDragVoice = -1;
    void sectionHeader(const char* title, float w, ofParameter<bool>* on = nullptr, ImU32 accent = 0);
    void slider(ofParameter<float>& p, const char* label, float w, const char* fmt = "%.2f", bool log = false);
    void sliderVec(ofParameter<std::vector<float>>& p, const char* label, float w, const char* fmt = "%.2f", bool log = false);
    void sliderInt(ofParameter<int>& p, const char* label, float w);
    void combo(ofParameter<int>& p, const char* label, float w);   // its names: dropdowns
    void syncDropdowns();
    void toggle(ofParameter<bool>& p, const char* label);
    float labelW = 70.0f;
    float matrixCell(int source, int target) const;
    void setMatrixCell(int source, int target, float v);

    // ── Snapshots (scSnapshots, shared by the synths) ──
    scSnapshots snaps;
    scDropdowns dropdowns;   // the combos' names (a published combo is a dropdown)
    ofJson defaultSnap;                   // the factory snapshots start from it
    bool snapExcluded(const std::string& key) const;
    bool drivenByConnection(const std::string& key) const;
    ofJson captureSnapshot() const;
    void applyState(const ofJson& a, const ofJson& b, float t, bool sw);   // sw: switches take b's
    std::vector<ofJson> factorySnapshots() const;
};
