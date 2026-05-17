//
//  scDynGenPolyNode.cpp
//  ofxOceanodeSuperCollider
//
//  Mono-authored DynGen node that auto-instantiates one script voice per
//  channel while preserving vector-addressable parameters in Oceanode.
//

#include "scDynGenPolyNode.h"
#include "../shared/scDynGenSlotPool.h"
#include "ofxSCSynth.h"
#include "ofxSCServer.h"
#include "ofxSuperCollider.h"
#include "imgui.h"
#include "ofJson.h"

#include <algorithm>
#include <sstream>
#include <fstream>
#include <cstring>
#include <cmath>
#include <chrono>
#include <cstdio>

const int scDynGenPolyNode::kSlotHashes[scDynGenPolyNode::kMaxSlots] = {
    -93156,
    595923,
    439118,
    810776,
    195493,
    249700,
    420841,
    5245,
    635120,
    -511249,
    -226989,
    -692348,
    -573336,
    809716,
    212865,
    105950
};

int32_t scDynGenPolyNode::scStringHash(const char* s) {
    uint32_t hash = 0;
    while(*s) {
        hash = hash * 1664525u + 1013904223u + (unsigned char)*s++;
    }
    return (int32_t)hash;
}

int scDynGenPolyNode::computeSlotHash(int slotIdx) {
    if(slotIdx >= 0 && slotIdx < kMaxSlots && kSlotHashes[slotIdx] != 0) {
        return kSlotHashes[slotIdx];
    }

    std::string name = "dyngenSlot" + std::to_string(slotIdx);
    int32_t h = scStringHash(name.c_str());
    int64_t absH = (h < 0) ? -(int64_t)h : (int64_t)h;
    int64_t modH = absH % (int64_t)(1 << 20);
    int sign = (h > 0) ? 1 : (h < 0) ? -1 : 0;
    return (int)(modH * sign);
}

int scDynGenPolyNode::acquireSlot() {
    return scDynGenSlotPool::acquireSlot("scDynGenPolyNode", kMaxSlots);
}

void scDynGenPolyNode::releaseSlot(int idx) {
    scDynGenSlotPool::releaseSlot("scDynGenPolyNode", idx, kMaxSlots);
}

scDynGenPolyNode::scDynGenPolyNode() : scNode("DynGen Poly") {
    description = "Mono-authored DynGen node that runs one independent script instance per channel while keeping vector-addressable parameters in Oceanode.";
    std::memset(editorBuf, 0, sizeof(editorBuf));
    std::memset(llmPromptBuf, 0, sizeof(llmPromptBuf));
    eel2StatusMsg = "Ready";

    auto tryReadKey = [&](const std::string& path) {
        std::ifstream f(path);
        if(f) {
            std::getline(f, llmApiKey);
            return !llmApiKey.empty();
        }
        return false;
    };

    const char* envKey = std::getenv("ANTHROPIC_API_KEY");
    if(envKey && *envKey) {
        llmApiKey = std::string(envKey);
    } else if(!tryReadKey(ofToDataPath("anthropic_api_key.txt"))) {
        tryReadKey(ofFilePath::getUserHomeDir() + "/.anthropic_api_key");
    }

    while(!llmApiKey.empty() && (llmApiKey.back() == '\n' ||
                                 llmApiKey.back() == '\r' ||
                                 llmApiKey.back() == ' ')) {
        llmApiKey.pop_back();
    }
}

scDynGenPolyNode::~scDynGenPolyNode() {
    try {
        listeners.unsubscribeAll();
        paramListeners.clear();

        if(slotIndex >= 0) {
            int hash = getSlotHash();
            for(auto& pair : synthInstances) {
                bool sentFree = false;
                for(auto& voice : pair.second) {
                    if(!voice.synth) continue;
                    if(!sentFree) {
                        ofxOscMessage freeMsg;
                        freeMsg.setAddress("/cmd");
                        freeMsg.addStringArg("dyngenfree");
                        freeMsg.addIntArg(hash);
                        pair.first->sendMsg(freeMsg);
                        sentFree = true;
                    }

                    voice.synth->free();
                    delete voice.synth;
                    voice.synth = nullptr;
                }
            }
            synthInstances.clear();
            releaseSlot(slotIndex);
        }
    } catch(const std::exception& e) {
        ofLogError("scDynGenPolyNode") << "Destructor: " << e.what();
    }
}

