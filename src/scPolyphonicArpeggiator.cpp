#include "scPolyphonicArpeggiator.h"
#include "ofxOceanodeShared.h"
#include "ofxSCSynth.h"
#include "imgui.h"
#include <chrono>

// ═══════════════════════════════════════════════════════════
// CONSTRUCTOR / DESTRUCTOR
// ═══════════════════════════════════════════════════════════

scPolyphonicArpeggiator::scPolyphonicArpeggiator() : scNode("Poly Arpeggiator AR") {
    rng = std::mt19937(std::chrono::steady_clock::now().time_since_epoch().count());
    dist01 = std::uniform_real_distribution<float>(0.0f, 1.0f);

    expandedScale.reserve(128);
    currentPitches.resize(MAX_SEQUENCE_SIZE, 60.0f);
    euclideanPattern.reserve(MAX_SEQUENCE_SIZE);
    euclideanAccents.reserve(MAX_SEQUENCE_SIZE);
    euclideanDurations.reserve(MAX_SEQUENCE_SIZE);
    deviationValues.resize(MAX_SEQUENCE_SIZE, 0.0f);

    snapshotSlots.resize(16);
    activeSnapshotSlot = -1;
    isMorphing = false;
}

scPolyphonicArpeggiator::~scPolyphonicArpeggiator() {
    listeners.unsubscribeAll();
    for(auto& pair : synthInstances) {
        if(pair.second) { pair.second->free(); delete pair.second; }
    }
    synthInstances.clear();
}

// ═══════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════

