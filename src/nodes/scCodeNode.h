//
//  scCodeNode.h
//  ofxOceanodeSuperCollider
//
//  SC Code: paste SuperCollider code (a typical community snippet: one or
//  more SynthDefs, maybe a {}.play, a Pbind, an s.waitForBoot) and play its
//  SynthDefs inside the Oceanode graph.
//
//  • Compile runs the bundled headless sclang on the code, block by block,
//    with no server (synthdefs/OceanodeCode). Every SynthDef the code builds
//    is kept, renamed oc<hash>_<name> so nodes never clash, and described in
//    a meta.json (controls, specs, buses, whether it frees itself). Results
//    are cached in data/Supercollider/CodeCache/<hash>/ by the code's hash.
//  • The chosen SynthDef shapes the node: an audio In / Out bus control
//    (In.ar(\in) / Out.ar(\out)) becomes an audio port, every other control
//    a vector parameter (range from the SynthDef's specs, SC's standard specs
//    or the default).
//  • Persistent mode: one synth per server, like any other node (effects,
//    drones, {}.play).
//  • Voices mode, chosen automatically for a SynthDef that frees itself
//    (doneAction: 2, DetectSilence...): Gate is a vector of lanes; a rising
//    edge starts a voice with that lane's parameter values, a falling edge
//    releases it (gate 0) if the SynthDef has a gate. Trig fires lane 0.
//    Voices sum on the node's output bus and free themselves; Voices caps
//    them, stealing the oldest.
//  • Each server gets a group in the graph's order; the synth or the voices
//    live inside it, so the graph never needs to know which.
//  • Promote: once it sounds right, the AI rewrites the code as an Oceanode
//    ~synthCreator source (OceanodeParameter controls, n channels, the
//    current values as defaults). It is saved into the SynthDef sources,
//    compiled into data/Supercollider/Synthdefs/<Category>/<Name>/, loaded
//    and registered as a regular node.
//

#pragma once

#include "scNode.h"
#include "ofxSCSynth.h"
#include "ofxSCGroup.h"
#include "ofxSCBus.h"
#include "ofxOceanodeAIAssistant.h"
#include "scSynthdefCompiler.h"
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

class serverManager;

class scCodeNode : public scNode {
public:
    static constexpr int MAX_VOICES = 256;
    static constexpr int MAX_LANES = 256;

    explicit scCodeNode(std::vector<serverManager*> servers);
    ~scCodeNode();

    void setup() override;
    void update(ofEventArgs& e) override;
    void draw(ofEventArgs& e) override;
    void activate() override;
    void deactivate() override;

    void buildSynth(ofxSCServer* server) override;
    void createSynth(ofxSCServer* server) override;
    void moveSynthBefore(ofxSCServer* server, int nodeID) override;
    void free(ofxSCServer* server) override;
    void setOutputBus(ofxSCServer* server, int index, int bus) override;
    void setInputBus(ofxSCServer* server, scNode* node, int bus) override;
    void resetInputBusses(ofxSCServer* server, int targetBus = 0) override;
    int getOutputBusIndex(ofxSCServer* server, int index) override;
    int getLastSynthID(ofxSCServer* server) override;
    void resendParametersForNRT() override;

    void presetSave(ofJson& json) override;
    void loadBeforeConnections(ofJson& json) override;
    void presetRecallAfterSettingParameters(ofJson& json) override;

    // ── What a capture run reports ──────────────────────────────────────────
    struct ControlDesc {
        std::string name;
        std::string rate;               // scalar, control, audio, trigger
        std::vector<float> defaults;    // more than one: an array control
        bool hasSpec = false;
        float min = 0, max = 1;
        std::string units;
    };
    struct BusDesc {
        std::string type;               // In, InFeedback, Out, ReplaceOut...
        std::string rate;               // audio, control
        int channels = 1;
        std::string control;            // bus set by this control, "" if not
        bool fixed = false;             // a constant bus number instead
        int number = 0;
    };
    struct DefDesc {
        std::string name;               // as written in the code
        std::string defName;            // as loaded on the server
        bool canFreeSynth = false;
        bool hasGate = false;
        bool functionPlay = false;      // built by {}.play
        std::vector<ControlDesc> controls;
        std::vector<BusDesc> inputs, outputs;
    };
    struct Meta {
        std::vector<DefDesc> defs;
        std::vector<std::string> errors;
    };
    static bool parseMeta(const std::string& text, Meta& meta, std::string& error);
    // Top-level ( ... ) blocks, and the code between them, each with the line
    // it starts on: run one by one, as the SC IDE runs a region at a time.
    static std::vector<std::pair<int, std::string>> splitBlocks(const std::string& code);

private:
    enum Mode { ModeAuto, ModePersistent, ModeVoices };

