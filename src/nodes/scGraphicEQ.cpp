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
#include <cfloat>

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
    addParameter(showWindow.set("Show", false));
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
    bandSeparators[0] = lastAddedParameter();
    bandParamHandles[0] = {
        addParameter(b1gain.set("B1 Gain",  gainDef,             gainMin,  gainMax)),
        addParameter(b1pitch.set("B1 Pitch",vector<float>{46.0f}, pitchMin, pitchMax)),
        addParameter(b1slope.set("B1 Slope",slopeDef,            slopeMin, slopeMax)) };

    // ── Band 2: Low-Mid Peak ─────────────────────────────────────────────────
    addSeparator("Band 2 — Low Mid", ofColor(100, 220, 140));
    bandSeparators[1] = lastAddedParameter();
    bandParamHandles[1] = {
        addParameter(b2gain.set("B2 Gain",  gainDef,             gainMin,  gainMax)),
        addParameter(b2pitch.set("B2 Pitch",vector<float>{71.0f}, pitchMin, pitchMax)),
        addParameter(b2q.set("B2 Q",        qDef,                qMin,     qMax)) };

    // ── Band 3: Mid Peak ─────────────────────────────────────────────────────
    addSeparator("Band 3 — Mid", ofColor(220, 200, 80));
    bandSeparators[2] = lastAddedParameter();
    bandParamHandles[2] = {
        addParameter(b3gain.set("B3 Gain",  gainDef,             gainMin,  gainMax)),
        addParameter(b3pitch.set("B3 Pitch",vector<float>{84.0f}, pitchMin, pitchMax)),
        addParameter(b3q.set("B3 Q",        qDef,                qMin,     qMax)) };

    // ── Band 4: High-Mid Peak ────────────────────────────────────────────────
    addSeparator("Band 4 — High Mid", ofColor(255, 140, 80));
    bandSeparators[3] = lastAddedParameter();
    bandParamHandles[3] = {
        addParameter(b4gain.set("B4 Gain",  gainDef,             gainMin,  gainMax)),
        addParameter(b4pitch.set("B4 Pitch",vector<float>{96.0f}, pitchMin, pitchMax)),
        addParameter(b4q.set("B4 Q",        qDef,                qMin,     qMax)) };

    // ── Band 5: High Shelf ───────────────────────────────────────────────────
    addSeparator("Band 5 — High Shelf", ofColor(200, 100, 255));
    bandSeparators[4] = lastAddedParameter();
    bandParamHandles[4] = {
        addParameter(b5gain.set("B5 Gain",  gainDef,              gainMin,  gainMax)),
        addParameter(b5pitch.set("B5 Pitch",vector<float>{115.0f}, pitchMin, pitchMax)),
        addParameter(b5slope.set("B5 Slope",slopeDef,             slopeMin, slopeMax)) };

    // ── Output ───────────────────────────────────────────────────────────────
    addSeparator("Output", ofColor(180));
    addParameter(mix.set("Mix", mixDef, mixMin, mixMax));

    // ── Inspector only ──────────────────────────────────────────────────────
    addInspectorParameter(showParamsInNode.set("Params In Node", true));
    addInspectorParameter(windowFFT.set("Window FFT", true));
    addInspectorParameter(windowDbRange.set("Window dB Range", 24, 6, 48));

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
                    // fftanalyzerN depends on the channel count too
                    if(fftSynthInstances.count(srv)) createFFTSynth(srv);
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

    // FFT analysis: window overlay (Show + Window FFT)
    listeners.push(showParamsInNode.newListener([this](bool &) { applyParamVisibility(); }));
    listeners.push(showWindow.newListener([this](bool &) { updateFFTState(); }));
    listeners.push(windowFFT.newListener([this](bool &)  { updateFFTState(); }));
    listeners.push(windowDbRange.newListener([this](int &) { windowCurveDirty = true; }));

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

    recomputeEQCurve();
}

// ─────────────────────────────────────────────────────────────────────────────
// update()
// ─────────────────────────────────────────────────────────────────────────────