void scPolyphonicArpeggiator::setup() {
    description = "SC polyphonic arpeggiator — audio-rate trigger input, "
                  "audio-rate gate/pitch/velocity/duration outputs per voice. "
                  "Scale, pattern and euclidean data are pre-computed in C++ "
                  "and uploaded to the SynthDef. Timing-critical logic "
                  "(step counter, gate envelopes, strum) runs in SuperCollider.";

    // ── SC INPUT ──
    scNode::addInput("Trig");

    // ── SNAPSHOTS ──
    addSeparator("Snapshots", ofColor(200));
    uiSnapshots.set("SnapshotsUI", [this](){ drawSnapshotSlots(); });
    addCustomRegion(uiSnapshots, [this](){ drawSnapshotSlots(); });
    addParameter(morphTime.set("Morph Time", 0.0f, 0.0f, 10.0f));

    // ── TRIGGER & CONTROL ──
    addSeparator("Trigger", ofColor(200));
    addParameter(reset.set("Reset"));
    addParameter(eucLen.set("EucLen", 8, 1, 64));
    addParameter(eucHits.set("EucHits", 8, 0, 64));
    addParameter(eucOff.set("EucOff", 0, 0, 63));
    addParameter(stepChance.set("Step%", 1.0f, 0.0f, 1.0f));
    addParameter(noteChance.set("Note%", 1.0f, 0.0f, 1.0f));

    // ── PITCH ──
    addSeparator("Pitch", ofColor(200));
    addParameter(scale.set("Scale", {0, 2, 4, 5, 7, 9, 11}, {-24}, {127}));
    addParameterDropdown(patternMode, "Pattern", 0, {"Ascending", "Descending", "Random", "User"});
    addParameter(idxPattern.set("IdxPatt", {0, 1, 2, 3}, {0}, {127}));
    addParameter(seqSize.set("SeqSize", 16, 1, MAX_SEQUENCE_SIZE));
    addParameter(degStart.set("IdxStart", 0, 0, 127));
    addParameter(stepInterval.set("StepInterval", 1, 1, 12));
    addParameter(transpose.set("Transpose", 0, 0, 96));

    // ── POLYPHONY ──
    addSeparator("Polyphony", ofColor(200));
    addParameter(polyphony.set("Polyphony", 1, 1, MAX_POLYPHONY));
    addParameter(polyInterval.set("PolyInterval", 2, 1, 12));
    addParameter(skipSteps.set("SkipSteps", 0, 0, 32));
    addParameter(strum.set("Strum", 0.0f, 0.0f, 500.0f));
    addParameter(strumRndm.set("StrumRndm", 0.0f, 0.0f, 200.0f));
    addParameterDropdown(strumDir, "StrumDir", 0, {"Ascending", "Descending", "Random"});

    // ── DEVIATION ──
    addSeparator("Deviation", ofColor(200));
    addParameter(octaveDev.set("OctDev", 0.0f, 0.0f, 1.0f));
    addParameter(octaveDevRng.set("OctDevRng", 1, 1, 4));
    addParameter(idxDev.set("IdxDev", 0.0f, 0.0f, 1.0f));
    addParameter(idxDevRng.set("IdxDevRng", 2, 1, 12));
    addParameter(pitchDev.set("PitchDev", 0.0f, 0.0f, 1.0f));
    addParameter(pitchDevRng.set("PitchDevRng", 2, 1, 12));

    // ── VELOCITY ──
    addSeparator("Velocity", ofColor(200));
    addParameter(velBase.set("VelBase", 0.8f, 0.0f, 1.0f));
    addParameter(velRndm.set("VelRndm", 0.1f, 0.0f, 1.0f));
    addParameter(eucAccLen.set("AccLen", 4, 1, 64));
    addParameter(eucAccHits.set("AccHits", 1, 0, 64));
    addParameter(eucAccOff.set("AccOff", 0, 0, 63));
    addParameter(eucAccStrength.set("AccStr", 0.2f, 0.0f, 1.0f));

    // ── DURATION ──
    addSeparator("Duration", ofColor(200));
    addParameter(durBase.set("DurBase", 100, 1, 5000));
    addParameter(durRndm.set("DurRndm", 20, 0, 1000));
    addParameter(eucDurLen.set("DurEucLen", 4, 1, 64));
    addParameter(eucDurHits.set("DurEucHits", 4, 0, 64));
    addParameter(eucDurOff.set("DurEucOff", 0, 0, 63));
    addParameter(durEucStrength.set("DurEucStr", 50, -5000, 5000));

    // ── SC SEED ──
    addSeparator("Random", ofColor(200));
    addParameter(seed.set("Seed", 0, 0, 65536));

    // ── C++ VISUALISATION OUTPUTS ──
    addSeparator("Output", ofColor(200));
    addOutputParameter(pitchOut.set("PitchOut",
        vector<float>(16, 60.0f), vector<float>(1, 0.0f), vector<float>(1, 127.0f)));
    addOutputParameter(activePitchOut.set("ActivePitchOut",
        vector<float>(1, 60.0f), vector<float>(1, 0.0f), vector<float>(1, 127.0f)));

    // ── SC OUTPUTS ──
    scNode::addOutput("GateOut");
    scNode::addOutput("PitchOut");
    scNode::addOutput("VelOut");
    scNode::addOutput("DurOut");

    // ── DISPLAY ──
    addSeparator("Display", ofColor(200));
    addInspectorParameter(guiWidth.set("GUI Width", 240.0f, 200.0f, 600.0f));
    addInspectorParameter(patternHeight.set("Pattern Height", 100.0f, 50.0f, 200.0f));
    addInspectorParameter(euclideanHeight.set("Euclidean Height", 80.0f, 40.0f, 150.0f));

    uiPattern.set("Pattern Display", [this](){ drawPatternDisplay(); });
    addCustomRegion(uiPattern, [this](){ drawPatternDisplay(); });

    uiEuclidean.set("Euclidean Display", [this](){ drawEuclideanDisplay(); });
    addCustomRegion(uiEuclidean, [this](){ drawEuclideanDisplay(); });

    // ── EVENT LISTENERS ──

    // Reset fires a single-sample pulse into the SC synth via the \reset parameter
    listeners.push(reset.newListener([this](void){
        for(auto& pair : synthInstances) {
            if(pair.second) pair.second->set("reset", 1.0f);
        }
    }));

    // Euclidean gate pattern
    auto rebuildGate = [this](int&){
        generateEuclideanPattern(euclideanPattern, eucLen, eucHits, eucOff);
        uploadArraysToSynths();
    };
    listeners.push(eucLen.newListener(rebuildGate));
    listeners.push(eucHits.newListener(rebuildGate));
    listeners.push(eucOff.newListener(rebuildGate));

    // Euclidean accent pattern
    auto rebuildAccent = [this](int&){
        generateEuclideanPattern(euclideanAccents, eucAccLen, eucAccHits, eucAccOff);
        uploadArraysToSynths();
    };
    listeners.push(eucAccLen.newListener(rebuildAccent));
    listeners.push(eucAccHits.newListener(rebuildAccent));
    listeners.push(eucAccOff.newListener(rebuildAccent));

    // Euclidean duration pattern
    auto rebuildDuration = [this](int&){
        generateEuclideanPattern(euclideanDurations, eucDurLen, eucDurHits, eucDurOff);
        uploadArraysToSynths();
    };
    listeners.push(eucDurLen.newListener(rebuildDuration));
    listeners.push(eucDurHits.newListener(rebuildDuration));
    listeners.push(eucDurOff.newListener(rebuildDuration));

    // Pitch sequence rebuild — two typed lambdas because scale is vector<float>, others are int
    auto rebuildPitchVf = [this](vector<float>&){
        rebuildExpandedScale();
        rebuildDeviations();
        rebuildPitchSequence();
        uploadArraysToSynths();
    };
    auto rebuildPitchI = [this](int&){
        rebuildExpandedScale();
        rebuildDeviations();
        rebuildPitchSequence();
        uploadArraysToSynths();
    };
    listeners.push(scale.newListener(rebuildPitchVf));
    listeners.push(degStart.newListener(rebuildPitchI));
    listeners.push(stepInterval.newListener(rebuildPitchI));
    listeners.push(transpose.newListener(rebuildPitchI));
    listeners.push(idxPattern.newListener([this](vector<int>&){ rebuildPitchSequence(); uploadArraysToSynths(); }));
    listeners.push(patternMode.newListener([this](int&){ rebuildPitchSequence(); uploadArraysToSynths(); }));

    // Deviation rebuild — two typed lambdas because *Dev is float, *DevRng is int
    auto rebuildDevF = [this](float&){
        rebuildDeviations();
        rebuildPitchSequence();
        uploadArraysToSynths();
    };
    auto rebuildDevI = [this](int&){
        rebuildDeviations();
        rebuildPitchSequence();
        uploadArraysToSynths();
    };
    listeners.push(octaveDev.newListener(rebuildDevF));
    listeners.push(octaveDevRng.newListener(rebuildDevI));
    listeners.push(idxDev.newListener(rebuildDevF));
    listeners.push(idxDevRng.newListener(rebuildDevI));
    listeners.push(pitchDev.newListener(rebuildDevF));
    listeners.push(pitchDevRng.newListener(rebuildDevI));

    // seqSize change — rebuild everything and switch SynthDef if needed
    listeners.push(seqSize.newListener([this](int& sz){
        currentPitches.resize(sz, 60.0f);
        deviationValues.resize(sz, 0.0f);
        rebuildDeviations();
        rebuildPitchSequence();
        uploadArraysToSynths();
        uploadScalarParamsToSynths();
    }));

    // Polyphony change — just send the scalar to SC; no synth recreation needed.
    // The SynthDef is fixed at 16 channels and masks unused voices via \polyphony KR.
    listeners.push(polyphony.newListener([this](int& n){
        for(auto& pair : synthInstances)
            if(pair.second) pair.second->set("polyphony", (float)n);
        activePitchOut.set(vector<float>(n, 60.0f));
    }));

    // Scalar parameters — send directly to SC synths
    listeners.push(polyInterval.newListener([this](int&){ uploadScalarParamsToSynths(); }));
    listeners.push(skipSteps.newListener([this](int&){ uploadScalarParamsToSynths(); }));
    listeners.push(strum.newListener([this](float&){ uploadScalarParamsToSynths(); }));
    listeners.push(strumRndm.newListener([this](float&){ uploadScalarParamsToSynths(); }));
    listeners.push(strumDir.newListener([this](int&){ uploadScalarParamsToSynths(); }));
    listeners.push(velBase.newListener([this](float&){ uploadScalarParamsToSynths(); }));
    listeners.push(velRndm.newListener([this](float&){ uploadScalarParamsToSynths(); }));
    listeners.push(eucAccStrength.newListener([this](float&){ uploadScalarParamsToSynths(); }));
    listeners.push(durBase.newListener([this](int&){ uploadScalarParamsToSynths(); }));
    listeners.push(durRndm.newListener([this](int&){ uploadScalarParamsToSynths(); }));
    listeners.push(durEucStrength.newListener([this](int&){ uploadScalarParamsToSynths(); }));
    listeners.push(stepChance.newListener([this](float&){ uploadScalarParamsToSynths(); }));
    listeners.push(noteChance.newListener([this](float&){ uploadScalarParamsToSynths(); }));
    listeners.push(seed.newListener([this](int&){ uploadScalarParamsToSynths(); }));

    // ── RESEND PARAMS (called by graph optimizer before createAndRun / moveBefore) ──
    listeners.push(resendParams.newListener([this](){
        uploadArraysToSynths();
        uploadScalarParamsToSynths();
        static const char* busNames[] = {"gateout", "pitchout", "velout", "durout"};
        for(auto& pair : synthInstances) {
            if(!pair.second) continue;
            auto* server = pair.first;
            auto* s = pair.second;
            // Resend input bus (trig)
            for(int i = 0; i < (int)inputs.size(); i++) {
                auto* nodeRef = inputs[i]->getNodeRef();
                if(nodeRef && inputBuses.count(server) && inputBuses[server].count(nodeRef))
                    s->set(ofToLower(inputs[i].getName()), inputBuses[server].at(nodeRef));
            }
            // Resend output buses
            for(int i = 0; i < 4; i++) {
                if(outputBuses.count(server) && outputBuses[server].count(i))
                    s->set(busNames[i], outputBuses[server].at(i));
            }
        }
    }));

    // ── INITIALISE ──
    generateEuclideanPattern(euclideanPattern,  eucLen,    eucHits,    eucOff);
    generateEuclideanPattern(euclideanAccents,  eucAccLen, eucAccHits, eucAccOff);
    generateEuclideanPattern(euclideanDurations, eucDurLen, eucDurHits, eucDurOff);
    rebuildExpandedScale();
    rebuildDeviations();
    rebuildPitchSequence();

    loadAllSnapshotsFromDisk();
}

