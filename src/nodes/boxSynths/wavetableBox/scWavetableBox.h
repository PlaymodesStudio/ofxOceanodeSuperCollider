//
//  scWavetableBox.h
//  ofxOceanodeSuperCollider
//
//  WavetableBox: a multi-frame wavetable oscillator in the Serum / Vital
//  family, with a 3D waterfall view of the table.
//
//  • Tables: a Serum / Vital / KRC wavetable WAV (frame size from the `clm `
//    chunk, else 2048), a single-cycle WAV (AKWF, any length), or a folder of
//    single cycles (every WAV in it becomes one frame). Decoded in C++ on a
//    background thread: mixed to mono, split into cycles, each cycle resampled
//    to 2048 samples through its harmonics (band-limited), optionally DC-free
//    and normalised, then written as a float WAV and loaded into one buffer per
//    server. No file: a built-in sine / triangle / saw / square table.
//  • Live: Source = Live takes the table from a vector (Live In, e.g. a drawn
//    shape or a Formula), cut into Live Frames equal parts, each resampled to
//    2048 (through its harmonics when that is cheap, else linearly). Each
//    change is written into the buffer of the A/B pair the synth is not
//    hearing, then the WavetableOscLive synth fades to it over Live XFade; the
//    next upload waits until that fade is over.
//  • Sound: WavetableOsc<n> / WavetableOsc2D<n> (synthdefs/Defaults/WavetableBox),
//    one GrainUtils OscXPM per voice: mipmapped + sinc-interpolated, so
//    band-limited at any pitch and under the phase warps; oscillator B (a sine
//    or the table itself) phase-modulates A, with cross feedback.
//  • Dimensions: Position (frames), Pos Y (rows, when Rows > 1 the table is a
//    grid), two chained phase warps (Sync, Bend, PWM, Asym, Mirror, Quantize,
//    PD Sine), FM from B.
//  • GUI: the node shows Show / Voices / Channels and whatever is published (Pitch,
//    Levels, Gate, Velocity and Position by default). Everything else is in the dockable
//    window (Show): the waterfall (frames stacked in depth, every voice's
//    position traced on them; dragging sets the position), the cycle each
//    voice actually reads (warps applied), the 2D grid, and all controls.
//    Right-click a control in the window to publish it to the node (or type a
//    value / vector, or modulate it). Shared Box LFOs, envelopes, routing,
//    piano notes, snapshots and morph are available in the same window.
//    Unpublished controls are saved with the preset.
//

#pragma once

#include "../../../scNode.h"
#include "../shared/scModulation.h"
#include "../shared/scKeyboardPiano.h"
#include "ofxSCGroup.h"
#include "ofxSCBus.h"
#include "../../../ui/scTimelineMenu.h"
#include "../shared/scSnapshots.h"
#include "../../../ui/scDropdowns.h"
#include "../../../ui/scEQEditor.h"
#include "ofxSCSynth.h"
#include "ofxSCBuffer.h"
#include "imgui.h"
#include <functional>
#include <future>
#include <array>
#include <map>
#include <string>
#include <vector>

class serverManager;

class scWavetableBox : public scNode {
public:
    // its window (SC Mixer Pro's "Go to origin synth window" opens and focuses it)
    std::string getWindowTitle() override { return (canvasID == "Canvas" ? "" : canvasID + "/") + "WavetableBox " + ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return &showWindow; }

    static constexpr int MAX_CHANNELS = 16;      // WavetableOsc<n> exists for n = 1..16
    static constexpr int CYCLE = 2048;           // samples per cycle in the buffer
    static constexpr int MAX_FRAMES = 512;
    static constexpr int PREVIEW_POINTS = 128;   // points per frame in the waterfall

    enum Warp { WarpOff, WarpSync, WarpBendPlus, WarpBendMinus, WarpPWM, WarpAsym,
                WarpMirror, WarpQuantize, WarpPDSine, NumWarps };

    explicit scWavetableBox(std::vector<serverManager*> servers);
    ~scWavetableBox();

    void setup() override;
    void update(ofEventArgs&) override;
    void draw(ofEventArgs&) override;
    void setBpm(float bpm) override;
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

