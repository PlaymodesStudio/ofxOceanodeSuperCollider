//
//  scModulation.h
//  ofxOceanodeSuperCollider
//
//  The modulation shared by the synth nodes (PolyComb, SampleBox, ...): LFOs,
//  envelopes and external sources, added one by one, routed to the node's
//  targets by a matrix.
//
//  • Server side: small synths from synthdefs/Defaults/Modulation, N voices
//    wide: ModLfo<N> and ModEnv<N> (the sources, each writing its own bus in
//    the source block) and one ModRoute<N> per routed matrix cell (source *
//    amount * the target's range, added into the target's bus in the
//    modulation block). Nothing is created that is not used; a target left
//    without routes is zeroed. The node reads its targets' buses (W wide,
//    N channels used) and writes the gates (audio, N) the envelopes follow,
//    and the velocity / random sources if it has them.
//  • Node side: the parameters (publishable through the node's own
//    registry), the LFOs' per-voice phase offsets (Oceanode's indexer), the
//    tabs (Envelopes, LFOs, Routing) and the "Modulate with" right-click
//    entries for the node's target controls.
//
//  Sources: LFO 1-8, Env 1-4, Velocity, Random, Ext 1-8, Voice.
//

#pragma once

#include "ofMain.h"
#include "ofxOceanodeShared.h"
#include "ofxSCServer.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "baseIndexer.h"
#include "imgui.h"
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class ofxOceanodeNodeModel;
class ofxOceanodeAbstractParameter;

class scModulation {
public:
    static constexpr int MAX_LFOS = 8, MAX_ENVS = 4, MAX_EXT = 8, W = 16;
    static constexpr int NUM_SOURCES = MAX_LFOS + MAX_ENVS + 2 + MAX_EXT + 1;
    static constexpr int SRC_ENV = MAX_LFOS, SRC_VEL = MAX_LFOS + MAX_ENVS, SRC_RND = SRC_VEL + 1,
                         SRC_EXT = SRC_RND + 1, SRC_VOICE = SRC_EXT + MAX_EXT;
    // room for a host's own sources after the built-in ones (extraSources)
    static constexpr int MAX_EXTRA_SOURCES = 4, MAX_SOURCES = NUM_SOURCES + MAX_EXTRA_SOURCES;

    // unipolar: an LFO moves it 0..1 x amount (positions: +1 reaches the end),
    // else -1..1 around the control's value
    struct Target { std::string name; float range; std::string unit; bool unipolar = false; };
    // What the module needs from its node
    struct Host {
        ofxOceanodeNodeModel* node = nullptr; // optional live LFO output pins
        // registers a parameter in the node's publishable registry; send runs on every change
        std::function<void(ofParameter<float>&, std::function<void()>)> regF;
        std::function<void(ofParameter<int>&, std::function<void()>)> regI;
        std::function<void(ofParameter<bool>&, std::function<void()>)> regB;
        std::function<void(ofParameter<std::vector<float>>&, std::function<void()>)> regV;
        std::function<void(const std::string& key)> publishMenu;          // the right-click menu of a control
        std::function<void(const std::string& key)> publishedUnderline;
        std::function<void(const std::string& key)> unpublish;
        std::function<const std::vector<std::string>&()> publishedKeys;
        std::function<int()> voices;                                      // N
        // any control of the node by key (for "Modulate" on every control); null: not modulatable
        std::function<ofAbstractParameter*(const std::string& key)> control;
        // Resend voice controls even when only voices after the first changed.
        std::function<void()> voiceParamsChanged;
    };

