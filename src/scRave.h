//
// scRave.h — RAVE neural audio timbre transfer
//
// Requires the nn.ar SuperCollider plugin (https://github.com/elgiano/nn.ar).
// The SynthDefs rave1..rave8 must be compiled from rave.scd and placed in
//   <appdata>/Supercollider/Synthdefs/rave/
//
// Model loading:  OSC /cmd "/nn_load" <slot=0> <path>  (slot is always 0)
// SynthDef name:  "rave" + numChannels  (e.g. "rave2" for stereo)
// Set N Chan to match the model's output channel count (typically 2 for stereo).
//
// Crash-loop prevention:
//   NNUGen hard-crashes the SC server if model slot 0 is empty.
//   Synth creation is therefore deferred until /done /cmd (model load complete).
//   If the server crashes DURING model loading (e.g. incompatible model file),
//   serverBootedEvent fires while pendingCreate is set → path is cleared and the
//   loop is broken.  The user must set a valid model path again to retry.
//
// Note: all scRave instances share model slot 0.
//

#ifndef scRave_h
#define scRave_h

#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanodeNodeModel.h"
#include "scNode.h"
#include "ofxSCSynth.h"
#include "serverManager.h"

class scRave : public scNode {
public:
    scRave(vector<serverManager*> outputServers) : scNode("RAVE") {
        serverList = outputServers;
    }

    ~scRave() {
        feedbackListeners.clear();
        freeAll();
    }

    void setup() override {
        description = "RAVE neural timbre transfer (nn.ar). Load a .ts model file first, then set N Chan to match model output channels (usually 2).";

        // Audio I/O — must be registered in order: inputs before numChannels, outputs last
        addInput("In");

        addParameter(numChannels.set("N Chan", 2, 1, 8));
        addParameter(levels.set("Levels", 1.0f, 0.0f, 4.0f));
        addParameter(path.set("Path", ""));
        addParameter(openFileDialog.set("Load Model"));
        addInspectorParameter(modelLoaded.set("Model Loaded", false));

        addOutput("Out");

        // Crash-loop guard: if the server reboots while a model load is in-flight
        // (pendingCreate is set), the model crashed the server.  Clear path to break loop.
        for(auto& sm : serverList) {
            if(!sm || !sm->getServer()) continue;
            ofxSCServer* server = sm->getServer();
            bootListeners.push(server->serverBootedEvent.newListener([this, server]() {
                if(pendingCreate.count(server)) {
                    ofLogError("scRave")
                        << "Server crashed during RAVE model loading.  "
                        << "The model may be incompatible with the installed nn.ar plugin.  "
                        << "Clearing path to stop crash loop.  "
                        << "Try a different model or check nn.ar/LibTorch compatibility.";
                    pendingCreate.erase(server);
                    feedbackListeners.erase(server);
                    // Clear path so createSynth won't retry on next recomputeGraph
                    path.set("");
                    modelLoaded = false;
                }
            }));
        }

        // numChannels: replace synths with new SynthDef variant (only if already running)
        listeners.push(numChannels.newListener([this](int& i) {
            if(i < 1 || i > 8) return;
            if(synths.empty()) return;
            for(auto& kv : synths) {
                if(!kv.second || pendingCreate.count(kv.first)) continue;
                ofxSCSynth* newSynth = new ofxSCSynth(getSynthdefName(), kv.first);
                newSynth->createAndRun(4, kv.second->nodeID, getActive()); // replace in-place
                delete kv.second;
                kv.second = newSynth;
            }
            resendParams.notify();
        }));

        listeners.push(levels.newListener([this](float& l) {
            for(auto& kv : synths)
                if(kv.second && !pendingCreate.count(kv.first)) kv.second->set("levels", l);
        }));

        listeners.push(path.newListener([this](string& p) {
            if(p.empty()) return;
            string abs = absolutePath(p);
            for(auto& sm : serverList) {
                if(!sm || !sm->getServer()) continue;
                ofxSCServer* server = sm->getServer();
                sendLoadModel(server, abs);
                // If a synth is built but waiting for a model, register /done listener now
                if(pendingCreate.count(server) && synths.count(server))
                    registerDoneListener(server);
            }
            modelLoaded = true;
        }));

        listeners.push(openFileDialog.newListener([this]() {
            auto r = ofSystemLoadDialog("Select RAVE model (.ts)", false,
                                        ofToDataPath("Supercollider/RAVE_MODELS", true));
            if(r.bSuccess) path = r.getPath();
        }));

        // resendParams: push all current values (incl. buses) to each synth.
        // Called before createAndRun so params arrive as /s_new init-args.
        listeners.push(resendParams.newListener([this]() {
            for(auto& kv : synths) {
                if(!kv.second) continue;
                kv.second->set("levels", levels.get());

                for(int i = 0; i < (int)inputs.size(); i++) {
                    auto nodeRef = inputs[i]->getNodeRef();
                    if(inputBuses[kv.first].count(nodeRef)) {
                        kv.second->set(ofToLower(inputs[i].getName()),
                                       inputBuses[kv.first][nodeRef]);
                    }
                }
                for(int i = 0; i < (int)outputs.size(); i++) {
                    int idx = outputs[i]->getIndex();
                    if(outputBuses[kv.first].count(idx)) {
                        kv.second->set(ofToLower(outputs[i].getName()),
                                       outputBuses[kv.first][idx]);
                    }
                }
            }
        }));
    }

    void activate() override {
        for(auto& kv : synths)
            if(kv.second && !pendingCreate.count(kv.first)) kv.second->run(true);
    }
    void deactivate() override {
        for(auto& kv : synths)
            if(kv.second && !pendingCreate.count(kv.first)) kv.second->run(false);
    }

