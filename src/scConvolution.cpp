//
//  scConvolution.cpp
//  ofxOceanodeSuperCollider
//

#include "scConvolution.h"
#include "ofxOceanodeShared.h"
#include "ofxSCSynth.h"
#include "ofxSCBuffer.h"
#include "ofxSuperCollider.h"
#include "ofxOscMessage.h"
#include "imgui.h"

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

scConvolution::scConvolution() : scNode("Convolution") {}

scConvolution::~scConvolution() {
    try {
        listeners.unsubscribeAll();
        for(auto& p : synthInstances)
            if(p.second) { p.second->free(); delete p.second; }
        synthInstances.clear();
        inputBuses.clear();
        outputBuses.clear();
        for(auto& p : serverStates) {
            ServerState& s = p.second;
            if(s.genSynth)          { s.genSynth->free();          delete s.genSynth;          }
            if(s.irBuffer)          { s.irBuffer->free();          delete s.irBuffer;          }
            if(s.activeSpecBuffer)  { s.activeSpecBuffer->free();  delete s.activeSpecBuffer;  }
            if(s.pendingSpecBuffer) { s.pendingSpecBuffer->free(); delete s.pendingSpecBuffer; }
        }
        serverStates.clear();
    } catch(...) {}
}

// ─────────────────────────────────────────────────────────────────────────────
// setup()
// ─────────────────────────────────────────────────────────────────────────────

void scConvolution::setup() {
    addParameter(numChannels.set("N Chan",     2,    1,    16));
    addParameter(wet.set(       "Wet",         0.0f, 0.0f, 1.0f));
    addParameter(level.set(     "Level",       1.0f, 0.0f, 2.0f));
    addParameter(rt60.set(      "RT60",        1.5f, 0.05f, 8.0f));
    addParameter(brightness.set("Brightness",  0.15f, 0.0f, 0.99f));
    addParameter(absorpCurve.set("Absorption", 0.7f, 0.1f, 1.0f));

    // Saved in presets but hidden from the main panel (path can be very long)
    addInspectorParameter(irFilePath.set("IR File", ""));

    oldNumChannels = numChannels;

    scNode::addInput("In");
    scNode::addOutput("Out");

    listeners.push(numChannels.newListener([this](int& ch) {
        if(ch < 1 || ch > 16 || ch == oldNumChannels) return;
        for(auto& pair : synthInstances) {
            if(!pair.second) continue;
            int         oldID    = pair.second->nodeID;
            ofxSCSynth* old      = pair.second;
            ofxSCSynth* ns       = new ofxSCSynth(getSynthDefName(), pair.first);
            pair.second = ns;
            resendParams.notify();
            ns->createAndRun(4, oldID, getActive());
            delete old;
        }
        oldNumChannels = ch;
    }));

    listeners.push(wet.newListener(  [this](float&) { resendParams.notify(); }));
    listeners.push(level.newListener([this](float&) { resendParams.notify(); }));

    // Synthetic IR params: any change regenerates the IR and clears file mode
    auto onSynthParam = [this](float&) {
        irFilePath.set("");
        startSyntheticIR();
    };
    listeners.push(rt60.newListener(        onSynthParam));
    listeners.push(brightness.newListener(  onSynthParam));
    listeners.push(absorpCurve.newListener( onSynthParam));

    // File path is serialised; triggers file load on preset recall
    listeners.push(irFilePath.newListener([this](std::string& path) {
        if(!path.empty()) loadImpulseResponse(path);
    }));

    listeners.push(resendParams.newListener([this]() {
        for(auto& p : synthInstances)
            if(p.second) sendParamsToSynth(p.first);
    }));

    addCustomRegion(
        ofParameter<std::function<void()>>().set("IR", [this](){ drawStatusWidget(); }),
        ofParameter<std::function<void()>>().set("IR", [this](){ drawStatusWidget(); })
    );
}

// ─────────────────────────────────────────────────────────────────────────────
// update()
// ─────────────────────────────────────────────────────────────────────────────

