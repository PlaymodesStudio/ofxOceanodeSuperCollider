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
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

scGraphicEQ::scGraphicEQ() : scNode("Graphic EQ") {
    combinedCurveDb.fill(0.0f);
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
        for(auto& pair : synthInstances) {
            if(pair.second) {
                ofxSCServer* srv = pair.first;
                int oldID = pair.second->nodeID;
                ofxSCSynth* newSynth = new ofxSCSynth(getSynthDefName(), srv);
                newSynth->create(4, oldID); // kAddAction_replace
                newSynth->run(getActive());
                delete pair.second;
                pair.second = newSynth;
                restoreFullState(srv);
            }
        }
        for(auto& output : outputs) output = output;
    }));

    // Any band parameter change → update synth + flag curve recompute
    auto paramListener = [this](vector<float>&) {
        curveNeedsUpdate = true;
        for(auto& pair : synthInstances)
            if(pair.second) sendAllParamsToSynth(pair.first);
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
        for(auto& pair : synthInstances)
            if(pair.second) sendAllParamsToSynth(pair.first);
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
    // Nothing needed at build time
}

void scGraphicEQ::createSynth(ofxSCServer* server) {
    if(!server) return;
    try {
        if(synthInstances[server]) {
            synthInstances[server]->free();
            delete synthInstances[server];
        }
        synthInstances[server] = new ofxSCSynth(getSynthDefName(), server);
        synthInstances[server]->create();
        synthInstances[server]->run(getActive());

        sendAllParamsToSynth(server);

        if(inputBuses.count(server) && !inputBuses[server].empty())
            synthInstances[server]->set("in", inputBuses[server].begin()->second);

        if(outputBuses.count(server) && outputBuses[server].count(0))
            synthInstances[server]->set("out", outputBuses[server].at(0));

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
            synthInstances[server]->set("in", bus);
            sendAllParamsToSynth(server);
            if(outputBuses.count(server) && outputBuses[server].count(0))
                synthInstances[server]->set("out", outputBuses[server].at(0));
        } catch(const std::exception& e) {
            ofLogError("scGraphicEQ") << "setInputBus error: " << e.what();
        }
    }
}