void scDynGenPolyNode::setup() {
    addParameter(numChannels.set("Num Channels", 1, 1, kMaxChans));

    const std::string defaultCode =
        "// DynGen Poly - write mono DSP, auto-instantiated per channel\n"
        "// Audio input:  in(0)\n"
        "// Param access: in(1 + pi)   where pi = param index\n"
        "// Output:       out0\n"
        "// Declare params: //@param Name : default, min, max\n"
        "// Oceanode vector params are split per channel automatically.\n"
        "\n"
        "@sample\n"
        "out0 = in(0);\n";

    addInspectorParameter(eel2CodeParam.set("EEL2 Code", defaultCode));
    std::strncpy(editorBuf, defaultCode.c_str(), kBufSize - 1);

    addInspectorParameter(editorWidth.set("Editor Width", 380.0f, 160.0f, 800.0f));
    addInspectorParameter(editorHeight.set("Editor Height", 220.0f, 80.0f, 600.0f));

    scNode::addInput("In");
    scNode::addOutput("Out");

    listeners.push(numChannels.newListener([this](int& ch) {
        if(ch < 1 || ch > kMaxChans) return;
        if(oldNumChannels != ch) {
            for(auto& pair : synthInstances) {
                rebuildServerVoices(pair.first);
            }
        }
        oldNumChannels = ch;
    }));

    listeners.push(eel2CodeParam.newListener([this](std::string& code) {
        std::strncpy(editorBuf, code.c_str(), kBufSize - 1);
        editorBuf[kBufSize - 1] = '\0';

        auto anns = parseAnnotations(code);
        mergeParamNames(anns);
        updateParamCount((int)anns.size(), anns);

        if(codeRequiresRecreate) {
            codeRequiresRecreate = false;
            for(auto& pair : synthInstances) {
                bool wasLatency = pair.first->getBLatency();
                pair.first->setBLatency(false);
                sendCodeToServer(pair.first);
                pair.first->setBLatency(wasLatency);
                rebuildServerVoices(pair.first);
            }
        } else {
            sendCodeToAllServers();
        }

        eel2StatusMsg = "Loaded";
    }));

    listeners.push(resendParams.newListener([this]() {
        for(auto& pair : synthInstances) {
            sendParamsToSynth(pair.first);
        }
    }));

    std::function<void()> drawFn = [this]() { drawEditor(); };
    editorRegion.set("EEL2 Editor", drawFn);
    addParameter(editorRegion,
        ofxOceanodeParameterFlags_DisableInConnection  |
        ofxOceanodeParameterFlags_DisableOutConnection |
        ofxOceanodeParameterFlags_DisableSavePreset    |
        ofxOceanodeParameterFlags_DisableSaveProject);

    ofDirectory::createDirectory(ofToDataPath(kScriptsDirectory), false, true);
}

void scDynGenPolyNode::update(ofEventArgs&) {
    if(pendingLoadDialog) {
        pendingLoadDialog = false;
        ofFileDialogResult r = ofSystemLoadDialog(
            "Load EEL2 Script", false, ofToDataPath(kScriptsDirectory));
        if(r.bSuccess) loadScriptFile(r.filePath);
    }
    if(!pendingScriptLoadPath.empty()) {
        std::string path = std::move(pendingScriptLoadPath);
        pendingScriptLoadPath.clear();
        loadScriptFile(path);
    }

    if(pendingSaveDialog) {
        pendingSaveDialog = false;
        ofFileDialogResult r = ofSystemSaveDialog(
            "dyngen_poly_script.eel2", "Save EEL2 Script");
        if(r.bSuccess) saveScriptFile(r.filePath);
    }

    if(llmPending.load() && llmFuture.valid()) {
        if(llmFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            std::string result = llmFuture.get();
            bool isError = (result.rfind("// Error:", 0) == 0 ||
                            result.rfind("// API error", 0) == 0);
            if(isError) {
                llmStatusMsg = result;
                llmHasError = true;
            } else {
                std::strncpy(editorBuf, result.c_str(), kBufSize - 1);
                editorBuf[kBufSize - 1] = '\0';
                codeDirty = true;
                codeDirtyTimer = 0.0f;
                codeRequiresRecreate = true;
                eel2StatusMsg = "AI generated";
                llmHasError = false;
            }
            llmPending.store(false);
            llmDone.store(true);
        }
    }

    if(!codeDirty) return;

    codeDirtyTimer += ofGetLastFrameTime();
    if(codeDirtyTimer < kDebounce) return;

    codeDirty = false;
    codeDirtyTimer = 0.0f;

    std::string newCode(editorBuf);
    eel2CodeParam.set(newCode);
    eel2StatusMsg = "Code sent";
}

void scDynGenPolyNode::activate() {
    for(auto& pair : synthInstances) {
        for(auto& voice : pair.second) {
            if(voice.synth) voice.synth->run(true);
        }
    }
}

void scDynGenPolyNode::deactivate() {
    for(auto& pair : synthInstances) {
        for(auto& voice : pair.second) {
            if(voice.synth) voice.synth->run(false);
        }
    }
}

void scDynGenPolyNode::buildSynth(ofxSCServer* server) {
    if(!server) return;

    if(slotIndex < 0) {
        slotIndex = acquireSlot();
        if(slotIndex < 0) {
            eel2StatusMsg = "Error: no free slots";
            return;
        }
        ofLogNotice("scDynGenPolyNode") << "Acquired slot " << slotIndex
                                        << " (hash=" << getSlotHash() << ")";
    }

    auto& voices = synthInstances[server];
    voices.clear();
    voices.reserve(getVoiceCount());
    for(int ch = 0; ch < getVoiceCount(); ch++) {
        ChannelVoice voice;
        voice.channelIndex = ch;
        voice.synth = new ofxSCSynth(getSynthDefName(), server);
        voices.push_back(voice);
    }
}

