//
//  scMixerPro.h
//  ofxOceanodeSuperCollider
//
//  SC Mixer Pro: a multichannel, multi-server mixer with a dockable DAW-style
//  window.
//
//  Multi-server. Every channel ("track") is assigned to one server, and the
//  node has one output per server ("Out S1".."Out Sn"), each to be connected
//  to an Output on that server. When server k builds its graph it walks up
//  from its Outputs; this node then only continues up the inputs of the
//  tracks assigned to k (appendOrderedNodes / getConnections), so each source
//  chain is built on its own track's server only. On server k the node runs
//  one synth per track of k, all summing into "Out Sk".
//
//  DSP (mixerpro.scd). mixerProTrackN is polyMixerTrackN verbatim (balance,
//  level, mute, masterLevel, NaN protection, VU). EQ / DC correction live in a
//  separate insert synth (mixerProEQN / mixerProDCN / mixerProEQDCN) created
//  the first time either is switched on, placed just before the track synth
//  and paused while both are off -- a track without them costs nothing.
//  COMP (mixerProCompN) and RVB (mixerProRvbN, SpaceMaster) are two more
//  inserts with the same rule: created the first time they are switched on,
//  paused while off. Switching off first fades: the compressor crossfades to
//  dry (\on, 30 ms), the reverb stops feeding its tank and lets the tail ring
//  out (Decay = RT60); then the node routes around the insert and pauses it
//  (update(), offAt). The compressor writes its gain reduction (dB) on the
//  track's vuBus, after the key channels, for the strip's GR meter.
//
//  Sidechain (hybrid). Each track can be ducked by another one: SC Source
//  (1-based track, 0 off), SC Strength, SC Attack / SC Release (ms). Every
//  track synth writes its key envelope after its VU channels; when source and
//  target run on the same server the target reads that bus directly (one
//  block of latency at most). Otherwise the node reads the source's held key
//  every frame and sends it to the target as scKey (~20-40 ms).
//
//  Channel adapter. A track's source can have a different channel count (M)
//  than the mixer (N): Adapt Mode (Direct / Wrap / Blocks / Stretch), In Ch
//  (0 = the source node's "N Chan") and Rotate decide a gain matrix, computed
//  here and run by mixerProAdaptM_N, created only when the matrix is not the
//  plain direct one. Chain: source -> adapter -> EQ/DC -> Comp -> Rvb -> track.
//
//  Submasters (buses). A bus is a strip like a track (same synth, inserts,
//  sidechain) whose input is a private audio bus. A track's Out (0 master,
//  k bus k) sends it to a bus on its own server; buses run after the tracks
//  and go to the server's output. Tracks into a bus get masterLevel 1 (the
//  bus applies it). Solo: a soloed bus keeps its tracks audible, a soloed
//  track keeps its bus audible.
//
//  Node GUI: Show, Show Faders, Gain Vec, Master Level, the outputs and the
//  inputs. Everything else lives in the window, and any of its controls can
//  be published to the node with its right-click menu. Track state is stored
//  in the node's preset JSON, not as Oceanode parameters, so large mixers
//  load quickly.
//

#pragma once

#include "ofxOceanodeNodeModel.h"
#include "ofxOceanodeShared.h"
#include "scNode.h"
#include "serverManager.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "scEQEditor.h"
#include "imgui.h"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class scMixerPro : public scNode {
public:
    static constexpr int MAX_CHANNELS = 16;
    static constexpr int MAX_TRACKS   = 64;
    static constexpr int MAX_BUSES    = 16;

    explicit scMixerPro(std::vector<serverManager*> servers);
    ~scMixerPro() override;

    void setup() override;
    void update(ofEventArgs&) override;
    void draw(ofEventArgs&) override;
    void activate() override;
    void deactivate() override;

    // Multi-server graph membership (see top of file)
    bool appendOrderedNodes(vector<scNode*>& nodesList,
                            map<scNode*, std::pair<int, vector<int>>>& visitedNodeChilds,
                            vector<scNode*> parents = {}) override;
    void getConnections(std::map<nodePort, vector<scNode*>>& connections) override;

    void buildSynth(ofxSCServer* server) override;
    void createSynth(ofxSCServer* server) override;
    void free(ofxSCServer* server) override;
    void moveSynthBefore(ofxSCServer* server, int nodeID) override;
    void setOutputBus(ofxSCServer* server, int index, int bus) override;
    void setInputBus(ofxSCServer* server, scNode* node, int bus) override;
    void resetInputBusses(ofxSCServer* server, int targetBus = 0) override;
    int  getOutputBusIndex(ofxSCServer* server, int index) override;
    int  getLastSynthID(ofxSCServer* server) override;
    bool isNRTStemPoint() const override { return true; }
    bool canEncapsulateSubgraphFrom(ofxOceanodeAbstractParameter& parameter) const override;
    void resendParametersForNRT() override;

    void presetSave(ofJson& json) override;
    void loadBeforeConnections(ofJson& json) override;
    void presetRecallAfterSettingParameters(ofJson& json) override;