    // Presets: published controls, window-only controls, embedded table
    void presetSave(ofJson& json) override;
    void loadBeforeConnections(ofJson& json) override;
    void presetRecallBeforeSettingParameters(ofJson& json) override;
    void presetRecallAfterSettingParameters(ofJson& json) override;
    void macroSave(ofJson& json, std::string presetFolderPath) override;
    void macroLoad(ofJson& json, std::string presetFolderPath) override;

    bool getTimelineInstrument(ofxOceanodeTimelineInstrument& out) override {
        auto handle = [this](const std::string& key) -> ofxOceanodeAbstractParameter* {
            auto found = publishedHandles.find(key);
            return found == publishedHandles.end() ? nullptr : found->second.get();
        };
        out.pitch = handle(pitch.getEscapedName());
        out.gate = handle(gate.getEscapedName());
        out.velocity = nullptr;
        out.voices = voices.get();
        return out.pitch && out.gate;
    }
    bool getMidiKeyboardInstrument(ofxOceanodeTimelineInstrument& out, bool bind = false) override {
        if(bind) {
            publishByKey(pitch.getEscapedName());
            publishByKey(gate.getEscapedName());
        }
        getTimelineInstrument(out);
        return true;
    }

    // Same formula as warp() in wavetable.scd
    static float warpPhase(float p, int mode, float amt);

private:
    // ── Decoding (background) ────────────────────────────────────────────────
    struct DecodeSettings {
        std::string path;     // file, folder, or empty for the built-in table
        int cycleMode = 0;    // index into the Cycle Size options
        bool normalize = true;
        bool removeDC = true;
        int maxFrames = 256;
    };
    struct Table {
        uint64_t generation = 0;
        std::vector<float> data;   // frames * CYCLE
        int frames = 0;
        std::string name;
        std::string wavPath;
        std::string error;
    };
    static Table decode(DecodeSettings settings, uint64_t generation, std::string tmpDir, std::string prefix);
    static Table builtinTable();
    std::string resolvePath(const std::string& path) const;
    std::string getTmpDir() const;
    void requestDecode();
    void startDecode();
    void applyTable(Table decoded);

    // ── Buffers ──────────────────────────────────────────────────────────────
    struct ServerBuffers {
        ofxSCBuffer* table = nullptr;
        ofxSCBuffer* sine = nullptr;
        int frames = 0;          // cycles in `table`
    };
    struct PendingLoad {
        ServerBuffers buffers;
        float startTime = 0;
    };
    static bool bufferIsLive(ofxSCServer* server, ofxSCBuffer* buffer);
    static void releaseBuffer(ofxSCServer* server, ofxSCBuffer*& buffer);
    bool buffersReady(ofxSCServer* server) const;
    bool synthReady(ofxSCServer* server) const;     // file/sine buffers, and the live table in Live mode
    void loadBuffersOnServer(ofxSCServer* server);
    void swapBuffers(ofxSCServer* server);
    void sendBuffers(ofxSCServer* server);

    // ── Live input ───────────────────────────────────────────────────────────
    struct LiveServer {
        ofxSCBuffer* buffer[2] = {nullptr, nullptr};
        int frames = 0;
        int playing = -1;          // buffer the synth reads, -1 before the first upload
        bool allocPending = false; // /b_alloc sent, waiting for /synced
        float allocTime = 0;
        float lastSwitch = -1;     // when the synth was last told to fade to the other buffer
        bool uploadPending = false;// new data waiting for that fade to finish
    };
    bool isLive() const { return source == 1; }
    void processLive();                         // Live In -> liveData (CPU), view, uploads
    void uploadLive(ofxSCServer* server);
    void releaseLive(ofxSCServer* server);
    void setSource(int s);

    // ── Synth ────────────────────────────────────────────────────────────────
    std::string getSynthDefName() const;
    void rebuildGraph();
    std::vector<float> expand(const std::vector<float>& values, float fallback) const;
    void sendVector(const std::string& control, const std::vector<float>& values, float fallback);
    void sendAll(ofxSCServer* server);
    void sendTriggers(const std::vector<int>& values);
    void retrigAll();