void scDynGenPolyNode::createSynth(ofxSCServer* server) {
    auto it = synthInstances.find(server);
    if(!server || it == synthInstances.end() || slotIndex < 0) return;

    try {
        bool wasLatency = server->getBLatency();
        server->setBLatency(false);
        sendCodeToServer(server);
        server->setBLatency(wasLatency);

        int previousNodeID = 1;
        bool firstVoice = true;
        for(auto& voice : it->second) {
            if(!voice.synth) continue;
            configureVoiceForChannel(server, voice);
            if(firstVoice) {
                voice.synth->createAndRun(0, 1, getActive());
                firstVoice = false;
            } else {
                voice.synth->createAndRun(3, previousNodeID, getActive());
            }
            previousNodeID = voice.synth->nodeID;
        }

        eel2StatusMsg = "Script sent";
    } catch(const std::exception& e) {
        ofLogError("scDynGenPolyNode") << "createSynth: " << e.what();
        eel2StatusMsg = std::string("Error: ") + e.what();
    }
}

void scDynGenPolyNode::moveSynthBefore(ofxSCServer* server, int nodeID) {
    auto it = synthInstances.find(server);
    if(!server || it == synthInstances.end()) return;

    try {
        resendParams.notify();
        int nextTarget = nodeID;
        for(auto voiceIt = it->second.rbegin(); voiceIt != it->second.rend(); ++voiceIt) {
            if(!voiceIt->synth) continue;
            voiceIt->synth->moveBefore(nextTarget);
            nextTarget = voiceIt->synth->nodeID;
        }
    } catch(const std::exception& e) {
        ofLogError("scDynGenPolyNode") << "moveSynthBefore: " << e.what();
    }
}

void scDynGenPolyNode::free(ofxSCServer* server) {
    if(!server) return;

    try {
        auto it = synthInstances.find(server);
        if(it != synthInstances.end()) {
            for(auto& voice : it->second) {
                if(!voice.synth) continue;
                voice.synth->free();
                delete voice.synth;
                voice.synth = nullptr;
            }
            synthInstances.erase(it);
        }
        inputBuses.erase(server);
        outputBuses.erase(server);
    } catch(const std::exception& e) {
        ofLogError("scDynGenPolyNode") << "free: " << e.what();
    }
}

void scDynGenPolyNode::setInputBus(ofxSCServer* server, scNode* node, int bus) {
    if(!server || !node) return;
    inputBuses[server][node] = bus;
    sendParamsToSynth(server);
}

void scDynGenPolyNode::resetInputBusses(ofxSCServer* server, int targetBus) {
    if(!server) return;
    inputBuses[server].clear();
    auto it = synthInstances.find(server);
    if(it == synthInstances.end()) return;

    for(auto& voice : it->second) {
        if(voice.synth) voice.synth->set("in", targetBus + voice.channelIndex);
    }
}

void scDynGenPolyNode::setOutputBus(ofxSCServer* server, int index, int bus) {
    if(!server) return;
    outputBuses[server][index] = bus;
    sendParamsToSynth(server);
}

int scDynGenPolyNode::getOutputBusIndex(ofxSCServer* server, int index) {
    if(outputBuses.count(server) && outputBuses[server].count(index)) {
        return outputBuses[server].at(index);
    }
    return -1;
}

int scDynGenPolyNode::getLastSynthID(ofxSCServer* server) {
    auto it = synthInstances.find(server);
    if(it == synthInstances.end() || it->second.empty()) return -1;

    for(auto voiceIt = it->second.rbegin(); voiceIt != it->second.rend(); ++voiceIt) {
        if(voiceIt->synth) return voiceIt->synth->nodeID;
    }
    return -1;
}

void scDynGenPolyNode::presetSave(ofJson& json) {
    for(int i = 0; i < activeParamCount; i++) {
        json["dyngen_poly_p" + std::to_string(i)] = paramSlots[i].param.get();
    }
    json["dyngen_poly_param_count"] = activeParamCount;
}

void scDynGenPolyNode::presetRecallAfterSettingParameters(ofJson& json) {
    for(int i = 0; i < activeParamCount; i++) {
        std::string key = "dyngen_poly_p" + std::to_string(i);
        if(json.contains(key) && json[key].is_array()) {
            std::vector<float> values;
            for(auto& e : json[key]) {
                if(e.is_number()) values.push_back(e.get<float>());
            }
            if(!values.empty()) paramSlots[i].param.set(values);
        }
    }
}

void scDynGenPolyNode::loadBeforeConnections(ofJson& json) {
    deserializeParameter(json, eel2CodeParam);
    const std::string code = eel2CodeParam.get();
    std::strncpy(editorBuf, code.c_str(), kBufSize - 1);
    editorBuf[kBufSize - 1] = '\0';
    auto anns = parseAnnotations(code);
    mergeParamNames(anns);
    updateParamCount((int)anns.size(), anns);
}

std::string scDynGenPolyNode::getSynthDefName() const {
    int slot = (slotIndex >= 0) ? slotIndex : 0;
    return "DynGenWrapper_1_" + std::to_string(slot);
}

int scDynGenPolyNode::getSlotHash() const {
    return computeSlotHash(slotIndex);
}

int scDynGenPolyNode::getVoiceCount() const {
    return std::max(1, std::min(numChannels.get(), kMaxChans));
}

float scDynGenPolyNode::valueForChannel(const std::vector<float>& values, int channelIndex) const {
    if(values.empty()) return 0.0f;
    int idx = std::min(channelIndex, (int)values.size() - 1);
    return values[idx];
}

