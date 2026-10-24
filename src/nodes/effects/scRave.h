//
// scRave.h — RAVE neural audio timbre transfer
//
// Requires the nn.ar SuperCollider plugin (https://github.com/elgiano/nn.ar),
// v0.0.8 or later recommended, and the rave_<in>_<out> SynthDefs from
// synthdefs/Defaults/Rave/rave.scd.
//
// Loading a model (per server):
//   /cmd /nn_load <slot> <path> <infoFile>   then   /sync <id>
// nn.ar answers /done only when the load succeeds and nothing when it fails,
// so the node waits for /synced instead and then reads the info file nn.ar
// wrote: no file means the load failed. The file also gives the shape of the
// model's "forward" method, which picks the SynthDef (rave_<in>_<out>).
//
// Slots: every load goes to a fresh slot id. A running NNUGen keeps raw
// pointers into its slot's model description, so a slot is never reloaded in
// place, and an old slot is unloaded only a few seconds after the synth that
// used it was replaced (see RaveSlotJanitor).
//
// Placement: the node owns a group that takes its place in the graph as soon
// as the graph is built. The NNUGen synth is added inside the group once the
// model is ready, so it still runs after its sources and before its
// destinations however long the load takes.
//
// Crash guard: if the server reboots while a load is in flight, the model may
// have crashed it. After two such reboots the path is refused for the rest of
// the session (choosing the file again retries it). The Path parameter itself
// is never changed by the node, so presets keep their model.
//

#ifndef scRave_h
#define scRave_h

#include "../../ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanodeNodeModel.h"
#include "../../scNode.h"
#include "ofxSCSynth.h"
#include "ofxSCGroup.h"
#include "../../serverManager.h"
#include "ofxOceanodeShared.h"
#include "imgui.h"

#include <filesystem>
#include <fstream>

// Unloads nn.ar model slots some time after their last synth was freed: the
// NNUGen compute thread stops asynchronously and still reads the slot's model
// description until it does.
class RaveSlotJanitor {
public:
    static RaveSlotJanitor& get() {
        static RaveSlotJanitor janitor;
        return janitor;
    }

    void scheduleUnload(ofxSCServer* server, int slot, uint64_t delayMs = 5000) {
        if(server == nullptr || slot < 0) return;
        entries.push_back({server, slot, ofGetElapsedTimeMillis() + delayMs});
    }

    // A rebooted server has no models left to unload
    void forgetServer(ofxSCServer* server) {
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [server](const Entry& e){ return e.server == server; }),
                      entries.end());
    }

    static int nextSlot() {
        // Clear of the low ids sclang's NN.load hands out by itself
        static int counter = 0;
        return 1000 + (counter++ % 60000);
    }

private:
    struct Entry { ofxSCServer* server; int slot; uint64_t due; };

    RaveSlotJanitor() {
        listener = ofEvents().update.newListener([this](ofEventArgs&){
            const uint64_t now = ofGetElapsedTimeMillis();
            for(auto it = entries.begin(); it != entries.end();) {
                if(it->due > now) { ++it; continue; }
                ofxOscMessage m;
                m.setAddress("/cmd");
                m.addStringArg("/nn_unload");
                m.addIntArg(it->slot);
                it->server->sendMsg(m);
                it = entries.erase(it);
            }
        });
    }

    std::vector<Entry> entries;
    ofEventListener listener;
};

class scRave : public scNode {
public:
    scRave(vector<serverManager*> outputServers) : scNode("RAVE") {
        serverList = outputServers;
    }

    ~scRave() {
        for(auto& kv : states) {
            ServerState& st = kv.second;
            releaseNodes(st, true);
            RaveSlotJanitor::get().scheduleUnload(kv.first, st.slot);
            RaveSlotJanitor::get().scheduleUnload(kv.first, st.loadingSlot);
        }
        states.clear();
    }