    // ── Publishing window controls into the node ─────────────────────────────
    struct PublishAction {
        std::string key;
        ofAbstractParameter* parameter = nullptr;
        std::vector<std::string> dropdown;   // options, for dropdown controls
        std::function<std::shared_ptr<ofxOceanodeAbstractParameter>()> publish;
        std::function<void(ofJson&)> save;
        std::function<void(const ofJson&)> load;
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
    void controlMenu(const std::string& key);   // right-click menu of the last item

    // ── Window ───────────────────────────────────────────────────────────────
    void drawWindow();
    void drawFileBar();
    void drawWaterfall(ImVec2 size);
    void drawCycleStrip(ImVec2 size);
    void drawGrid(float side);
    void drawControls();
    void drawHeader();
    void setupOutputControls();
    template<typename T> void registerOutput(ofParameter<T>& p, std::vector<std::string> choices = {});
    void drawPanning();
    void drawEffects();
    void drawLibrary();
    void scanLibrary();
    void migrateLegacy(ofJson& json);
    bool sliderFloat(ofParameter<float>& parameter, const char* format = "%.2f", bool logarithmic = false);
    bool checkbox(ofParameter<bool>& parameter);
    bool sliderVector(ofParameter<std::vector<float>>& parameter, const char* format = "%.3f");
    bool comboVector(ofParameter<std::vector<int>>& parameter, const std::vector<std::string>& options);
    bool comboInt(ofParameter<int>& parameter, const std::vector<std::string>& options);
    bool sliderInt(ofParameter<int>& parameter);
    void beginGroup(const char* title, float width);
    void endGroup();
    float voiceFrame(int voice) const;   // fractional frame index a voice reads
    float voiceValue(int voice, float phase) const;
    float tableAt(float frameIndex, float phase) const;
    void stepFile(int direction);
    void openDialog(bool folder);

    // ── Parameters ───────────────────────────────────────────────────────────
    // On the node
    ofParameter<bool> showWindow;
    ofParameter<int> voices, outChannels;
    ofParameter<float> master, spreadWidth, spreadCenter;
    ofParameter<int> placement, speakerLayout;
    ofParameter<std::vector<float>> pan;
    enum FxStage { FxComp, FxEq, FxBlur, FxEcho, FxReverb, NumFx };
    std::array<ofParameter<bool>,NumFx> fxOn;
    ofParameter<float> compThreshold, compRatio, compAttack, compRelease, compMakeup, compMix;
    std::array<ofParameter<float>,5> eqGain, eqFreq, eqShape;
    scEQEditor eqEditor;
    ofParameter<float> blurTime, blurAmount, blurDensity, blurSpread, blurLowCut, blurHighCut, blurMix;
    ofParameter<float> echoDelay, echoFeed, echoCutoff, echoResonance, echoPingPong, echoMix, echoBeatVal;
    ofParameter<int> echoFilter;
    ofParameter<bool> echoBeats;
    ofParameter<float> reverbMix, reverbDecay, reverbSize, reverbPredelay, reverbLate, reverbSpread, reverbLowpass;
    bool fxPending=false;
    std::string fxDef(int stage) const;
    void sendEffect(ofxSCSynth* synth,int stage);
    void updateOutput(ofxSCServer* server);
    void updateOutputs();
    void freeOutput(ofxSCServer* server);
    void requireEffectSynthdefs();
    std::vector<float> voicePositions() const;
    struct LibraryEntry { std::string name, category, path; };
    std::vector<LibraryEntry> library;
    std::vector<std::string> libraryCategories;
    std::string libraryRoot;
    int libraryCategory=0;
    char librarySearch[128]{};
    ofParameter<int> framesOut;
    // In the window (publishable)
    ofParameter<int> source;                       // 0 File, 1 Live
    ofParameter<std::vector<float>> liveIn;
    ofParameter<int> liveFrames;
    ofParameter<bool> rescale;
    ofParameter<float> liveXFade;                  // ms
    ofParameter<std::string> path;
    ofParameter<int> cycleMode, maxFrames, oversample;
    ofParameter<bool> normalize, removeDC, embedTable;
    ofParameter<std::vector<float>> pitch, levels, position, posY;
    ofParameter<std::vector<float>> gate;
    ofParameter<bool> drone;
    ofParameter<void> resetLFOs;
    ofParameter<int> rows;
    ofParameter<std::vector<int>> warp1, warp2;
    ofParameter<std::vector<float>> warp1Amt, warp2Amt;
    ofParameter<int> bSource;
    ofParameter<std::vector<float>> fm, fmRatio, bPos, bFeedback, fmDamp;
    ofParameter<std::vector<float>> phase;
    ofParameter<std::vector<int>> trig;