// ═══════════════════════════════════════════════════════════
// UPDATE
// ═══════════════════════════════════════════════════════════

void scPolyphonicArpeggiator::update(ofEventArgs &e) {
    if(isMorphing) updateMorph();
}

// ═══════════════════════════════════════════════════════════
// SC NODE — SYNTH LIFECYCLE
// ═══════════════════════════════════════════════════════════

void scPolyphonicArpeggiator::buildSynth(ofxSCServer* server) {
    if(!server) return;
    synthInstances[server] = new ofxSCSynth(getSynthDefName(), server);
}

void scPolyphonicArpeggiator::createSynth(ofxSCServer* server) {
    if(!server) return;
    if(synthInstances.count(server) == 0) return;
    // Notify first so all buses and params are queued as init-args in /s_new
    resendParams.notify();
    synthInstances[server]->createAndRun(0, 1, getActive());
}

void scPolyphonicArpeggiator::moveSynthBefore(ofxSCServer* server, int nodeID) {
    if(!server) return;
    if(synthInstances.count(server) == 0 || !synthInstances[server]) return;
    resendParams.notify();
    synthInstances[server]->moveBefore(nodeID);
}

void scPolyphonicArpeggiator::free(ofxSCServer* server) {
    if(!server) return;
    if(synthInstances.count(server) && synthInstances[server]) {
        synthInstances[server]->free();
        delete synthInstances[server];
        synthInstances.erase(server);
    }
    outputBuses.erase(server);
    inputBuses.erase(server);
}

void scPolyphonicArpeggiator::setInputBus(ofxSCServer* server, scNode* node, int bus) {
    if(!server || !node) return;
    inputBuses[server][node] = bus;
    if(synthInstances.count(server) && synthInstances[server]) {
        for(int i = 0; i < (int)inputs.size(); i++) {
            if(inputs[i]->getNodeRef() == node) {
                synthInstances[server]->set(ofToLower(inputs[i].getName()), bus);
            }
        }
    }
}

void scPolyphonicArpeggiator::resetInputBusses(ofxSCServer* server, int targetBus) {
    if(!server) return;
    if(synthInstances.count(server) == 0) return;
    inputBuses[server].clear();
    if(synthInstances[server]) {
        for(int i = 0; i < (int)inputs.size(); i++) {
            synthInstances[server]->set(ofToLower(inputs[i].getName()), targetBus);
        }
    }
}

int scPolyphonicArpeggiator::getOutputBusIndex(ofxSCServer* server, int index) {
    if(outputBuses.count(server) && outputBuses[server].count(index))
        return outputBuses[server].at(index);
    return -1;
}

int scPolyphonicArpeggiator::getLastSynthID(ofxSCServer* server) {
    auto it = synthInstances.find(server);
    if(it != synthInstances.end() && it->second) return it->second->nodeID;
    return -1;
}

void scPolyphonicArpeggiator::setOutputBus(ofxSCServer* server, int index, int bus) {
    if(!server) return;
    outputBuses[server][index] = bus;
    if(!synthInstances.count(server) || !synthInstances[server]) return;

    auto synth = synthInstances[server];
    // The SynthDef has named outputs: gateout, pitchout, velout, durout
    static const char* busNames[] = {"gateout", "pitchout", "velout", "durout"};
    if(index >= 0 && index < 4) {
        synth->set(busNames[index], bus);
    }
}

void scPolyphonicArpeggiator::activate() {
    for(auto& pair : synthInstances)
        if(pair.second) pair.second->run(true);
}

void scPolyphonicArpeggiator::deactivate() {
    for(auto& pair : synthInstances)
        if(pair.second) pair.second->run(false);
}

// ═══════════════════════════════════════════════════════════
// SYNTHDEF NAME
// ═══════════════════════════════════════════════════════════

string scPolyphonicArpeggiator::getSynthDefName() const {
    return "ScPolyArpeggiator16"; // fixed 16-channel SynthDef; polyphony is a runtime KR parameter
}

// ═══════════════════════════════════════════════════════════
// ARRAY UPLOAD TO SC
// ═══════════════════════════════════════════════════════════

void scPolyphonicArpeggiator::uploadArraysToSynths() {
    if(synthInstances.empty()) return;

    int sz = MAX_SEQUENCE_SIZE; // Always upload full 64-element arrays

    // Pitch values — padded to 64
    vector<float> pitchArr(sz, 60.0f);
    int seqSz = std::min((int)currentPitches.size(), sz);
    for(int i = 0; i < seqSz; i++) pitchArr[i] = currentPitches[i];
    pitchOut.set(vector<float>(currentPitches.begin(), currentPitches.begin() + seqSz));

    // Euclidean gate — padded to 64
    vector<float> eucGateArr(sz, 1.0f);
    int gateLen = (int)euclideanPattern.size();
    for(int i = 0; i < sz; i++)
        eucGateArr[i] = euclideanPattern[i % std::max(1, gateLen)] ? 1.0f : 0.0f;

    // Euclidean accent — padded to 64
    vector<float> eucAccArr(sz, 0.0f);
    int accLen = (int)euclideanAccents.size();
    for(int i = 0; i < sz; i++)
        eucAccArr[i] = euclideanAccents[i % std::max(1, accLen)] ? 1.0f : 0.0f;

    // Euclidean duration — padded to 64
    vector<float> eucDurArr(sz, 0.0f);
    int durLen = (int)euclideanDurations.size();
    for(int i = 0; i < sz; i++)
        eucDurArr[i] = euclideanDurations[i % std::max(1, durLen)] ? 1.0f : 0.0f;

    for(auto& pair : synthInstances) {
        if(!pair.second) continue;
        pair.second->set("pitchvalues", pitchArr);
        pair.second->set("eucgate",     eucGateArr);
        pair.second->set("eucaccent",   eucAccArr);
        pair.second->set("eucdur",      eucDurArr);
    }

    // Update active pitch visualisation output
    int poly = polyphony.get();
    int polyInt = polyInterval.get();
    int currentSeqSize = seqSize.get();
    vector<float> active(poly, 60.0f);
    for(int i = 0; i < poly; i++) {
        int voiceIdx = (i * polyInt) % std::max(1, currentSeqSize);
        if(voiceIdx < (int)currentPitches.size())
            active[i] = currentPitches[voiceIdx];
    }
    activePitchOut.set(active);
}

