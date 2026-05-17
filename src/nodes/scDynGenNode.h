//
//  scDynGenNode.h
//  ofxOceanodeSuperCollider
//
//  Live-coding EEL2 DSP node powered by DynGen.
//
//  Architecture (slot-based):
//    Each node instance occupies a fixed "slot" (dyngenSlot0..dyngenSlot15).
//    A SynthDef is pre-compiled for every (numChannels × slotIndex) pair by
//    dyngen.scd.  The slot symbol's hash is baked into the SynthDef at compile
//    time — it cannot be a runtime NamedControl.
//
//  Parameter layout (Feature 1: per-channel vector params):
//    p<i> is an array NamedControl with numChannels slots.
//    EEL2 access: in(n + pi*n + ci) for param pi, channel ci.
//    Mono backward-compat: in(1 + pi) unchanged (n=1 → pi*1+0 = pi).
//    C++ sends vector<float> values; if length < n, last value is broadcast.
//
//  Feature 2: //@param annotations rename sliders live.
//  Feature 3: Only as many sliders as @param annotations are shown.
//  Feature 4: Load/Save buttons + script dropdown (dyngenScripts/ folder).
//  Feature 5: Word-wrap toggle in the editor toolbar.
//

#ifndef scDynGenNode_h
#define scDynGenNode_h

#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanodeNodeModel.h"
#include "scNode.h"
#include "ofxSCSynth.h"

#include <map>
#include <array>
#include <bitset>
#include <mutex>
#include <vector>
#include <string>
#include <memory>
#include <cstdint>
#include <future>
#include <atomic>

class ofxSCServer;
class ofxSCSynth;

class scDynGenNode : public scNode {
public:
    // ── Constants (keep in sync with dyngen.scd) ─────────────────────────────
    static constexpr int kMaxSlots  = 16;   // pre-compiled slot count
    static constexpr int kMaxParams =  8;   // max param slots p0..p7
    static constexpr int kMaxChans  = 16;   // max audio channels

    // Slot hashes verified against SC via dyngen.scd / DynGenDef.prHashSymbol().
    // Regenerate if kMaxSlots changes: evaluate dyngen.scd and paste the output.
    static const int kSlotHashes[kMaxSlots];

    // ── Public interface ──────────────────────────────────────────────────────
    scDynGenNode();
    ~scDynGenNode();

    void setup()  override;
    void update(ofEventArgs &args) override;
    void activate()   override;
    void deactivate() override;

    // scNode interface
    void buildSynth(ofxSCServer* server) override;
    void createSynth(ofxSCServer* server) override;
    void moveSynthBefore(ofxSCServer* server, int nodeID) override;
    void free(ofxSCServer* server) override;

    void setOutputBus(ofxSCServer* server, int index, int bus) override;
    void setInputBus(ofxSCServer* server, scNode* node, int bus) override;
    void resetInputBusses(ofxSCServer* server, int targetBus = 0) override;

    int getOutputBusIndex(ofxSCServer* server, int index) override;
    int getLastSynthID(ofxSCServer* server) override;

    // Preset handling
    void presetSave(ofJson& json) override;
    void presetRecallAfterSettingParameters(ofJson& json) override;
    void loadBeforeConnections(ofJson& json) override;

    ofEvent<void> resendParams;
    int oldNumChannels = 0;

private:
    // ── Global slot pool ──────────────────────────────────────────────────────
    static std::bitset<kMaxSlots> sSlotPool;
    static std::mutex             sSlotMutex;

    static int  acquireSlot();
    static void releaseSlot(int idx);

    static int32_t scStringHash(const char* s);
    static int     computeSlotHash(int slotIdx);

    // ── Per-instance SC resources ─────────────────────────────────────────────
    int slotIndex         = -1;
    int activeServerCount =  0;

    std::map<ofxSCServer*, ofxSCSynth*>             synthInstances;
    std::map<ofxSCServer*, std::map<scNode*, int>>  inputBuses;
    std::map<ofxSCServer*, std::map<int, int>>      outputBuses;

    // ── Core parameter ────────────────────────────────────────────────────────
    ofParameter<int> numChannels;