    struct Voice {
        ofxSCSynth* synth = nullptr;
        int lane = 0;
        uint64_t id = 0;
        float started = 0;
        bool released = false;          // gate sent to 0
        bool stolen = false;            // freed or force-released for a new voice
        std::vector<float> sent;        // scalar value per control parameter
    };
    struct ServerState {
        ofxSCGroup* group = nullptr;
        bool groupCreated = false;
        float groupReadyTime = 0;       // its /g_new is timetagged until then
        ofxSCSynth* synth = nullptr;    // persistent mode
        std::vector<Voice> voices;
        std::map<std::string, int> outputBus;   // bus control -> bus
        std::map<int, int> outputBusByIndex;    // port index -> bus
        std::map<scNode*, int> inputBusByNode;  // source node -> bus
        int silentBus = -1;             // for inputs with nothing connected
        ofxSCBus* scratch = nullptr;    // for an output port with no bus yet
        std::string builtDef;           // what the synth / voices run
        bool builtVoices = false;
    };
    struct ControlParam {
        ControlDesc desc;
        int index = 0;
        bool array = false;
        bool trigger = false;
        ofParameter<std::vector<float>> param;
        std::vector<float> lastSent;    // to the persistent synths
        ofEventListener listener;
    };
    struct BuildOutcome {
        bool ok = false;
        std::string error;
        std::string hash;
        std::string folder;
        std::string metaText;
        std::string logPath;
    };

    // ── Code, compile and cache ─────────────────────────────────────────────
    static std::string hashCode(const std::string& code);
    static std::string cacheRoot();
    void compile(const std::string& source);
    static BuildOutcome runCapture(scSynthdefCompiler::ScriptJob job, std::string runner,
                                   std::string hash, std::string folder, std::string prefix,
                                   std::vector<std::pair<int, std::string>> blocks);
    void finishCompile(BuildOutcome outcome);
    // A compiled code: its description, the folder of its SynthDefs
    void applyMeta(const std::string& source, const std::string& hash, const std::string& metaText, bool fromPreset);
    void requireFolder();
    bool defsLoading() const;
    std::string findRunnerFolder();

    // ── Shape of the node ───────────────────────────────────────────────────
    const DefDesc* currentDef() const;
    bool voiceMode() const;
    std::vector<std::string> busControls(const std::vector<BusDesc>& list) const;
    bool isHiddenControl(const std::string& name) const;
    void applyStructure();
    void setVoiceParameters(bool on);
    std::string uniqueName(const std::string& wanted, const std::string& keep = "") const;
    void reorderParameters();
    void selectDef(const std::string& name);
    void setMode(int mode);

    // ── Synths ──────────────────────────────────────────────────────────────
    void rebuildSynths(bool force = false);
    void rebuildSynths(ofxSCServer* server, bool force = false);
    void createPersistent(ofxSCServer* server, ServerState& st);
    void freeVoices(ServerState& st, bool sendFree);
    void freeState(ServerState& st);
    void sendBuses(ofxSCServer* server, ServerState& st, ofxSCSynth* synth);
    void setGroupControl(ofxSCServer* server, ServerState& st, const std::string& name, float value);
    int inputBusFor(ServerState& st, const std::string& control) const;
    int outputBusFor(ofxSCServer* server, ServerState& st, const std::string& control);
    const ControlDesc* findControl(const std::string& name) const;
    void sendControl(ControlParam& c, bool force);
    void setOnSynth(ofxSCSynth* synth, const ControlParam& c, const std::vector<float>& value);
    float laneValue(const std::vector<float>& v, int lane, float fallback) const;
    bool sendsNow(ServerState& st) const;