    // Shared modulation runs before the oscillator: gates -> sources -> routes -> voice.
    enum Target { TPitch, TPosition, TPosY, TWarp1, TWarp2, TFM, TRatio, TBPos, TFeedback, TDamp, TAmp, TPan, NUM_TARGETS };
    struct ModState {
        ofxSCGroup* root = nullptr;
        ofxSCGroup* sources = nullptr;
        ofxSCGroup* routes = nullptr;
        ofxSCGroup* voice = nullptr;
        ofxSCBus* gates = nullptr;
        ofxSCSynth* gateSynth = nullptr;
        ofxSCGroup* processing = nullptr;
        ofxSCBus* voiceBus = nullptr;
        ofxSCBus* panBus = nullptr;
        std::array<ofxSCBus*,NumFx> fxBus{};
        std::array<ofxSCSynth*,NumFx> effects{};
        ofxSCSynth* spreadSynth = nullptr;
        ofxSCSynth* outputSynth = nullptr;
        int outputChannels=0;
    };
    std::map<ofxSCServer*, ModState> modStates;
    scModulation modulation;
    scKeyboardPiano keyboardPiano;
    std::vector<float> modulationView;
    float nextModulationRead = 0;
    float visualValue(const ofParameter<std::vector<float>>& p, int target, int voice) const;
    float currentBpm = 120;
    bool restoring = false;
    void setupModulation();
    void createModulation(ofxSCServer* server);
    void freeModulation(ofxSCServer* server);
    void sendModulation(ofxSCServer* server);
    void markControl(const std::string& key);
    template<typename T> void regMod(ofParameter<T>& p, std::function<void()> send);

    // ── State ────────────────────────────────────────────────────────────────
    std::vector<serverManager*> servers;
    std::map<ofxSCServer*, ofxSCSynth*> synths;
    std::map<ofxSCServer*, int> outputBuses;
    std::map<ofxSCServer*, ServerBuffers> buffers;
    std::map<ofxSCServer*, PendingLoad> pendingLoads;
    std::string sineWavPath;
    std::map<ofxSCServer*, LiveServer> live;
    std::vector<float> liveData;                    // frames * CYCLE, last processed Live In
    int liveDataFrames = 0;
    bool liveDirty = false;
    int lastSource = 0;

    Table table;                                    // the table playing (CPU copy for the view)
    std::vector<std::vector<float>> preview;        // PREVIEW_POINTS per frame
    std::vector<std::string> oldWavPaths;           // replaced, deleted once every server swapped
    std::string status = "Built-in table";

    uint64_t decodeGeneration = 0;
    bool decodeRequested = false;
    float decodeRequestTime = 0;
    std::future<Table> decodeTask;

    std::string pathToSetAfterLoad;
    std::vector<int> lastTrig;
    std::map<std::string, std::vector<float>> lastSent;
    int builtVoices = 0;
    bool built2D = false;
    bool builtLive = false;
    int builtOversample = -1;

    std::vector<PublishAction> publishActions;
    scSnapshots snaps;                    // the shared snapshots: library, morph, presets
    ofJson initial;                       // the sound at setup (the factory's base)
    bool notInSound(const std::string& key) const;
    ofJson captureSound() const;
    void applySound(const ofJson& a, const ofJson& b, float t, bool sw);
    std::vector<ofJson> factorySounds() const;
    std::vector<std::string> publishedKeys;
    scTimelineMenu timeline;              // "Add to Timeline" from the window
    std::map<std::string, std::shared_ptr<ofxOceanodeAbstractParameter>> publishedHandles;
    bool publishedSeparatorAdded = false;

    float waterfallHeight = 300.0f;
    int pendingDialog = 0;       // 1 file, 2 folder: opened from update(), outside the ImGui frame
    ofEventListeners nodeListeners;
};