    // ── Param slots p0..p7 ────────────────────────────────────────────────────
    // Storage for all kMaxParams slots; only activeParamCount are registered
    // with Oceanode at any given time (Feature 3).
    struct ParamSlot {
        ofParameter<vector<float>> param;
        std::shared_ptr<ofxOceanodeParameter<vector<float>>> registeredParam;
    };
    std::array<ParamSlot, kMaxParams> paramSlots;

    // Feature 3: how many param sliders are currently registered
    int activeParamCount = 0;

    // Dynamic param listeners (rebuilt when count changes)
    std::vector<std::unique_ptr<ofEventListener>> paramListeners;

    // ── Inspector params ──────────────────────────────────────────────────────
    ofParameter<float> editorWidth;
    ofParameter<float> editorHeight;

    // EEL2 code persisted as ofParameter<std::string> for preset save/load
    ofParameter<std::string> eel2CodeParam;

    // ── EEL2 code state ───────────────────────────────────────────────────────
    std::string              eel2StatusMsg;
    std::vector<std::string> accParamNames;  // rebuilt from @param annotations

    bool  codeDirty      = false;
    float codeDirtyTimer = 0.0f;
    static constexpr float kDebounce = 0.4f;

    static constexpr int kBufSize = 32768;
    char editorBuf[kBufSize];

    // ── Feature 4: file browser ───────────────────────────────────────────────
    std::vector<std::string> scriptFiles;     // cached list of .eel2 files
    int  selectedScriptIdx = 0;
    bool scriptListDirty   = true;
    bool pendingLoadDialog = false;
    bool pendingSaveDialog = false;
    std::string pendingScriptLoadPath;

    void refreshScriptList();
    void loadScriptFile(const std::string& path);
    void saveScriptFile(const std::string& path);

    // ── Editor widget region (stored so it can be re-ordered after params) ───
    // Params must appear BEFORE the editor in the Oceanode group.
    // updateParamCount() removes the editor, adds/removes params, then re-adds.
    ofxOceanodeNodeModel::customGuiRegion editorRegion;

    // ── Feature 5: word-wrap view ─────────────────────────────────────────────
    bool wrapView = false;

    // ── Helpers ───────────────────────────────────────────────────────────────
    std::string getSynthDefName() const;
    int         getSlotHash()     const;

    void sendCodeToServer(ofxSCServer* server);
    void sendCodeToAllServers();
    void sendParamsToSynth(ofxSCServer* server);

    // Feature 1: expand a vector to n channels (broadcast last element)
    std::vector<float> expandToChannels(const std::vector<float>& v, int n) const;

    // Feature 2+3: update registered param count + names from annotations
    struct ParamAnnotation {
        std::string name;
        float       defVal = 0.0f;
        float       minVal = 0.0f;
        float       maxVal = 1.0f;
    };
    std::vector<ParamAnnotation> parseAnnotations(const std::string& code);
    void updateParamCount(int newCount, const std::vector<ParamAnnotation>& anns = {});
    void rebuildParamListeners();
    void mergeParamNames(const std::vector<ParamAnnotation>& anns);

    static std::string trimStr(const std::string& s);

    void drawEditor();

    // ── Feature 6: LLM code generation (Claude via Anthropic API) ─────────────
    std::string              llmApiKey;
    std::future<std::string> llmFuture;
    std::atomic<bool>        llmPending  { false };
    std::atomic<bool>        llmDone     { false };
    bool                     llmHasError = false;
    bool                     llmFixMode  = false;   // false=Generate, true=Fix
    std::string              llmStatusMsg;
    char                     llmPromptBuf[2048] = {};

    // Set true before calling eel2CodeParam.set() to force synth recreation
    // (so @init runs fresh).  Cleared by the listener after recreation.
    bool codeRequiresRecreate = false;

    static std::string buildDynGenSystemPrompt();
    static std::string callAnthropicAPI(const std::string& fullPrompt,
                                        const std::string& systemPrompt,
                                        const std::string& apiKey);
    void requestLLMCode(const std::string& prompt);

    ofEventListeners listeners;
};

#endif /* scDynGenNode_h */