    // ── scNode audio-graph interface ──────────────────────────────────────────

    void buildSynth(ofxSCServer* server) override {
        synths[server] = new ofxSCSynth(getSynthdefName(), server);
    }

    void createSynth(ofxSCServer* server) override {
        if(!synths.count(server)) return;
        string p = path.get();
        if(p.empty()) {
            // No model yet — defer until path is set (path listener handles creation)
            pendingCreate.insert(server);
            return;
        }
        // Reload model (needed after server restart), defer synth until /done /cmd.
        // If this load crashes the server, the serverBootedEvent listener above will
        // detect pendingCreate and clear path to break the loop.
        sendLoadModel(server, absolutePath(p));
        pendingCreate.insert(server);
        registerDoneListener(server);
    }

    void moveSynthBefore(ofxSCServer* server, int nodeID) override {
        if(!synths.count(server) || pendingCreate.count(server)) return;
        resendParams.notify();
        synths[server]->moveBefore(nodeID);
    }

    void free(ofxSCServer* server) override {
        feedbackListeners.erase(server);
        pendingCreate.erase(server);
        if(synths.count(server)) {
            synths[server]->free();
            delete synths[server];
            synths.erase(server);
        }
    }

    void setOutputBus(ofxSCServer* server, int index, int bus) override {
        outputBuses[server][index] = bus;
        if(pendingCreate.count(server)) return; // stored; sent as init-args via resendParams
        for(int i = 0; i < (int)outputs.size(); i++) {
            if(outputs[i]->getIndex() == index && synths.count(server) && synths[server])
                synths[server]->set(ofToLower(outputs[i].getName()), bus);
        }
    }

    void setInputBus(ofxSCServer* server, scNode* node, int bus) override {
        inputBuses[server][node] = bus;
        if(pendingCreate.count(server)) return; // stored; sent as init-args via resendParams
        for(int i = 0; i < (int)inputs.size(); i++) {
            if(inputs[i]->getNodeRef() == node && synths.count(server) && synths[server])
                synths[server]->set(ofToLower(inputs[i].getName()), bus);
        }
    }

    void resetInputBusses(ofxSCServer* server, int targetBus = 0) override {
        if(!synths.count(server)) return;
        inputBuses[server].clear();
        if(pendingCreate.count(server)) return; // applied via resendParams on createAndRun
        for(int i = 0; i < (int)inputs.size(); i++)
            synths[server]->set(ofToLower(inputs[i].getName()), targetBus);
    }

    int getOutputBusIndex(ofxSCServer* server, int index) override {
        return outputBuses[server][index];
    }

    int getLastSynthID(ofxSCServer* server) override {
        return synths.count(server) ? synths[server]->nodeID : -1;
    }

    // Public so external code / preset lifecycle can re-push params
    ofEvent<void> resendParams;

private:
    string getSynthdefName() {
        return "rave" + ofToString(numChannels.get());
    }

    string absolutePath(const string& p) {
        return (p.empty() || p[0] == '/') ? p
             : ofToDataPath("Supercollider/RAVE_MODELS/" + p, true);
    }

    // OSC: /cmd "/nn_load" 0 <path>
    // Slot 0 matches modelIdx=0 compiled into the rave1..rave8 SynthDefs.
    void sendLoadModel(ofxSCServer* server, const string& modelPath) {
        ofxOscMessage m;
        m.setAddress("/cmd");
        m.addStringArg("/nn_load");
        m.addIntArg(0);          // integer slot — must match modelIdx in SynthDef
        m.addStringArg(modelPath);
        server->sendMsg(m);
        ofLog() << "scRave: loading model -> " << modelPath;
    }

    // Register a one-shot listener on server->newFeedbackMessage.
    // Waits for /done /cmd (SC response when /nn_load completes), then calls createAndRun.
    // Filtering on "/cmd" avoids false-triggering on /done /b_alloc, /done /b_read, etc.
    void registerDoneListener(ofxSCServer* server) {
        feedbackListeners.erase(server); // remove any previous listener
        feedbackListeners[server] = server->newFeedbackMessage.newListener(
            [this, server](ofxOscMessage& msg) {
                if(msg.getAddress() != "/done") return;
                // SC sends /done with first arg = original command name
                if(msg.getNumArgs() < 1 ||
                   msg.getArgType(0) != OFXOSC_TYPE_STRING ||
                   msg.getArgAsString(0) != "/cmd") return;

                if(!pendingCreate.count(server)) return;
                ofLog() << "scRave: model ready, creating synth";
                pendingCreate.erase(server);
                if(synths.count(server)) {
                    resendParams.notify();
                    synths[server]->createAndRun(0, 1, getActive());
                }
            });
    }

    void freeAll() {
        for(auto& kv : synths) kv.second->free();
        synths.clear();
    }

    // Parameters
    ofParameter<int>    numChannels;
    ofParameter<float>  levels;
    ofParameter<string> path;
    ofParameter<void>   openFileDialog;
    ofParameter<bool>   modelLoaded;

    // Per-server state
    std::map<ofxSCServer*, ofxSCSynth*>              synths;
    std::map<ofxSCServer*, std::map<int, int>>        outputBuses;
    std::map<ofxSCServer*, std::map<scNode*, int>>    inputBuses;

    // Deferred-creation and crash-loop-prevention state
    std::set<ofxSCServer*>                            pendingCreate;
    std::map<ofxSCServer*, ofEventListener>           feedbackListeners;
    ofEventListeners                                   bootListeners;

    vector<serverManager*> serverList;
};

#endif /* scRave_h */