void scDynGenPolyNode::configureVoiceForChannel(ofxSCServer* server, ChannelVoice& voice) {
    if(!voice.synth) return;

    int inputBase = 0;
    if(inputBuses.count(server) && !inputBuses[server].empty()) {
        inputBase = inputBuses[server].begin()->second;
    }
    voice.synth->set("in", inputBase + voice.channelIndex);

    if(outputBuses.count(server) && outputBuses[server].count(0)) {
        voice.synth->set("out", outputBuses[server].at(0) + voice.channelIndex);
    }

    for(int i = 0; i < kMaxParams; i++) {
        if(i < activeParamCount) {
            voice.synth->set("p" + std::to_string(i),
                             valueForChannel(paramSlots[i].param.get(), voice.channelIndex));
        } else {
            voice.synth->set("p" + std::to_string(i), 0.0);
        }
    }
}

void scDynGenPolyNode::sendParamsToSynth(ofxSCServer* server) {
    auto it = synthInstances.find(server);
    if(it == synthInstances.end()) return;

    for(auto& voice : it->second) {
        configureVoiceForChannel(server, voice);
    }
}

void scDynGenPolyNode::sendCodeToServer(ofxSCServer* server) {
    if(!server || slotIndex < 0) return;

    const std::string code(editorBuf);
    int hash = getSlotHash();
    int numParams = (int)accParamNames.size();

    size_t estimatedSize = 32 + code.size() + 4;
    for(auto& p : accParamNames) estimatedSize += p.size() + 8;

    static constexpr size_t kOscThreshold = 16000;

    if(estimatedSize < kOscThreshold) {
        ofxOscMessage msg;
        msg.setAddress("/cmd");
        msg.addStringArg("dyngenscript");
        msg.addIntArg(hash);
        msg.addStringArg(code);
        msg.addIntArg(numParams);
        for(auto& p : accParamNames) msg.addStringArg("_" + p);
        server->sendMsg(msg);
    } else {
        std::string tmpPath = ofGetTimestampString(
            ofGetEnv("TMPDIR", "/tmp") + "/dyngen_poly_%Y%m%d_%H%M%S_%i.eel2");
        std::ofstream f(tmpPath);
        if(!f.is_open()) {
            ofLogError("scDynGenPolyNode") << "Cannot write temp file: " << tmpPath;
            eel2StatusMsg = "Error: temp file write failed";
            return;
        }
        f << code;
        f.close();

        ofxOscMessage msg;
        msg.setAddress("/cmd");
        msg.addStringArg("dyngenfile");
        msg.addIntArg(hash);
        msg.addStringArg(tmpPath);
        msg.addIntArg(numParams);
        for(auto& p : accParamNames) msg.addStringArg("_" + p);
        server->sendMsg(msg);
    }
}

void scDynGenPolyNode::sendCodeToAllServers() {
    for(auto& pair : synthInstances) {
        sendCodeToServer(pair.first);
    }
}

void scDynGenPolyNode::rebuildServerVoices(ofxSCServer* server) {
    auto it = synthInstances.find(server);
    if(server == nullptr || it == synthInstances.end()) return;

    auto& voices = it->second;
    int newVoiceCount = getVoiceCount();
    int oldVoiceCount = (int)voices.size();
    int reusedVoices = std::min(oldVoiceCount, newVoiceCount);
    int previousNodeID = 1;

    for(int i = 0; i < reusedVoices; i++) {
        ChannelVoice newVoice;
        newVoice.channelIndex = i;
        newVoice.synth = new ofxSCSynth(getSynthDefName(), server);
        configureVoiceForChannel(server, newVoice);
        newVoice.synth->createAndRun(4, voices[i].synth->nodeID, getActive());
        delete voices[i].synth;
        voices[i] = newVoice;
        previousNodeID = newVoice.synth->nodeID;
    }

    for(int i = reusedVoices; i < oldVoiceCount; i++) {
        if(voices[i].synth) {
            voices[i].synth->free();
            delete voices[i].synth;
        }
    }

    voices.resize(reusedVoices);

    for(int ch = reusedVoices; ch < newVoiceCount; ch++) {
        ChannelVoice voice;
        voice.channelIndex = ch;
        voice.synth = new ofxSCSynth(getSynthDefName(), server);
        configureVoiceForChannel(server, voice);
        if(ch == 0) {
            voice.synth->createAndRun(0, 1, getActive());
        } else {
            voice.synth->createAndRun(3, previousNodeID, getActive());
        }
        previousNodeID = voice.synth->nodeID;
        voices.push_back(voice);
    }

    for(int ch = 0; ch < (int)voices.size(); ch++) {
        voices[ch].channelIndex = ch;
    }
}