void scGraphicEQ::resetInputBusses(ofxSCServer* server, int targetBus) {
    inputBuses[server].clear();
    if(synthInstances.count(server) && synthInstances[server]) {
        synthInstances[server]->set("in", targetBus);
    }
    if(fftSynthInstances.count(server) && fftSynthInstances[server]) {
        fftSynthInstances[server]->set("in", targetBus);
    }
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
    auto it = synthInstances.find(server);
    if(it == synthInstances.end() || !it->second) return;

    try {
        restoreFullState(server);
        it->second->moveBefore(nodeID);

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

        fftSynth->create(3, synthInstances[server]->nodeID); // addAfter
        fftSynth->run(getActive());
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
    if(!server) return;
    auto it = synthInstances.find(server);
    if(it == synthInstances.end() || !it->second) return;
    ofxSCSynth* synth = it->second;
    try {
        sendAllParamsToSynth(server);
        if(inputBuses.count(server) && !inputBuses[server].empty())
            synth->set("in", inputBuses[server].begin()->second);
        if(outputBuses.count(server) && outputBuses[server].count(0))
            synth->set("out", outputBuses[server].at(0));
    } catch(const std::exception& e) {
        ofLogError("scGraphicEQ") << "restoreFullState error: " << e.what();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Biquad coefficient computation (Robert Bristow-Johnson Audio EQ Cookbook)
// ─────────────────────────────────────────────────────────────────────────────

scGraphicEQ::BiquadCoeffs scGraphicEQ::computeLowShelf(
    float freqHz, float gainDb, float slope, float sr)
{
    float freq = ofClamp(freqHz, 10.0f, sr * 0.499f);
    float A    = std::pow(10.0f, gainDb / 40.0f);
    float w0   = 2.0f * (float)M_PI * freq / sr;
    float cosW = std::cos(w0);
    float sinW = std::sin(w0);
    float alpha = sinW / 2.0f * std::sqrt((A + 1.0f/A) * (1.0f/slope - 1.0f) + 2.0f);
    float sqA  = std::sqrt(A);

    float b0 =  A * ((A+1) - (A-1)*cosW + 2.0f*sqA*alpha);
    float b1 = 2.0f*A * ((A-1) - (A+1)*cosW);
    float b2 =  A * ((A+1) - (A-1)*cosW - 2.0f*sqA*alpha);
    float a0 =       (A+1) + (A-1)*cosW + 2.0f*sqA*alpha;
    float a1 = -2.0f * ((A-1) + (A+1)*cosW);
    float a2 =       (A+1) + (A-1)*cosW - 2.0f*sqA*alpha;

    return { b0/a0, b1/a0, b2/a0, a1/a0, a2/a0 };
}

scGraphicEQ::BiquadCoeffs scGraphicEQ::computeHighShelf(
    float freqHz, float gainDb, float slope, float sr)
{
    float freq = ofClamp(freqHz, 10.0f, sr * 0.499f);
    float A    = std::pow(10.0f, gainDb / 40.0f);
    float w0   = 2.0f * (float)M_PI * freq / sr;
    float cosW = std::cos(w0);
    float sinW = std::sin(w0);
    float alpha = sinW / 2.0f * std::sqrt((A + 1.0f/A) * (1.0f/slope - 1.0f) + 2.0f);
    float sqA  = std::sqrt(A);

    float b0 =  A * ((A+1) + (A-1)*cosW + 2.0f*sqA*alpha);
    float b1 = -2.0f*A * ((A-1) + (A+1)*cosW);
    float b2 =  A * ((A+1) + (A-1)*cosW - 2.0f*sqA*alpha);
    float a0 =       (A+1) - (A-1)*cosW + 2.0f*sqA*alpha;
    float a1 =  2.0f * ((A-1) - (A+1)*cosW);
    float a2 =       (A+1) - (A-1)*cosW - 2.0f*sqA*alpha;

    return { b0/a0, b1/a0, b2/a0, a1/a0, a2/a0 };
}

scGraphicEQ::BiquadCoeffs scGraphicEQ::computePeakEQ(
    float freqHz, float gainDb, float qFactor, float sr)
{
    float freq = ofClamp(freqHz, 10.0f, sr * 0.499f);
    float q    = std::max(qFactor, 0.01f);
    float A    = std::pow(10.0f, gainDb / 40.0f);
    float w0   = 2.0f * (float)M_PI * freq / sr;
    float cosW = std::cos(w0);
    float sinW = std::sin(w0);
    float alpha = sinW / (2.0f * q);

    float b0 =  1.0f + alpha * A;
    float b1 = -2.0f * cosW;
    float b2 =  1.0f - alpha * A;
    float a0 =  1.0f + alpha / A;
    float a1 = -2.0f * cosW;
    float a2 =  1.0f - alpha / A;

    return { b0/a0, b1/a0, b2/a0, a1/a0, a2/a0 };
}

float scGraphicEQ::computeMagnitudeDb(const BiquadCoeffs& c, float freqHz, float sr) {
    float freq = ofClamp(freqHz, 10.0f, sr * 0.499f);
    float w    = 2.0f * (float)M_PI * freq / sr;
    float cw   = std::cos(w);
    float c2w  = std::cos(2.0f * w);

    float numRe = c.b0 + c.b1*cw + c.b2*c2w;
    float numIm = -(c.b1*std::sin(w) + c.b2*std::sin(2.0f*w));
    float denRe = 1.0f + c.a1*cw + c.a2*c2w;
    float denIm = -(c.a1*std::sin(w) + c.a2*std::sin(2.0f*w));

    float numSq = numRe*numRe + numIm*numIm;
    float denSq = denRe*denRe + denIm*denIm;

    if(denSq < 1e-12f) return 0.0f;
    float mag = std::sqrt(numSq / denSq);
    return 20.0f * std::log10f(std::max(mag, 1e-12f));
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

    BiquadCoeffs band1 = computeLowShelf (p0hz(b1pitch, 46.f),  v0(b1gain, 0.f), v0(b1slope, 1.f), SAMPLE_RATE);
    BiquadCoeffs band2 = computePeakEQ   (p0hz(b2pitch, 71.f),  v0(b2gain, 0.f), v0(b2q, 1.f),     SAMPLE_RATE);
    BiquadCoeffs band3 = computePeakEQ   (p0hz(b3pitch, 84.f),  v0(b3gain, 0.f), v0(b3q, 1.f),     SAMPLE_RATE);
    BiquadCoeffs band4 = computePeakEQ   (p0hz(b4pitch, 96.f),  v0(b4gain, 0.f), v0(b4q, 1.f),     SAMPLE_RATE);
    BiquadCoeffs band5 = computeHighShelf(p0hz(b5pitch, 115.f), v0(b5gain, 0.f), v0(b5slope, 1.f), SAMPLE_RATE);

    float logMin = std::log10f(FREQ_MIN);
    float logMax = std::log10f(FREQ_MAX);

    for(int i = 0; i < NUM_FREQ_POINTS; i++) {
        float t    = (float)i / (float)(NUM_FREQ_POINTS - 1);
        float freq = std::pow(10.0f, logMin + t * (logMax - logMin));

        float db = computeMagnitudeDb(band1, freq, SAMPLE_RATE)
                 + computeMagnitudeDb(band2, freq, SAMPLE_RATE)
                 + computeMagnitudeDb(band3, freq, SAMPLE_RATE)
                 + computeMagnitudeDb(band4, freq, SAMPLE_RATE)
                 + computeMagnitudeDb(band5, freq, SAMPLE_RATE);

        combinedCurveDb[i] = ofClamp(db, GAIN_MIN_DB - 3.0f, GAIN_MAX_DB + 3.0f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ImGui visualization helpers
// ─────────────────────────────────────────────────────────────────────────────

float scGraphicEQ::logFreqToX(float freq, float xStart, float width) {
    float logMin = std::log10f(FREQ_MIN);
    float logMax = std::log10f(FREQ_MAX);
    float t = (std::log10f(ofClamp(freq, FREQ_MIN, FREQ_MAX)) - logMin) / (logMax - logMin);
    return xStart + t * width;
}

float scGraphicEQ::dbToY(float db, float yStart, float height) {
    float t = (GAIN_MAX_DB - db) / (GAIN_MAX_DB - GAIN_MIN_DB);
    return yStart + ofClamp(t, 0.0f, 1.0f) * height;
}

// ─────────────────────────────────────────────────────────────────────────────
// drawEQWidget()
// ─────────────────────────────────────────────────────────────────────────────

void scGraphicEQ::drawEQWidget() {
    ImDrawList* dl     = ImGui::GetWindowDrawList();
    ImVec2      cursor = ImGui::GetCursorScreenPos();

    const float W   = widgetWidth.get();
    const float H   = widgetHeight.get();
    const float pad = 2.0f;
    const float xS  = cursor.x + pad;
    const float yS  = cursor.y + pad;
    const float xE  = xS + W;
    const float yE  = yS + H;

    // Background
    dl->AddRectFilled(ImVec2(xS, yS), ImVec2(xE, yE), IM_COL32(12, 14, 20, 255));
    dl->AddRect(ImVec2(xS, yS), ImVec2(xE, yE), IM_COL32(70, 70, 90, 255));

    // ── Frequency grid (vertical) ─────────────────────────────────────────
    static const float gridFreqs[]  = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
    static const char* gridLabels[] = { "20", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k" };
    for(int g = 0; g < 10; g++) {
        float gx = logFreqToX(gridFreqs[g], xS, W);
        dl->AddLine(ImVec2(gx, yS), ImVec2(gx, yE - 14.0f), IM_COL32(40, 40, 55, 200));
        dl->AddText(ImVec2(gx + 2.0f, yE - 14.0f), IM_COL32(100, 100, 120, 220), gridLabels[g]);
    }

    // ── dB grid (horizontal) ─────────────────────────────────────────────
    static const float dbGrid[] = { -48, -36, -24, -12, 0, 12, 24, 36, 48 };
    for(float db : dbGrid) {
        float gy    = dbToY(db, yS, H);
        ImU32 color = (db == 0.0f) ? IM_COL32(200, 200, 200, 160) : IM_COL32(40, 40, 55, 160);
        dl->AddLine(ImVec2(xS, gy), ImVec2(xE, gy), color);
        if(db != GAIN_MIN_DB && db != GAIN_MAX_DB) {
            char label[8]; sprintf(label, "%+.0f", db);
            dl->AddText(ImVec2(xS + 2.0f, gy - 11.0f), IM_COL32(90, 90, 110, 200), label);
        }
    }

    // ── Optional FFT spectrum overlay ─────────────────────────────────────
    if(showFFT.get()) {
        const float fftLogRatio = std::log(FFT_FREQ_MAX / FFT_FREQ_MIN);
        const float eqLogMin    = std::log10f(FREQ_MIN);
        const float eqLogMax    = std::log10f(FREQ_MAX);

        for(int b = 0; b < NUM_BINS; b++) {
            float fc1 = FFT_FREQ_MIN * std::exp(fftLogRatio * (float)b       / (float)NUM_BINS);
            float fc2 = FFT_FREQ_MIN * std::exp(fftLogRatio * (float)(b + 1) / (float)NUM_BINS);

            if(fc2 <= FREQ_MIN) continue;
            if(fc1 >= FREQ_MAX) break;
            fc1 = std::max(fc1, FREQ_MIN);
            fc2 = std::min(fc2, FREQ_MAX);

            float x1 = xS + W * ofClamp((std::log10f(fc1) - eqLogMin) / (eqLogMax - eqLogMin), 0.0f, 1.0f);
            float x2 = xS + W * ofClamp((std::log10f(fc2) - eqLogMin) / (eqLogMax - eqLogMin), 0.0f, 1.0f);
            if(x2 <= x1) continue;

            float mag   = fftMagnitudes[b];
            float magDb = 20.0f * std::log10f(std::max(mag, 1e-12f));
            float barH  = H * ofClamp((magDb - FFT_DB_FLOOR) / (-FFT_DB_FLOOR), 0.0f, 1.0f);
            float barTop = yE - barH;

            dl->AddRectFilled(ImVec2(x1, barTop), ImVec2(x2, yE),
                              IM_COL32(60, 160, 80, 65));
        }
    }

    // ── Band frequency markers (channel 0 pitch → Hz) ────────────────────
    auto p0hz = [&](const vector<float>& v, float defPitch) -> float {
        return pitchToHz(v.empty() ? defPitch : v[0]);
    };
    const float  bandFreqs[5]  = {
        p0hz(b1pitch, 46.f), p0hz(b2pitch, 71.f), p0hz(b3pitch, 84.f),
        p0hz(b4pitch, 96.f), p0hz(b5pitch, 115.f)
    };
    const ImU32  bandColors[5] = {
        IM_COL32(100, 200, 255, 140),
        IM_COL32(100, 255, 150, 140),
        IM_COL32(255, 220, 80,  140),
        IM_COL32(255, 140, 80,  140),
        IM_COL32(210, 100, 255, 140),
    };
    for(int b = 0; b < 5; b++) {
        float bx = logFreqToX(bandFreqs[b], xS, W);
        dl->AddLine(ImVec2(bx, yS), ImVec2(bx, yE - 14.0f), bandColors[b]);
    }

    // ── 0 dB reference line — drawn BEFORE curve so curve sits on top ─────
    {
        float y0 = dbToY(0.0f, yS, H);
        dl->AddLine(ImVec2(xS, y0), ImVec2(xE, y0), IM_COL32(220, 220, 220, 180), 1.0f);
    }

    // ── EQ curve (channel 0) ──────────────────────────────────────────────
    {
        float logMin = std::log10f(FREQ_MIN);
        float logMax = std::log10f(FREQ_MAX);
        for(int i = 0; i < NUM_FREQ_POINTS - 1; i++) {
            float t1 = (float)i       / (float)(NUM_FREQ_POINTS - 1);
            float t2 = (float)(i + 1) / (float)(NUM_FREQ_POINTS - 1);
            float freq1 = std::pow(10.0f, logMin + t1 * (logMax - logMin));
            float freq2 = std::pow(10.0f, logMin + t2 * (logMax - logMin));

            float x1 = logFreqToX(freq1, xS, W);
            float y1 = dbToY(combinedCurveDb[i],   yS, H);
            float x2 = logFreqToX(freq2, xS, W);
            float y2 = dbToY(combinedCurveDb[i+1], yS, H);

            dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(80, 210, 255, 255), 2.0f);
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + H + 2.0f * pad));
    ImGui::Dummy(ImVec2(W, 4.0f));
}