    // ── Voices ──────────────────────────────────────────────────────────────
    void onGate(std::vector<float>& values);
    void spawn(int lane, float gateValue, float autoRelease = -1);
    void release(int lane);
    void reapVoices();
    int liveVoiceCount() const;

    // ── GUI ─────────────────────────────────────────────────────────────────
    void drawWindow();
    void drawToolbar();
    void drawEditor(ImVec2 size);
    void drawStatusRegion();
    void fixHardwareOut();
    void refreshScriptList();
    static std::string scriptsFolder();
    static std::string scriptLabel(const std::string& path);
    void saveToCollection();
    void loadScriptFile(const std::string& path);
    void saveScriptFile(const std::string& path);
    static int editorCallback(ImGuiInputTextCallbackData* data);
    void setupAIAssistant();
    static std::string buildSystemPrompt();
    static int resizeCallback(ImGuiInputTextCallbackData* data);

    // ── Promote ─────────────────────────────────────────────────────────────
    struct PromoteOutcome {
        bool ok = false;
        std::string error;
        std::string name, category;
        std::string installedFolder;
        std::string logPath;
    };
    void openPromote();
    void drawPromotePopup();
    void requestConversion();
    void startInstall();
    void finishInstall(PromoteOutcome outcome);
    std::string promotePrompt() const;
    static std::string promoteSystemPrompt();
    static std::string addonSynthdefsFolder();
    static PromoteOutcome runInstall(scSynthdefCompiler::ScriptJob job, std::string runner,
                                     std::vector<std::string> sourceFolders, std::string synthdefRoot,
                                     std::string work, std::string name, std::string category,
                                     std::string source, int maxChannels);

    std::vector<serverManager*> servers;
    std::map<ofxSCServer*, ServerState> states;

    // Parameters
    ofParameter<bool> showWindow;
    ofParameter<std::vector<float>> gate;
    ofParameter<void> trig;
    ofParameter<int> maxVoices;
    ofParameter<float> trigLength;
    bool voiceParamsRegistered = false;
    customGuiRegion statusRegion;
    std::vector<std::unique_ptr<ControlParam>> controls;
    std::vector<std::string> inputControls;     // bus control of each scNode input
    std::vector<std::string> outputControls;    // bus control of each scNode output
    ofEventListeners listeners;

    // Code
    std::string editorText;             // what the editor shows
    std::string compiledCode;           // what the node runs
    std::string compiledHash;
    std::string compiledFolder;
    std::string metaText;
    Meta meta;
    std::string selectedDef;            // DefDesc::name
    int mode = ModeAuto;
    bool builtVoiceMode = false;        // the mode the synths were built for
    std::string builtDefName;           // what they run
    bool rebuildPending = false;
    bool firstUpdate = true;

    std::future<BuildOutcome> buildTask;
    std::string buildingHash;
    std::string buildingSource;
    std::string queuedSource;
    bool hasQueuedSource = false;
    std::string status;
    bool statusError = false;
    std::vector<std::string> errors;
    std::string logPath;
    std::string logText;
    bool showLog = false;
    std::vector<int> blockLines;        // line of each block of the last compile

    // Voices
    std::vector<float> lastGate;
    uint64_t nextVoiceId = 1;
    struct PendingRelease { uint64_t id; float time; };
    std::vector<PendingRelease> pendingReleases;

    // Scripts
    std::vector<std::string> scriptFiles;
    int selectedScript = 0;
    bool scriptListDirty = true;
    bool pendingOpenDialog = false;
    bool pendingSaveDialog = false;
    std::string pendingScriptPath;
    std::string saveName;               // collection name of the current script
    bool editorFocusCompile = false;    // Cmd+Return / Shift+Return in the editor

    ofxOceanodeAIAssistant ai;

    bool promoteOpenRequest = false;
    std::string promoteName, promoteCategory, promoteNotes, promoteSource;
    int promoteChannels = 2;
    bool promoteOverwrite = false;
    std::future<ofxOceanodeAIAssistant::TextResult> conversionTask;
    std::future<PromoteOutcome> installTask;
    std::string promoteStatus;
    bool promoteStatusError = false;
};