    void setup() override {
        description = "RAVE neural timbre transfer (nn.ar). Load a .ts model; the channel count follows the model.";

        // Audio I/O — inputs before parameters, outputs last
        addInput("In");

        addParameter(inputGain.set("Input Gain", 1.0f, 0.0f, 4.0f));
        addParameter(levels.set("Levels", 1.0f, 0.0f, 4.0f));
        // Models often generate far above full scale whatever the input level
        addParameter(limiter.set("Limiter", true));
        addParameter(path.set("Path", ""));
        addParameter(openFileDialog.set("Load Model"));
        addInspectorParameter(bufferSize.set("Buffer Size", -1, -1, 32768)); // -1 = auto

        addCustomRegion(
            ofParameter<std::function<void()>>().set("Status", [this](){ drawStatus(); }),
            ofParameter<std::function<void()>>().set("Status", [this](){ drawStatus(); })
        );

        addOutput("Out");

        requireSynthdefs();

        for(auto* sm : serverList) {
            if(!sm || !sm->getServer()) continue;
            ofxSCServer* server = sm->getServer();

            serverListeners.push(server->newFeedbackMessage.newListener([this, server](ofxOscMessage& msg) {
                if(msg.getAddress() != "/synced" || msg.getNumArgs() < 1) return;
                auto it = states.find(server);
                if(it == states.end() || it->second.syncId < 0) return;
                if(msg.getArgAsInt(0) == it->second.syncId) finishLoad(server, it->second);
            }));

            // A fresh server has no synths, groups or models
            serverListeners.push(server->serverBootedEvent.newListener([this, server]() {
                RaveSlotJanitor::get().forgetServer(server);
                auto it = states.find(server);
                if(it == states.end()) return;
                ServerState& st = it->second;
                if(st.loadingSlot >= 0) {
                    int& crashes = crashCounts()[st.loadingPath];
                    crashes++;
                    ofLogError("scRave") << "Server restarted while loading " << st.loadingPath
                                         << " (" << crashes << "x). The model may be incompatible with the"
                                         << " installed nn.ar plugin.";
                }
                releaseNodes(st, false);
                st.slot = -1;
                st.slotPath.clear();
                st.loadingSlot = -1;
                st.syncId = -1;
                updateStatus();
            }));
        }

        listeners.push(levels.newListener([this](float& l) {
            for(auto& kv : states)
                if(kv.second.synth) kv.second.synth->set("levels", l);
        }));

        listeners.push(inputGain.newListener([this](float& g) {
            for(auto& kv : states)
                if(kv.second.synth) kv.second.synth->set("inputgain", g);
        }));

        listeners.push(limiter.newListener([this](bool& b) {
            for(auto& kv : states)
                if(kv.second.synth) kv.second.synth->set("limit", b ? 1 : 0);
        }));

        listeners.push(bufferSize.newListener([this](int&) {
            // bufsize is read once by NNUGen: replace the synth
            for(auto& kv : states)
                if(kv.second.synth) startSynth(kv.first, kv.second);
        }));

        listeners.push(path.newListener([this](string&) {
            const string abs = resolvedPath();
            for(auto& kv : states) {
                ServerState& st = kv.second;
                if(!st.group) continue; // not in the graph: createSynth loads it
                if(abs.empty()) {
                    // No model: stop the synth, keep the group's place in the graph
                    stopSynth(st);
                    RaveSlotJanitor::get().scheduleUnload(kv.first, st.slot);
                    st.slot = -1;
                    st.slotPath.clear();
                    continue;
                }
                if(st.slot >= 0 && st.slotPath == abs) continue;
                startLoad(kv.first, st);
            }
            updateStatus();
        }));

        listeners.push(openFileDialog.newListener([this]() {
            auto r = ofSystemLoadDialog("Select RAVE model (.ts)", false, modelsDirectory());
            if(!r.bSuccess) return;
            // Choosing a file is an explicit retry, even after a crash
            crashCounts().erase(absolutePath(r.getPath()));
            path = storedPath(r.getPath());
        }));

        updateStatus();
    }

    void activate() override {
        for(auto& kv : states)
            if(kv.second.group && kv.second.group->isCreated()) kv.second.group->run(true);
    }
    void deactivate() override {
        for(auto& kv : states)
            if(kv.second.group && kv.second.group->isCreated()) kv.second.group->run(false);
    }

    // ── scNode audio-graph interface ──────────────────────────────────────────