    scModulation() = default;
    ~scModulation();
    void saveLfoOutputs(ofJson& j) const;
    void loadLfoOutputs(const ofJson& j); // before restoring node connections
    bool hasLfoOutputs() const;
    // Set before setup: each LFO also gets "across partials" phase controls
    // (Partial Spread, Partial Random), for a node that runs its own copies of
    // the LFOs per partial (AdditiveBox) with lfoTable / lfoRate / lfoPhase
    bool partialLfos = false;
    bool unifiedLfoModels = false; // PolyComb: Off / Classic / Noise / Random
    // Set before setup: the most voices the node can have (the indexer ranges; GrainBox: 500)
    int maxVoices = 16;
    // The bus stride of every source and target: as many voices as the host
    // can have. W (16) for every host but PolyComb (32). Set before create().
    int busWidth = W;
    // A host's own sources after the built-in ones (PolyComb: "Delta"): matrix
    // rows NUM_SOURCES..., written by the host on sourceBus(server, NUM_SOURCES + k).
    // Set before setup(). None: the layout every other host has.
    std::vector<std::string> extraSources;
    int sourceCount() const { return NUM_SOURCES + std::min((int)extraSources.size(), (int)MAX_EXTRA_SOURCES); }
    // Set before setup: appended to every parameter's name (GrainBox's engines: " 2"...)
    std::string nameSuffix;
    // How many LFOs / envelopes / external sources the node runs (the "+ Add" buttons stop there)
    int maxLfos = MAX_LFOS, maxEnvs = MAX_ENVS, maxExts = MAX_EXT;
    // Set before setup: GrainBox's extras per LFO: "Smp" (the cycle in
    // lengths of the sample region: regionSec()) and "Steps" (quantize)
    bool grainExtras = false;
    std::function<float()> regionSec;
    // A node that runs the sources itself (no create()): its live values for the window
    std::function<float(int source, int voice)> liveHook;
    // An LFO the node runs per partial (AdditiveBox): its value now on each partial
    // (0..1, voice 1), drawn as the card's bars instead of the voices'; empty: per voice
    std::function<std::vector<float>(int lfo)> partialBars;
    // A route the node runs itself: no ModRoute for it (AdditiveBox: LFO ->
    // per-partial targets, run inside the voices)
    std::function<bool(int source, int target)> skipRoute;
    // Anything an own LFO copy depends on changed (an LFO setting, the matrix,
    // the tempo, a reset, a preset)
    std::function<void()> changed;
    // perVoiceLfo: each voice its own LFO phase (the indexer); false: one LFO for all
    void setup(Host host, std::vector<Target> targets, bool perVoiceLfo, bool hasVelocity, bool hasRandom);

    // ── Server side ──────────────────────────────────────────────────────────
    // the node's groups for the sources and for the routes (sources before
    // routes; the node reads the targets after, or a block later); gateBus:
    // an audio bus of N gates the envelopes follow
    void create(ofxSCServer* server, int sourceGroup, int routeGroup, int gateBus);
    void free(ofxSCServer* server, bool sendFree = true);     // the synths and buses
    // Transfer a running, identically configured voice bank without restarting
    // its LFOs/envelopes. Frontend parameter bindings remain with this module.
    bool hasVoiceBank(ofxSCServer* server, int voices) const;
    bool adoptServerState(scModulation& source, ofxSCServer* server);
    void rebuild(ofxSCServer* server);                        // a new voice count: the synths again
    void setGateBus(ofxSCServer* server, int gateBus);
    int targetBus(ofxSCServer* server, int target) const;     // W wide, N channels used
    int sourceBus(ofxSCServer* server, int source) const;
    void resend();                                            // everything again (NRT)
    void setBpm(float bpm);
    void resetLfos();
    void voicesChanged();                                     // a new voice count (no server synths: GrainBox)
    int resets() const { return resetCounter; }
    // The LFOs for a node's own copies (first value of vector settings)
    std::vector<float> lfoTable(int li, int size) const;      // one cycle, 0..1, last point = first
    float lfoRate(int li, int voice) const;                   // Hz
    float lfoPhase(int li, int voice) const;                  // start phase + the voice's offset
    float lfoPartialSpread(int li) const { return lfoPSpread[li].get(); }
    float lfoPartialRandom(int li) const { return lfoPRand[li].get(); }
    float lfoInitPhase(int li) const { return lfoInit[li].get(); }
    float lfoVoiceOffset(int li, int voice) const;            // the indexer's offset of a voice
    std::vector<float> lfoVoiceOffsets(int li) const;         // every voice's
    float lfoSampleHold(int li) const;                        // S&H steps per second (first value), 0 off
    float lfoShGlide(int li) const { return lfoSHGlide[li].get(); }   // S&H glide (s)
    // Noise (smooth 2D random values): x the rising counter, y = index x Scale + seed
    static float noise2(float x, float y, float salt = 0.0f);   // salt: each LFO (voice) its own field
    bool lfoIsNoise(int li) const { return effectiveNoise(li); }
    float lfoNoiseScale(int li) const { return lfoNScale[li].get(); }
    float lfoNoiseSeed(int li) const { return li * 17.31f; }
    std::vector<float> lfoTransfer(int li, int size) const;   // Gain / Steps / Pow / BiPow as a curve
    struct EnvValues { float a, d, s, r, ca, cd, cr; };
    EnvValues envValues(int e) const;
    std::vector<float> extValues(int k) const;                // one per voice (n)
    void update();                                            // reads the live values (while shown)
    bool hasLiveValues() const { return !live.empty(); }
    float liveValue(int source, int voice) const;
    bool showing = false;                                     // set by the node: read live values