void scConvolution::update(ofEventArgs& args) {
    const float dt = ofGetLastFrameTime();

    for(auto& kv : serverStates) {
        ofxSCServer* srv = kv.first;
        ServerState& s   = kv.second;

        // ── kLoadingIR ────────────────────────────────────────────────────────
        if(s.irState == kLoadingIR) {
            s.timer += dt;

            if(s.fileMode) {
                // Send b_query at 600 ms so /b_info arrives before we transition
                if(s.timer >= 0.6f && s.timer - dt < 0.6f)
                    s.irBuffer->query();

                // At 800 ms: b_allocReadChannel + b_query response should both be done
                if(s.timer >= 0.8f) {
                    s.irState      = kPreparingSpec;
                    s.timer        = 0.0f;
                    s.prepConvSent = false;
                    allocExactSpecBuffer(srv, s.irBuffer->frames);
                }
            } else {
                // Synthetic: wait 300 ms for b_alloc(irBuffer) to settle,
                // then launch the IR generator synth.
                if(s.timer >= 0.3f) {
                    s.irState      = kPreparingSpec;
                    s.timer        = 0.0f;
                    s.prepConvSent = false;

                    // Spec buffer can be sized exactly because we allocated irBuffer
                    // ourselves and know irBuffer->frames.
                    allocExactSpecBuffer(srv, s.irBuffer->frames);

                    // Launch IR generator — kept in s.genSynth so we can free it
                    // before freeing irBuffer if the user restarts generation.
                    s.genSynth = new ofxSCSynth("scconvIRGen", srv);
                    s.genSynth->set("irBuf",       s.irBuffer->index);
                    s.genSynth->set("rt60",        rt60.get());
                    s.genSynth->set("brightness",  brightness.get());
                    s.genSynth->set("absorpCurve", absorpCurve.get());
                    s.genSynth->createAndRun(0, 1, true);
                }
            }

        // ── kPreparingSpec ────────────────────────────────────────────────────
        } else if(s.irState == kPreparingSpec) {
            s.timer += dt;

            // Send PreparePartConv once the IR data has been written:
            //   File mode  → immediately (buffer already loaded)
            //   Synth mode → after rt60 seconds (gen synth is writing for that long)
            float prepDelay = s.fileMode ? 0.0f : rt60.get();
            if(!s.prepConvSent && s.timer >= prepDelay) {
                s.prepConvSent = true;
                // Free the gen synth reference; it will have freed itself via
                // doneAction, but we release the pointer regardless.
                if(s.genSynth) { delete s.genSynth; s.genSynth = nullptr; }
                preparePartConv(srv);
            }

            // Allow 1.5 s after PreparePartConv before swapping the synth
            if(s.prepConvSent && s.timer >= prepDelay + 1.5f)
                swapAndReplaceWithNewSpec(srv);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// activate / deactivate
// ─────────────────────────────────────────────────────────────────────────────

void scConvolution::activate()   { for(auto& p : synthInstances) if(p.second) p.second->run(true);  }
void scConvolution::deactivate() { for(auto& p : synthInstances) if(p.second) p.second->run(false); }

// ─────────────────────────────────────────────────────────────────────────────
// scNode interface
// ─────────────────────────────────────────────────────────────────────────────

void scConvolution::buildSynth(ofxSCServer* server) {
    if(!server) return;
    synthInstances[server] = new ofxSCSynth(getSynthDefName(), server);

    ServerState& s = serverStates[server];

    // Silent spec buffer: PartConv reads specbuf at init; this zeroed single-
    // partition buffer produces silence until a real IR is loaded.
    if(!s.activeSpecBuffer) {
        s.activeSpecBuffer = new ofxSCBuffer(kFftSize, 1, server);
        s.activeSpecBuffer->alloc();
    }

    // Auto-start synthetic IR on first build if no file IR is queued
    if(s.irState == kIdle && irFilePath.get().empty())
        startSyntheticIRForServer(server);
}

void scConvolution::createSynth(ofxSCServer* server) {
    if(!server || !synthInstances.count(server)) return;
    resendParams.notify();
    synthInstances[server]->createAndRun(0, 1, getActive());
}

void scConvolution::free(ofxSCServer* server) {
    if(!server) return;
    if(synthInstances.count(server) && synthInstances[server]) {
        synthInstances[server]->free();
        delete synthInstances[server];
        synthInstances.erase(server);
    }
    inputBuses.erase(server);
    outputBuses.erase(server);

    // Keep serverStates alive across graph recomputes.
    // Cancel any in-progress load to avoid orphaned pending buffers.
    if(serverStates.count(server)) {
        ServerState& s = serverStates[server];
        if(s.pendingSpecBuffer) {
            s.pendingSpecBuffer->free(); delete s.pendingSpecBuffer; s.pendingSpecBuffer = nullptr;
        }
        // Free genSynth BEFORE irBuffer (genSynth writes into irBuffer)
        if(s.genSynth) { s.genSynth->free(); delete s.genSynth; s.genSynth = nullptr; }
        if(s.irBuffer)  { s.irBuffer->free();  delete s.irBuffer;  s.irBuffer  = nullptr; }
        s.irState      = s.activeSpecBuffer ? kReady : kIdle;
        s.prepConvSent = false;
        s.timer        = 0.0f;
    }
}

void scConvolution::moveSynthBefore(ofxSCServer* server, int nodeID) {
    if(!server || !synthInstances.count(server) || !synthInstances[server]) return;
    resendParams.notify();
    synthInstances[server]->moveBefore(nodeID);
}

void scConvolution::setInputBus(ofxSCServer* server, scNode* node, int bus) {
    if(!server || !node) return;
    inputBuses[server][node] = bus;
    if(synthInstances.count(server) && synthInstances[server])
        synthInstances[server]->set("in", bus);
}

void scConvolution::setOutputBus(ofxSCServer* server, int index, int bus) {
    if(!server) return;
    outputBuses[server][index] = bus;
    if(synthInstances.count(server) && synthInstances[server])
        synthInstances[server]->set("out", bus);
}

void scConvolution::resetInputBusses(ofxSCServer* server, int targetBus) {
    if(!server) return;
    inputBuses[server].clear();
    if(synthInstances.count(server) && synthInstances[server])
        synthInstances[server]->set("in", targetBus);
}

int scConvolution::getOutputBusIndex(ofxSCServer* server, int index) {
    if(outputBuses.count(server) && outputBuses[server].count(index))
        return outputBuses[server].at(index);
    return -1;
}

int scConvolution::getLastSynthID(ofxSCServer* server) {
    auto it = synthInstances.find(server);
    if(it != synthInstances.end() && it->second) return it->second->nodeID;
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// IR loading helpers
// ─────────────────────────────────────────────────────────────────────────────

std::string scConvolution::getSynthDefName() const {
    return "scconvolution" + ofToString(numChannels.get());
}

void scConvolution::cancelLoad(ofxSCServer* srv) {
    ServerState& s = serverStates[srv];
    // Always free genSynth BEFORE irBuffer — the gen synth writes into irBuffer,
    // and buffer indices are recycled by the allocator.  If we freed irBuffer
    // first and the allocator immediately reused its index, the still-running
    // gen synth would corrupt whatever new buffer received that index.
    if(s.genSynth)          { s.genSynth->free();          delete s.genSynth;         s.genSynth         = nullptr; }
    if(s.irBuffer)          { s.irBuffer->free();          delete s.irBuffer;         s.irBuffer         = nullptr; }
    if(s.pendingSpecBuffer) { s.pendingSpecBuffer->free(); delete s.pendingSpecBuffer; s.pendingSpecBuffer = nullptr; }
    s.prepConvSent = false;
}

void scConvolution::startSyntheticIRForServer(ofxSCServer* srv) {
    serverStates[srv]; // ensure entry exists
    cancelLoad(srv);

    ServerState& s = serverStates[srv];
    int irFrames   = (int)(rt60.get() * 44100.0f) + 512;
    s.irBuffer     = new ofxSCBuffer(irFrames, 1, srv);
    s.irBuffer->alloc();

    s.fileMode = false;
    s.irState  = kLoadingIR;
    s.timer    = 0.0f;
}

void scConvolution::startSyntheticIR() {
    for(auto& kv : synthInstances) startSyntheticIRForServer(kv.first);
}

void scConvolution::loadImpulseResponse(const std::string& path) {
    for(auto& kv : synthInstances) serverStates[kv.first];

    for(auto& kv : serverStates) {
        ofxSCServer* srv = kv.first;
        cancelLoad(srv);

        ServerState& s = serverStates[srv];
        s.irBuffer = new ofxSCBuffer(1, 1, srv);

        // Send /b_allocReadChannel directly with numFrames = -1 (all frames)
        // and channels = {0} (load as mono regardless of source channel count).
        // The library's readChannel() sends numFrames=0 which reads nothing.
        ofxOscMessage m;
        m.setAddress("/b_allocReadChannel");
        m.addIntArg(s.irBuffer->index);
        m.addStringArg(path);
        m.addIntArg(0);    // startFrame
        m.addIntArg(-1);   // numFrames: -1 = all
        m.addIntArg(0);    // channel 0 only → mono
        srv->sendMsg(m);

        s.fileMode = true;
        s.irState  = kLoadingIR;
        s.timer    = 0.0f;
    }
}

void scConvolution::allocExactSpecBuffer(ofxSCServer* srv, int irFrames) {
    ServerState& s = serverStates[srv];
    if(s.pendingSpecBuffer) { s.pendingSpecBuffer->free(); delete s.pendingSpecBuffer; s.pendingSpecBuffer = nullptr; }

    if(irFrames <= 1) irFrames = 5 * 44100; // fallback if frame count unknown
    int partSize    = kFftSize / 2;
    int nPartitions = (irFrames + partSize - 1) / partSize;
    s.pendingSpecBuffer = new ofxSCBuffer(nPartitions * kFftSize, 1, srv);
    s.pendingSpecBuffer->alloc();
}

void scConvolution::preparePartConv(ofxSCServer* server) {
    ServerState& s = serverStates[server];
    if(!s.irBuffer || !s.pendingSpecBuffer) return;

    ofxOscMessage m;
    m.setAddress("/b_gen");
    m.addIntArg(s.pendingSpecBuffer->index);
    m.addStringArg("PreparePartConv");
    m.addIntArg(s.irBuffer->index);
    m.addIntArg(kFftSize);
    server->sendMsg(m);
}

void scConvolution::swapAndReplaceWithNewSpec(ofxSCServer* srv) {
    ServerState& s = serverStates[srv];
    s.irState      = kReady;
    s.timer        = 0.0f;
    s.prepConvSent = false;

    if(!synthInstances.count(srv) || !synthInstances[srv]) return;

    ofxSCBuffer* oldActive = s.activeSpecBuffer;
    s.activeSpecBuffer     = s.pendingSpecBuffer;
    s.pendingSpecBuffer    = nullptr;

    int         oldID    = synthInstances[srv]->nodeID;
    ofxSCSynth* old      = synthInstances[srv];
    ofxSCSynth* ns       = new ofxSCSynth(getSynthDefName(), srv);
    synthInstances[srv]  = ns;
    resendParams.notify();                       // queues specbuf + buses + wet/level
    ns->createAndRun(4, oldID, getActive());     // kAddAction_replace
    delete old;

    if(oldActive) { oldActive->free(); delete oldActive; }
}

void scConvolution::sendParamsToSynth(ofxSCServer* server) {
    auto it = synthInstances.find(server);
    if(it == synthInstances.end() || !it->second) return;
    ofxSCSynth* synth = it->second;

    if(inputBuses.count(server)  && !inputBuses[server].empty())
        synth->set("in",  inputBuses[server].begin()->second);
    if(outputBuses.count(server) && outputBuses[server].count(0))
        synth->set("out", outputBuses[server].at(0));

    synth->set("wet",   wet.get());
    synth->set("level", level.get());

    if(serverStates.count(server) && serverStates[server].activeSpecBuffer)
        synth->set("specbuf", serverStates[server].activeSpecBuffer->index);
}

// ─────────────────────────────────────────────────────────────────────────────
// File dialog + status widget
// ─────────────────────────────────────────────────────────────────────────────

void scConvolution::openFileDialog() {
    ofFileDialogResult res = ofSystemLoadDialog("Choose impulse response (WAV / AIFF)");
    if(res.bSuccess) irFilePath = res.getPath();
}

void scConvolution::drawStatusWidget() {
    float zoom = ofxOceanodeShared::getZoomLevel();
    bool anyReady    = false;
    bool anyLoading  = false;
    bool anyFileMode = false;
    float progress   = 0.0f;

    for(auto& kv : serverStates) {
        const ServerState& s = kv.second;
        if(s.irState == kReady)               anyReady    = true;
        if(s.irState == kLoadingIR ||
           s.irState == kPreparingSpec)        anyLoading  = true;
        if(s.fileMode)                         anyFileMode = true;
        if(s.irState == kPreparingSpec && !s.fileMode) {
            float total = rt60.get() + 1.5f;
            progress = ofClamp(s.timer / total, 0.0f, 1.0f);
        }
    }

    ImVec4 col;
    std::string label;

    if(anyReady && !anyLoading) {
        col   = ImVec4(0.3f, 0.9f, 0.3f, 1.0f);
        label = anyFileMode
              ? ofFile(irFilePath.get()).getFileName()
              : "Synth IR";
    } else if(anyLoading) {
        col   = ImVec4(1.0f, 0.82f, 0.2f, 1.0f);
        for(auto& kv : serverStates) {
            switch(kv.second.irState) {
                case kLoadingIR:    label = kv.second.fileMode ? "Reading file..." : "Allocating..."; break;
                case kPreparingSpec:
                    label = kv.second.prepConvSent ? "Preparing spectrum..."
                          : (kv.second.fileMode    ? "Preparing spectrum..."
                                                   : "Generating IR...");
                    break;
                default: break;
            }
        }
    } else {
        col   = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
        label = "No IR";
    }

    ImGui::TextColored(col, "%s", label.c_str());
    ImGui::SameLine();
    if(ImGui::SmallButton("Load File"))  openFileDialog();
    if(anyFileMode) {
        ImGui::SameLine();
        if(ImGui::SmallButton("Synth")) { irFilePath.set(""); startSyntheticIR(); }
    }
    ImGui::Dummy(ImVec2(0, 2.0f * zoom));
}