void scPolyphonicArpeggiator::uploadScalarParamsToSynths() {
    if(synthInstances.empty()) return;

    for(auto& pair : synthInstances) {
        if(!pair.second) continue;
        auto s = pair.second;
        s->set("seqsize",      (float)seqSize.get());
        s->set("euclen",       (float)eucLen.get());
        s->set("eucacclen",    (float)eucAccLen.get());
        s->set("eucdurlen",    (float)eucDurLen.get());
        s->set("polyphony",    (float)polyphony.get());
        s->set("polyinterval", (float)polyInterval.get());
        s->set("skipsteps",    (float)skipSteps.get());
        s->set("velbase",      velBase.get());
        s->set("velrndm",      velRndm.get());
        s->set("accstr",       eucAccStrength.get());
        s->set("durbase",      (float)durBase.get());
        s->set("durrndm",      (float)durRndm.get());
        s->set("dureucstr",    (float)durEucStrength.get());
        s->set("stepchance",   stepChance.get());
        s->set("notechance",   noteChance.get());
        s->set("strum",        strum.get());
        s->set("strumrndm",    strumRndm.get());
        s->set("strumdir",     (float)strumDir.get());
        s->set("seed",         (float)seed.get());
    }
}

// ═══════════════════════════════════════════════════════════
// PITCH SEQUENCE REBUILD (identical logic to polyphonicArpeggiator)
// ═══════════════════════════════════════════════════════════

void scPolyphonicArpeggiator::rebuildExpandedScale() {
    expandedScale.clear();
    if(scale->empty()) { expandedScale.push_back(60.0f); return; }
    for(int octave = -2; octave <= 8; octave++) {
        for(float note : scale.get()) {
            float expandedNote = note + (octave * 12);
            if(expandedNote >= 0 && expandedNote <= 127)
                expandedScale.push_back(expandedNote);
        }
    }
    std::sort(expandedScale.begin(), expandedScale.end());
}

float scPolyphonicArpeggiator::getScaleDegree(int index) {
    if(expandedScale.empty()) {
        rebuildExpandedScale();
        if(expandedScale.empty()) return 60.0f;
    }
    int sz = (int)expandedScale.size();
    int wrappedIndex = index % sz;
    if(wrappedIndex < 0) wrappedIndex += sz;
    return expandedScale[wrappedIndex];
}

void scPolyphonicArpeggiator::rebuildDeviations() {
    int sz = seqSize.get();
    if(sz <= 0) return;
    deviationValues.resize(sz, 0.0f);
    for(int i = 0; i < sz; i++) {
        float deviation = 0.0f;
        int noteIndex = degStart.get() + (i * stepInterval.get());

        if(octaveDev.get() > 0 && dist01(rng) < octaveDev.get()) {
            if(octaveDevRng.get() > 0) {
                std::uniform_int_distribution<int> octDist(1, octaveDevRng.get());
                deviation += octDist(rng) * 12;
            }
        }
        if(idxDev.get() > 0 && dist01(rng) < idxDev.get()) {
            if(idxDevRng.get() > 0) {
                std::uniform_int_distribution<int> idxDist(1, idxDevRng.get());
                int shift = idxDist(rng);
                float basePitch  = getScaleDegree(noteIndex);
                float shiftedPitch = getScaleDegree(noteIndex + shift);
                deviation += (shiftedPitch - basePitch);
            }
        }
        if(pitchDev.get() > 0 && dist01(rng) < pitchDev.get()) {
            if(pitchDevRng.get() > 0) {
                std::uniform_int_distribution<int> chromDist(1, pitchDevRng.get());
                deviation += chromDist(rng);
            }
        }
        deviationValues[i] = deviation;
    }
}

void scPolyphonicArpeggiator::rebuildPitchSequence() {
    int sz = seqSize.get();
    if(sz <= 0) return;
    currentPitches.resize(sz, 60.0f);

    int stepInt    = stepInterval.get();
    int degreeStart = degStart.get();
    int transp     = transpose.get();
    int mode       = patternMode.get();

    vector<int> pattern;
    if(mode == 0) {
        for(int i = 0; i < sz; i++) pattern.push_back(i);
    } else if(mode == 1) {
        for(int i = sz - 1; i >= 0; i--) pattern.push_back(i);
    } else if(mode == 2) {
        for(int i = 0; i < sz; i++) {
            std::uniform_int_distribution<int> randDist(0, sz - 1);
            pattern.push_back(randDist(rng));
        }
    } else {
        pattern = idxPattern.get();
        if(pattern.empty()) pattern = {0};
    }

    for(int i = 0; i < sz; i++) {
        int patternIdx = i % (int)pattern.size();
        int seqIdx     = pattern[patternIdx] % sz;
        int noteIndex  = degreeStart + (seqIdx * stepInt);
        float pitch    = getScaleDegree(noteIndex);
        if(i < (int)deviationValues.size()) pitch += deviationValues[i];
        pitch += transp;
        pitch = ofClamp(pitch, 0.0f, 127.0f);
        currentPitches[i] = pitch;
    }

    pitchOut.set(vector<float>(currentPitches.begin(), currentPitches.end()));
}

// ═══════════════════════════════════════════════════════════
// EUCLIDEAN PATTERN GENERATION
// ═══════════════════════════════════════════════════════════

void scPolyphonicArpeggiator::generateEuclideanPattern(vector<bool>& pattern, int length, int hits, int offset) {
    pattern.clear();
    pattern.resize(length, false);
    if(hits <= 0 || length <= 0) return;
    if(hits > length) hits = length;
    for(int j = 0; j < hits; j++) {
        int index = ((j * length) / hits + offset) % length;
        if(index < 0) index += length;
        pattern[index] = true;
    }
}

// ═══════════════════════════════════════════════════════════
// PRESET SAVE / LOAD
// ═══════════════════════════════════════════════════════════

void scPolyphonicArpeggiator::presetSave(ofJson &json) {
    json["activeSnapshotSlot"] = activeSnapshotSlot;
}