    void buildSynth(ofxSCServer* server) override {
        ServerState& st = states[server];
        releaseNodes(st, true);
        st.group = new ofxSCGroup(server);
    }

    void createSynth(ofxSCServer* server) override {
        auto it = states.find(server);
        if(it == states.end() || !it->second.group) return;
        ServerState& st = it->second;

        st.group->create(0, 1, false);
        if(!getActive()) st.group->run(false);

        const string abs = resolvedPath();
        if(abs.empty()) { updateStatus(); return; }
        // The slot outlives free(): a graph rebuild or a preset load does not
        // reload the model
        if(st.slot >= 0 && st.slotPath == abs) startSynth(server, st);
        else if(st.loadingSlot < 0 || st.loadingPath != abs) startLoad(server, st);
        updateStatus();
    }

    void moveSynthBefore(ofxSCServer* server, int nodeID) override {
        auto it = states.find(server);
        if(it == states.end() || !it->second.group) return;
        // Only this node's bus wiring can have changed (see scBusResend.h)
        sendBuses(it->second, it->second.synth);
        it->second.group->moveBefore(nodeID);
    }

    void free(ofxSCServer* server) override {
        auto it = states.find(server);
        if(it == states.end()) return;
        // Keeps the loaded slot for the next createSynth
        releaseNodes(it->second, true);
    }

    void setOutputBus(ofxSCServer* server, int index, int bus) override {
        ServerState& st = states[server];
        st.outputBuses[index] = bus;
        if(!st.synth) return; // sent with /s_new
        for(int i = 0; i < (int)outputs.size(); i++)
            if(outputs[i]->getIndex() == index)
                st.synth->set(ofToLower(outputs[i].getName()), bus);
    }

    void setInputBus(ofxSCServer* server, scNode* node, int bus) override {
        ServerState& st = states[server];
        st.inputBuses[node] = bus;
        if(!st.synth) return; // sent with /s_new
        for(int i = 0; i < (int)inputs.size(); i++)
            if(inputs[i]->getNodeRef() == node)
                st.synth->set(ofToLower(inputs[i].getName()), bus);
    }

    void resetInputBusses(ofxSCServer* server, int targetBus = 0) override {
        auto it = states.find(server);
        if(it == states.end()) return;
        ServerState& st = it->second;
        st.inputBuses.clear();
        st.unconnectedInputBus = targetBus;
        if(!st.synth) return;
        for(int i = 0; i < (int)inputs.size(); i++)
            st.synth->set(ofToLower(inputs[i].getName()), targetBus);
    }

    int getOutputBusIndex(ofxSCServer* server, int index) override {
        return states[server].outputBuses[index];
    }

    int getLastSynthID(ofxSCServer* server) override {
        auto it = states.find(server);
        return (it != states.end() && it->second.group) ? it->second.group->nodeID : -1;
    }

    void resendParametersForNRT() override {
        for(auto& kv : states) {
            if(!kv.second.synth) continue;
            kv.second.synth->set("levels", levels.get());
            kv.second.synth->set("inputgain", inputGain.get());
            kv.second.synth->set("limit", limiter.get() ? 1 : 0);
            sendBuses(kv.second, kv.second.synth);
        }
    }

private:
    struct ModelInfo {
        string methodName;
        int methodIdx = 0;
        int inDim = 0;
        int outDim = 0;
        int sampleRate = 0;
        int minBufferSize = 0;
    };

    struct ServerState {
        ofxSCGroup* group = nullptr;   // the node's place in the graph
        ofxSCSynth* synth = nullptr;   // NNUGen synth inside the group

        int slot = -1;                 // loaded, confirmed slot
        string slotPath;
        ModelInfo info;

        int loadingSlot = -1;          // load in flight
        string loadingPath;
        string infoFile;
        int syncId = -1;

        std::map<int, int> outputBuses;
        std::map<scNode*, int> inputBuses;
        int unconnectedInputBus = 0;
    };

    // ── model loading ─────────────────────────────────────────────────────────