std::string scDynGenPolyNode::trimStr(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if(a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<scDynGenPolyNode::ParamAnnotation>
scDynGenPolyNode::parseAnnotations(const std::string& code) {
    std::vector<ParamAnnotation> result;
    std::istringstream ss(code);
    std::string line;

    while(std::getline(ss, line) && (int)result.size() < kMaxParams) {
        auto pos = line.find("//@param ");
        if(pos == std::string::npos) continue;

        std::string rest = line.substr(pos + 9);
        auto colon = rest.find(':');

        ParamAnnotation ann;
        if(colon == std::string::npos) {
            ann.name = trimStr(rest);
        } else {
            ann.name = trimStr(rest.substr(0, colon));
            std::string vals = rest.substr(colon + 1);
            std::istringstream vs(vals);
            std::string tok;
            std::vector<float> nums;
            while(std::getline(vs, tok, ',')) {
                try { nums.push_back(std::stof(trimStr(tok))); }
                catch(...) {}
            }
            if(nums.size() >= 1) ann.defVal = nums[0];
            if(nums.size() >= 2) ann.minVal = nums[1];
            if(nums.size() >= 3) ann.maxVal = nums[2];
        }

        if(!ann.name.empty()) result.push_back(ann);
    }
    return result;
}

void scDynGenPolyNode::updateParamCount(int newCount,
                                        const std::vector<ParamAnnotation>& anns) {
    newCount = std::max(0, std::min(newCount, kMaxParams));

    bool countChanged = (newCount != activeParamCount);
    bool namesChanged = false;
    if(!countChanged) {
        for(int i = 0; i < newCount; i++) {
            std::string expectedName = (i < (int)anns.size() && !anns[i].name.empty())
                                       ? anns[i].name : ("P" + std::to_string(i));
            if(paramSlots[i].param.getName() != expectedName) {
                namesChanged = true;
                break;
            }
        }
    }
    if(!countChanged && !namesChanged) return;

    removeParameter("EEL2 Editor");
    for(int i = 0; i < activeParamCount; i++) {
        if(paramSlots[i].registeredParam) {
            removeParameter(paramSlots[i].registeredParam->getName());
            paramSlots[i].registeredParam.reset();
        } else {
            removeParameter(paramSlots[i].param.getName());
        }
    }

    for(int i = 0; i < newCount; i++) {
        std::string name = (i < (int)anns.size() && !anns[i].name.empty())
                           ? anns[i].name : ("P" + std::to_string(i));
        float minV = (i < (int)anns.size()) ? anns[i].minVal : 0.0f;
        float maxV = (i < (int)anns.size()) ? anns[i].maxVal : 1.0f;
        float defV = (i < (int)anns.size()) ? anns[i].defVal : 0.0f;
        if(minV >= maxV) maxV = minV + 1.0f;

        vector<float> slotValue = (i < activeParamCount)
                                  ? paramSlots[i].param.get()
                                  : vector<float>{defV};
        if(slotValue.empty()) slotValue = vector<float>{defV};

        paramSlots[i].param.set(name,
                                slotValue,
                                vector<float>{minV},
                                vector<float>{maxV});

        paramSlots[i].registeredParam = addParameter(paramSlots[i].param);
    }

    for(int i = newCount; i < kMaxParams; i++) {
        paramSlots[i].registeredParam.reset();
    }

    addParameter(editorRegion,
        ofxOceanodeParameterFlags_DisableInConnection  |
        ofxOceanodeParameterFlags_DisableOutConnection |
        ofxOceanodeParameterFlags_DisableSavePreset    |
        ofxOceanodeParameterFlags_DisableSaveProject);

    activeParamCount = newCount;
    rebuildParamListeners();

    for(auto& pair : synthInstances) {
        sendParamsToSynth(pair.first);
    }
}

void scDynGenPolyNode::rebuildParamListeners() {
    paramListeners.clear();
    for(int i = 0; i < activeParamCount; i++) {
        int idx = i;
        paramListeners.push_back(std::make_unique<ofEventListener>(
            paramSlots[idx].param.newListener([this, idx](vector<float>& values) {
                for(auto& pair : synthInstances) {
                    for(auto& voice : pair.second) {
                        if(voice.synth) {
                            voice.synth->set("p" + std::to_string(idx),
                                             valueForChannel(values, voice.channelIndex));
                        }
                    }
                }
            })
        ));
    }
}

void scDynGenPolyNode::mergeParamNames(const std::vector<ParamAnnotation>& anns) {
    accParamNames.clear();
    for(auto& ann : anns) {
        std::string lc = ann.name;
        std::transform(lc.begin(), lc.end(), lc.begin(), ::tolower);
        if(std::find(accParamNames.begin(), accParamNames.end(), lc) == accParamNames.end()) {
            accParamNames.push_back(lc);
        }
    }
}

void scDynGenPolyNode::refreshScriptList() {
    scriptFiles.clear();
    ofDirectory dir(ofToDataPath(kScriptsDirectory));
    dir.allowExt("eel2");
    if(dir.exists()) {
        dir.listDir();
        for(int i = 0; i < (int)dir.size(); i++) {
            scriptFiles.push_back(dir.getPath(i));
        }
    }
    scriptListDirty = false;
    selectedScriptIdx = std::min(selectedScriptIdx, (int)scriptFiles.size() - 1);
    if(selectedScriptIdx < 0) selectedScriptIdx = 0;
}

void scDynGenPolyNode::loadScriptFile(const std::string& path) {
    std::ifstream f(path);
    if(!f.is_open()) {
        eel2StatusMsg = "Error: cannot open file";
        return;
    }

    std::string code((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    codeRequiresRecreate = true;
    eel2CodeParam.set(code);
    eel2StatusMsg = "Loaded: " + ofFilePath::getFileName(path);
    scriptListDirty = false;
}

void scDynGenPolyNode::saveScriptFile(const std::string& path) {
    std::string dir = ofFilePath::getEnclosingDirectory(path);
    ofDirectory::createDirectory(dir, false, true);

    std::ofstream f(path);
    if(!f.is_open()) {
        eel2StatusMsg = "Error: cannot save file";
        return;
    }

    f << std::string(editorBuf);
    eel2StatusMsg = "Saved: " + ofFilePath::getFileName(path);
    scriptListDirty = true;
}

std::string scDynGenPolyNode::buildDynGenPolySystemPrompt() {
    return
R"SYSPROMPT(You are an EEL2 code generator for the DynGen Poly node in Oceanode, a modular audio environment built on SuperCollider. DynGen Poly runs ONE mono DynGen script per audio channel. The same mono script is instantiated independently for every channel.

Output ONLY valid EEL2 code. No markdown, no code fences, no prose before or after. Start directly with //@param declarations, @init, or @sample.

=== CORE MODEL ===
1. ALWAYS write MONO code for a single voice.
2. Read audio only from in(0).
3. Read params only from in(1 + pIndex). Example: p0=in(1), p1=in(2).
4. Write audio only to out0.
5. Oceanode duplicates the script per channel and fans vector params out channel-by-channel outside the script.
6. DO NOT reference channel counts, channel loops, out1/out2, or stereo-specific indexing.

=== CRITICAL CONSTRAINTS ===
1. num_ch is NEVER available (always 0). Do not use it.
2. $pi is NOT defined. Use 6.28318530 for 2π and 3.14159265 for π.
3. No file I/O, no strings, no external calls, no classes.
4. buf[] is the ONLY persistent heap. Partition it in @init with non-overlapping base offsets.
5. Keep total buf[] usage under 100000 floats.
6. Each buffer MUST wrap using ITS OWN size.
7. Available math/functions include:
   exp() sin() cos() tan() asin() acos() atan() atan2() log() log10() sqrt() invsqrt()
   abs() sign() floor() ceil() max() min() pow()
   clip() wrap() fold() mod() lin() cubic()
   delta(state, signal) history(state, signal) latch(state, signal, trigger)
   bufRead() bufReadL() bufReadC() bufWrite() bufRate() bufChannels() bufFrames()
   poll() print() printMem() setDone() doneAction()
8. srate is available in all sections.
9. blockSize, numIn, numOut, sampleIndex, and blockNum are available in DynGen v0.4.0.
10. Conditional syntax: condition ? value_if_true : value_if_false

=== OCEANODE PARAM DECLARATION ===
// @param lines are Oceanode UI annotations:
//@param Name : default, min, max
Maximum 8 params (p0..p7). These are not native DynGen @param declarations.
Do not output native DynGen @param lines and do not use _paramName variables.

=== INPUT LAYOUT FOR THIS NODE ===
in(0)      = current audio sample for this channel
in(1)      = p0 for this channel
in(2)      = p1 for this channel
in(3)      = p2 for this channel
...

=== OUTPUT LAYOUT FOR THIS NODE ===
out0 = processed audio sample for this channel

=== CODE SECTIONS ===
@init   — runs once when this channel voice is created.
@block  — runs once per audio block.
@sample — runs once per audio sample.

=== EXAMPLES ===

// Passthrough
@sample
out0 = in(0);

// Gain
//@param Gain : 1.0, 0.0, 4.0
@sample
out0 = in(0) * in(1);

// Hard-clip distortion
//@param Drive : 3.0, 1.0, 20.0
//@param Mix   : 0.5, 0.0, 1.0
@block
drive = in(1);
mix = in(2);
@sample
wet = in(0) * drive;
wet = max(-1.0, min(1.0, wet));
out0 = in(0) * (1 - mix) + wet * mix;

// One-pole low-pass
//@param Cutoff : 1200.0, 20.0, 20000.0
@init
lp_state = 0;
@block
cutoff_hz = in(1);
lp_a = 1.0 - exp(-6.28318530 * cutoff_hz / srate);
@sample
lp_state += lp_a * (in(0) - lp_state);
out0 = lp_state;

// Sine oscillator
//@param Freq : 440.0, 20.0, 20000.0
//@param Amp  : 0.5, 0.0, 1.0
@init
phase = 0;
@block
freq = in(1);
amp = in(2);
phase_inc = 6.28318530 * freq / srate;
@sample
out0 = sin(phase) * amp;
phase += phase_inc;
phase >= 6.28318530 ? phase -= 6.28318530;
)SYSPROMPT";
}

std::string scDynGenPolyNode::callAnthropicAPI(const std::string& fullPrompt,
                                               const std::string& systemPrompt,
                                               const std::string& apiKey) {
    try {
        ofJson requestBody = {
            {"model", "claude-sonnet-4-5"},
            {"max_tokens", 2048},
            {"system", systemPrompt},
            {"messages", ofJson::array({{{"role", "user"}, {"content", fullPrompt}}})}
        };
        std::string bodyStr = requestBody.dump();

        std::string tmpPath = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
                              + "/dyngen_poly_api_req.json";
        {
            std::ofstream tf(tmpPath);
            if(!tf.is_open()) return "// Error: cannot write temp file " + tmpPath;
            tf << bodyStr;
        }

        std::string cmd =
            "curl -s"
            " -X POST https://api.anthropic.com/v1/messages"
            " -H 'content-type: application/json'"
            " -H 'x-api-key: " + apiKey + "'"
            " -H 'anthropic-version: 2023-06-01'"
            " -d @" + tmpPath +
            " 2>&1";

        FILE* pipe = popen(cmd.c_str(), "r");
        if(!pipe) return "// Error: popen failed";

        std::string respStr;
        char buf[4096];
        while(fgets(buf, sizeof(buf), pipe)) respStr += buf;
        pclose(pipe);

        std::remove(tmpPath.c_str());

        ofJson obj = ofJson::parse(respStr);
        if(obj.contains("error")) {
            return "// API error: " + obj["error"].value("message", "unknown error");
        }

        auto& content = obj["content"];
        if(!content.is_array() || content.empty()) {
            return "// Error: unexpected response format";
        }
        return content[0].value("text", "");
    } catch(const std::exception& e) {
        return std::string("// Error: ") + e.what();
    }
}

void scDynGenPolyNode::requestLLMCode(const std::string& userPrompt) {
    if(llmApiKey.empty()) {
        llmStatusMsg = "// Error: no API key. Place key in data/anthropic_api_key.txt";
        llmHasError = true;
        llmDone.store(true);
        return;
    }

    llmPending.store(true);
    llmDone.store(false);
    llmHasError = false;
    llmStatusMsg = "Generating...";

    int channels = numChannels.get();
    std::string sysP = buildDynGenPolySystemPrompt();
    std::string key = llmApiKey;

    std::string fullPrompt;
    if(llmFixMode) {
        std::string issue = userPrompt.empty()
            ? "Fix any bugs and make sure the mono script runs correctly when duplicated across channels."
            : "Issue: " + userPrompt;
        fullPrompt =
            "Fix this DynGen Poly mono EEL2 script. The node will instantiate it across "
            + std::to_string(channels) + " channels outside the script.\n\n"
            "Current code:\n" + std::string(editorBuf) + "\n\n" + issue;
    } else {
        fullPrompt =
            "Generate a mono DynGen Poly EEL2 script. The node will instantiate it across "
            + std::to_string(channels) + " channels outside the script.\n\n"
            + userPrompt;
    }

    llmFuture = std::async(std::launch::async,
        [fullPrompt, sysP, key]() -> std::string {
            return callAnthropicAPI(fullPrompt, sysP, key);
        });
}

void scDynGenPolyNode::drawEditor() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 cursor = ImGui::GetCursorScreenPos();

    const float W = editorWidth.get();
    const float H = editorHeight.get();
    const float pad = 4.0f;
    const float rowH = ImGui::GetTextLineHeight() + 4.0f;
    const float statusH = 16.0f;

    if(scriptListDirty) refreshScriptList();

    const float totalH = rowH + H + statusH + pad * 4;
    dl->AddRectFilled(
        cursor,
        ImVec2(cursor.x + W + pad * 2, cursor.y + totalH),
        IM_COL32(10, 12, 18, 255), 4.0f);

    ImGui::SetCursorScreenPos(ImVec2(cursor.x + pad, cursor.y + pad));

    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(35, 55, 85, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(50, 80, 120, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(70, 110, 160, 255));

    if(ImGui::Button("Open", ImVec2(46, 0))) pendingLoadDialog = true;
    ImGui::SameLine(0, 4);
    if(ImGui::Button("Save", ImVec2(46, 0))) pendingSaveDialog = true;
    ImGui::SameLine(0, 4);

    {
        bool wv = wrapView;
        if(wv) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(45, 90, 45, 255));
        if(ImGui::Button(wv ? "Edit" : "Wrap", ImVec2(46, 0))) wrapView = !wrapView;
        if(wv) ImGui::PopStyleColor();
    }
    ImGui::SameLine(0, 4);

    {
        bool busy = llmPending.load();
        ImGui::PushStyleColor(ImGuiCol_Button,
            busy ? IM_COL32(40, 20, 65, 255) : IM_COL32(60, 38, 90, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(90, 58, 130, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(115, 78, 165, 255));
        if(ImGui::Button(busy ? "···" : "AI", ImVec2(34, 0)) && !busy) {
            ImGui::OpenPopup("##llm_popup");
        }
        ImGui::PopStyleColor(3);
    }
    ImGui::SameLine(0, 4);

    if(!scriptFiles.empty()) {
        std::vector<std::string> names;
        for(auto& fp : scriptFiles) names.push_back(ofFilePath::getFileName(fp));
        std::vector<const char*> items;
        for(auto& n : names) items.push_back(n.c_str());

        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(25, 38, 58, 255));
        ImGui::SetNextItemWidth(W - 246.0f);
        ImGui::Combo("##scripts", &selectedScriptIdx, items.data(), (int)items.size());
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 4);
        if(ImGui::Button("Load##file", ImVec2(46, 0))) {
            if(selectedScriptIdx >= 0 && selectedScriptIdx < (int)scriptFiles.size()) {
                pendingScriptLoadPath = scriptFiles[selectedScriptIdx];
            }
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(80, 90, 100, 200));
        ImGui::TextUnformatted("(no scripts)");
        ImGui::PopStyleColor();
    }

    ImGui::PopStyleColor(3);

    ImGui::SetCursorScreenPos(ImVec2(cursor.x + pad, cursor.y + pad + rowH + 2.0f));

    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(18, 22, 30, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(195, 220, 175, 255));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(10, 12, 18, 200));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(60, 80, 120, 200));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(18, 22, 30, 255));

    if(wrapView) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
        bool childOpen = ImGui::BeginChild("##wrapview", ImVec2(W, H), false, ImGuiWindowFlags_None);
        if(childOpen) {
            if(ImGui::IsMouseClicked(0) && ImGui::IsWindowHovered()) wrapView = false;
            ImGui::TextWrapped("%s", editorBuf);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImVec2 editHintPos = ImVec2(cursor.x + pad + W - 100.0f,
                                    cursor.y + pad + rowH + 6.0f);
        dl->AddText(editHintPos, IM_COL32(80, 100, 80, 180), "click to edit");
    } else {
        bool changed = ImGui::InputTextMultiline(
            "##dyngen_poly_eel2",
            editorBuf, kBufSize,
            ImVec2(W, H),
            ImGuiInputTextFlags_AllowTabInput);

        if(changed) {
            codeDirty = true;
            codeDirtyTimer = 0.0f;
            eel2StatusMsg = "Editing...";
        }
    }

    ImGui::PopStyleColor(5);

    float statusY = cursor.y + pad + rowH + 2.0f + H + 2.0f;
    ImU32 statusCol = (eel2StatusMsg.find("Error") != std::string::npos)
                    ? IM_COL32(255, 80, 80, 255)
                    : IM_COL32(110, 200, 110, 255);
    dl->AddText(ImVec2(cursor.x + pad, statusY), statusCol, eel2StatusMsg.c_str());

    ImGui::SetNextWindowSize(ImVec2(520, 230), ImGuiCond_Appearing);
    if(ImGui::BeginPopup("##llm_popup", ImGuiWindowFlags_None)) {
        ImGui::TextColored(ImVec4(0.72f, 0.60f, 1.0f, 1.0f),
                           "Ask Claude to write mono EEL2 for DynGen Poly");
        ImGui::SameLine();
        if(llmApiKey.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "  [No API key]");
        } else {
            ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.55f, 1.0f), "  [Key loaded]");
        }

        ImGui::TextDisabled("One mono script voice will be instantiated across %d channels.",
                            numChannels.get());
        ImGui::Spacing();

        auto modeBtn = [&](const char* label, bool active) -> bool {
            ImGui::PushStyleColor(ImGuiCol_Button,
                active ? IM_COL32(65, 40, 95, 255) : IM_COL32(28, 28, 38, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(90, 58, 130, 255));
            bool clicked = ImGui::Button(label, ImVec2(122, 0));
            ImGui::PopStyleColor(2);
            return clicked;
        };
        if(modeBtn("  Generate  ##m", !llmFixMode)) llmFixMode = false;
        ImGui::SameLine(0, 2);
        if(modeBtn(" Fix current ##m", llmFixMode)) llmFixMode = true;

        ImGui::Separator();
        ImGui::Spacing();

        if(llmPending.load()) {
            static const char* kSpinChars = "|/-\\";
            static int sSpin = 0;
            sSpin = (sSpin + 1) % 4;
            ImGui::Text("Generating... %c", kSpinChars[sSpin]);
            ImGui::TextDisabled("Waiting for Claude's response.");
        } else if(llmDone.load()) {
            if(llmHasError) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.4f, 1));
                ImGui::TextWrapped("%s", llmStatusMsg.c_str());
                ImGui::PopStyleColor();
                ImGui::Spacing();
                if(ImGui::Button("Close##llmerr", ImVec2(80, 0))) {
                    llmDone.store(false);
                    llmHasError = false;
                    ImGui::CloseCurrentPopup();
                }
            } else {
                llmDone.store(false);
                ImGui::CloseCurrentPopup();
            }
        } else {
            ImGui::InputTextMultiline("##llm_input", llmPromptBuf,
                                      sizeof(llmPromptBuf), ImVec2(504, 100));
            if(llmFixMode) {
                ImGui::TextDisabled("Describe the issue, or leave empty for a general fix.");
            } else {
                ImGui::TextDisabled("Describe the mono effect, synth, or processor you want.");
            }
            ImGui::Spacing();

            bool canGenerate = !llmApiKey.empty() && (llmFixMode || llmPromptBuf[0] != '\0');
            if(!canGenerate) ImGui::BeginDisabled();
            const char* btnLabel = llmFixMode ? "Fix code##llm" : "Generate##llm";
            if(ImGui::Button(btnLabel, ImVec2(100, 0))) {
                requestLLMCode(std::string(llmPromptBuf));
            }
            if(!canGenerate) ImGui::EndDisabled();

            ImGui::SameLine(0, 10);
            if(ImGui::Button("Cancel##llm", ImVec2(70, 0))) {
                ImGui::CloseCurrentPopup();
            }

            if(llmApiKey.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1, 0.65f, 0.3f, 1), "No API key found. Add one of:");
                ImGui::TextDisabled("  data/anthropic_api_key.txt  (just paste the key as plain text)");
                ImGui::TextDisabled("  ~/.anthropic_api_key");
                ImGui::TextDisabled("  ANTHROPIC_API_KEY env var");
            }
        }

        ImGui::EndPopup();
    }

    ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + totalH));
    ImGui::Dummy(ImVec2(W + pad * 2, 2.0f));
}