void scPolyphonicArpeggiator::presetRecallAfterSettingParameters(ofJson &json) {
    if(json.contains("activeSnapshotSlot")) activeSnapshotSlot = json["activeSnapshotSlot"];

    generateEuclideanPattern(euclideanPattern,   eucLen,    eucHits,    eucOff);
    generateEuclideanPattern(euclideanAccents,   eucAccLen, eucAccHits, eucAccOff);
    generateEuclideanPattern(euclideanDurations, eucDurLen, eucDurHits, eucDurOff);

    int sz = seqSize;
    currentPitches.resize(sz, 60.0f);
    deviationValues.resize(sz, 0.0f);

    rebuildExpandedScale();
    rebuildDeviations();
    rebuildPitchSequence();
    uploadArraysToSynths();
    uploadScalarParamsToSynths();
}

// ═══════════════════════════════════════════════════════════
// SNAPSHOT SYSTEM
// ═══════════════════════════════════════════════════════════

string scPolyphonicArpeggiator::getSnapshotsFolderPath() {
    return ofToDataPath("nodeSnapshots/ScPolyphonicArpeggiator/", true);
}

string scPolyphonicArpeggiator::getSnapshotFilePath(int slot) {
    return getSnapshotsFolderPath() + "snapshot_" + ofToString(slot) + ".json";
}

void scPolyphonicArpeggiator::saveSnapshotToDisk(int slot) {
    if(slot < 0 || slot >= 16) return;
    if(!snapshotSlots[slot].hasData) return;

    ofDirectory dir(getSnapshotsFolderPath());
    if(!dir.exists()) dir.create(true);

    ScArpeggiatorSnapshot& snap = snapshotSlots[slot];
    ofJson json;

    json["seqSize"] = snap.seqSize;
    json["scale"] = snap.scale;
    json["patternMode"] = snap.patternMode;
    json["idxPattern"] = snap.idxPattern;
    json["degStart"] = snap.degStart;
    json["stepInterval"] = snap.stepInterval;
    json["transpose"] = snap.transpose;
    json["polyphony"] = snap.polyphony;
    json["polyInterval"] = snap.polyInterval;
    json["skipSteps"] = snap.skipSteps;
    json["strum"] = snap.strum;
    json["strumRndm"] = snap.strumRndm;
    json["strumDir"] = snap.strumDir;
    json["octaveDev"] = snap.octaveDev;
    json["octaveDevRng"] = snap.octaveDevRng;
    json["idxDev"] = snap.idxDev;
    json["idxDevRng"] = snap.idxDevRng;
    json["pitchDev"] = snap.pitchDev;
    json["pitchDevRng"] = snap.pitchDevRng;
    json["velBase"] = snap.velBase;
    json["velRndm"] = snap.velRndm;
    json["eucAccStrength"] = snap.eucAccStrength;
    json["durBase"] = snap.durBase;
    json["durRndm"] = snap.durRndm;
    json["durEucStrength"] = snap.durEucStrength;
    json["eucLen"] = snap.eucLen;
    json["eucHits"] = snap.eucHits;
    json["eucOff"] = snap.eucOff;
    json["eucAccLen"] = snap.eucAccLen;
    json["eucAccHits"] = snap.eucAccHits;
    json["eucAccOff"] = snap.eucAccOff;
    json["eucDurLen"] = snap.eucDurLen;
    json["eucDurHits"] = snap.eucDurHits;
    json["eucDurOff"] = snap.eucDurOff;
    json["stepChance"] = snap.stepChance;
    json["noteChance"] = snap.noteChance;

    ofSavePrettyJson(getSnapshotFilePath(slot), json);
}

void scPolyphonicArpeggiator::loadSnapshotFromDisk(int slot) {
    if(slot < 0 || slot >= 16) return;
    ofFile file(getSnapshotFilePath(slot));
    if(!file.exists()) return;
    ofJson json = ofLoadJson(getSnapshotFilePath(slot));
    if(json.empty()) return;

    ScArpeggiatorSnapshot snap;
    snap.seqSize = json.value("seqSize", 16);
    snap.scale = json.value("scale", vector<float>{0, 2, 4, 5, 7, 9, 11});
    snap.patternMode = json.value("patternMode", 0);
    snap.idxPattern = json.value("idxPattern", vector<int>{0, 1, 2, 3});
    snap.degStart = json.value("degStart", 0);
    snap.stepInterval = json.value("stepInterval", 1);
    snap.transpose = json.value("transpose", 0);
    snap.polyphony = json.value("polyphony", 1);
    snap.polyInterval = json.value("polyInterval", 2);
    snap.skipSteps = json.value("skipSteps", 0);
    snap.strum = json.value("strum", 0.0f);
    snap.strumRndm = json.value("strumRndm", 0.0f);
    snap.strumDir = json.value("strumDir", 0);
    snap.octaveDev = json.value("octaveDev", 0.0f);
    snap.octaveDevRng = json.value("octaveDevRng", 1);
    snap.idxDev = json.value("idxDev", 0.0f);
    snap.idxDevRng = json.value("idxDevRng", 2);
    snap.pitchDev = json.value("pitchDev", 0.0f);
    snap.pitchDevRng = json.value("pitchDevRng", 2);
    snap.velBase = json.value("velBase", 0.8f);
    snap.velRndm = json.value("velRndm", 0.1f);
    snap.eucAccStrength = json.value("eucAccStrength", 0.2f);
    snap.durBase = json.value("durBase", 100);
    snap.durRndm = json.value("durRndm", 20);
    snap.durEucStrength = json.value("durEucStrength", 50);
    snap.eucLen = json.value("eucLen", 8);
    snap.eucHits = json.value("eucHits", 8);
    snap.eucOff = json.value("eucOff", 0);
    snap.eucAccLen = json.value("eucAccLen", 4);
    snap.eucAccHits = json.value("eucAccHits", 1);
    snap.eucAccOff = json.value("eucAccOff", 0);
    snap.eucDurLen = json.value("eucDurLen", 4);
    snap.eucDurHits = json.value("eucDurHits", 4);
    snap.eucDurOff = json.value("eucDurOff", 0);
    snap.stepChance = json.value("stepChance", 1.0f);
    snap.noteChance = json.value("noteChance", 1.0f);
    snap.hasData = true;
    snapshotSlots[slot] = snap;
}

void scPolyphonicArpeggiator::loadAllSnapshotsFromDisk() {
    for(int i = 0; i < 16; i++) loadSnapshotFromDisk(i);
}

void scPolyphonicArpeggiator::deleteSnapshotFromDisk(int slot) {
    if(slot < 0 || slot >= 16) return;
    ofFile file(getSnapshotFilePath(slot));
    if(file.exists()) file.remove();
    snapshotSlots[slot] = ScArpeggiatorSnapshot();
    snapshotSlots[slot].hasData = false;
    if(activeSnapshotSlot == slot) activeSnapshotSlot = -1;
}