    void startLoad(ofxSCServer* server, ServerState& st) {
        const string abs = resolvedPath();
        if(abs.empty()) return;

        // A load superseded by this one still completes: unload it after
        if(st.loadingSlot >= 0) RaveSlotJanitor::get().scheduleUnload(server, st.loadingSlot, 0);
        st.loadingSlot = -1;
        st.syncId = -1;

        if(!ofFile::doesFileExist(abs, false)) {
            ofLogError("scRave") << "Model file not found: " << abs;
            updateStatus();
            return;
        }
        if(crashCounts()[abs] >= maxCrashes) {
            ofLogError("scRave") << "Not loading " << abs << ": it restarted the server while loading."
                                 << " Choose the file again to retry.";
            updateStatus();
            return;
        }

        st.loadingSlot = RaveSlotJanitor::nextSlot();
        st.loadingPath = abs;
        st.infoFile = (std::filesystem::temp_directory_path() /
                       ("oceanode-rave-" + ofToString(st.loadingSlot) + "-" + ofToString(ofGetSystemTimeMillis()) + ".yaml")).string();
        std::error_code ec;
        std::filesystem::remove(st.infoFile, ec);

        ofxOscMessage m;
        m.setAddress("/cmd");
        m.addStringArg("/nn_load");
        m.addIntArg(st.loadingSlot);
        m.addStringArg(abs);
        m.addStringArg(st.infoFile);
        server->sendMsg(m);
        st.syncId = server->requestSync();

        ofLogNotice("scRave") << "Loading model " << abs << " into slot " << st.loadingSlot;
        updateStatus();
    }

    void finishLoad(ofxSCServer* server, ServerState& st) {
        const int newSlot = st.loadingSlot;
        const string newPath = st.loadingPath;
        st.syncId = -1;
        st.loadingSlot = -1;

        ModelInfo info;
        const bool parsed = parseModelInfo(st.infoFile, info);
        std::error_code ec;
        std::filesystem::remove(st.infoFile, ec);

        if(!parsed) {
            // nn.ar posted the reason to the server's log
            ofLogError("scRave") << "nn.ar could not load " << newPath;
            loadError = "nn.ar could not load the model (see the SuperCollider log)";
            RaveSlotJanitor::get().scheduleUnload(server, newSlot, 0);
            updateStatus();
            return;
        }
        if(info.inDim < 1 || info.inDim > maxInputs || info.outDim < 1 || info.outDim > maxOutputs) {
            loadError = "Unsupported model: method '" + info.methodName + "' has "
                + ofToString(info.inDim) + " inputs and " + ofToString(info.outDim)
                + " outputs (max " + ofToString(maxInputs) + " in, " + ofToString(maxOutputs) + " out)";
            ofLogError("scRave") << loadError;
            RaveSlotJanitor::get().scheduleUnload(server, newSlot, 0);
            updateStatus();
            return;
        }

        loadError.clear();
        crashCounts().erase(newPath);

        const int oldSlot = st.slot;
        st.slot = newSlot;
        st.slotPath = newPath;
        st.info = info;
        ofLogNotice("scRave") << "Model ready: " << newPath << " (" << info.methodName << ": "
                              << info.inDim << " in, " << info.outDim << " out)";

        if(st.group) startSynth(server, st);
        // After the synth using it was replaced above
        RaveSlotJanitor::get().scheduleUnload(server, oldSlot);
        updateStatus();
    }

    // Reads the YAML nn.ar writes for one model:
    //   - idx: 1000
    //     modelPath: ...
    //     sampleRate: 44100        (nn.ar >= 0.0.8)
    //     methods:
    //       - name: forward
    //         inDim: 1
    //         ...
    //     attributes:
    static bool parseModelInfo(const string& file, ModelInfo& info) {
        std::ifstream in(file);
        if(!in) return false;

        struct Method { string name; int inDim = 0; int outDim = 0; };
        std::vector<Method> methods;
        bool inMethods = false;
        string line;
        while(std::getline(in, line)) {
            string t = ofTrim(line);
            if(t.rfind("- ", 0) == 0) t = ofTrim(t.substr(2));
            const size_t colon = t.find(':');
            if(colon == string::npos) continue;
            const string key = ofTrim(t.substr(0, colon));
            const string value = ofTrim(t.substr(colon + 1));

            if(key == "sampleRate") info.sampleRate = ofToInt(value);
            else if(key == "minBufferSize") info.minBufferSize = ofToInt(value);
            else if(key == "methods") inMethods = true;
            else if(key == "attributes") inMethods = false;
            else if(inMethods && key == "name") methods.push_back({value});
            else if(inMethods && !methods.empty() && key == "inDim") methods.back().inDim = ofToInt(value);
            else if(inMethods && !methods.empty() && key == "outDim") methods.back().outDim = ofToInt(value);
        }
        if(methods.empty()) return false;

        int idx = 0;
        for(int i = 0; i < (int)methods.size(); i++)
            if(methods[i].name == "forward") { idx = i; break; }
        info.methodIdx = idx;
        info.methodName = methods[idx].name;
        info.inDim = methods[idx].inDim;
        info.outDim = methods[idx].outDim;
        return true;
    }

