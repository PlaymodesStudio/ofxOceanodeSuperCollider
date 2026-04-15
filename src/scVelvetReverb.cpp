//
//  scVelvetReverb.cpp
//  ofxOceanodeSuperCollider
//
//  scConvolution.cpp with:
//    • file loading removed
//    • scconvIRGen replaced by C++ velvet noise + /b_setn
//    • lpf, hpf, fbAmt parameters added
//  Everything else is verbatim scConvolution.
//

#include "scVelvetReverb.h"
#include "ofxSCSynth.h"
#include "ofxSCBuffer.h"
#include "ofxSuperCollider.h"
#include "ofxOscMessage.h"
#include "ofxOceanodeShared.h"
#include "imgui.h"
#include <cmath>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

scVelvetReverb::scVelvetReverb() : scNode("VelvetReverb") {}

scVelvetReverb::~scVelvetReverb() {
    try {
        listeners.unsubscribeAll();
        for(auto& p : synthInstances)
            if(p.second) { p.second->free(); delete p.second; }
        synthInstances.clear();
        inputBuses.clear();
        outputBuses.clear();
        for(auto& p : serverStates) {
            ServerState& s = p.second;
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

void scVelvetReverb::setup() {
    // Identical layout to scConvolution + lpf/hpf/fbAmt
    addParameter(numChannels.set("N Chan",      1,     1,    16));
    addParameter(wet.set(       "Wet",          0.0f,  0.0f, 1.0f));
    addParameter(level.set(     "Level",        1.0f,  0.0f, 2.0f));
    addParameter(rt60.set(      "RT60",         1.5f,  0.05f, 8.0f));
    addParameter(brightness.set("Brightness",   0.15f, 0.0f, 0.99f));
    addParameter(damping.set(   "Damping",      0.5f,  0.0f, 1.0f));
    addParameter(diffusion.set( "Diffusion",    0.1f,  0.0f, 0.5f));
    addParameter(lpf.set(       "LPF Hz",       12000.0f, 200.0f, 20000.0f));
    addParameter(hpf.set(       "HPF Hz",       20.0f, 20.0f, 2000.0f));
    addParameter(fbAmt.set(     "Feedback",     0.0f,  0.0f, 0.95f));

    oldNumChannels = numChannels;

    scNode::addInput("In");
    scNode::addOutput("Out");

    // numChannels: replace synth (verbatim scConvolution)
    listeners.push(numChannels.newListener([this](int& ch) {
        if(ch < 1 || ch > 16 || ch == oldNumChannels) return;
        for(auto& pair : synthInstances) {
            if(!pair.second) continue;
            int         oldID = pair.second->nodeID;
            ofxSCSynth* old   = pair.second;
            ofxSCSynth* ns    = new ofxSCSynth(getSynthDefName(), pair.first);
            pair.second = ns;
            resendParams.notify();
            ns->createAndRun(4, oldID, getActive());
            delete old;
        }
        oldNumChannels = ch;
    }));

    // wet / level: direct set (verbatim scConvolution)
    listeners.push(wet.newListener(  [this](float&) { resendParams.notify(); }));
    listeners.push(level.newListener([this](float&) { resendParams.notify(); }));

    // lpf / hpf / fbAmt: direct set to running synth
    listeners.push(lpf.newListener(  [this](float& v) {
        for(auto& p : synthInstances) if(p.second) p.second->set("lpf",   v);
    }));
    listeners.push(hpf.newListener(  [this](float& v) {
        for(auto& p : synthInstances) if(p.second) p.second->set("hpf",   v);
    }));
    listeners.push(fbAmt.newListener([this](float& v) {
        for(auto& p : synthInstances) if(p.second) p.second->set("fbAmt", v);
    }));

    // IR params: regenerate when changed manually.
    auto onIRParam = [this](float&) { startSyntheticIR(); };
    listeners.push(rt60.newListener(       onIRParam));
    listeners.push(brightness.newListener( onIRParam));
    listeners.push(damping.newListener(    onIRParam));
    listeners.push(diffusion.newListener(  onIRParam));

    // Preset load: param listeners may be suppressed by the framework during
    // bulk loading, so we hook presetHasLoaded directly to guarantee the IR is
    // recalculated with the restored param values after every preset load.
    listeners.push(ofxOceanodeShared::getPresetHasLoadedEvent().newListener([this](){
        startSyntheticIR();
    }));

    listeners.push(resendParams.newListener([this]() {
        for(auto& p : synthInstances)
            if(p.second) sendParamsToSynth(p.first);
    }));

    addCustomRegion(
        ofParameter<std::function<void()>>().set("VR", [this](){ drawStatusWidget(); }),
        ofParameter<std::function<void()>>().set("VR", [this](){ drawStatusWidget(); })
    );
}

// ─────────────────────────────────────────────────────────────────────────────
// update() — verbatim scConvolution, synthetic path only, no genSynth
// ─────────────────────────────────────────────────────────────────────────────

void scVelvetReverb::update(ofEventArgs& args) {
    const float dt = ofGetLastFrameTime();

    for(auto& kv : serverStates) {
        ofxSCServer* srv = kv.first;
        ServerState& s   = kv.second;

        // ── kLoadingIR ────────────────────────────────────────────────────────
        if(s.irState == kLoadingIR) {
            s.timer += dt;
            // Wait 300 ms for /b_alloc to complete on SC's NRT thread,
            // then generate IR in C++ and upload via /b_setn.
            if(s.timer >= 0.3f) {
                 if(s.irBuffer) {
                    const float  sr   = 44100.0f;
                    unsigned int seed = (unsigned int)(rt60.get()        * 1000.0f)
                                      ^ (unsigned int)(brightness.get()  * 10000.0f)
                                      ^ (unsigned int)(damping.get()     * 100000.0f);
                    auto ir = generateIR(s.irBuffer->frames, brightness.get(),
                                          damping.get(), diffusion.get(), sr, seed);
                    sendIRData(srv, s.irBuffer, ir);
                }
                s.irState      = kPreparingSpec;
                s.timer        = 0.0f;
                s.prepConvSent = false;
                allocExactSpecBuffer(srv, s.irBuffer ? s.irBuffer->frames : 0);
            }

        // ── kPreparingSpec ────────────────────────────────────────────────────
        } else if(s.irState == kPreparingSpec) {
            s.timer += dt;

            // C++ IR is fully written before we enter this state,
            // so PreparePartConv can be sent immediately (prepDelay = 0).
            if(!s.prepConvSent && s.timer >= 0.0f) {
                s.prepConvSent = true;
                preparePartConv(srv);
            }

            // Allow 1.5 s after PreparePartConv (verbatim scConvolution)
            if(s.prepConvSent && s.timer >= 1.5f)
                swapAndReplaceWithNewSpec(srv);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// activate / deactivate
// ─────────────────────────────────────────────────────────────────────────────

void scVelvetReverb::activate()   { for(auto& p : synthInstances) if(p.second) p.second->run(true);  }
void scVelvetReverb::deactivate() { for(auto& p : synthInstances) if(p.second) p.second->run(false); }

// ─────────────────────────────────────────────────────────────────────────────
// scNode interface — verbatim scConvolution
// ─────────────────────────────────────────────────────────────────────────────

void scVelvetReverb::buildSynth(ofxSCServer* server) {
    if(!server) return;
    synthInstances[server] = new ofxSCSynth(getSynthDefName(), server);

    ServerState& s = serverStates[server];

    if(!s.activeSpecBuffer) {
        s.activeSpecBuffer = new ofxSCBuffer(kFftSize, 1, server);
        s.activeSpecBuffer->alloc();
    }

    if(s.irState == kIdle)
        startSyntheticIRForServer(server);
}

void scVelvetReverb::createSynth(ofxSCServer* server) {
    if(!server || !synthInstances.count(server)) return;
    resendParams.notify();
    synthInstances[server]->createAndRun(0, 1, getActive());
}

void scVelvetReverb::free(ofxSCServer* server) {
    if(!server) return;
    if(synthInstances.count(server) && synthInstances[server]) {
        synthInstances[server]->free();
        delete synthInstances[server];
        synthInstances.erase(server);
    }
    inputBuses.erase(server);
    outputBuses.erase(server);

    // Keep serverStates alive across graph recomputes — verbatim scConvolution.
    // Only preserve kReady if we genuinely had a built IR; if we were mid-build
    // reset to kIdle so buildSynth restarts it.
    if(serverStates.count(server)) {
        ServerState& s = serverStates[server];
        bool wasReady = (s.irState == kReady);
        cancelLoad(server);
        s.irState      = (wasReady && s.activeSpecBuffer) ? kReady : kIdle;
        s.prepConvSent = false;
        s.timer        = 0.0f;
    }
}

void scVelvetReverb::moveSynthBefore(ofxSCServer* server, int nodeID) {
    if(!server || !synthInstances.count(server) || !synthInstances[server]) return;
    resendParams.notify();
    synthInstances[server]->moveBefore(nodeID);
}

void scVelvetReverb::setInputBus(ofxSCServer* server, scNode* node, int bus) {
    if(!server || !node) return;
    inputBuses[server][node] = bus;
    if(synthInstances.count(server) && synthInstances[server])
        synthInstances[server]->set("in", bus);
}

void scVelvetReverb::setOutputBus(ofxSCServer* server, int index, int bus) {
    if(!server) return;
    outputBuses[server][index] = bus;
    if(synthInstances.count(server) && synthInstances[server])
        synthInstances[server]->set("out", bus);
}

void scVelvetReverb::resetInputBusses(ofxSCServer* server, int targetBus) {
    if(!server) return;
    inputBuses[server].clear();
    if(synthInstances.count(server) && synthInstances[server])
        synthInstances[server]->set("in", targetBus);
}

int scVelvetReverb::getOutputBusIndex(ofxSCServer* server, int index) {
    if(outputBuses.count(server) && outputBuses[server].count(index))
        return outputBuses[server].at(index);
    return -1;
}

int scVelvetReverb::getLastSynthID(ofxSCServer* server) {
    auto it = synthInstances.find(server);
    if(it != synthInstances.end() && it->second) return it->second->nodeID;
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// IR helpers — verbatim scConvolution naming/structure
// ─────────────────────────────────────────────────────────────────────────────

std::string scVelvetReverb::getSynthDefName() const {
    return "scvelvetreverb" + ofToString(numChannels.get());
}

void scVelvetReverb::cancelLoad(ofxSCServer* srv) {
    ServerState& s = serverStates[srv];
    if(s.irBuffer)          { s.irBuffer->free();          delete s.irBuffer;         s.irBuffer         = nullptr; }
    if(s.pendingSpecBuffer) { s.pendingSpecBuffer->free(); delete s.pendingSpecBuffer; s.pendingSpecBuffer = nullptr; }
    s.prepConvSent = false;
}

void scVelvetReverb::startSyntheticIRForServer(ofxSCServer* srv) {
    serverStates[srv]; // ensure entry exists
    cancelLoad(srv);

    ServerState& s  = serverStates[srv];
    int irFrames    = (int)(rt60.get() * 44100.0f) + 512;
    s.irBuffer      = new ofxSCBuffer(irFrames, 1, srv);
    s.irBuffer->alloc();

    s.irState = kLoadingIR;
    s.timer   = 0.0f;
}

void scVelvetReverb::startSyntheticIR() {
    for(auto& kv : synthInstances) startSyntheticIRForServer(kv.first);
}

void scVelvetReverb::allocExactSpecBuffer(ofxSCServer* srv, int irFrames) {
    ServerState& s = serverStates[srv];
    if(s.pendingSpecBuffer) {
        s.pendingSpecBuffer->free(); delete s.pendingSpecBuffer; s.pendingSpecBuffer = nullptr;
    }
    if(irFrames <= 1) irFrames = (int)(rt60.get() * 44100.0f) + 512;
    int partSize    = kFftSize / 2;
    int nPartitions = (irFrames + partSize - 1) / partSize;
    s.pendingSpecBuffer = new ofxSCBuffer(nPartitions * kFftSize, 1, srv);
    s.pendingSpecBuffer->alloc();
}

void scVelvetReverb::preparePartConv(ofxSCServer* server) {
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

void scVelvetReverb::swapAndReplaceWithNewSpec(ofxSCServer* srv) {
    ServerState& s = serverStates[srv];
    s.irState      = kReady;
    s.timer        = 0.0f;
    s.prepConvSent = false;

    if(!synthInstances.count(srv) || !synthInstances[srv]) return;

    ofxSCBuffer* oldActive = s.activeSpecBuffer;
    s.activeSpecBuffer     = s.pendingSpecBuffer;
    s.pendingSpecBuffer    = nullptr;

    int         oldID = synthInstances[srv]->nodeID;
    ofxSCSynth* old   = synthInstances[srv];
    ofxSCSynth* ns    = new ofxSCSynth(getSynthDefName(), srv);
    synthInstances[srv] = ns;
    resendParams.notify();
    ns->createAndRun(4, oldID, getActive());
    delete old;

    if(oldActive) { oldActive->free(); delete oldActive; }
}

void scVelvetReverb::sendParamsToSynth(ofxSCServer* server) {
    auto it = synthInstances.find(server);
    if(it == synthInstances.end() || !it->second) return;
    ofxSCSynth* synth = it->second;

    if(inputBuses.count(server)  && !inputBuses[server].empty())
        synth->set("in",  inputBuses[server].begin()->second);
    if(outputBuses.count(server) && outputBuses[server].count(0))
        synth->set("out", outputBuses[server].at(0));

    synth->set("wet",   wet.get());
    synth->set("level", level.get());
    synth->set("lpf",   lpf.get());
    synth->set("hpf",   hpf.get());
    synth->set("fbAmt", fbAmt.get());

    if(serverStates.count(server) && serverStates[server].activeSpecBuffer)
        synth->set("specbuf", serverStates[server].activeSpecBuffer->index);
}

// ─────────────────────────────────────────────────────────────────────────────
// C++ velvet noise IR generation (replaces scconvIRGen)
// ─────────────────────────────────────────────────────────────────────────────

std::vector<float> scVelvetReverb::generateIR(int numFrames, float brt, float dmp,
                                               float diff, float sr, unsigned int seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    std::uniform_real_distribution<float> vel(-1.0f, 1.0f);

    std::vector<float> ir(numFrames, 0.0f);

    const float maxDensity = 8000.0f;
    for(int i = 0; i < numFrames; i++) {
        float p = (float)i / (float)numFrames;
        if(u01(rng) < p * maxDensity / sr)
            ir[i] = vel(rng);
    }

    float startCoef   = 1.0f - brt;
    float absorpCurve = 0.2f + (1.0f - dmp) * 1.8f;
    float prev = 0.0f;
    for(int i = 0; i < numFrames; i++) {
        float t    = (float)i / (float)numFrames;
        float coef = std::pow(startCoef + t * (1.0f - startCoef), absorpCurve);
        ir[i] = (1.0f - coef) * ir[i] + coef * prev;
        prev  = ir[i];
    }

    float plateauSamples = diff * sr;
    float decayTime      = (float)numFrames / sr - diff;
    float decayRate      = (decayTime > 0.001f) ? std::log(0.001f) / decayTime : -100.0f;
    for(int i = 0; i < numFrames; i++) {
        float env = ((float)i < plateauSamples)
                  ? 1.0f
                  : std::exp(decayRate * ((float)(i - (int)plateauSamples) / sr));
        ir[i] *= env * 0.3f;
    }
    return ir;
}

void scVelvetReverb::sendIRData(ofxSCServer* srv, ofxSCBuffer* buf,
                                 const std::vector<float>& data) {
    if(!srv || !buf) return;
    const int chunkSize = 4096;
    int total = (int)data.size(), offset = 0;
    while(offset < total) {
        int count = std::min(chunkSize, total - offset);
        ofxOscMessage m;
        m.setAddress("/b_setn");
        m.addIntArg(buf->index);
        m.addIntArg(offset);
        m.addIntArg(count);
        for(int i = 0; i < count; i++) m.addFloatArg(data[offset + i]);
        srv->sendMsg(m);
        offset += count;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// drawStatusWidget — verbatim scConvolution (synthetic only)
// ─────────────────────────────────────────────────────────────────────────────

void scVelvetReverb::drawStatusWidget() {
    float zoom = ofxOceanodeShared::getZoomLevel();
    bool anyReady   = false;
    bool anyLoading = false;

    for(auto& kv : serverStates) {
        const ServerState& s = kv.second;
        if(s.irState == kReady)                                        anyReady   = true;
        if(s.irState == kLoadingIR || s.irState == kPreparingSpec)     anyLoading = true;
    }

    ImVec4      col;
    std::string label;

    if(anyReady && !anyLoading) {
        col   = ImVec4(0.3f, 0.9f, 0.3f, 1.0f);
        label = "Synth IR";
    } else if(anyLoading) {
        col   = ImVec4(1.0f, 0.82f, 0.2f, 1.0f);
        for(auto& kv : serverStates) {
            switch(kv.second.irState) {
                case kLoadingIR:     label = "Uploading IR...";       break;
                case kPreparingSpec: label = "Preparing spectrum..."; break;
                default: break;
            }
        }
    } else {
        col   = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
        label = "No IR";
    }

    ImGui::TextColored(col, "%s", label.c_str());
    ImGui::Dummy(ImVec2(0, 2.0f * zoom));
}