void scPolyphonicArpeggiator::storeToSlot(int slot) {
    if(slot < 0 || slot >= 16) return;

    ScArpeggiatorSnapshot snap;
    snap.seqSize = seqSize.get();
    snap.scale = scale.get();
    snap.patternMode = patternMode.get();
    snap.idxPattern = idxPattern.get();
    snap.degStart = degStart.get();
    snap.stepInterval = stepInterval.get();
    snap.transpose = transpose.get();
    snap.polyphony = polyphony.get();
    snap.polyInterval = polyInterval.get();
    snap.skipSteps = skipSteps.get();
    snap.strum = strum.get();
    snap.strumRndm = strumRndm.get();
    snap.strumDir = strumDir.get();
    snap.octaveDev = octaveDev.get();
    snap.octaveDevRng = octaveDevRng.get();
    snap.idxDev = idxDev.get();
    snap.idxDevRng = idxDevRng.get();
    snap.pitchDev = pitchDev.get();
    snap.pitchDevRng = pitchDevRng.get();
    snap.velBase = velBase.get();
    snap.velRndm = velRndm.get();
    snap.eucAccStrength = eucAccStrength.get();
    snap.durBase = durBase.get();
    snap.durRndm = durRndm.get();
    snap.durEucStrength = durEucStrength.get();
    snap.eucLen = eucLen.get();
    snap.eucHits = eucHits.get();
    snap.eucOff = eucOff.get();
    snap.eucAccLen = eucAccLen.get();
    snap.eucAccHits = eucAccHits.get();
    snap.eucAccOff = eucAccOff.get();
    snap.eucDurLen = eucDurLen.get();
    snap.eucDurHits = eucDurHits.get();
    snap.eucDurOff = eucDurOff.get();
    snap.stepChance = stepChance.get();
    snap.noteChance = noteChance.get();
    snap.hasData = true;

    snapshotSlots[slot] = snap;
    activeSnapshotSlot = slot;
    saveSnapshotToDisk(slot);
}

void scPolyphonicArpeggiator::recallSlot(int slot) {
    if(slot < 0 || slot >= 16) return;
    if(!snapshotSlots[slot].hasData) return;

    activeSnapshotSlot = slot;

    if(morphTime.get() <= 0.001f) {
        ScArpeggiatorSnapshot snap = snapshotSlots[slot];

        seqSize.set(snap.seqSize);
        scale.set(snap.scale);
        patternMode.set(snap.patternMode);
        idxPattern.set(snap.idxPattern);
        degStart.set(snap.degStart);
        stepInterval.set(snap.stepInterval);
        transpose.set(snap.transpose);
        polyphony.set(snap.polyphony);
        polyInterval.set(snap.polyInterval);
        skipSteps.set(snap.skipSteps);
        strum.set(snap.strum);
        strumRndm.set(snap.strumRndm);
        strumDir.set(snap.strumDir);
        octaveDev.set(snap.octaveDev);
        octaveDevRng.set(snap.octaveDevRng);
        idxDev.set(snap.idxDev);
        idxDevRng.set(snap.idxDevRng);
        pitchDev.set(snap.pitchDev);
        pitchDevRng.set(snap.pitchDevRng);
        velBase.set(snap.velBase);
        velRndm.set(snap.velRndm);
        eucAccStrength.set(snap.eucAccStrength);
        durBase.set(snap.durBase);
        durRndm.set(snap.durRndm);
        durEucStrength.set(snap.durEucStrength);
        eucLen.set(snap.eucLen);
        eucHits.set(snap.eucHits);
        eucOff.set(snap.eucOff);
        eucAccLen.set(snap.eucAccLen);
        eucAccHits.set(snap.eucAccHits);
        eucAccOff.set(snap.eucAccOff);
        eucDurLen.set(snap.eucDurLen);
        eucDurHits.set(snap.eucDurHits);
        eucDurOff.set(snap.eucDurOff);
        stepChance.set(snap.stepChance);
        noteChance.set(snap.noteChance);
    } else {
        // Capture start state
        startSnapshot.seqSize = seqSize.get();
        startSnapshot.scale = scale.get();
        startSnapshot.patternMode = patternMode.get();
        startSnapshot.idxPattern = idxPattern.get();
        startSnapshot.degStart = degStart.get();
        startSnapshot.stepInterval = stepInterval.get();
        startSnapshot.transpose = transpose.get();
        startSnapshot.polyphony = polyphony.get();
        startSnapshot.polyInterval = polyInterval.get();
        startSnapshot.skipSteps = skipSteps.get();
        startSnapshot.strum = strum.get();
        startSnapshot.strumRndm = strumRndm.get();
        startSnapshot.strumDir = strumDir.get();
        startSnapshot.octaveDev = octaveDev.get();
        startSnapshot.octaveDevRng = octaveDevRng.get();
        startSnapshot.idxDev = idxDev.get();
        startSnapshot.idxDevRng = idxDevRng.get();
        startSnapshot.pitchDev = pitchDev.get();
        startSnapshot.pitchDevRng = pitchDevRng.get();
        startSnapshot.velBase = velBase.get();
        startSnapshot.velRndm = velRndm.get();
        startSnapshot.eucAccStrength = eucAccStrength.get();
        startSnapshot.durBase = durBase.get();
        startSnapshot.durRndm = durRndm.get();
        startSnapshot.durEucStrength = durEucStrength.get();
        startSnapshot.eucLen = eucLen.get();
        startSnapshot.eucHits = eucHits.get();
        startSnapshot.eucOff = eucOff.get();
        startSnapshot.eucAccLen = eucAccLen.get();
        startSnapshot.eucAccHits = eucAccHits.get();
        startSnapshot.eucAccOff = eucAccOff.get();
        startSnapshot.eucDurLen = eucDurLen.get();
        startSnapshot.eucDurHits = eucDurHits.get();
        startSnapshot.eucDurOff = eucDurOff.get();
        startSnapshot.stepChance = stepChance.get();
        startSnapshot.noteChance = noteChance.get();

        targetSnapshot = snapshotSlots[slot];
        morphStartTime = ofGetElapsedTimef();
        isMorphing = true;
    }
}