    // ── synth lifetime ────────────────────────────────────────────────────────

    // Creates the synth for the loaded slot, replacing a running one in place
    void startSynth(ofxSCServer* server, ServerState& st) {
        if(st.slot < 0 || !st.group) return;
        auto* synth = new ofxSCSynth(synthDefName(st.info), server);
        synth->set("model", st.slot);
        synth->set("method", st.info.methodIdx);
        synth->set("bufsize", effectiveBufferSize(st.info));
        synth->set("levels", levels.get());
        synth->set("inputgain", inputGain.get());
        synth->set("limit", limiter.get() ? 1 : 0);
        sendBuses(st, synth);

        if(st.synth) {
            synth->createAndRun(4, st.synth->nodeID, true); // replace
            delete st.synth;
        } else {
            synth->createAndRun(1, st.group->nodeID, true);  // tail of the group
        }
        st.synth = synth;
    }

    // Auto (<= 0; 0 would run the model on the audio thread): the model's own
    // buffer is often too short for the compute thread to keep up (a model
    // with 128 samples played silence), so never go below 2048. NNUGen rounds
    // explicit sizes up to a power of two and to the model's minimum.
    int effectiveBufferSize(const ModelInfo& info) const {
        if(bufferSize.get() > 0) return bufferSize.get();
        return std::max(2048, info.minBufferSize);
    }

    void stopSynth(ServerState& st) {
        if(!st.synth) return;
        st.synth->free();
        delete st.synth;
        st.synth = nullptr;
    }

    // sendFree: false after a server reboot, when the nodes no longer exist
    void releaseNodes(ServerState& st, bool sendFree) {
        if(st.synth) {
            if(sendFree) st.synth->free();
            delete st.synth;
            st.synth = nullptr;
        }
        if(st.group) {
            if(sendFree && st.group->isCreated()) st.group->free();
            delete st.group;
            st.group = nullptr;
        }
    }

    void sendBuses(ServerState& st, ofxSCSynth* synth) {
        if(!synth) return;
        for(int i = 0; i < (int)inputs.size(); i++) {
            auto nodeRef = inputs[i]->getNodeRef();
            auto bus = st.inputBuses.find(nodeRef);
            synth->set(ofToLower(inputs[i].getName()),
                       bus != st.inputBuses.end() ? bus->second : st.unconnectedInputBus);
        }
        for(int i = 0; i < (int)outputs.size(); i++) {
            auto bus = st.outputBuses.find(outputs[i]->getIndex());
            if(bus != st.outputBuses.end())
                synth->set(ofToLower(outputs[i].getName()), bus->second);
        }
    }

    // ── SynthDefs ─────────────────────────────────────────────────────────────

    static string synthDefName(const ModelInfo& info) {
        return "rave_" + ofToString(info.inDim) + "_" + ofToString(info.outDim);
    }

