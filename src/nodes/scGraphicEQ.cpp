//
//  scGraphicEQ.cpp
//  ofxOceanodeSupercollider
//
//  5-band parametric EQ with graphical EQ curve and optional FFT overlay.
//  All band parameters are vector<float> for per-channel modulation.
//

#include "ofxOceanodeSuperColliderConfig.h"
#include "scGraphicEQ.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "ofxSuperCollider.h"
#include "imgui.h"
#include "ofxOceanodeShared.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

scGraphicEQ::scGraphicEQ() : scNode("Graphic EQ") {
    fftMagnitudes.fill(0.0f);
}

scGraphicEQ::~scGraphicEQ() {
    try {
        listeners.unsubscribeAll();

        freeAllFFTSynths();

        for(auto& pair : synthInstances) {
            if(pair.second) { pair.second->free(); delete pair.second; }
        }
        synthInstances.clear();
        inputBuses.clear();
        outputBuses.clear();
    } catch(const std::exception& e) {
        ofLogError("scGraphicEQ") << "Destructor error: " << e.what();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers for vector parameter expansion
// ─────────────────────────────────────────────────────────────────────────────

// Broadcast scalar OR pass-through N-length vector to exactly nCh elements.
vector<float> scGraphicEQ::expandToChannels(const vector<float>& v, int nCh) const {
    if(nCh <= 0) return {};
    vector<float> out(nCh);
    for(int i = 0; i < nCh; i++)
        out[i] = v.empty() ? 0.0f : v[i % (int)v.size()];
    return out;
}

// Convert pitch vector (MIDI notes) to Hz per channel, expanded to nCh.
vector<float> scGraphicEQ::pitchVecToHz(const vector<float>& pitchVec, int nCh) const {
    vector<float> hz = expandToChannels(pitchVec, nCh);
    for(auto& p : hz) p = pitchToHz(p);
    return hz;
}

// Convert Q vector to rq (1/Q) vector, expanded to nCh.
vector<float> scGraphicEQ::qToRq(const vector<float>& qVec, int nCh) const {
    vector<float> rq = expandToChannels(qVec, nCh);
    for(auto& r : rq) r = 1.0f / std::max(r, 0.01f);
    return rq;
}

// ─────────────────────────────────────────────────────────────────────────────
// setup()
// ─────────────────────────────────────────────────────────────────────────────

void scGraphicEQ::setup() {
    // ── Core ────────────────────────────────────────────────────────────────
    addParameter(numChannels.set("Num Channels", 2, 1, MAX_NODE_CHANNELS));
    oldNumChannels = numChannels;

    // Convenience: single-element default / min / max vectors
    const vector<float> gainDef  = { 0.0f };
    const vector<float> gainMin  = { GAIN_MIN_DB };
    const vector<float> gainMax  = { GAIN_MAX_DB };
    const vector<float> pitchMin = { PITCH_MIN };
    const vector<float> pitchMax = { PITCH_MAX };
    const vector<float> slopeDef = { 1.0f };
    const vector<float> slopeMin = { 0.1f };
    const vector<float> slopeMax = { 4.0f };
    const vector<float> qDef     = { 1.0f };
    const vector<float> qMin     = { 0.1f };
    const vector<float> qMax     = { 20.0f };
    const vector<float> mixDef   = { 1.0f };
    const vector<float> mixMin   = { 0.0f };
    const vector<float> mixMax   = { 1.0f };

    // ── Band 1: Low Shelf ────────────────────────────────────────────────────
    addSeparator("Band 1 — Low Shelf", ofColor(100, 180, 255));
    addParameter(b1gain.set("B1 Gain",  gainDef,             gainMin,  gainMax));
    addParameter(b1pitch.set("B1 Pitch",vector<float>{46.0f}, pitchMin, pitchMax));
    addParameter(b1slope.set("B1 Slope",slopeDef,            slopeMin, slopeMax));

    // ── Band 2: Low-Mid Peak ─────────────────────────────────────────────────
    addSeparator("Band 2 — Low Mid", ofColor(100, 220, 140));
    addParameter(b2gain.set("B2 Gain",  gainDef,             gainMin,  gainMax));
    addParameter(b2pitch.set("B2 Pitch",vector<float>{71.0f}, pitchMin, pitchMax));
    addParameter(b2q.set("B2 Q",        qDef,                qMin,     qMax));

    // ── Band 3: Mid Peak ─────────────────────────────────────────────────────
    addSeparator("Band 3 — Mid", ofColor(220, 200, 80));
    addParameter(b3gain.set("B3 Gain",  gainDef,             gainMin,  gainMax));
    addParameter(b3pitch.set("B3 Pitch",vector<float>{84.0f}, pitchMin, pitchMax));
    addParameter(b3q.set("B3 Q",        qDef,                qMin,     qMax));

    // ── Band 4: High-Mid Peak ────────────────────────────────────────────────
    addSeparator("Band 4 — High Mid", ofColor(255, 140, 80));
    addParameter(b4gain.set("B4 Gain",  gainDef,             gainMin,  gainMax));
    addParameter(b4pitch.set("B4 Pitch",vector<float>{96.0f}, pitchMin, pitchMax));
    addParameter(b4q.set("B4 Q",        qDef,                qMin,     qMax));

    // ── Band 5: High Shelf ───────────────────────────────────────────────────
    addSeparator("Band 5 — High Shelf", ofColor(200, 100, 255));
    addParameter(b5gain.set("B5 Gain",  gainDef,              gainMin,  gainMax));
    addParameter(b5pitch.set("B5 Pitch",vector<float>{115.0f}, pitchMin, pitchMax));
    addParameter(b5slope.set("B5 Slope",slopeDef,             slopeMin, slopeMax));

    // ── Output ───────────────────────────────────────────────────────────────
    addSeparator("Output", ofColor(180));
    addParameter(mix.set("Mix", mixDef, mixMin, mixMax));

    // ── Inspector only ──────────────────────────────────────────────────────
    addInspectorParameter(showFFT.set("Show FFT", false));
    addInspectorParameter(widgetWidth.set("Widget Width",  240.0f, 150.0f, 800.0f));
    addInspectorParameter(widgetHeight.set("Widget Height", 160.0f,  80.0f, 400.0f));

    // ── Audio ports ─────────────────────────────────────────────────────────
    scNode::addInput("In");
    scNode::addOutput("Out");

    // ── Listeners ────────────────────────────────────────────────────────────

    listeners.push(numChannels.newListener([this](int &ch) {
        if(ch < 1 || ch > MAX_NODE_CHANNELS) return;
        if(oldNumChannels != ch) {
            for(auto& pair : synthInstances) {
                if(pair.second) {
                    ofxSCServer* srv = pair.first;
                    int oldID = pair.second->nodeID;
                    ofxSCSynth* newSynth = new ofxSCSynth(getSynthDefName(), srv);
                    newSynth->createAndRun(4, oldID, getActive()); // kAddAction_replace
                    delete pair.second;
                    pair.second = newSynth;
                }
            }
            resendParams.notify();
        }
        oldNumChannels = ch;
    }));

    // Any band parameter change → update synth + flag curve recompute
    auto paramListener = [this](vector<float>&) {
        curveNeedsUpdate = true;
        resendParams.notify();
    };
    listeners.push(b1gain.newListener(paramListener));
    listeners.push(b2gain.newListener(paramListener));
    listeners.push(b3gain.newListener(paramListener));
    listeners.push(b4gain.newListener(paramListener));
    listeners.push(b5gain.newListener(paramListener));
    listeners.push(b1pitch.newListener(paramListener));
    listeners.push(b2pitch.newListener(paramListener));
    listeners.push(b3pitch.newListener(paramListener));
    listeners.push(b4pitch.newListener(paramListener));
    listeners.push(b5pitch.newListener(paramListener));
    listeners.push(b1slope.newListener(paramListener));
    listeners.push(b2q.newListener(paramListener));
    listeners.push(b3q.newListener(paramListener));
    listeners.push(b4q.newListener(paramListener));
    listeners.push(b5slope.newListener(paramListener));

    // Mix doesn't affect EQ curve, just send to synth
    listeners.push(mix.newListener([this](vector<float>&) {
        resendParams.notify();
    }));

    // showFFT toggle
    listeners.push(showFFT.newListener([this](bool &v) {
        if(v) {
            for(auto& pair : synthInstances)
                if(pair.second) createFFTSynth(pair.first);
        } else {
            freeAllFFTSynths();
        }
    }));

    // resendParams event listener - centralized parameter synchronization
    listeners.push(resendParams.newListener([this]() {
        for(auto& pair : synthInstances) {
            if(pair.second) {
                sendAllParamsToSynth(pair.first);
                
                // Restore input buses
                if(inputBuses.count(pair.first) && !inputBuses[pair.first].empty()) {
                    pair.second->set("in", inputBuses[pair.first].begin()->second);
                }
                
                // Restore output buses
                if(outputBuses.count(pair.first) && outputBuses[pair.first].count(0)) {
                    pair.second->set("out", outputBuses[pair.first].at(0));
                }
            }
        }
    }));

    // EQ curve widget
    addCustomRegion(
        ofParameter<std::function<void()>>().set("EQ Display", [this](){ drawEQWidget(); }),
        ofParameter<std::function<void()>>().set("EQ Display", [this](){ drawEQWidget(); })
    );

    recomputeEQCurve();
}

// ─────────────────────────────────────────────────────────────────────────────
// update()
// ─────────────────────────────────────────────────────────────────────────────

void scGraphicEQ::update(ofEventArgs &args) {
    if(curveNeedsUpdate) {
        recomputeEQCurve();
        curveNeedsUpdate = false;
    }

    if(showFFT.get()) {
        for(auto& pair : fftBuses) {
            if(!pair.second) continue;
            const vector<float>& raw = pair.second->readValues;
            if((int)raw.size() == NUM_BINS) {
                for(int i = 0; i < NUM_BINS; i++) {
                    fftMagnitudes[i] = fftMagnitudes[i] * fftSmoothingCoeff
                                     + raw[i] * (1.0f - fftSmoothingCoeff);
                }
            }
            pair.second->requestValues();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// activate / deactivate
// ─────────────────────────────────────────────────────────────────────────────

void scGraphicEQ::activate() {
    for(auto& pair : synthInstances)
        if(pair.second) pair.second->run(true);
    for(auto& pair : fftSynthInstances)
        if(pair.second) pair.second->run(true);
}

void scGraphicEQ::deactivate() {
    for(auto& pair : synthInstances)
        if(pair.second) pair.second->run(false);
    for(auto& pair : fftSynthInstances)
        if(pair.second) pair.second->run(false);
}

// ─────────────────────────────────────────────────────────────────────────────
// scNode interface
// ─────────────────────────────────────────────────────────────────────────────

void scGraphicEQ::buildSynth(ofxSCServer* server) {
    if(!server) return;
    synthInstances[server] = new ofxSCSynth(getSynthDefName(), server);
}

void scGraphicEQ::createSynth(ofxSCServer* server) {
    if(!server) return;
    if(synthInstances.count(server) == 0) return;
    
    try {
        resendParams.notify();
        synthInstances[server]->createAndRun(0, 1, getActive());

        if(showFFT.get()) createFFTSynth(server);

    } catch(const std::exception& e) {
        ofLogError("scGraphicEQ") << "createSynth error: " << e.what();
    }
}

void scGraphicEQ::free(ofxSCServer* server) {
    if(!server) return;
    try {
        freeFFTSynth(server);

        if(synthInstances.count(server) && synthInstances[server]) {
            synthInstances[server]->free();
            delete synthInstances[server];
            synthInstances.erase(server);
        }
        inputBuses.erase(server);
        outputBuses.erase(server);
    } catch(const std::exception& e) {
        ofLogError("scGraphicEQ") << "free() error: " << e.what();
    }
}

void scGraphicEQ::setInputBus(ofxSCServer* server, scNode* node, int bus) {
    if(!server || !node) return;
    inputBuses[server][node] = bus;

    if(synthInstances.count(server) && synthInstances[server]) {
        try {
            // Set the input bus parameter
            synthInstances[server]->set("in", bus);
        } catch(const std::exception& e) {
            ofLogError("scGraphicEQ") << "setInputBus error: " << e.what();
        }
    }
}

void scGraphicEQ::resetInputBusses(ofxSCServer* server, int targetBus) {
    if(!server) return;
    if(synthInstances.count(server) == 0) return;
    
    inputBuses[server].clear();
    
    if(synthInstances[server]) {
        synthInstances[server]->set("in", targetBus);
    }
    if(fftSynthInstances.count(server) && fftSynthInstances[server]) {
        fftSynthInstances[server]->set("in", targetBus);
    }
    
    // Notify audio-rate bus assignments (for future audio-rate modulation support)
    auto args = std::make_pair(server, targetBus);
    resetAudioRateBusAssignments.notify(args);
}

void scGraphicEQ::setOutputBus(ofxSCServer* server, int index, int bus) {
    if(!server) return;
    outputBuses[server][index] = bus;

    if(synthInstances.count(server) && synthInstances[server]) {
        try {
            synthInstances[server]->set("out", bus);

            if(index == 0 && showFFT.get() &&
               fftSynthInstances.count(server) && fftSynthInstances[server]) {
                fftSynthInstances[server]->set("in", bus);
            }
        } catch(const std::exception& e) {
            ofLogError("scGraphicEQ") << "setOutputBus error: " << e.what();
        }
    }
}

int scGraphicEQ::getOutputBusIndex(ofxSCServer* server, int index) {
    if(outputBuses.count(server) && outputBuses[server].count(index))
        return outputBuses[server].at(index);
    return -1;
}

void scGraphicEQ::moveSynthBefore(ofxSCServer* server, int nodeID) {
    if(!server) return;
    if(synthInstances.count(server) == 0) return;
    if(!synthInstances[server]) return;

    try {
        resendParams.notify();
        synthInstances[server]->moveBefore(nodeID);

        if(showFFT.get() && fftSynthInstances.count(server) && fftSynthInstances[server]) {
            fftSynthInstances[server]->moveBefore(nodeID);
        }
    } catch(const std::exception& e) {
        ofLogError("scGraphicEQ") << "moveSynthBefore error: " << e.what();
    }
}

int scGraphicEQ::getLastSynthID(ofxSCServer* server) {
    if(!server) return -1;

    if(showFFT.get() && fftSynthInstances.count(server) && fftSynthInstances[server])
        return fftSynthInstances[server]->nodeID;

    auto it = synthInstances.find(server);
    if(it != synthInstances.end() && it->second)
        return it->second->nodeID;
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// FFT synth management
// ─────────────────────────────────────────────────────────────────────────────

void scGraphicEQ::createFFTSynth(ofxSCServer* server) {
    if(!server) return;
    if(!synthInstances.count(server) || !synthInstances[server]) return;

    freeFFTSynth(server);

    try {
        ofxSCBus* bus = new ofxSCBus(RATE_CONTROL, NUM_BINS, server);
        if(!bus || bus->index < 0 || bus->index >= 4096) {
            if(bus) delete bus;
            return;
        }
        if(!server->controlBusses[bus->index]) {
            delete bus;
            return;
        }
        fftBuses[server] = bus;

        string fftDefName = "fftanalyzer" + ofToString(numChannels.get());
        ofxSCSynth* fftSynth = new ofxSCSynth(fftDefName, server);

        int inBus = -1;
        if(outputBuses.count(server) && outputBuses[server].count(0))
            inBus = outputBuses[server].at(0);
        if(inBus < 0 && inputBuses.count(server) && !inputBuses[server].empty())
            inBus = inputBuses[server].begin()->second;

        fftSynth->createAndRun(3, synthInstances[server]->nodeID, getActive()); // addAfter
        if(inBus >= 0) fftSynth->set("in", inBus);
        fftSynth->set("fftbus", bus->index);

        fftSynthInstances[server] = fftSynth;
        bus->requestValues();

    } catch(const std::exception& e) {
        ofLogError("scGraphicEQ") << "createFFTSynth error: " << e.what();
    }
}

void scGraphicEQ::freeFFTSynth(ofxSCServer* server) {
    if(!server) return;
    if(fftSynthInstances.count(server) && fftSynthInstances[server]) {
        fftSynthInstances[server]->free();
        delete fftSynthInstances[server];
        fftSynthInstances.erase(server);
    }
    if(fftBuses.count(server) && fftBuses[server]) {
        fftBuses[server]->free();
        delete fftBuses[server];
        fftBuses.erase(server);
    }
}

void scGraphicEQ::freeAllFFTSynths() {
    vector<ofxSCServer*> servers;
    for(auto& pair : fftSynthInstances) servers.push_back(pair.first);
    for(auto& s : servers) freeFFTSynth(s);
    fftMagnitudes.fill(0.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
// SC helpers
// ─────────────────────────────────────────────────────────────────────────────

std::string scGraphicEQ::getSynthDefName() const {
    return "graphiceq" + ofToString(numChannels.get());
}

void scGraphicEQ::sendAllParamsToSynth(ofxSCServer* server) {
    auto it = synthInstances.find(server);
    if(it == synthInstances.end() || !it->second) return;
    ofxSCSynth* synth = it->second;

    int nCh = numChannels.get();

    try {
        synth->set("b1gain",  expandToChannels(b1gain.get(),  nCh));
        synth->set("b1freq",  pitchVecToHz(b1pitch.get(), nCh));  // pitch → Hz
        synth->set("b1slope", expandToChannels(b1slope.get(), nCh));

        synth->set("b2gain", expandToChannels(b2gain.get(), nCh));
        synth->set("b2freq", pitchVecToHz(b2pitch.get(), nCh));
        synth->set("b2rq",   qToRq(b2q.get(), nCh));  // BPeakEQ uses rq = 1/Q

        synth->set("b3gain", expandToChannels(b3gain.get(), nCh));
        synth->set("b3freq", pitchVecToHz(b3pitch.get(), nCh));
        synth->set("b3rq",   qToRq(b3q.get(), nCh));

        synth->set("b4gain", expandToChannels(b4gain.get(), nCh));
        synth->set("b4freq", pitchVecToHz(b4pitch.get(), nCh));
        synth->set("b4rq",   qToRq(b4q.get(), nCh));

        synth->set("b5gain",  expandToChannels(b5gain.get(),  nCh));
        synth->set("b5freq",  pitchVecToHz(b5pitch.get(), nCh));
        synth->set("b5slope", expandToChannels(b5slope.get(), nCh));

        // Mix: scalar (same for all channels — XFade2 in SC handles per-channel audio)
        float mixVal = mix.get().empty() ? 1.0f : mix.get()[0];
        synth->set("mix", mixVal);
    } catch(const std::exception& e) {
        ofLogError("scGraphicEQ") << "sendAllParamsToSynth error: " << e.what();
    }
}

void scGraphicEQ::restoreFullState(ofxSCServer* server) {
    // This method is now deprecated - use resendParams.notify() instead
    // Kept for backward compatibility
    if(!server) return;
    resendParams.notify();
}

// ─────────────────────────────────────────────────────────────────────────────
// EQ curve (biquad response and drawing: scEQEditor.h)
// ─────────────────────────────────────────────────────────────────────────────

float scGraphicEQ::getDisplaySampleRate() const {
    if(!synthInstances.empty() && synthInstances.begin()->first != nullptr) {
        return (float)serverManager::getSampleRateForServer(synthInstances.begin()->first);
    }
    return (float)scPreferences().hardwareSampleRate;
}

void scGraphicEQ::recomputeEQCurve() {
    // Use channel 0 value (index 0) of each parameter vector for the display curve.
    // Pitch parameters are converted to Hz for biquad computation.
    auto v0 = [](const vector<float>& v, float def) -> float {
        return v.empty() ? def : v[0];
    };
    auto p0hz = [&](const vector<float>& v, float defPitch) -> float {
        return pitchToHz(v.empty() ? defPitch : v[0]);
    };
    eqEditor.dbRange = GAIN_MAX_DB;
    eqEditor.bands[0] = { p0hz(b1pitch, 46.f),  v0(b1gain, 0.f), v0(b1slope, 1.f) };
    eqEditor.bands[1] = { p0hz(b2pitch, 71.f),  v0(b2gain, 0.f), v0(b2q, 1.f)     };
    eqEditor.bands[2] = { p0hz(b3pitch, 84.f),  v0(b3gain, 0.f), v0(b3q, 1.f)     };
    eqEditor.bands[3] = { p0hz(b4pitch, 96.f),  v0(b4gain, 0.f), v0(b4q, 1.f)     };
    eqEditor.bands[4] = { p0hz(b5pitch, 115.f), v0(b5gain, 0.f), v0(b5slope, 1.f) };
    eqEditor.recompute(getDisplaySampleRate());
}

// ─────────────────────────────────────────────────────────────────────────────
// drawEQWidget()
// ─────────────────────────────────────────────────────────────────────────────

void scGraphicEQ::drawEQWidget() {
    float zoom = ofxOceanodeShared::getZoomLevel();
    static const scEQEditor::Colors colors = scEQEditor::defaultColors();
    eqEditor.draw(widgetWidth.get() * zoom, widgetHeight.get() * zoom, colors, false,
                  showFFT.get() ? fftMagnitudes.data() : nullptr, NUM_BINS);
}