void scPolyphonicArpeggiator::updateMorph() {
    float now = ofGetElapsedTimef();
    float progress = (now - morphStartTime) / std::max(morphTime.get(), 0.001f);
    if(progress >= 1.0f) { progress = 1.0f; isMorphing = false; }

    // Lerp scalar values
    seqSize.set((int)ofLerp(startSnapshot.seqSize,        targetSnapshot.seqSize,        progress));
    transpose.set((int)ofLerp(startSnapshot.transpose,    targetSnapshot.transpose,      progress));
    degStart.set((int)ofLerp(startSnapshot.degStart,      targetSnapshot.degStart,       progress));
    stepInterval.set((int)ofLerp(startSnapshot.stepInterval, targetSnapshot.stepInterval, progress));
    polyphony.set((int)ofLerp(startSnapshot.polyphony,    targetSnapshot.polyphony,      progress));
    polyInterval.set((int)ofLerp(startSnapshot.polyInterval, targetSnapshot.polyInterval, progress));
    skipSteps.set((int)ofLerp(startSnapshot.skipSteps,    targetSnapshot.skipSteps,      progress));
    strum.set(ofLerp(startSnapshot.strum,                 targetSnapshot.strum,          progress));
    strumRndm.set(ofLerp(startSnapshot.strumRndm,         targetSnapshot.strumRndm,      progress));
    octaveDev.set(ofLerp(startSnapshot.octaveDev,         targetSnapshot.octaveDev,      progress));
    octaveDevRng.set((int)ofLerp(startSnapshot.octaveDevRng, targetSnapshot.octaveDevRng, progress));
    idxDev.set(ofLerp(startSnapshot.idxDev,               targetSnapshot.idxDev,         progress));
    idxDevRng.set((int)ofLerp(startSnapshot.idxDevRng,    targetSnapshot.idxDevRng,      progress));
    pitchDev.set(ofLerp(startSnapshot.pitchDev,           targetSnapshot.pitchDev,       progress));
    pitchDevRng.set((int)ofLerp(startSnapshot.pitchDevRng, targetSnapshot.pitchDevRng,   progress));
    velBase.set(ofLerp(startSnapshot.velBase,             targetSnapshot.velBase,        progress));
    velRndm.set(ofLerp(startSnapshot.velRndm,             targetSnapshot.velRndm,        progress));
    eucAccStrength.set(ofLerp(startSnapshot.eucAccStrength, targetSnapshot.eucAccStrength, progress));
    durBase.set((int)ofLerp(startSnapshot.durBase,        targetSnapshot.durBase,        progress));
    durRndm.set((int)ofLerp(startSnapshot.durRndm,        targetSnapshot.durRndm,        progress));
    durEucStrength.set((int)ofLerp(startSnapshot.durEucStrength, targetSnapshot.durEucStrength, progress));
    eucLen.set((int)ofLerp(startSnapshot.eucLen,          targetSnapshot.eucLen,         progress));
    eucHits.set((int)ofLerp(startSnapshot.eucHits,        targetSnapshot.eucHits,        progress));
    eucOff.set((int)ofLerp(startSnapshot.eucOff,          targetSnapshot.eucOff,         progress));
    eucAccLen.set((int)ofLerp(startSnapshot.eucAccLen,    targetSnapshot.eucAccLen,      progress));
    eucAccHits.set((int)ofLerp(startSnapshot.eucAccHits,  targetSnapshot.eucAccHits,     progress));
    eucAccOff.set((int)ofLerp(startSnapshot.eucAccOff,    targetSnapshot.eucAccOff,      progress));
    eucDurLen.set((int)ofLerp(startSnapshot.eucDurLen,    targetSnapshot.eucDurLen,      progress));
    eucDurHits.set((int)ofLerp(startSnapshot.eucDurHits,  targetSnapshot.eucDurHits,     progress));
    eucDurOff.set((int)ofLerp(startSnapshot.eucDurOff,    targetSnapshot.eucDurOff,      progress));
    stepChance.set(ofLerp(startSnapshot.stepChance,       targetSnapshot.stepChance,     progress));
    noteChance.set(ofLerp(startSnapshot.noteChance,       targetSnapshot.noteChance,     progress));

    // Discrete values only at the end
    if(progress >= 1.0f) {
        scale.set(targetSnapshot.scale);
        patternMode.set(targetSnapshot.patternMode);
        idxPattern.set(targetSnapshot.idxPattern);
        strumDir.set(targetSnapshot.strumDir);
    }
}

// ═══════════════════════════════════════════════════════════
// GUI: PATTERN DISPLAY
// ═══════════════════════════════════════════════════════════

void scPolyphonicArpeggiator::drawPatternDisplay() {
    float zoom = ofxOceanodeShared::getZoomLevel();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float width  = guiWidth.get() * zoom;
    float height = patternHeight.get() * zoom;

    ImGui::InvisibleButton("##scpattern", ImVec2(width, height));

    drawList->AddRectFilled(p, ImVec2(p.x + width, p.y + height), IM_COL32(30, 30, 30, 255));
    drawList->AddRect(p, ImVec2(p.x + width, p.y + height), IM_COL32(80, 80, 80, 255));

    int sz = seqSize;
    if(sz <= 0 || currentPitches.empty()) return;
    float stepWidth = width / sz;

    float minPitch = 127.0f, maxPitch = 0.0f;
    for(int i = 0; i < sz && i < (int)currentPitches.size(); i++) {
        minPitch = std::min(minPitch, currentPitches[i]);
        maxPitch = std::max(maxPitch, currentPitches[i]);
    }
    if(maxPitch <= minPitch) { minPitch = 48; maxPitch = 84; }
    float pitchRange = std::max(12.0f, maxPitch - minPitch);

    int poly    = polyphony.get();
    int polyInt = polyInterval.get();

    for(int i = 0; i < sz; i++) {
        float x = p.x + i * stepWidth;
        if(i > 0) drawList->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + height), IM_COL32(50, 50, 55, 255));

        if(i < (int)currentPitches.size()) {
            float normalized = (currentPitches[i] - minPitch) / pitchRange;
            normalized = ofClamp(normalized, 0.0f, 1.0f);
            float barH = normalized * height * 0.8f;
            float barY = p.y + height - barH - height * 0.05f;

            // Highlight active voice slots
            bool isActiveVoice = false;
            for(int v = 0; v < poly; v++) {
                if((v * polyInt) % std::max(1, sz) == i) { isActiveVoice = true; break; }
            }

            ImU32 barColor = isActiveVoice
                ? IM_COL32(100, 220, 180, 255)
                : IM_COL32(60, 100, 80, 140);

            drawList->AddRectFilled(ImVec2(x + 1, barY),
                                   ImVec2(x + stepWidth - 1, p.y + height - height * 0.05f),
                                   barColor);
        }

        if(i % 4 == 0) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", i);
            drawList->AddText(ImVec2(x + 2.0f * zoom, p.y + 2.0f * zoom), IM_COL32(140, 140, 140, 200), buf);
        }
    }

    char info[80];
    snprintf(info, sizeof(info), "Poly %d | PolyInt %d | Trsp %d", poly, polyInt, (int)transpose);
    ImVec2 infoSize = ImGui::CalcTextSize(info);
    drawList->AddText(ImVec2(p.x + width - infoSize.x - 4.0f * zoom, p.y + height - infoSize.y - 2.0f * zoom),
                     IM_COL32(160, 160, 170, 200), info);
}