    void requireSynthdefs() {
        std::filesystem::path folder;
        const std::string expected = "rave_1_1.scsyndef";
        try {
            const std::filesystem::path dataRoot = ofToDataPath(SYNTHDEF_DIRECTORY, true);
            if(std::filesystem::exists(dataRoot)) {
                for(const auto& entry : std::filesystem::recursive_directory_iterator(
                        dataRoot, std::filesystem::directory_options::skip_permission_denied)) {
                    if(entry.is_regular_file() && entry.path().filename() == expected) {
                        folder = entry.path().parent_path();
                        break;
                    }
                }
            }
            // Development fallback: binaries compiled beside the source
            if(folder.empty()) {
                const std::filesystem::path bundled = std::filesystem::path(__FILE__).parent_path()
                    .parent_path().parent_path().parent_path() / "synthdefs/Defaults/Rave/CompiledSynthdefs/Rave";
                if(std::filesystem::exists(bundled / expected)) folder = bundled;
            }
        } catch(const std::exception& error) {
            ofLogWarning("scRave") << "SynthDef search failed: " << error.what();
        }

        if(folder.empty()) {
            ofLogError("scRave") << "RAVE SynthDefs (rave_<in>_<out>) were not found: compile synthdefs/Defaults/Rave/rave.scd";
            return;
        }
        for(auto* manager : serverList) if(manager) manager->requireSynthdefFolder(folder.string());
    }

    // ── paths ─────────────────────────────────────────────────────────────────

    static string modelsDirectory() {
        return ofToDataPath("Supercollider/RAVE_MODELS", true);
    }

    static string absolutePath(const string& p) {
        if(p.empty() || p[0] == '/') return p;
        return ofFilePath::join(modelsDirectory(), p);
    }

    string resolvedPath() const { return absolutePath(path.get()); }

    // Files inside RAVE_MODELS are stored relative to it, so presets survive
    // moving the app's data folder
    static string storedPath(const string& p) {
        const string dir = modelsDirectory() + "/";
        if(p.rfind(dir, 0) == 0) return p.substr(dir.size());
        return p;
    }

    static std::map<string, int>& crashCounts() {
        static std::map<string, int> counts;
        return counts;
    }

    // ── status ────────────────────────────────────────────────────────────────

    void updateStatus() {
        const string abs = resolvedPath();
        statusIsError = false;
        if(abs.empty()) { status = "No model"; return; }

        const string name = ofFilePath::getFileName(abs);
        if(!ofFile::doesFileExist(abs, false)) {
            status = "File not found: " + name; statusIsError = true; return;
        }
        if(crashCounts().count(abs) && crashCounts()[abs] >= maxCrashes) {
            status = name + " restarted the server while loading. Load it again to retry.";
            statusIsError = true; return;
        }

        for(auto& kv : states) {
            const ServerState& st = kv.second;
            if(st.loadingSlot >= 0 && st.loadingPath == abs) { status = "Loading " + name + "..."; return; }
        }
        for(auto& kv : states) {
            const ServerState& st = kv.second;
            if(st.slot < 0 || st.slotPath != abs) continue;
            status = name + "  (" + ofToString(st.info.inDim) + " in, " + ofToString(st.info.outDim) + " out)";
            const int serverRate = serverManager::getSampleRateForServer(kv.first);
            if(st.info.sampleRate > 0 && serverRate > 0 && st.info.sampleRate != serverRate) {
                status += "\nTrained at " + ofToString(st.info.sampleRate) + " Hz, server runs at "
                        + ofToString(serverRate) + " Hz: pitch and timbre will shift";
                statusIsError = true;
            }
            return;
        }
        if(!loadError.empty()) { status = loadError; statusIsError = true; return; }
        status = name + " (loads when connected to an output)";
    }

    void drawStatus() {
        const float width = 220.0f * ofxOceanodeShared::getZoomLevel();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        if(statusIsError) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", status.c_str());
        else ImGui::TextDisabled("%s", status.c_str());
        ImGui::PopTextWrapPos();
    }

    static constexpr int maxInputs = 4;   // rave.scd builds rave_1_1 .. rave_4_8
    static constexpr int maxOutputs = 8;
    static constexpr int maxCrashes = 2;

    // Parameters
    ofParameter<float>  inputGain;
    ofParameter<float>  levels;
    ofParameter<bool>   limiter;
    ofParameter<string> path;
    ofParameter<void>   openFileDialog;
    ofParameter<int>    bufferSize;

    std::map<ofxSCServer*, ServerState> states;
    string status;
    string loadError;
    bool statusIsError = false;

    ofEventListeners serverListeners;
    vector<serverManager*> serverList;
};

#endif /* scRave_h */
