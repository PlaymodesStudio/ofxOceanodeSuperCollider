//
//  scDynGenPolyNode.h
//  ofxOceanodeSuperCollider
//
//  Mono-authored DynGen node that auto-instantiates one script voice per
//  channel while preserving vector-addressable parameters in Oceanode.
//

#ifndef scDynGenPolyNode_h
#define scDynGenPolyNode_h

#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanodeNodeModel.h"
#include "scNode.h"
#include "ofxSCSynth.h"

#include <map>
#include <array>
#include <vector>
#include <string>
#include <memory>
#include <cstdint>
#include <future>
#include <atomic>

class ofxSCServer;
class ofxSCSynth;

class scDynGenPolyNode : public scNode {
public:
    static constexpr int kMaxSlots  = 16;
    static constexpr int kMaxParams = 8;
    static constexpr int kMaxChans  = 16;

    static const int kSlotHashes[kMaxSlots];

    scDynGenPolyNode();
    ~scDynGenPolyNode();

    void setup() override;
    void update(ofEventArgs &args) override;
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

    void presetSave(ofJson& json) override;
    void presetRecallAfterSettingParameters(ofJson& json) override;
    void loadBeforeConnections(ofJson& json) override;

    ofEvent<void> resendParams;
    int oldNumChannels = 0;

private:
    struct ChannelVoice {
        int channelIndex = 0;
        ofxSCSynth* synth = nullptr;
    };

    static int32_t scStringHash(const char* s);
    static int computeSlotHash(int slotIdx);
    static int acquireSlot();
    static void releaseSlot(int idx);

    int slotIndex = -1;

    std::map<ofxSCServer*, std::vector<ChannelVoice>> synthInstances;
    std::map<ofxSCServer*, std::map<scNode*, int>> inputBuses;
    std::map<ofxSCServer*, std::map<int, int>> outputBuses;

    ofParameter<int> numChannels;

    struct ParamSlot {
        ofParameter<vector<float>> param;
        std::shared_ptr<ofxOceanodeParameter<vector<float>>> registeredParam;
    };
    std::array<ParamSlot, kMaxParams> paramSlots;
    int activeParamCount = 0;
    std::vector<std::unique_ptr<ofEventListener>> paramListeners;

    ofParameter<float> editorWidth;
    ofParameter<float> editorHeight;
    ofParameter<std::string> eel2CodeParam;

    std::string eel2StatusMsg;
    std::vector<std::string> accParamNames;

    bool codeDirty = false;
    float codeDirtyTimer = 0.0f;
    static constexpr float kDebounce = 0.4f;

    static constexpr int kBufSize = 32768;
    char editorBuf[kBufSize];

    static constexpr const char* kScriptsDirectory = "Supercollider/dyngenPolyScripts";
    std::vector<std::string> scriptFiles;
    int selectedScriptIdx = 0;
    bool scriptListDirty = true;
    bool pendingLoadDialog = false;
    bool pendingSaveDialog = false;
    std::string pendingScriptLoadPath;

    void refreshScriptList();
    void loadScriptFile(const std::string& path);
    void saveScriptFile(const std::string& path);

    ofxOceanodeNodeModel::customGuiRegion editorRegion;
    bool wrapView = false;

    std::string getSynthDefName() const;
    int getSlotHash() const;

    int getVoiceCount() const;
    float valueForChannel(const std::vector<float>& values, int channelIndex) const;
    void configureVoiceForChannel(ofxSCServer* server, ChannelVoice& voice);
    void sendParamsToSynth(ofxSCServer* server);
    void sendCodeToServer(ofxSCServer* server);
    void sendCodeToAllServers();
    void rebuildServerVoices(ofxSCServer* server);

    struct ParamAnnotation {
        std::string name;
        float defVal = 0.0f;
        float minVal = 0.0f;
        float maxVal = 1.0f;
    };
    std::vector<ParamAnnotation> parseAnnotations(const std::string& code);
    void updateParamCount(int newCount, const std::vector<ParamAnnotation>& anns = {});
    void rebuildParamListeners();
    void mergeParamNames(const std::vector<ParamAnnotation>& anns);

    static std::string trimStr(const std::string& s);

    void drawEditor();

    std::string llmApiKey;
    std::future<std::string> llmFuture;
    std::atomic<bool> llmPending { false };
    std::atomic<bool> llmDone { false };
    bool llmHasError = false;
    bool llmFixMode = false;
    std::string llmStatusMsg;
    char llmPromptBuf[2048] = {};

    bool codeRequiresRecreate = false;

    static std::string buildDynGenPolySystemPrompt();
    static std::string callAnthropicAPI(const std::string& fullPrompt,
                                        const std::string& systemPrompt,
                                        const std::string& apiKey);
    void requestLLMCode(const std::string& prompt);

    ofEventListeners listeners;
};

#endif /* scDynGenPolyNode_h */