// ═══════════════════════════════════════════════════════════
// GUI: EUCLIDEAN DISPLAY
// ═══════════════════════════════════════════════════════════

void scPolyphonicArpeggiator::drawEuclideanDisplay() {
    float zoom = ofxOceanodeShared::getZoomLevel();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float width  = guiWidth.get() * zoom;
    float height = euclideanHeight.get() * zoom;

    ImGui::InvisibleButton("##sceuclidean", ImVec2(width, height));

    drawList->AddRectFilled(p, ImVec2(p.x + width, p.y + height), IM_COL32(30, 30, 30, 255));
    drawList->AddRect(p, ImVec2(p.x + width, p.y + height), IM_COL32(80, 80, 80, 255));

    float rowHeight = height / 3.0f;

    // Row 1: Gate euclidean
    {
        int len = std::max(1, (int)eucLen);
        float stepW = width / len;
        for(int i = 0; i < len && i < (int)euclideanPattern.size(); i++) {
            float x = p.x + i * stepW;
            if(euclideanPattern[i]) {
                drawList->AddRectFilled(ImVec2(x + 1.0f * zoom, p.y + 2.0f * zoom),
                    ImVec2(x + stepW - 1.0f * zoom, p.y + rowHeight - 2.0f * zoom), IM_COL32(200, 100, 100, 255));
            }
        }
        drawList->AddText(ImVec2(p.x + 2.0f * zoom, p.y + 2.0f * zoom), IM_COL32(255, 255, 255, 180), "Gates");
    }

    // Row 2: Accent euclidean
    {
        float rowY = p.y + rowHeight;
        int len = std::max(1, (int)eucAccLen);
        float stepW = width / len;
        for(int i = 0; i < len && i < (int)euclideanAccents.size(); i++) {
            float x = p.x + i * stepW;
            if(euclideanAccents[i]) {
                drawList->AddRectFilled(ImVec2(x + 1.0f * zoom, rowY + 2.0f * zoom),
                    ImVec2(x + stepW - 1.0f * zoom, rowY + rowHeight - 2.0f * zoom), IM_COL32(100, 200, 100, 255));
            }
        }
        drawList->AddText(ImVec2(p.x + 2.0f * zoom, rowY + 2.0f * zoom), IM_COL32(255, 255, 255, 180), "Accents");
    }

    // Row 3: Duration euclidean
    {
        float rowY = p.y + 2 * rowHeight;
        int len = std::max(1, (int)eucDurLen);
        float stepW = width / len;
        for(int i = 0; i < len && i < (int)euclideanDurations.size(); i++) {
            float x = p.x + i * stepW;
            if(euclideanDurations[i]) {
                drawList->AddRectFilled(ImVec2(x + 1.0f * zoom, rowY + 2.0f * zoom),
                    ImVec2(x + stepW - 1.0f * zoom, rowY + rowHeight - 2.0f * zoom), IM_COL32(100, 100, 200, 255));
            }
        }
        drawList->AddText(ImVec2(p.x + 2.0f * zoom, rowY + 2.0f * zoom), IM_COL32(255, 255, 255, 180), "Duration");
    }
}

// ═══════════════════════════════════════════════════════════
// GUI: SNAPSHOT SLOTS
// ═══════════════════════════════════════════════════════════

void scPolyphonicArpeggiator::drawSnapshotSlots() {
    float zoom = ofxOceanodeShared::getZoomLevel();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float width = guiWidth.get() * zoom;

    float slotSize = width / 8.0f;
    float height   = slotSize * 2.0f;

    ImGui::InvisibleButton("##ScSnapshots", ImVec2(width, height));
    bool isActive  = ImGui::IsItemActive();
    ImVec2 mouse   = ImGui::GetIO().MousePos;
    bool leftClick = ImGui::IsMouseClicked(0);
    bool rightClick = ImGui::IsMouseClicked(1);
    bool shift     = ImGui::GetIO().KeyShift;

    drawList->AddRectFilled(p, ImVec2(p.x + width, p.y + height), IM_COL32(25, 25, 25, 255));
    drawList->AddRect(p, ImVec2(p.x + width, p.y + height), IM_COL32(80, 80, 80, 255));

    for(int i = 0; i < 16; i++) {
        int row    = i / 8;
        int column = i % 8;
        ImVec2 slotPos = ImVec2(p.x + column * slotSize, p.y + row * slotSize);
        ImVec2 slotMax = ImVec2(slotPos.x + slotSize - 2.0f * zoom, slotPos.y + slotSize - 2.0f * zoom);

        bool hasData = snapshotSlots[i].hasData;
        bool hovered = (mouse.x >= slotPos.x && mouse.x < slotMax.x &&
                       mouse.y >= slotPos.y && mouse.y < slotMax.y);

        if(hovered && isActive) {
            if(leftClick) {
                if(shift) storeToSlot(i);
                else recallSlot(i);
            } else if(rightClick && hasData) {
                deleteSnapshotFromDisk(i);
            }
        }

        ImU32 slotColor;
        if(i == activeSnapshotSlot)       slotColor = IM_COL32(180, 220, 255, 255);
        else if(hasData)                   slotColor = IM_COL32(100, 150, 180, 255);
        else                               slotColor = IM_COL32(50, 50, 50, 255);

        if(hovered) {
            int r = (int)(slotColor & 0xFF) + 30;
            int g = (int)((slotColor >> 8) & 0xFF) + 30;
            int b = (int)((slotColor >> 16) & 0xFF) + 30;
            slotColor = IM_COL32(std::min(r, 255), std::min(g, 255), std::min(b, 255), 255);
        }

        drawList->AddRectFilled(slotPos, slotMax, slotColor);
        drawList->AddRect(slotPos, slotMax, IM_COL32(100, 100, 100, 200));

        char buf[8];
        sprintf(buf, "%d", i + 1);
        drawList->AddText(ImVec2(slotPos.x + 3.0f * zoom, slotPos.y + 3.0f * zoom), IM_COL32(255, 255, 255, 200), buf);

        if(shift && hovered) {
            drawList->AddText(ImVec2(slotPos.x + slotSize - 15.0f * zoom, slotPos.y + slotSize - 15.0f * zoom),
                             IM_COL32(255, 100, 100, 255), "S");
        } else if(hovered && hasData) {
            drawList->AddText(ImVec2(slotPos.x + slotSize - 15.0f * zoom, slotPos.y + slotSize - 15.0f * zoom),
                             IM_COL32(255, 80, 80, 180), "X");
        }
    }
}