    // ── Node side ────────────────────────────────────────────────────────────
    int lfoCount = 0, envCount = 0, extCount = 0;
    std::vector<float> matrix;                                // sources x targets
    std::map<std::string, int> targetOfKey;                   // the node's controls that are targets
    std::string sourceName(int s) const;
    bool sourceUsed(int s) const;
    ImU32 sourceColor(int s) const;
    const std::vector<Target>& targets() const { return targetList; }
    // A matrix saved with fewer targets (rows of sources x targets): the new columns at 0
    static std::vector<float> fitMatrix(const std::vector<float>& m, int sources, int targets);
    void sendMatrix();
    void modulateMenu(const std::string& key);                // inside a control's right-click menu
    // the mod envelopes (cards, 4 per row); lead: a card of the node's own first (PolyComb's amp envelope)
    void drawEnvelopeCards(float w, const std::vector<std::function<void(float)>>& leads);
    void drawEnvelopes(float w, const std::function<void(float)>& lead = nullptr);
    // the sources created / freed after lfoCount / envCount changed, and the routes
    void refresh();
    void drawLfos(float w);
    void drawRouting(float w);
    // ── Any control as a target ("Modulate" in its right-click menu) ─────────
    // The control's own value moves: its value (the base) + the routed sources
    // x its range (amount 1: the whole range), written every frame (so steps
    // at the frame rate). The base is what is saved; the node's own saving
    // calls patchBases on its json of controls.
    bool isParamTarget(const std::string& key) const;
    // Scalar editor controls whose DSP input belongs to each voice. Effects,
    // transport and structural settings remain shared unless explicitly added.
    void addVoiceParam(const ofAbstractParameter& p) { voiceParamKeys.insert(p.getEscapedName()); }
    float voiceValue(const ofParameter<float>& p, int voice) const;
    std::vector<float> voiceValues(const ofParameter<float>& p) const;
    void patchBases(ofJson& controls) const;                   // modulated keys in it -> their base
    ofJson paramsJson() const;                                 // the targets, amounts, bases
    void loadParamsJson(const ofJson& j);                      // (set the bases: afterLoad / applyParamBases)
    void applyParamBases();
    void save(ofJson& j, const std::string& prefix) const;
    void load(const ofJson& j, const std::string& prefix);
    void afterLoad();                                          // after the node's controls are back

private:
    // Tables depend on shape settings, not tempo, routing or envelope edits.
    // Keep each LFO's last cycle and transfer at their requested size.
    struct TableCache {
        std::array<float, 9> key{};
        std::vector<float> values;
    };
    mutable std::array<TableCache, MAX_LFOS> cycleCache, transferCache;
    struct PreviewCache {
        std::array<float, 8> key{};
        std::array<float, 97> values{};
        bool valid = false;
    };
    std::array<PreviewCache, MAX_LFOS> previewCache;

    struct ParamTarget {
        std::string key, name;
        ofAbstractParameter* p = nullptr;
        int kind = 0;                                          // 0 float, 1 int, 2 vector<float>, 3 vector<int>
        float lo = 0.0f, hi = 1.0f;
        std::vector<float> base, written, voiceWritten;
        std::array<float, MAX_SOURCES> amount{};
        bool applied = false;                                  // the control holds base + modulation now
        bool pending = false;                                  // a loaded base, not set yet
        bool routed() const { for(float a : amount) if(std::abs(a) > 1e-4f) return true; return false; }
    };
    std::vector<ParamTarget> params;
    std::set<std::string> voiceParamKeys;
    std::set<std::string> ownKeys;                             // the module's own controls (not targets)
    bool resolveParam(const std::string& key, ParamTarget& pt) const;
    std::vector<float> readParam(const ParamTarget& pt) const;
    void writeParam(ParamTarget& pt, const std::vector<float>& v);
    ParamTarget* findParam(const std::string& key);
    void applyParams();
    float paramSource(int s, int voice) const;                 // a source's value as it moves a control
    bool paramsActive() const;
    void removeParam(size_t i);
    struct EnvParams { ofParameter<float> a, d, s, r, ca, cd, cr, vt; };   // vt: velocity tracking 0..1
    struct ServerState {
        ofxSCBus* srcBlock = nullptr;                          // NUM_SOURCES * W
        ofxSCBus* modBlock = nullptr;                          // (targets + 1) * W
        std::array<ofxSCSynth*, MAX_LFOS> lfo{};
        std::array<std::string, MAX_LFOS> lfoDef;   // ModLfo or ModLfoRnd
        std::array<ofxSCSynth*, MAX_ENVS> env{};
        std::map<int, ofxSCSynth*> routes;
        int sourceGroup = -1, routeGroup = -1, gateBus = -1;
        bool created = false;
        int builtVoices = 0;
    };
    Host host;
    std::array<ofParameter<std::vector<float>>, MAX_LFOS> lfoOutputs;
    std::array<std::shared_ptr<ofxOceanodeAbstractParameter>, MAX_LFOS> lfoOutputPins{};
    bool publishLfoOutput(int li, bool publish);
    void updateLfoOutputs();
    std::vector<Target> targetList;
    bool perVoice = true, velocity = false, random = false;
    // Random: each destination its own value per note (a hash of the note's draw),
    // unless the user wants one value everywhere
    bool randomShared = false;
    static float decorrelate(float r, float salt) {
        const float x = std::sin(r * 91.3458f + salt) * 437.5853f;
        return (x - std::floor(x)) * 2.0f - 1.0f;
    }
    std::map<ofxSCServer*, ServerState> states;
    std::vector<float> live;
    float nextLiveRead = 0.0f;
    float currentBpm = 120.0f;
    int resetCounter = 0;
    int n() const { return host.voices ? std::max(1, host.voices()) : 1; }
    int T() const { return (int)targetList.size(); }