void scGraphicEQ::update(ofEventArgs &args) {
    // Connections can appear / disappear at any time (a connected band
    // parameter stays visible while the others are hidden)
    if(!showParamsInNode.get()) applyParamVisibility();

    if(curveNeedsUpdate) {
        recomputeEQCurve();
        curveNeedsUpdate = false;
        windowCurveDirty = true;
    }

    if(fftWanted()) {
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

        if(fftWanted()) createFFTSynth(server);

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

            if(index == 0 &&
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

        if(fftSynthInstances.count(server) && fftSynthInstances[server]) {
            fftSynthInstances[server]->moveBefore(nodeID);
        }
    } catch(const std::exception& e) {
        ofLogError("scGraphicEQ") << "moveSynthBefore error: " << e.what();
    }
}

int scGraphicEQ::getLastSynthID(ofxSCServer* server) {
    if(!server) return -1;

    if(fftSynthInstances.count(server) && fftSynthInstances[server])
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

void scGraphicEQ::updateFFTState() {
    if(fftWanted()) {
        for(auto& pair : synthInstances)
            if(pair.second && !fftSynthInstances.count(pair.first)) createFFTSynth(pair.first);
    } else {
        freeAllFFTSynths();
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
// Band parameter visibility in the node GUI ("Params In Node")
// ─────────────────────────────────────────────────────────────────────────────

ofxOceanodeAbstractParameter* scGraphicEQ::lastAddedParameter() {
    auto& group = getParameterGroup();
    if(group.size() == 0) return nullptr;
    return &static_cast<ofxOceanodeAbstractParameter&>(group.get(group.size() - 1));
}

void scGraphicEQ::applyParamVisibility() {
    const bool show = showParamsInNode.get();
    auto setHidden = [](ofxOceanodeAbstractParameter* p, bool hidden) {
        if(!p) return;
        const ofxOceanodeParameterFlags f = p->getFlags();
        const ofxOceanodeParameterFlags nf = hidden ? (f | ofxOceanodeParameterFlags_NoGuiWidget)
                                                    : (f & ~ofxOceanodeParameterFlags_NoGuiWidget);
        if(nf != f) p->setFlags(nf);
    };
    for(int b = 0; b < 5; b++) {
        bool anyVisible = false;
        for(auto& h : bandParamHandles[b]) {
            if(!h) continue;
            const bool visible = show || h->hasInConnection() || h->hasOutConnections();
            setHidden(h.get(), !visible);
            anyVisible = anyVisible || visible;
        }
        setHidden(bandSeparators[b], !anyVisible);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Editor window: dockable ImGui window with the interactive curve editor
// (scEQEditor handles, as in scGrainBox's FX EQ) over the FFT of the output.
//   drag a handle: frequency / gain (Shift: fine), wheel: Q / slope,
//   double-click: 0 dB, right-click: exact values.
// ─────────────────────────────────────────────────────────────────────────────

void scGraphicEQ::draw(ofEventArgs &) {
    if(showWindow.get()) drawEditorWindow();
}

void scGraphicEQ::applyBandEdit(int band, const scEQEditor::Band& bd) {
    if(band < 0 || band >= scEQEditor::NUM_BANDS) return;
    ofParameter<vector<float>>* gainP[5]  = { &b1gain,  &b2gain,  &b3gain,  &b4gain,  &b5gain  };
    ofParameter<vector<float>>* pitchP[5] = { &b1pitch, &b2pitch, &b3pitch, &b4pitch, &b5pitch };
    ofParameter<vector<float>>* shapeP[5] = { &b1slope, &b2q,     &b3q,     &b4q,     &b5slope };

    // Moves element 0 to newV0; other channels follow by the same delta
    // (additive) or ratio (multiplicative), clamped to the parameter range.
    auto shift = [](ofParameter<vector<float>>& p, float newV0, bool multiplicative) {
        const vector<float>& cur = p.get();
        const float lo = p.getMin().empty() ? -FLT_MAX : p.getMin()[0];
        const float hi = p.getMax().empty() ?  FLT_MAX : p.getMax()[0];
        newV0 = ofClamp(newV0, lo, hi);
        vector<float> v = cur.empty() ? vector<float>{ newV0 } : cur;
        const float old0 = v[0];
        for(size_t i = 0; i < v.size(); i++) {
            float nv;
            if(i == 0)               nv = newV0;
            else if(multiplicative)  nv = old0 > 0.0f ? v[i] * (newV0 / old0) : newV0;
            else                     nv = v[i] + (newV0 - old0);
            v[i] = ofClamp(nv, lo, hi);
        }
        if(v != cur) p.set(v);
    };

    shift(*pitchP[band], hzToPitch(bd.freqHz), false);
    shift(*gainP[band],  bd.gainDb,            false);
    shift(*shapeP[band], bd.shape,             true);
}

void scGraphicEQ::drawEditorWindow() {
    const float zoom = ofxOceanodeShared::getZoomLevel();

    ImGui::SetNextWindowSize(ImVec2(640 * zoom, 360 * zoom), ImGuiCond_FirstUseEver);
    std::string title = "Graphic EQ " + ofToString(getNumIdentifier());
    bool open = true;
    const bool visible = ImGui::Begin(title.c_str(), &open,
                                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if(!visible) {
        ImGui::End();
        if(!open) showWindow = false;
        return;
    }

    static const char* bandShort[5] = { "LS", "LM", "MID", "HM", "HS" };
    static const scEQEditor::Colors col = [] {
        scEQEditor::Colors c = scEQEditor::defaultColors();
        c.fft = IM_COL32(60, 170, 90, 95);
        return c;
    }();

    // ── Toolbar ──────────────────────────────────────────────────────────────
    {
        bool f = windowFFT.get();
        if(ImGui::Checkbox("FFT", &f)) windowFFT = f;

        ImGui::SameLine();
        static const int ranges[] = { 6, 12, 24, 48 };
        char rl[16]; std::snprintf(rl, sizeof(rl), "+/-%d dB", windowDbRange.get());
        ImGui::SetNextItemWidth(90 * zoom);
        if(ImGui::BeginCombo("##gEqRange", rl)) {
            for(int r : ranges) {
                char l[16]; std::snprintf(l, sizeof(l), "+/-%d dB", r);
                if(ImGui::Selectable(l, windowDbRange.get() == r)) windowDbRange = r;
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        float m = mix.get().empty() ? 1.0f : mix.get()[0];
        ImGui::SetNextItemWidth(120 * zoom);
        if(ImGui::SliderFloat("Mix", &m, 0.0f, 1.0f, "%.2f")) {
            vector<float> v = mix.get();
            if(v.empty()) v = { m };
            const float d = m - v[0];
            for(auto& x : v) x = ofClamp(x + d, 0.0f, 1.0f);
            v[0] = m;
            mix = v;
        }

        ImGui::SameLine();
        bool pin = showParamsInNode.get();
        if(ImGui::Checkbox("Show parameters in node", &pin)) showParamsInNode = pin;

        ImGui::SameLine();
        if(ImGui::Button("Flat")) {
            for(auto* g : { &b1gain, &b2gain, &b3gain, &b4gain, &b5gain })
                g->set(vector<float>(std::max<size_t>(1, g->get().size()), 0.0f));
        }

        bool perChannel = false;
        for(auto* p : { &b1gain, &b2gain, &b3gain, &b4gain, &b5gain,
                        &b1pitch, &b2pitch, &b3pitch, &b4pitch, &b5pitch,
                        &b1slope, &b2q, &b3q, &b4q, &b5slope })
            if(p->get().size() > 1) { perChannel = true; break; }
        if(perChannel) {
            ImGui::SameLine();
            ImGui::TextDisabled("(curve: ch 1, edits keep channel offsets)");
        }
    }

    // ── Curve editor ─────────────────────────────────────────────────────────
    const ImVec2 avail   = ImGui::GetContentRegionAvail();
    const float  pad     = 2.0f * zoom;
    const float  rowH    = ImGui::GetTextLineHeightWithSpacing();
    const float  W       = std::max(160.0f * zoom, avail.x - 2.0f * pad);
    const float  H       = std::max(80.0f * zoom, avail.y - 2.0f * pad - 4.0f * zoom - rowH);

    // Parameters (channel 0) → editor
    windowEditor.bands     = eqEditor.bands;
    windowEditor.dbRange   = (float)windowDbRange.get();
    windowEditor.gainLimit = GAIN_MAX_DB;
    windowEditor.qMin      = b2q.getMin().empty()     ? 0.1f  : b2q.getMin()[0];
    windowEditor.qMax      = b2q.getMax().empty()     ? 20.0f : b2q.getMax()[0];
    windowEditor.slopeMin  = b1slope.getMin().empty() ? 0.1f  : b1slope.getMin()[0];
    windowEditor.slopeMax  = b1slope.getMax().empty() ? 4.0f  : b1slope.getMax()[0];
    if(windowCurveDirty) {
        windowEditor.recompute(getDisplaySampleRate());
        windowCurveDirty = false;
    }

    // Right-click on a handle: exact values
    windowEditor.bandMenu = [this](int b) {
        ImGui::TextUnformatted(scEQEditor::bandName(b));
        ImGui::Separator();
        scEQEditor::Band bd = windowEditor.bands[b];
        bool changed = false;
        float pitch = hzToPitch(bd.freqHz);
        ImGui::SetNextItemWidth(140.0f * ofxOceanodeShared::getZoomLevel());
        if(ImGui::DragFloat("Pitch", &pitch, 0.05f, PITCH_MIN, PITCH_MAX, "%.2f")) {
            bd.freqHz = pitchToHz(pitch); changed = true;
        }
        ImGui::SameLine(); ImGui::TextDisabled("%.1f Hz", bd.freqHz);
        ImGui::SetNextItemWidth(140.0f * ofxOceanodeShared::getZoomLevel());
        if(ImGui::DragFloat("Gain", &bd.gainDb, 0.05f, GAIN_MIN_DB, GAIN_MAX_DB, "%+.2f dB")) changed = true;
        const bool shelf = scEQEditor::isShelf(b);
        ImGui::SetNextItemWidth(140.0f * ofxOceanodeShared::getZoomLevel());
        if(ImGui::DragFloat(shelf ? "Slope" : "Q", &bd.shape, 0.01f,
                            shelf ? windowEditor.slopeMin : windowEditor.qMin,
                            shelf ? windowEditor.slopeMax : windowEditor.qMax, "%.2f")) changed = true;
        if(ImGui::Button("0 dB")) { bd.gainDb = 0.0f; changed = true; }
        if(changed) {
            windowEditor.bands[b] = bd;
            applyBandEdit(b, bd);
            windowCurveDirty = true;
        }
    };

    const bool withFFT = windowFFT.get() && !fftSynthInstances.empty();
    if(windowEditor.draw(W, H, col, true, withFFT ? fftMagnitudes.data() : nullptr, NUM_BINS, "##gEqWindowCurve")) {
        for(int b = 0; b < scEQEditor::NUM_BANDS; b++) {
            const auto& nb = windowEditor.bands[b];
            const auto& ob = eqEditor.bands[b];
            if(nb.freqHz != ob.freqHz || nb.gainDb != ob.gainDb || nb.shape != ob.shape)
                applyBandEdit(b, nb);
        }
        windowEditor.recompute(getDisplaySampleRate());   // no one-frame lag while dragging
    }

    // ── Band readouts ────────────────────────────────────────────────────────
    {
        const float x0   = ImGui::GetCursorPosX();
        const float colW = W / (float)scEQEditor::NUM_BANDS;
        for(int b = 0; b < scEQEditor::NUM_BANDS; b++) {
            if(b > 0) ImGui::SameLine(x0 + colW * b);
            const auto& bd = windowEditor.bands[b];
            ImU32 c = col.bandMarker[b] | IM_COL32(0, 0, 0, 255);
            char freq[16];
            if(bd.freqHz >= 1000.0f) std::snprintf(freq, sizeof(freq), "%.2fk", bd.freqHz / 1000.0f);
            else                     std::snprintf(freq, sizeof(freq), "%.0f", bd.freqHz);
            ImGui::PushStyleColor(ImGuiCol_Text, c);
            ImGui::Text("%s %s %+.1f %s%.2f", bandShort[b], freq, bd.gainDb,
                        scEQEditor::isShelf(b) ? "S" : "Q", bd.shape);
            ImGui::PopStyleColor();
        }
    }

    ImGui::End();
    if(!open) showWindow = false;
}