private:
    // An on-demand insert (Comp, Rvb): created when first switched on, paused while off
    struct FxStage {
        ofxSCSynth* synth = nullptr;
        ofxSCBus* bus = nullptr;
        bool running = false;                // in the chain (fading out still counts)
        float offAt = -1.0f;                 // switched off: pause at this time (s)
    };

    struct Track {
        int index = 0;
        std::string name;
        ofColor color;
        ofParameter<std::function<void()>> inputBadge; // colour key beside the node inlet
        ofParameter<std::function<void()>> faderBadge; // colour key beside the node fader
        int server = 0;                      // index into servers
        bool isBus = false;                  // a submaster
        ofParameter<int> output;             // tracks: 0 master, k = bus k

        // Publishable controls
        ofParameter<float> level;            // linear gain, 0..4
        std::string levelKey;                 // stable publish identity while its label changes
        ofParameter<float> balance;          // -1..1
        ofParameter<bool>  mute, solo;
        ofParameter<bool>  eq, dc;
        ofParameter<vector<float>> vuOut;    // node output, when published
        ofParameter<int>   scSource;         // 1-based track, 0 = off
        ofParameter<float> scStrength, scAttack, scRelease, scThreshold;
        ofParameter<int>   adaptMode;        // 0 Direct, 1 Wrap, 2 Blocks, 3 Stretch
        ofParameter<int>   inChannels;       // 0 = auto (source's N Chan)
        ofParameter<int>   rotate;           // output rotation, channels
        std::array<ofParameter<float>, scEQEditor::NUM_BANDS> eqFreq, eqGain, eqShape;
        // Compressor insert
        ofParameter<bool>  comp, compAuto;
        ofParameter<float> compThreshold, compRatio, compKnee, compAttack, compRelease;
        ofParameter<float> compMakeup, compRms, compHpf, compMix;
        // Reverb insert (SpaceMaster)
        ofParameter<bool>  rvb;
        ofParameter<float> rvbMix, rvbDecay, rvbSize, rvbPredelay, rvbPosition, rvbSpread;
        ofParameter<float> rvbLowpass, rvbHighDamp, rvbLowDamp, rvbModFreq, rvbSpin, rvbWander;
        ofEventListeners listeners;

        // Runtime, on liveServer only
        ofxSCServer* liveServer = nullptr;
        ofxSCSynth* synth  = nullptr;
        ofxSCSynth* insert = nullptr;
        int  insertVariant = 0;              // 1 EQ, 2 DC, 3 both
        ofxSCSynth* adapt = nullptr;         // channel adapter, before the insert
        ofxSCBus* adaptBus = nullptr;
        int  adaptInputs = 0;                // M of the adapter synth
        bool adaptRunning = false;
        int  detectedInputs = 0;             // last effective M, polled in update()
        std::vector<float> sentMatrix;
        bool insertRunning = false;
        FxStage compFx, rvbFx;
        float gr = 0.0f;                     // compressor gain reduction (dB), read with the VU
        ofxSCBus* vuBus     = nullptr;
        ofxSCBus* insertBus = nullptr;
        ofxSCBus* mixBus    = nullptr;       // buses: what the tracks write into
        int  srcBus = -1;
        float sentMute = -1.0f;
        int   sentScBus = -2;                // -2: nothing sent yet
        float sentScKey = -1.0f;
        int   scMode = 0;                    // 0 off, 1 same server, 2 via Oceanode
        float key = 0.0f;                    // held key, read when another server needs it
        bool  keyNeeded = false;

        // Meters and display
        std::vector<float> vu, peak, peakAge;
        scEQEditor eqEditor;
        bool eqCurveDirty = true;
    };

    struct ServerState {
        bool active = false;                 // this node is part of the server's graph
        std::vector<ofxSCSynth*> order;      // our synths in execution order
        std::map<int, int> outputBuses;
        int silentBus = 0;
    };

    // --- servers ---
    std::vector<serverManager*> servers;
    std::map<ofxSCServer*, ServerState> serverStates;
    int serverIndexOf(ofxSCServer* server) const;
    ofxSCServer* serverAt(int index) const;
    bool isServerActive(int index) const;

    // --- node parameters ---
    ofParameter<bool> showWindow;
    ofParameter<bool> showFaders;
    ofParameter<vector<float>> gainVec;
    ofParameter<vector<float>> balanceVec;
    ofParameter<vector<float>> masterLevel;
    ofParameter<int> numTracks;
    ofParameter<int> numChannels;
    ofParameter<float> vuAttack, vuRelease;
    ofEventListeners nodeListeners;
    int currentChannels = 2;
    bool rebuildingTracks = false;
    int  pendingTrackCount = -1;   // a Num Inputs change, applied in update()
                                   // (inputs can't change while the node GUI draws)
    bool pendingFaderUpdate = false; // Show Faders also changes the node layout
    bool pendingTrackLabelsUpdate = false;

    // --- tracks ---
    std::vector<std::unique_ptr<Track>> tracks;
    void setTrackCount(int count);
    void addTrack();
    void removeLastTrack();
    void syncTrackInputNames();
    void useLegacyInputNames();
    void syncFaderNames();
    void useLegacyFaderNames();
    void reorderNodeParameters();
    std::vector<std::unique_ptr<Track>> buses;
    int pendingBusCount = -1;
    // Track reorder / delete from the window, applied in update()
    int pendingMoveFrom = -1, pendingMoveTo = -1, pendingDelete = -1;
    std::vector<ofAbstractParameter*> stripParams(Track& t);
    void reorderTracks(const std::vector<int>& order);   // order[new] = old
    bool holdGraphRefresh = false;                       // refreshGraph deferred while true
    void initStrip(Track& t, const std::string& n);
    void setBusCount(int count);
    void addBus();
    void removeLastBus();
    void setStripServer(Track& tr, int server);
    std::vector<Track*> allStrips();
    Track* busFor(const Track& tr) const;    // the live-able bus a track goes to
    bool routedToBus(const Track& tr) const; // ... and that bus is running
    int  trackOutBus(Track& tr);
    void refreshTrackOutputs();
    bool isSilenced(const Track& tr) const;
    int  eqId(const Track& tr) const { return tr.isBus ? 1000 + tr.index : tr.index; }
    Track* eqTarget();
    ofJson saveStrip(Track& tr);
    void loadStrip(Track& tr, const ofJson& j);
    void setChannelCount(int channels);
    bool anySolo() const;
    void updateMutes();
    void refreshGraph();

    // --- synths ---
    std::string trackDefName() const;
    std::string insertDefName(int variant) const;
    void createRuntime(Track& tr, ofxSCServer* server);
    void destroyRuntime(Track& tr, bool sendFree);
    void updateInsert(Track& tr);
    // Comp (1) / Rvb (2) inserts. Stages: 0 EQ/DC, 1 Comp, 2 Rvb, 3 the track
    std::vector<ofAbstractParameter*> fxParams(Track& t);
    void updateFx(Track& tr, int which);
    void sendComp(Track& tr);
    void sendRvb(Track& tr);
    void processFxTimers();
    float rvbTail(const Track& tr) const;
    int  stageInput(const Track& tr, int stage) const;
    ofxSCSynth* chainSynthAfter(const Track& tr, int stage) const;
    void applyInputRouting(Track& tr);
    void sendAll(Track& tr);
    void sendLevel(Track& tr);
    void sendMasterLevel(Track& tr);
    void sendEQ(Track& tr);
    int  outBusFor(ofxSCServer* server);
    void orderInsertBefore(ofxSCServer* server, ofxSCSynth* added, ofxSCSynth* before);
    void orderReplace(ofxSCServer* server, ofxSCSynth* oldSynth, ofxSCSynth* newSynth);
    void orderRemove(ofxSCServer* server, ofxSCSynth* synth);
    void requireSynthdefs();
    void routeSidechains();
    int  sourceChannels(Track& tr);
    std::vector<float> adaptMatrix(const Track& tr, int inputs) const;
    void updateAdapter(Track& tr);
    void dropAdapter(Track& tr, bool sendFree);
    int  chainInput(const Track& tr) const;
    void sendSidechainKeys();
    void sendSidechainShape(Track& tr);
    int  vuBusChannels() const { return currentChannels + 3; }   // VU + key + held key + comp GR

    // --- publishing (as GrainBox / BeatRepeat Pro) ---
    struct PublishAction {
        std::string key;
        ofAbstractParameter* parameter = nullptr;
        std::function<std::shared_ptr<ofxOceanodeAbstractParameter>()> add;
        bool output = false;                 // a node output (VU): no typed value
    };
    std::map<std::string, PublishAction> publishActions;
    std::vector<std::string> publishedKeys;
    std::map<std::string, std::shared_ptr<ofxOceanodeAbstractParameter>> nodeHandles; // published or fader
    std::vector<std::string> faderKeys;
    bool publishedSeparatorAdded = false;
    bool faderSeparatorAdded = false;
    bool submasterFaderSeparatorAdded = false;
    void registerTrackActions(Track& tr);
    void unregisterTrackActions(Track& tr);
    bool isPublished(const std::string& key) const;
    bool hasNodeConnection(const std::string& key) const;
    bool publishKey(const std::string& key);
    bool unpublishKey(const std::string& key);
    void syncPublished(const std::vector<std::string>& keys);
    void updateFaders();
    void syncFaderSeparators();
    Track* stripForLevelKey(const std::string& key);
    void ensureFaderBadge(Track& tr);
    void removeFaderBadge(Track& tr);
    void removeNodeHandleIfUnused(const std::string& key);
    void drawPublishPopup(const std::string& key);
    void drawPublishItems(const std::string& key, const char* title, bool focus);
    void markPublished(const std::string& key) const;
    template<typename T>
    void addAction(const std::string& key, ofParameter<T>& parameter) {
        PublishAction action;
        action.key = key;
        action.parameter = &parameter;
        ofParameter<T>* p = &parameter;
        action.add = [this, p]() -> std::shared_ptr<ofxOceanodeAbstractParameter> { return addParameter(*p); };
        publishActions[key] = action;
    }
    void addOutputAction(const std::string& key, ofParameter<vector<float>>& parameter) {
        PublishAction action;
        action.key = key;
        action.parameter = &parameter;
        action.output = true;
        ofParameter<vector<float>>* p = &parameter;
        action.add = [this, p]() -> std::shared_ptr<ofxOceanodeAbstractParameter> { return addOutputParameter(*p); };
        publishActions[key] = action;
    }
    void drawVuPublishMenu(const std::string& key);
    ofParameter<vector<float>> masterVuOut;

    // --- window ---
    bool windowVisible = false;
    int editTracks = 0, editChannels = 0, editBuses = 0;   // toolbar fields while being typed in
    int selectedEqTrack = -1;
    int panelTab = 0;                          // channel panel: 0 EQ, 1 Comp, 2 Reverb
    std::vector<float> masterVU, masterPeak, masterPeakAge;
    void drawWindow();
    void drawToolbar();
    void drawMasterStrip(float w, float h);
    void drawTrackStrip(Track& tr, float w, float h);
    void drawEqPanel(float w, float h);
    void drawCompPanel(Track& tr);
    void drawRvbPanel(Track& tr);
    void drawMiniCurve(Track& tr, ImVec2 pos, ImVec2 size);
    bool drawFader(const char* id, float& gain, ImVec2 pos, ImVec2 size, float maxGain = 2.0f);
    bool drawKnob(const char* id, float& value, float radius);
    void drawMeters(const std::vector<float>& vu, const std::vector<float>& peak, ImVec2 pos, ImVec2 size, bool bus = false);
    void updatePeaks(std::vector<float>& vu, std::vector<float>& peak, std::vector<float>& age, float dt);
    void recomputeEqCurve(Track& tr);

    static float gainToPos(float gain);
    static float posToGain(float pos);
    static float ampToDb(float amp);
};