    // parameters
    std::array<ofParameter<bool>, MAX_LFOS> lfoSync, lfoCentered, lfoNorm, lfoNoise;
    std::array<ofParameter<bool>, MAX_LFOS> lfoRndSame;    // Random: one value for every voice (else each its own)
    // Random type (ModLfoRnd): 0 off (the waveform / noise), 1 stepped, 2 smooth, 3 jumps;
    // Random Smooth: the stepped values' glide (s)
    std::array<ofParameter<int>, MAX_LFOS> lfoRndType, lfoModel;
    std::array<ofParameter<float>, MAX_LFOS> lfoManual;
    int effectiveRandom(int li) const { return unifiedLfoModels ? (lfoModel[li].get() == 3 ? 1 : 0) : lfoRndType[li].get(); }
    bool effectiveNoise(int li) const { return unifiedLfoModels ? lfoModel[li].get() == 2 : lfoNoise[li].get(); }
    bool manualLfo(int li) const { return unifiedLfoModels && lfoModel[li].get() == 0; }
    std::array<ofParameter<float>, MAX_LFOS> lfoRndSmooth;
    static float rndPreview(int li, float ph, int type, float smooth);   // a fixed random pattern, for the drawings
    std::array<ofParameter<std::vector<float>>, MAX_LFOS> lfoGain;
    std::array<ofParameter<std::vector<float>>, MAX_LFOS> lfoBeats, lfoHz, lfoDetune, lfoRound, lfoSkew, lfoPw,
                                                          lfoPow, lfoBiPow, lfoSH;
    std::array<ofParameter<float>, MAX_LFOS> lfoPSpread, lfoPRand, lfoSHGlide;
    std::array<ofParameter<bool>, MAX_LFOS> lfoRegion;
    std::array<ofParameter<int>, MAX_LFOS> lfoSteps;
    std::array<ofParameter<float>, MAX_LFOS> lfoNScale, lfoInit, lfoNWaves, lfoInvert, lfoShuffle, lfoRand, lfoOffset, lfoComb;
    std::array<ofParameter<int>, MAX_LFOS> lfoSym, lfoQuant, lfoModulo;
    std::array<baseIndexer, MAX_LFOS> lfoIndexers;
    std::array<std::vector<float>, MAX_LFOS> lfoPhases;
    std::array<EnvParams, MAX_ENVS> envs;
    std::array<ofParameter<std::vector<float>>, MAX_EXT> exts;

    void notify() { if(changed) changed(); }
    bool ownsKey(const std::string& key) const;
    void forStates(const std::function<void(ofxSCServer*, ServerState&)>& f);
    void sendVec(ofxSCSynth* s, const std::string& name, const std::vector<float>& v);
    void sendLfo(ServerState& st, int li);
    void sendEnv(ServerState& st, int e);
    void updateSources(ofxSCServer* server, ServerState& st);
    void syncRoutes(ofxSCServer* server, ServerState& st);
    void writeSources(ofxSCServer* server, ServerState& st);
    void updatePhases();
    std::vector<float> lfoHzFor(int li) const;
    std::vector<float> lfoShFor(int li) const;
    std::string def(const std::string& name) const { return name + ofToString(n()); }

    // widgets (GrainBox's look)
    float labelW = 70.0f;
    int matrixDrag = -1;
    float matrixDragStart = 0.0f, matrixDragValue = 0.0f;
    void sectionHeader(const char* title, float w, ImU32 accent);
    void slider(ofParameter<float>& p, const char* label, float w, const char* fmt, bool log = false);
    void sliderVec(ofParameter<std::vector<float>>& p, const char* label, float w, const char* fmt, bool log = false);
    void sliderInt(ofParameter<int>& p, const char* label, float w);
    void toggle(ofParameter<bool>& p, const char* label);
    void drawEnvCard(int e, float w);
    void drawLfoCard(int li, float w);
};
