//
//  fullStepSequencer.cpp
//  ofxOceanodeSuperCollider
//

#include "fullStepSequencer.h"
#include "ofxSuperCollider.h"
#include <algorithm>
#include <cmath>

// ════════════════════════════════════════════════════════════════════════════
// Constructor / Destructor
// ════════════════════════════════════════════════════════════════════════════

fullStepSequencer::fullStepSequencer(vector<serverManager*> servers)
    : scNode("FullStepSequencer"), allServers(servers)
{
    memset(nameEditBuf, 0, sizeof(nameEditBuf));

    // Cache the first available server for preview playback
    for(auto* sm : allServers) {
        if(sm && sm->getServer()) {
            previewServer = sm->getServer();
            break;
        }
    }

    // Pre-allocate per-track storage (active tracks are 0..numTracks-1)
    trackBufs.resize(MAX_TRACKS);
    samplePaths.resize(MAX_TRACKS);
    waveformPeaks.resize(MAX_TRACKS);
    currentStep.assign(MAX_TRACKS, 0);
}

fullStepSequencer::~fullStepSequencer() {
    try {
        nodeListeners.unsubscribeAll();
        stopPreview();
        freeAllSamples();

        for(auto& [srv, synths] : trackSynths) {
            for(auto* s : synths) {
                if(s) { s->free(); delete s; }
            }
        }
        trackSynths.clear();

        for(auto& [srv, buses] : privateBuses) {
            for(auto* b : buses) { if(b) { b->free(); delete b; } }
        }
        privateBuses.clear();

        for(auto& [srv, buses] : stepBuses) {
            for(auto* b : buses) { if(b) { b->free(); delete b; } }
        }
        stepBuses.clear();

        for(auto& [srv, buses] : gateBuses) {
            for(auto* b : buses) { if(b) { b->free(); delete b; } }
        }
        gateBuses.clear();
    } catch(const std::exception& e) {
        ofLogError("fullStepSequencer") << "Destructor: " << e.what();
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Setup
// ════════════════════════════════════════════════════════════════════════════

void fullStepSequencer::setup() {
    // ── Node GUI parameters (with separators for visual grouping) ───────────
    addSeparator("Sequencer");
    addParameter(showWindow.set("Show",  false));
    addParameter(resetSeq.set("Reset", 0, 0, 1));
    addParameter(numTracksP.set("Tracks", 1, 1, MAX_TRACKS));
    addParameter(currentSlotP.set("Slot", 0, 0, MAX_SLOTS - 1));
    addParameter(embedInProject.set("Embed", false));

    addSeparator("Volume");
    addParameter(globalVolP .set("Vol",        {1.0f},  {0.0f},   {1.0f}));

    addSeparator("Probability");
    addParameter(globalProbP.set("Prob",       {1.0f},  {0.0f},   {1.0f}));

    addSeparator("Mute");
    addParameter(muteP.set("Mute",
        vector<int>(MAX_TRACKS, 0),
        vector<int>(MAX_TRACKS, 0),
        vector<int>(MAX_TRACKS, 1)));

    addSeparator("Swing");
    addParameter(swingP.set("Swing", 0.0f, 0.0f, 0.5f));

    addSeparator("Transpose");
    addParameter(transposeP .set("Transpose", {0.0f},  {-24.0f}, {24.0f}));

    addSeparator("Output");
    addOutputParameter(gateOut.set("Gate", 0, 0, 1));

    // ── Initial output port for track 0 ──────────────────────────────────────
    scNode::addOutput("Out 1");

    // ── Slot / track data initialisation ─────────────────────────────────────
    initSlots();

    // Sync ImGui name edit buffer with initial track config name
    snprintf(nameEditBuf[0], 64, "%s", trackConfigs[0].name.c_str());

    // ── File browser default location ─────────────────────────────────────────
    browseDir = ofToDataPath("Supercollider/Samples", true);
    if(!std::filesystem::exists(browseDir))
        browseDir = ofFilePath::getUserHomeDir();
    refreshBrowse(browseDir);

    // ── Parameter listeners ───────────────────────────────────────────────────
    nodeListeners.push(resetSeq.newListener([this](int& v) {
        // Mirror the resetSeq value (0 or 1) to the SC 'reset' arg on every change.
        // The SynthDef uses HPZ1.kr(reset) to detect the 0→1 rising edge and fires
        // a one-shot resetTrig that immediately advances the Stepper to step 0.
        // Sending both the rising (1) and falling (0) edges keeps HPZ1 aligned for
        // future resets without requiring synth recreation.
        lastResetVal = v;
        for(auto& [srv, synths] : trackSynths)
            for(auto* s : synths) if(s) s->set("reset", (float)v);
    }));

    nodeListeners.push(transposeP.newListener([this](vector<float>& v) {
        for(int ti = 0; ti < numTracks && ti < (int)v.size(); ti++) {
            trackConfigs[ti].trackPitch = v[ti];
            for(auto& [srv, synths] : trackSynths)
                if(ti < (int)synths.size() && synths[ti])
                    synths[ti]->set("globalPitch", v[ti]);
        }
    }));
    nodeListeners.push(globalVolP.newListener([this](vector<float>& v) {
        for(int ti = 0; ti < numTracks && ti < (int)v.size(); ti++) {
            trackConfigs[ti].globalVol = v[ti];
            for(auto& [srv, synths] : trackSynths)
                if(ti < (int)synths.size() && synths[ti])
                    synths[ti]->set("globalVol", v[ti]);
        }
    }));
    nodeListeners.push(globalProbP.newListener([this](vector<float>& v) {
        for(int ti = 0; ti < numTracks && ti < (int)v.size(); ti++) {
            trackConfigs[ti].globalProb = v[ti];
            for(auto& [srv, synths] : trackSynths)
                if(ti < (int)synths.size() && synths[ti])
                    synths[ti]->set("globalProb", v[ti]);
        }
    }));
    nodeListeners.push(muteP.newListener([this](vector<int>& v) {
        for(int ti = 0; ti < numTracks && ti < (int)v.size(); ti++) {
            trackConfigs[ti].muted = (v[ti] != 0);
            float activeVal = (getActive() && !trackConfigs[ti].muted) ? 1.0f : 0.0f;
            for(auto& [srv, synths] : trackSynths)
                if(ti < (int)synths.size() && synths[ti])
                    synths[ti]->set("active", activeVal);
        }
    }));
    nodeListeners.push(swingP.newListener([this](float& v) {
        for(auto& [srv, synths] : trackSynths)
            for(auto* s : synths) if(s) s->set("swing", v);
    }));

    nodeListeners.push(numTracksP.newListener([this](int& n) {
        setNumTracks(n);
    }));

    nodeListeners.push(currentSlotP.newListener([this](int& /*s*/) {
        reloadCurrentSlot();
    }));

    // ── Per-track step-data parameters with auto-send listeners (FM7Drone pattern) ─
    pStepOn     .resize(MAX_TRACKS);
    pStepVol    .resize(MAX_TRACKS);
    pStepProb   .resize(MAX_TRACKS);
    pStepPan    .resize(MAX_TRACKS);
    pStepCut    .resize(MAX_TRACKS);
    pStepRes    .resize(MAX_TRACKS);
    pStepPitch  .resize(MAX_TRACKS);
    pStepReverse.resize(MAX_TRACKS);

    for(int ti = 0; ti < MAX_TRACKS; ti++) {
        vector<float> zeros(MAX_STEPS, 0.0f);
        vector<float> ones (MAX_STEPS, 1.0f);
        vector<int>   izeros(MAX_STEPS, 0);
        pStepOn     [ti].set("stepOn_"     +ofToString(ti), zeros,  zeros,  ones );
        pStepVol    [ti].set("stepVol_"    +ofToString(ti), ones,   zeros,  ones );
        pStepProb   [ti].set("stepProb_"   +ofToString(ti), ones,   zeros,  ones );
        pStepPan    [ti].set("stepPan_"    +ofToString(ti), zeros,  vector<float>(MAX_STEPS,-1.f), ones);
        pStepCut    [ti].set("stepCut_"    +ofToString(ti), zeros,  vector<float>(MAX_STEPS,-1.f), ones);
        pStepRes    [ti].set("stepRes_"    +ofToString(ti), zeros,  zeros,  ones );
        pStepPitch  [ti].set("stepPitch_"  +ofToString(ti), izeros, vector<int>(MAX_STEPS,-12), vector<int>(MAX_STEPS,12));
        pStepReverse[ti].set("stepReverse_"+ofToString(ti), zeros,  zeros,  ones );

        // Listeners: fire synth->set() for every server's synth when value changes.
        // If synth->created==true  → ofxSCSynth::set() sends /n_setn immediately.
        // If synth->created==false → goes to vecArgs → resendStoredArgs on /n_go.
        nodeListeners.push(pStepOn[ti].newListener([this, ti](vector<float>& v){
            for(auto& [srv, synths] : trackSynths)
                if(ti < (int)synths.size() && synths[ti]) synths[ti]->set("stepOn", v);
        }));
        nodeListeners.push(pStepVol[ti].newListener([this, ti](vector<float>& v){
            for(auto& [srv, synths] : trackSynths)
                if(ti < (int)synths.size() && synths[ti]) synths[ti]->set("stepVol", v);
        }));
        nodeListeners.push(pStepProb[ti].newListener([this, ti](vector<float>& v){
            for(auto& [srv, synths] : trackSynths)
                if(ti < (int)synths.size() && synths[ti]) synths[ti]->set("stepProb", v);
        }));
        nodeListeners.push(pStepPan[ti].newListener([this, ti](vector<float>& v){
            for(auto& [srv, synths] : trackSynths)
                if(ti < (int)synths.size() && synths[ti]) synths[ti]->set("stepPan", v);
        }));
        nodeListeners.push(pStepCut[ti].newListener([this, ti](vector<float>& v){
            for(auto& [srv, synths] : trackSynths)
                if(ti < (int)synths.size() && synths[ti]) synths[ti]->set("stepCut", v);
        }));
        nodeListeners.push(pStepRes[ti].newListener([this, ti](vector<float>& v){
            for(auto& [srv, synths] : trackSynths)
                if(ti < (int)synths.size() && synths[ti]) synths[ti]->set("stepRes", v);
        }));
        nodeListeners.push(pStepPitch[ti].newListener([this, ti](vector<int>& v){
            vector<float> fv(v.begin(), v.end());
            for(auto& [srv, synths] : trackSynths)
                if(ti < (int)synths.size() && synths[ti]) synths[ti]->set("stepPitch", fv);
        }));
        nodeListeners.push(pStepReverse[ti].newListener([this, ti](vector<float>& v){
            for(auto& [srv, synths] : trackSynths)
                if(ti < (int)synths.size() && synths[ti]) synths[ti]->set("stepReverse", v);
        }));
    }
}

// ════════════════════════════════════════════════════════════════════════════
// update / draw
// ════════════════════════════════════════════════════════════════════════════

void fullStepSequencer::update(ofEventArgs& /*args*/) {
    // Poll KR buses on the primary server for playhead and gate readback.
    if(!stepBuses.empty()) {
        auto& busList = stepBuses.begin()->second;
        for(int ti = 0; ti < numTracks && ti < (int)busList.size(); ti++) {
            if(!busList[ti]) continue;
            if(!busList[ti]->readValues.empty())
                currentStep[ti] = (int)std::round(busList[ti]->readValues[0]);
            busList[ti]->requestValues();
        }
    }
    if(!gateBuses.empty()) {
        // Gate output reflects track 0 only
        auto& busList = gateBuses.begin()->second;
        if(!busList.empty() && busList[0]) {
            if(!busList[0]->readValues.empty())
                gateOut = (int)std::round(busList[0]->readValues[0]);
            busList[0]->requestValues();
        }
    }

}

void fullStepSequencer::draw(ofEventArgs& /*args*/) {
    if(showWindow) drawSequencerWindow();
}

// ════════════════════════════════════════════════════════════════════════════
// BPM / activate / deactivate
// ════════════════════════════════════════════════════════════════════════════

void fullStepSequencer::setBpm(float bpm) {
    currentBpm = bpm;
    sendBpmToAll();
}

void fullStepSequencer::activate() {
    for(auto& [srv, synths] : trackSynths)
        for(auto* s : synths) if(s) s->run(true);
}

void fullStepSequencer::deactivate() {
    for(auto& [srv, synths] : trackSynths)
        for(auto* s : synths) if(s) s->run(false);
}

// ════════════════════════════════════════════════════════════════════════════
// scNode interface
// ════════════════════════════════════════════════════════════════════════════

void fullStepSequencer::buildSynth(ofxSCServer* /*srv*/) {
    // Intentional no-op.
    //
    // The graph manager calls buildSynth → setOutputBus → createSynth.
    // We need setOutputBus to run BEFORE the synth is created so the correct
    // bus is baked into /s_new.  setOutputBus always writes to outputBuses[srv]
    // regardless of synth state, so createSynth can read it from there.
    // All allocation and spawning happens inside createSynth.
}

void fullStepSequencer::createSynth(ofxSCServer* srv) {
    if(!srv) return;

    // Full rebuild — pre-size all per-server vectors then delegate per track.
    freeServerSynths(srv);
    trackSynths [srv].assign(numTracks, nullptr);
    privateBuses[srv].assign(numTracks, nullptr);
    stepBuses   [srv].assign(numTracks, nullptr);
    gateBuses   [srv].assign(numTracks, nullptr);

    for(int ti = 0; ti < numTracks; ti++)
        createTrackSynth(srv, ti);
}

void fullStepSequencer::free(ofxSCServer* srv) {
    freeServerSynths(srv);
    outputBuses.erase(srv);
}

void fullStepSequencer::setOutputBus(ofxSCServer* srv, int idx, int bus) {
    // Free private placeholder bus for this track if it exists
    if(privateBuses.count(srv) && idx < (int)privateBuses[srv].size()
       && privateBuses[srv][idx]) {
        privateBuses[srv][idx]->free();
        delete privateBuses[srv][idx];
        privateBuses[srv][idx] = nullptr;
    }
    outputBuses[srv][idx] = bus;
    if(trackSynths.count(srv) && idx < (int)trackSynths[srv].size()) {
        if(trackSynths[srv][idx])
            trackSynths[srv][idx]->set("out", bus);
    }
}

int fullStepSequencer::getOutputBusIndex(ofxSCServer* srv, int idx) {
    if(outputBuses.count(srv) && outputBuses[srv].count(idx))
        return outputBuses[srv][idx];
    return -1;
}

void fullStepSequencer::moveSynthBefore(ofxSCServer* srv, int nodeID) {
    if(!trackSynths.count(srv)) return;
    auto& synths = trackSynths[srv];
    for(int ti = 0; ti < (int)synths.size(); ti++) {
        auto* s = synths[ti];
        if(!s) continue;
        // Resend all params before moving — same pattern as scFM7Drone.
        // If created=true (existing synth being reordered): set() sends /n_setn immediately.
        // If created=false (brand-new synth not yet /n_go'd): goes to vecArgs and is
        // delivered via resendStoredArgs when /n_go arrives.
        if(ti < numTracks) {
            const TrackData&   tdi = track(ti);
            const TrackConfig& tci = trackConfig(ti);
            s->set("bpm",           currentBpm);
            s->set("numBeats",      (float)tci.numBeats);
            s->set("stepsPerBeat",  (float)tci.stepsPerBeat);
            s->set("numSteps",      (float)tci.getNumSteps());
            s->set("shift",         (float)tdi.shift);
            s->set("swing",         swingP.get());
            s->set("globalPitch",   tci.trackPitch);
            s->set("globalVol",     tci.globalVol);
            s->set("globalProb",    tci.globalProb);
            s->set("bufnum",        (float)getBufnum(ti, srv));
            s->set("inPoint",       tci.inPoint);
            s->set("outPoint",      tci.outPoint);
            s->set("loopSample",    tci.loopEnabled   ? 1.0f : 0.0f);
            s->set("fixDuration",   tci.fixDuration   ? 1.0f : 0.0f);
            s->set("durationBeats", tci.durationBeats);
            s->set("mono",          tci.monoMode      ? 1.0f : 0.0f);
            s->set("volLatch",      tci.volLatch       ? 1.0f : 0.0f);
            s->set("envEnabled",    tci.envEnabled     ? 1.0f : 0.0f);
            s->set("envAttack",     tci.envAttack);
            s->set("envHold",       tci.envHold);
            s->set("envDecay",      tci.envDecay);
            s->set("envRelease",    0.0f);
            s->set("envCurveA",     tci.envCurveA);
            s->set("envCurveD",     tci.envCurveD);
            s->set("eqEnabled",     tci.eqEnabled   ? 1.0f : 0.0f);
            s->set("eqHPFreq",      tci.eqHPFreq);
            s->set("eqHPRq",        1.0f / std::max(tci.eqHPQ,   0.01f));
            s->set("eqPeakFreq",    tci.eqPeakFreq);
            s->set("eqPeakGain",    tci.eqPeakGain);
            s->set("eqPeakRq",      1.0f / std::max(tci.eqPeakQ, 0.01f));
            s->set("eqLPFreq",      tci.eqLPFreq);
            s->set("eqLPRq",        1.0f / std::max(tci.eqLPQ,   0.01f));
            fireStepParams(ti);  // arrays: created=true → /n_setn; false → vecArgs
        }
        s->moveBefore(nodeID);
    }
}

int fullStepSequencer::getLastSynthID(ofxSCServer* srv) {
    if(!trackSynths.count(srv)) return -1;
    // Return the last active track's synth ID (tail of the chain)
    for(int ti = numTracks - 1; ti >= 0; ti--)
        if(ti < (int)trackSynths[srv].size() && trackSynths[srv][ti])
            return trackSynths[srv][ti]->nodeID;
    return -1;
}

// ════════════════════════════════════════════════════════════════════════════
// Synth helpers
// ════════════════════════════════════════════════════════════════════════════

void fullStepSequencer::createTrackSynth(ofxSCServer* srv, int ti) {
    if(!srv || ti >= MAX_TRACKS) return;

    // Ensure per-server vectors are large enough for this track index
    auto ensureSize = [&](auto& m, auto* nullVal) {
        if(!m.count(srv)) m[srv].assign(ti + 1, nullVal);
        else if(ti >= (int)m[srv].size()) m[srv].resize(ti + 1, nullVal);
    };
    ensureSize(trackSynths,  (ofxSCSynth*)nullptr);
    ensureSize(privateBuses, (ofxSCBus*)nullptr);
    ensureSize(stepBuses,    (ofxSCBus*)nullptr);
    ensureSize(gateBuses,    (ofxSCBus*)nullptr);

    // Free any existing synth at this slot
    if(trackSynths[srv][ti]) {
        trackSynths[srv][ti]->free();
        delete trackSynths[srv][ti];
        trackSynths[srv][ti] = nullptr;
    }

    const TrackData&   td = track(ti);
    const TrackConfig& tc = trackConfig(ti);
    int bufnum = getBufnum(ti, srv);

    auto* s = new ofxSCSynth("FullStepSeqTrack", srv);
    trackSynths[srv][ti] = s;

    // Set timing and sample parameters
    s->set("bpm",           currentBpm);
    s->set("numBeats",      (float)tc.numBeats);
    s->set("stepsPerBeat",  (float)tc.stepsPerBeat);
    s->set("shift",         (float)td.shift);
    s->set("swing",         swingP.get());
    s->set("numSteps",      (float)tc.getNumSteps());
    s->set("globalPitch",   tc.trackPitch);
    s->set("globalVol",     tc.globalVol);
    s->set("globalProb",    tc.globalProb);
    s->set("bufnum",        bufnum);
    s->set("reset",         0);
    s->set("active",        (getActive() && !tc.muted) ? 1.0f : 0.0f);
    s->set("mono",          tc.monoMode      ? 1.0f : 0.0f);
    s->set("volLatch",      tc.volLatch       ? 1.0f : 0.0f);
    s->set("inPoint",       tc.inPoint);
    s->set("outPoint",      tc.outPoint);
    s->set("loopSample",    tc.loopEnabled   ? 1.0f : 0.0f);
    s->set("fixDuration",   tc.fixDuration   ? 1.0f : 0.0f);
    s->set("durationBeats", tc.durationBeats);
    s->set("envEnabled",    tc.envEnabled    ? 1.0f : 0.0f);
    s->set("envAttack",     tc.envAttack);
    s->set("envHold",       tc.envHold);
    s->set("envDecay",      tc.envDecay);
    s->set("envRelease",    0.0f);
    s->set("envCurveA",     tc.envCurveA);
    s->set("envCurveD",     tc.envCurveD);
    s->set("eqEnabled",     tc.eqEnabled   ? 1.0f : 0.0f);
    s->set("eqHPFreq",      tc.eqHPFreq);
    s->set("eqHPRq",        1.0f / std::max(tc.eqHPQ,    0.01f));
    s->set("eqPeakFreq",    tc.eqPeakFreq);
    s->set("eqPeakGain",    tc.eqPeakGain);
    s->set("eqPeakRq",      1.0f / std::max(tc.eqPeakQ,  0.01f));
    s->set("eqLPFreq",      tc.eqLPFreq);
    s->set("eqLPRq",        1.0f / std::max(tc.eqLPQ,    0.01f));

    // ── Audio output bus ────────────────────────────────────────────────────
    // If the graph manager has already assigned a bus (setOutputBus was called
    // before createSynth for this track), use it directly.
    // Otherwise allocate a private internal audio bus so audio NEVER leaks to
    // hardware bus 0 while the output port is unconnected.
    if(outputBuses.count(srv) && outputBuses[srv].count(ti)) {
        s->set("out", outputBuses[srv].at(ti));
    } else {
        // Free any stale private bus for this slot
        if(privateBuses.count(srv) && ti < (int)privateBuses[srv].size()
           && privateBuses[srv][ti]) {
            privateBuses[srv][ti]->free();
            delete privateBuses[srv][ti];
            privateBuses[srv][ti] = nullptr;
        }
        auto* pb = new ofxSCBus(RATE_AUDIO, 2, srv);
        if(!privateBuses.count(srv)) privateBuses[srv].assign(1, nullptr);
        privateBuses[srv][ti] = pb;
        s->set("out", pb->index);
    }

    // ── KR step-readback bus (for playhead indicator) ───────────────────────
    if(stepBuses.count(srv) && ti < (int)stepBuses[srv].size() && stepBuses[srv][ti]) {
        stepBuses[srv][ti]->free();
        delete stepBuses[srv][ti];
    }
    if(!stepBuses.count(srv)) stepBuses[srv].assign(1, nullptr);
    auto* sb = new ofxSCBus(RATE_CONTROL, 1, srv);
    stepBuses[srv][ti] = sb;
    s->set("stepBus", sb->index);

    if(gateBuses.count(srv) && ti < (int)gateBuses[srv].size() && gateBuses[srv][ti]) {
        gateBuses[srv][ti]->free();
        delete gateBuses[srv][ti];
    }
    if(!gateBuses.count(srv)) gateBuses[srv].assign(1, nullptr);
    auto* gb = new ofxSCBus(RATE_CONTROL, 1, srv);
    gateBuses[srv][ti] = gb;
    s->set("gateBus", gb->index);

    // Add to head of default group so reverse-iteration ordering is correct
    // (same as scFM7Drone — serverManager iterates newNodesList in reverse
    // and relies on addToHead so sources end up before outputs in SC).
    s->createAndRun(0, 1, getActive());

    // Queue step arrays via the pStep* ofParameter listeners.
    // At this point s->created is still false (/n_go hasn't arrived yet), so
    // the synth->set(name, vector) calls inside the listeners land in vecArgs
    // rather than firing /n_setn immediately.  When SC confirms the node (/n_go),
    // ofxSCNode::feedbackListener sets created=true and calls resendStoredArgs(),
    // which sends a single /n_set message containing all the queued arrays — the
    // same mechanism scPolyMixer and scFM7Drone rely on.
    fireStepParams(ti);
}

void fullStepSequencer::freeServerSynths(ofxSCServer* srv) {
    // Free synths
    if(trackSynths.count(srv)) {
        for(auto* s : trackSynths[srv]) { if(s) { s->free(); delete s; } }
        trackSynths[srv].clear();
    }
    // Free private audio buses
    if(privateBuses.count(srv)) {
        for(auto* b : privateBuses[srv]) { if(b) { b->free(); delete b; } }
        privateBuses[srv].clear();
    }
    if(stepBuses.count(srv)) {
        for(auto* b : stepBuses[srv]) { if(b) { b->free(); delete b; } }
        stepBuses[srv].clear();
    }
    if(gateBuses.count(srv)) {
        for(auto* b : gateBuses[srv]) { if(b) { b->free(); delete b; } }
        gateBuses[srv].clear();
    }
}

void fullStepSequencer::sendStepData(ofxSCSynth* /*s*/, const TrackData& /*td*/) {
    // No-op: step data is now sent via pStep* ofParameter listeners.
    // Call fireStepParams(ti) instead.
}

void fullStepSequencer::sendStepDataToAll(int ti) {
    fireStepParams(ti);
}

void fullStepSequencer::fireStepParams(int ti) {
    if(ti >= MAX_TRACKS) return;
    const TrackData&   td = track(ti);
    const TrackConfig& tc = trackConfig(ti);
    int n = tc.getNumSteps();

    vector<float> on   (MAX_STEPS, 0.f), vol  (MAX_STEPS, 1.f),
                  prob (MAX_STEPS, 1.f), pan  (MAX_STEPS, 0.f),
                  cut  (MAX_STEPS, 0.f), res  (MAX_STEPS, 0.f),
                  rev  (MAX_STEPS, 0.f);
    vector<int>   pitch(MAX_STEPS, 0);

    for(int i = 0; i < n; i++) {
        on   [i] = (i < (int)td.stepOn.size()      && td.stepOn[i])      ? 1.f : 0.f;
        vol  [i] = (i < (int)td.stepVol.size())    ? td.stepVol[i]    : 1.f;
        prob [i] = (i < (int)td.stepProb.size())   ? td.stepProb[i]   : 1.f;
        pan  [i] = (i < (int)td.stepPan.size())    ? td.stepPan[i]    : 0.f;
        cut  [i] = (i < (int)td.stepCut.size())    ? td.stepCut[i]    : 0.f;
        res  [i] = (i < (int)td.stepRes.size())    ? td.stepRes[i]    : 0.f;
        pitch[i] = (i < (int)td.stepPitch.size())  ? td.stepPitch[i]  : 0;
        rev  [i] = (i < (int)td.stepReverse.size() && td.stepReverse[i]) ? 1.f : 0.f;
    }

    pStepOn     [ti].set(on);
    pStepVol    [ti].set(vol);
    pStepProb   [ti].set(prob);
    pStepPan    [ti].set(pan);
    pStepCut    [ti].set(cut);
    pStepRes    [ti].set(res);
    pStepPitch  [ti].set(pitch);
    pStepReverse[ti].set(rev);
}

void fullStepSequencer::sendStepDataDirect(ofxSCSynth* s, const TrackData& td, ofxSCServer* srv) {
    // Send step arrays via /n_setn directly, bypassing the ofxSCSynth created-flag check.
    // Called immediately after createAndRun so SC has the correct data from the first step.
    if(!s || s->nodeID == 0 || !srv) return;

    // Note: getNumSteps() now lives on TrackConfig; use MAX_STEPS as a safe upper bound here
    // since we don't have a ti index. This function is currently unused (fireStepParams handles it).
    int n = MAX_STEPS;
    std::vector<float> on   (MAX_STEPS, 0.0f);
    std::vector<float> vol  (MAX_STEPS, 1.0f);
    std::vector<float> prob (MAX_STEPS, 1.0f);
    std::vector<float> pan  (MAX_STEPS, 0.0f);
    std::vector<float> cut  (MAX_STEPS, 0.0f);
    std::vector<float> res  (MAX_STEPS, 0.0f);
    std::vector<float> pitch(MAX_STEPS, 0.0f);
    for(int i = 0; i < n; i++) {
        on[i]    = (i < (int)td.stepOn.size()    && td.stepOn[i])    ? 1.0f : 0.0f;
        vol[i]   = (i < (int)td.stepVol.size())   ? td.stepVol[i]   : 1.0f;
        prob[i]  = (i < (int)td.stepProb.size())  ? td.stepProb[i]  : 1.0f;
        pan[i]   = (i < (int)td.stepPan.size())   ? td.stepPan[i]   : 0.0f;
        cut[i]   = (i < (int)td.stepCut.size())   ? td.stepCut[i]   : 0.0f;
        res[i]   = (i < (int)td.stepRes.size())   ? td.stepRes[i]   : 0.0f;
        pitch[i] = (i < (int)td.stepPitch.size()) ? (float)td.stepPitch[i] : 0.0f;
    }

    auto setn = [&](const std::string& key, const std::vector<float>& vals) {
        ofxOscMessage m;
        m.setAddress("/n_setn");
        m.addIntArg(s->nodeID);
        m.addStringArg(key);
        m.addIntArg((int)vals.size());
        for(float v : vals) m.addFloatArg(v);
        srv->sendMsg(m);
    };
    setn("stepOn",    on);
    setn("stepVol",   vol);
    setn("stepProb",  prob);
    setn("stepPan",   pan);
    setn("stepCut",   cut);
    setn("stepRes",   res);
    setn("stepPitch", pitch);
}

void fullStepSequencer::setNumTracks(int n) {
    n = ofClamp(n, 1, MAX_TRACKS);
    int old = numTracks;
    if(n == old) return;
    numTracks = n;

    // ── Resize slot track arrays (all slots) ─────────────────────────────────
    for(auto& slot : slots) {
        int oldSize = (int)slot.tracks.size();
        slot.tracks.resize(n);
        for(int ti = oldSize; ti < n; ti++)
            slot.tracks[ti].resizeSteps();
    }
    // ── Resize track configs ──────────────────────────────────────────────────
    int oldCfgSize = (int)trackConfigs.size();
    trackConfigs.resize(n);
    for(int ti = oldCfgSize; ti < n; ti++)
        trackConfigs[ti].name = "Track " + ofToString(ti + 1);
    currentStep.resize(n, 0);

    // ── Resize vector parameters ─────────────────────────────────────────────
    {
        auto tv = transposeP .get(); tv .resize(n, 0.0f); transposeP .set(tv);
        auto vv = globalVolP .get(); vv .resize(n, 1.0f); globalVolP .set(vv);
        auto pv = globalProbP.get(); pv.resize(n, 1.0f); globalProbP.set(pv);
        auto mv = muteP      .get(); mv .resize(n, 0);    muteP      .set(mv);
    }

    // ── Output port management ────────────────────────────────────────────────
    if(n > old) {
        for(int ti = old; ti < n; ti++)
            scNode::addOutput("Out " + ofToString(ti + 1));
    } else {
        for(int ti = old - 1; ti >= n; ti--)
            scNode::removeOutput(ti);
    }

    // ── Synth management (skip during preset loading) ─────────────────────────
    if(!ofxOceanodeShared::isPresetLoading()) {
        if(n > old) {
            for(int ti = old; ti < n; ti++) {
                snprintf(nameEditBuf[ti], 64, "%s", trackConfigs[ti].name.c_str());
                for(auto* sm : allServers)
                    if(sm && sm->getServer())
                        createTrackSynth(sm->getServer(), ti);
            }
        } else {
            for(int ti = old - 1; ti >= n; ti--) {
                // Free synths
                for(auto& [srv, synths] : trackSynths)
                    if(ti < (int)synths.size() && synths[ti]) {
                        synths[ti]->free(); delete synths[ti]; synths[ti] = nullptr;
                    }
                // Free KR buses
                for(auto& [srv, buses] : stepBuses)
                    if(ti < (int)buses.size() && buses[ti])
                        { buses[ti]->free(); delete buses[ti]; buses[ti] = nullptr; }
                for(auto& [srv, buses] : gateBuses)
                    if(ti < (int)buses.size() && buses[ti])
                        { buses[ti]->free(); delete buses[ti]; buses[ti] = nullptr; }
                for(auto& [srv, buses] : privateBuses)
                    if(ti < (int)buses.size() && buses[ti])
                        { buses[ti]->free(); delete buses[ti]; buses[ti] = nullptr; }
                freeSampleForTrack(ti);
            }
        }
    }
}

void fullStepSequencer::sendBpmToAll() {
    for(auto& [srv, synths] : trackSynths)
        for(auto* s : synths)
            if(s) s->set("bpm", currentBpm);
}

// ════════════════════════════════════════════════════════════════════════════
// Track / slot data management
// ════════════════════════════════════════════════════════════════════════════

fullStepSequencer::TrackData& fullStepSequencer::track(int ti) {
    int slot = currentSlotP.get();
    return slots[slot].tracks[ti];
}

const fullStepSequencer::TrackData& fullStepSequencer::track(int ti) const {
    int slot = currentSlotP.get();
    return slots[slot].tracks[ti];
}

fullStepSequencer::TrackConfig& fullStepSequencer::trackConfig(int ti) {
    return trackConfigs[ti];
}

const fullStepSequencer::TrackConfig& fullStepSequencer::trackConfig(int ti) const {
    return trackConfigs[ti];
}

void fullStepSequencer::initSlots() {
    // Initialise global per-track configs (one per track, shared across all slots)
    trackConfigs.resize(MAX_TRACKS);
    for(int ti = 0; ti < MAX_TRACKS; ti++)
        trackConfigs[ti].name = "Track " + ofToString(ti + 1);

    // Initialise per-slot step/shift data
    slots.resize(MAX_SLOTS);
    for(int s = 0; s < MAX_SLOTS; s++) {
        slots[s].tracks.resize(numTracks);
        for(int ti = 0; ti < numTracks; ti++)
            slots[s].tracks[ti].resizeSteps();
    }
}

void fullStepSequencer::reloadCurrentSlot() {
    // Sync node-GUI name buffers from global TrackConfig (name is per-track, not per-slot).
    for(int ti = 0; ti < numTracks; ti++)
        snprintf(nameEditBuf[ti], 64, "%s", trackConfigs[ti].name.c_str());

    // Rebuild vector parameters from global track config (fires listeners → SC synths)
    {
        auto tv = transposeP.get();  auto vv = globalVolP.get();
        auto pv = globalProbP.get(); auto mv = muteP.get();
        tv.resize(numTracks, 0.0f);  vv.resize(numTracks, 1.0f);
        pv.resize(numTracks, 1.0f);  mv.resize(numTracks, 0);
        for(int ti = 0; ti < numTracks; ti++) {
            tv[ti] = trackConfigs[ti].trackPitch;
            vv[ti] = trackConfigs[ti].globalVol;
            pv[ti] = trackConfigs[ti].globalProb;
            mv[ti] = trackConfigs[ti].muted ? 1 : 0;
        }
        transposeP.set(tv);
        globalVolP.set(vv);
        globalProbP.set(pv);
        muteP.set(mv);  // fires listener → sets active on SC synths
    }

    // Push slot-specific (shift) and config params to running synths.
    // Config params don't change between slots, but synths may have been recreated.
    for(auto& [srv, synths] : trackSynths) {
        for(int ti = 0; ti < numTracks && ti < (int)synths.size(); ti++) {
            auto* s = synths[ti];
            if(!s) continue;
            const TrackData&   tdi = track(ti);
            const TrackConfig& tci = trackConfigs[ti];
            s->set("shift",         (float)tdi.shift);
            s->set("numBeats",      (float)tci.numBeats);
            s->set("stepsPerBeat",  (float)tci.stepsPerBeat);
            s->set("numSteps",      (float)tci.getNumSteps());
            s->set("swing",         swingP.get());
            s->set("mono",          tci.monoMode      ? 1.0f : 0.0f);
            s->set("volLatch",      tci.volLatch       ? 1.0f : 0.0f);
            s->set("inPoint",       tci.inPoint);
            s->set("outPoint",      tci.outPoint);
            s->set("loopSample",    tci.loopEnabled   ? 1.0f : 0.0f);
            s->set("fixDuration",   tci.fixDuration   ? 1.0f : 0.0f);
            s->set("durationBeats", tci.durationBeats);
            s->set("envEnabled",    tci.envEnabled    ? 1.0f : 0.0f);
            s->set("envAttack",     tci.envAttack);
            s->set("envHold",       tci.envHold);
            s->set("envDecay",      tci.envDecay);
            s->set("envRelease",    0.0f);
            s->set("envCurveA",     tci.envCurveA);
            s->set("envCurveD",     tci.envCurveD);
            s->set("eqEnabled",     tci.eqEnabled   ? 1.0f : 0.0f);
            s->set("eqHPFreq",      tci.eqHPFreq);
            s->set("eqHPRq",        1.0f / std::max(tci.eqHPQ,   0.01f));
            s->set("eqPeakFreq",    tci.eqPeakFreq);
            s->set("eqPeakGain",    tci.eqPeakGain);
            s->set("eqPeakRq",      1.0f / std::max(tci.eqPeakQ, 0.01f));
            s->set("eqLPFreq",      tci.eqLPFreq);
            s->set("eqLPRq",        1.0f / std::max(tci.eqLPQ,   0.01f));
            s->set("bufnum",        (float)getBufnum(ti, srv));
        }
    }
    // Fire step params — one call per track covers all servers via the pStep* listener.
    for(int ti = 0; ti < numTracks; ti++)
        fireStepParams(ti);
}

// ════════════════════════════════════════════════════════════════════════════
// Sample buffer management
// ════════════════════════════════════════════════════════════════════════════

int fullStepSequencer::getBufnum(int ti, ofxSCServer* srv) const {
    if(ti >= (int)trackBufs.size()) return 0;
    auto it = trackBufs[ti].find(srv);
    if(it != trackBufs[ti].end() && it->second)
        return it->second->index;
    return 0;  // SC buffer 0 = silent when not loaded
}

void fullStepSequencer::loadSampleForTrack(int ti, const std::string& path) {
    if(ti >= MAX_TRACKS || path.empty()) return;

    freeSampleForTrack(ti);  // frees old buffer + clears samplePaths[ti]

    ofxSCServer* primarySrv = nullptr;
    for(auto* sm : allServers) {
        if(sm && sm->getServer()) { primarySrv = sm->getServer(); break; }
    }
    if(!primarySrv) return;

    loadWaveformData(ti, path);   // pre-compute peak display data for WAV tab

    try {
        auto* buf = new ofxSCBuffer(0, 0, primarySrv);
        buf->read(path);  // load all channels (stereo/mono as-is)
        trackBufs[ti][primarySrv] = buf;
        ofLogNotice("fullStepSequencer") << "Loaded track " << ti << " : " << path
                                          << " bufnum=" << buf->index;
    } catch(const std::exception& e) {
        ofLogError("fullStepSequencer") << "loadSampleForTrack ti=" << ti << " : " << e.what();
        return;
    }

    // Mirror to additional servers
    for(size_t j = 1; j < allServers.size(); j++) {
        if(!allServers[j] || !allServers[j]->getServer()) continue;
        ofxSCServer* srv = allServers[j]->getServer();
        try {
            auto* buf = new ofxSCBuffer(0, 0, srv);
            buf->read(path);
            trackBufs[ti][srv] = buf;
        } catch(const std::exception& e) {
            ofLogError("fullStepSequencer") << "mirror ti=" << ti << " srv" << j << ": " << e.what();
        }
    }

    // Store path AFTER freeSampleForTrack (which cleared it) so display and
    // preset serialisation have the correct value.
    samplePaths[ti] = path;
}

void fullStepSequencer::freeSampleForTrack(int ti) {
    if(ti >= (int)trackBufs.size()) return;
    for(auto& [srv, buf] : trackBufs[ti]) {
        if(buf) { buf->free(); delete buf; }
    }
    trackBufs[ti].clear();
    if(ti < (int)waveformPeaks.size()) waveformPeaks[ti].clear();
    if(ti < (int)samplePaths.size()) samplePaths[ti].clear();
}

void fullStepSequencer::freeAllSamples() {
    for(int i = 0; i < MAX_TRACKS; i++)
        freeSampleForTrack(i);
}


// ════════════════════════════════════════════════════════════════════════════
// Waveform peak extraction  (handles 16-bit, 24-bit, 32-bit float PCM WAV)
// ════════════════════════════════════════════════════════════════════════════

void fullStepSequencer::loadWaveformData(int ti, const std::string& path) {
    if(ti < 0 || ti >= MAX_TRACKS) return;
    waveformPeaks[ti].clear();

    std::string ext = ofToLower(std::filesystem::path(path).extension().string());
    if(ext != ".wav") {
        // Non-WAV (AIFF etc.): show a flat half-amplitude placeholder
        waveformPeaks[ti].assign(WAVEFORM_BINS, 0.45f);
        return;
    }

    std::ifstream file(path, std::ios::binary);
    if(!file.is_open()) return;

    // ── Parse RIFF/WAVE header ───────────────────────────────────────────────
    char tag[4];
    uint32_t chunkSize;
    file.read(tag, 4);
    if(std::strncmp(tag, "RIFF", 4) != 0) return;
    file.read((char*)&chunkSize, 4);
    file.read(tag, 4);
    if(std::strncmp(tag, "WAVE", 4) != 0) return;

    uint16_t numChannels = 1, bitsPerSample = 16, audioFormat = 1;
    uint32_t numFrames = 0;
    std::streampos dataStart = 0;

    while(file.good()) {
        char chunkID[4];
        uint32_t cSize;
        file.read(chunkID, 4);
        file.read((char*)&cSize, 4);
        if(!file.good()) break;

        if(std::strncmp(chunkID, "fmt ", 4) == 0) {
            uint16_t nCh, blockAlign, bps;
            uint32_t sampleRate, byteRate;
            file.read((char*)&audioFormat, 2);
            file.read((char*)&nCh,         2);
            file.read((char*)&sampleRate,  4);
            file.read((char*)&byteRate,    4);
            file.read((char*)&blockAlign,  2);
            file.read((char*)&bps,         2);
            numChannels   = nCh;
            bitsPerSample = bps;
            int remaining = (int)cSize - 16;
            if(remaining > 0) file.seekg(remaining, std::ios::cur);
        } else if(std::strncmp(chunkID, "data", 4) == 0) {
            dataStart = file.tellg();
            uint32_t bytesPerFrame = numChannels * (bitsPerSample / 8);
            numFrames = (bytesPerFrame > 0) ? cSize / bytesPerFrame : 0;
            break;
        } else {
            file.seekg(cSize + (cSize & 1), std::ios::cur);
        }
    }

    if(numFrames == 0 || dataStart == 0) return;

    // ── Read all PCM samples into a float buffer ─────────────────────────────
    int bytesPerSample = bitsPerSample / 8;
    long long totalSamples = (long long)numFrames * numChannels;
    std::vector<float> samples;
    samples.reserve((size_t)totalSamples);

    file.seekg(dataStart);

    for(long long i = 0; i < totalSamples && file.good(); i++) {
        float s = 0.0f;
        if(bitsPerSample == 16) {
            int16_t raw;
            file.read((char*)&raw, 2);
            s = raw / 32768.0f;
        } else if(bitsPerSample == 24) {
            uint8_t bytes[3];
            file.read((char*)bytes, 3);
            int32_t raw = (int32_t)((uint32_t)bytes[2] << 24 |
                                    (uint32_t)bytes[1] << 16 |
                                    (uint32_t)bytes[0] << 8) >> 8;
            s = raw / 8388608.0f;
        } else if(bitsPerSample == 32) {
            // Could be float (audioFormat==3) or int32
            if(audioFormat == 3) {
                file.read((char*)&s, 4);
            } else {
                int32_t raw;
                file.read((char*)&raw, 4);
                s = raw / 2147483648.0f;
            }
        } else {
            break;  // unsupported bit depth — leave peaks empty
        }
        samples.push_back(s);
    }

    if(samples.empty()) return;
    long long actualFrames = (long long)samples.size() / numChannels;

    // ── Compute per-bin peak ─────────────────────────────────────────────────
    waveformPeaks[ti].resize(WAVEFORM_BINS, 0.0f);
    for(int b = 0; b < WAVEFORM_BINS; b++) {
        long long startF = (long long)b * actualFrames / WAVEFORM_BINS;
        long long endF   = (long long)(b + 1) * actualFrames / WAVEFORM_BINS;
        float peak = 0.0f;
        for(long long f = startF; f < endF; f++) {
            for(int c = 0; c < numChannels; c++) {
                long long idx = f * numChannels + c;
                if(idx < (long long)samples.size())
                    peak = std::max(peak, std::abs(samples[(size_t)idx]));
            }
        }
        waveformPeaks[ti][b] = peak;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Euclidean rhythm helper
// ════════════════════════════════════════════════════════════════════════════

std::vector<bool> fullStepSequencer::euclideanRhythm(int k, int n) {
    std::vector<bool> pattern(n, false);
    if(k <= 0 || n <= 0) return pattern;
    k = std::min(k, n);
    // Bresenham / equal-distribution: place k pulses as evenly as possible in n steps.
    for(int i = 0; i < k; i++)
        pattern[((long long)i * n) / k] = true;
    return pattern;
}

// ════════════════════════════════════════════════════════════════════════════
// File browser
// ════════════════════════════════════════════════════════════════════════════

void fullStepSequencer::refreshBrowse(const std::string& dir) {
    browseEntries.clear();
    browseDir = dir;
    if(!std::filesystem::exists(dir)) return;
    try {
        std::vector<BrowseEntry> dirs, files;
        for(auto& e : std::filesystem::directory_iterator(dir)) {
            std::string n = e.path().filename().string();
            if(n.empty() || n.front() == '.') continue;
            if(e.is_directory()) {
                dirs.push_back({true, n, e.path().string()});
            } else {
                std::string ext = ofToLower(e.path().extension().string());
                if(ext == ".wav" || ext == ".aif" || ext == ".aiff")
                    files.push_back({false, n, e.path().string()});
            }
        }
        std::sort(dirs.begin(),  dirs.end(),  [](auto& a, auto& b){ return a.name < b.name; });
        std::sort(files.begin(), files.end(), [](auto& a, auto& b){ return a.name < b.name; });
        browseEntries.insert(browseEntries.end(), dirs.begin(),  dirs.end());
        browseEntries.insert(browseEntries.end(), files.begin(), files.end());
    } catch(const std::exception& e) {
        ofLogWarning("fullStepSequencer") << "refreshBrowse: " << e.what();
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Preview playback
// ════════════════════════════════════════════════════════════════════════════

void fullStepSequencer::triggerPreview(const std::string& path) {
    stopPreview();
    if(path.empty() || !previewServer) return;
    try {
        previewBuf = new ofxSCBuffer(0, 0, previewServer);
        previewBuf->read(path);
        previewSynth = new ofxSCSynth("BufferBrowserPreview", previewServer);
        previewSynth->set("bufnum", previewBuf->index);
        previewSynth->set("out",    0);
        previewSynth->set("gain",   0.7f);
        previewSynth->addToTail();
    } catch(const std::exception& e) {
        ofLogError("fullStepSequencer") << "preview: " << e.what();
        stopPreview();
    }
}

void fullStepSequencer::stopPreview() {
    if(previewSynth) { previewSynth->free(); delete previewSynth; previewSynth = nullptr; }
    if(previewBuf)   { previewBuf->free();   delete previewBuf;   previewBuf   = nullptr; }
}

// ════════════════════════════════════════════════════════════════════════════
// ImGui – Sequencer window
// ════════════════════════════════════════════════════════════════════════════

void fullStepSequencer::drawSequencerWindow() {
    string title = "Step Sequencer " + ofToString(getNumIdentifier());
    ImGui::SetNextWindowSize(ImVec2(1100, 660), ImGuiCond_FirstUseEver);

    bool open = showWindow.get();
    if(ImGui::Begin(title.c_str(), &open,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        if(!open) showWindow = false;

        ImVec2 avail = ImGui::GetContentRegionAvail();
        const float splitterW = 6.0f;
        const float marginW   = 6.0f;  // gap between splitter and tracks content
        browserW = ofClamp(browserW, 80.0f, avail.x - 120.0f);
        const float tracksW = avail.x - browserW - splitterW - marginW;

        // ── Left: file browser ────────────────────────────────────────────────
        ImGui::BeginChild("##browser", ImVec2(browserW, avail.y), false);
        drawBrowser(browserW, avail.y);
        ImGui::EndChild();

        // ── Drag splitter ─────────────────────────────────────────────────────
        ImGui::SameLine(0, 0);
        ImGui::InvisibleButton("##splitter", ImVec2(splitterW, avail.y));
        if(ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if(ImGui::IsItemActive())
            browserW = ofClamp(browserW + ImGui::GetIO().MouseDelta.x, 80.0f, avail.x - 120.0f);
        // Draw a subtle separator line in the middle of the splitter area
        {
            ImVec2 p = ImGui::GetItemRectMin();
            ImVec2 q = ImGui::GetItemRectMax();
            float cx = (p.x + q.x) * 0.5f;
            bool active = ImGui::IsItemHovered() || ImGui::IsItemActive();
            ImU32 col = active ? IM_COL32(180,180,180,200) : IM_COL32(90,90,90,150);
            ImGui::GetWindowDrawList()->AddLine(ImVec2(cx, p.y), ImVec2(cx, q.y), col, 1.5f);
        }
        ImGui::SameLine(0, marginW);

        // ── Right: scrollable tracks area ─────────────────────────────────────
        ImGui::BeginChild("##tracks", ImVec2(tracksW, avail.y), false,
                          ImGuiWindowFlags_HorizontalScrollbar);

        ImGui::Spacing();

        for(int ti = 0; ti < numTracks; ti++) {
            ImGui::PushID(ti);
            drawTrack(ti);
            ImGui::PopID();
        }

        ImGui::EndChild();
    }
    ImGui::End();
}

// ════════════════════════════════════════════════════════════════════════════
// ImGui – File browser panel
// ════════════════════════════════════════════════════════════════════════════

void fullStepSequencer::drawBrowser(float /*w*/, float /*h*/) {
    // Navigation bar
    if(ImGui::Button("...")) {
        auto res = ofSystemLoadDialog("Select Samples Folder", true, browseDir);
        if(res.bSuccess) refreshBrowse(res.getPath());
    }
    ImGui::SameLine();
    // Store filename in a local to avoid dangling-pointer UB (filename() returns
    // a temporary path whose .string() temporary is destroyed before TextUnformatted reads it).
    {
        std::string dirName = std::filesystem::path(browseDir).filename().string();
        if(dirName.empty()) dirName = browseDir; // root or drive letter
        ImGui::TextUnformatted(dirName.c_str());
    }

    if(ImGui::Button("^ ..")) {
        auto parent = std::filesystem::path(browseDir).parent_path().string();
        if(!parent.empty() && parent != browseDir)
            refreshBrowse(parent);
    }
    ImGui::Separator();

    int n = (int)browseEntries.size();
    browserSel = std::min(browserSel, n - 1); // clamp after any refresh

    ImGui::BeginChild("##blist", ImVec2(0, 0), false);

    // ── Keyboard navigation (only when this child is focused / hovered) ───────
    if(ImGui::IsWindowFocused() || ImGui::IsWindowHovered()) {
        if(ImGui::IsKeyPressed(ImGuiKey_DownArrow) && n > 0) {
            browserSel = std::min(browserSel + 1, n - 1);
            if(browserSel >= 0 && browserSel < n && !browseEntries[browserSel].isDir)
                triggerPreview(browseEntries[browserSel].fullPath);
        }
        if(ImGui::IsKeyPressed(ImGuiKey_UpArrow) && n > 0) {
            browserSel = std::max(browserSel - 1, 0);
            if(browserSel >= 0 && browserSel < n && !browseEntries[browserSel].isDir)
                triggerPreview(browseEntries[browserSel].fullPath);
        }
        if(ImGui::IsKeyPressed(ImGuiKey_Enter) && browserSel >= 0 && browserSel < n) {
            if(browseEntries[browserSel].isDir) {
                std::string navPath = browseEntries[browserSel].fullPath;
                browserSel = -1;
                refreshBrowse(navPath);
                n = 0; // skip the for loop below — browseEntries is rebuilt
            }
            // files: already previewing from arrow key; Enter just confirms
        }
    }

    bool navigated = false;
    for(int i = 0; i < n && !navigated; i++) {
        auto& e = browseEntries[i];
        ImGui::PushID(i);

        std::string lbl = (e.isDir ? "[D] " : "    ") + e.name;
        // Single unified selection highlight: browserSel is the source of truth.
        // Clicking a file sets browserSel so there is never more than one highlighted row.
        bool selected = (i == browserSel);

        if(ImGui::Selectable(lbl.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
            if(e.isDir) {
                std::string navPath = e.fullPath;  // copy before refreshBrowse clears browseEntries
                browserSel = -1;
                ImGui::PopID();
                refreshBrowse(navPath);
                navigated = true;
                break;
            } else {
                browserSel = i;           // highlight moves to clicked row
                triggerPreview(e.fullPath);
            }
        }

        // Auto-scroll to keep the keyboard-selected item visible
        if(selected) ImGui::SetScrollHereY(0.5f);

        // Drag source for audio files → drop onto track name
        if(!e.isDir) {
            if(ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload("FSS_SAMPLE", e.fullPath.c_str(), e.fullPath.size() + 1);
                ImGui::TextUnformatted(("  " + e.name).c_str());
                ImGui::EndDragDropSource();
            }
        }

        if(!e.isDir && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", e.fullPath.c_str());

        ImGui::PopID();
    }
    ImGui::EndChild();
}

// ════════════════════════════════════════════════════════════════════════════
// ImGui – Single track row
// ════════════════════════════════════════════════════════════════════════════

void fullStepSequencer::drawTrack(int ti) {
    TrackData&   td = track(ti);
    TrackConfig& tc = trackConfig(ti);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ── Per-track accent color ─────────────────────────────────────────────────
    static constexpr ImVec4 accentPalette[MAX_TRACKS] = {
        {0.27f, 0.53f, 0.95f, 1.f},  // 0 blue
        {0.28f, 0.82f, 0.48f, 1.f},  // 1 green
        {0.95f, 0.60f, 0.18f, 1.f},  // 2 orange
        {0.72f, 0.38f, 0.92f, 1.f},  // 3 purple
        {0.20f, 0.84f, 0.90f, 1.f},  // 4 cyan
        {0.95f, 0.28f, 0.30f, 1.f},  // 5 red
        {0.94f, 0.88f, 0.20f, 1.f},  // 6 yellow
        {0.80f, 0.32f, 0.70f, 1.f},  // 7 violet
    };
    const ImVec4 acc    = accentPalette[ti % MAX_TRACKS];
    const ImU32  accU32 = ImGui::ColorConvertFloat4ToU32(acc);

    // ── Card setup (splitter: ch0 = BG drawn behind, ch1 = content) ───────────
    const float accentBarW = 5.0f;
    const float cardPadX   = 10.0f;
    const float cardPadTop = 7.0f;
    const float cardPadBot = 9.0f;

    ImVec2 cardMin = ImGui::GetCursorScreenPos();
    float  cardW   = ImGui::GetContentRegionAvail().x;

    ImDrawListSplitter splitter;
    splitter.Split(dl, 2);
    splitter.SetCurrentChannel(dl, 1);  // content goes to ch1

    ImGui::Dummy({0.f, cardPadTop});
    ImGui::Indent(accentBarW + cardPadX);

    // ═══════════════════════════════════════════════════════════════════════════
    // Header row 1: [N] | name | [sample] | MUTE
    // ═══════════════════════════════════════════════════════════════════════════
    ImGui::TextColored(acc, "[%d]", ti + 1);
    ImGui::SameLine(0, 6);

    ImGui::SetNextItemWidth(90.0f);
    if(ImGui::InputText("##name", nameEditBuf[ti], 64))
        tc.name = nameEditBuf[ti];

    if(ImGui::BeginDragDropTarget()) {
        if(const ImGuiPayload* p = ImGui::AcceptDragDropPayload("FSS_SAMPLE")) {
            std::string path(static_cast<const char*>(p->Data), p->DataSize - 1);
            samplePaths[ti] = path;
            loadSampleForTrack(ti, path);
            for(auto& [srv, synths] : trackSynths)
                if(ti < (int)synths.size() && synths[ti])
                    synths[ti]->set("bufnum", (float)getBufnum(ti, srv));
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::SameLine(0, 8);
    if(!samplePaths[ti].empty())
        ImGui::TextDisabled("[%s]", std::filesystem::path(samplePaths[ti]).filename().string().c_str());
    else
        ImGui::TextDisabled("[no sample]");

    // MUTE button
    ImGui::SameLine(0, 14);
    {
        bool m = tc.muted;
        ImGui::PushStyleColor(ImGuiCol_Button,
            m ? ImVec4(0.72f, 0.15f, 0.15f, 1.f) : ImVec4(0.20f, 0.22f, 0.28f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            m ? ImVec4(0.82f, 0.22f, 0.22f, 1.f) : ImVec4(0.28f, 0.32f, 0.40f, 1.f));
        if(ImGui::Button(m ? "MUTED##mt" : "MUTE##mt", {58.f, 20.f})) {
            tc.muted = !tc.muted;
            auto mv = muteP.get();
            mv.resize(MAX_TRACKS, 0);
            mv[ti] = tc.muted ? 1 : 0;
            muteP.set(mv);
        }
        ImGui::PopStyleColor(2);
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // Header row 2: Beats | Steps/Beat | Shift | MONO | LATCH
    // ═══════════════════════════════════════════════════════════════════════════
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.62f, 0.70f, 1.f));
    ImGui::TextUnformatted("Beats");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 4);
    ImGui::SetNextItemWidth(38.0f);
    int nb = tc.numBeats;
    if(ImGui::InputInt("##nb", &nb, 0, 0)) {
        nb = ofClamp(nb, 1, 16);
        if(nb != tc.numBeats) {
            tc.numBeats = nb;
            for(auto& slot : slots) if(ti < (int)slot.tracks.size()) slot.tracks[ti].resizeSteps();
            for(auto& [srv, synths] : trackSynths) {
                if(ti < (int)synths.size() && synths[ti]) {
                    synths[ti]->set("numBeats",  (float)tc.numBeats);
                    synths[ti]->set("numSteps",  (float)tc.getNumSteps());
                }
            }
            fireStepParams(ti);
        }
    }

    ImGui::SameLine(0, 12);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.62f, 0.70f, 1.f));
    ImGui::TextUnformatted("Steps/Beat");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 4);
    ImGui::SetNextItemWidth(38.0f);
    int spb = tc.stepsPerBeat;
    if(ImGui::InputInt("##spb", &spb, 0, 0)) {
        spb = ofClamp(spb, 1, 16);
        if(spb != tc.stepsPerBeat) {
            tc.stepsPerBeat = spb;
            for(auto& slot : slots) if(ti < (int)slot.tracks.size()) slot.tracks[ti].resizeSteps();
            for(auto& [srv, synths] : trackSynths) {
                if(ti < (int)synths.size() && synths[ti]) {
                    synths[ti]->set("stepsPerBeat", (float)tc.stepsPerBeat);
                    synths[ti]->set("numSteps",     (float)tc.getNumSteps());
                }
            }
            fireStepParams(ti);
        }
    }

    ImGui::SameLine(0, 12);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.62f, 0.70f, 1.f));
    ImGui::TextUnformatted("Shift");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 4);
    if(ImGui::ArrowButton("##shL", ImGuiDir_Left)) {
        td.shift--;
        for(auto& [srv, synths] : trackSynths)
            if(ti < (int)synths.size() && synths[ti])
                synths[ti]->set("shift", (float)td.shift);
    }
    ImGui::SameLine(0, 3);
    ImGui::Text("%d", td.shift);
    ImGui::SameLine(0, 3);
    if(ImGui::ArrowButton("##shR", ImGuiDir_Right)) {
        td.shift++;
        for(auto& [srv, synths] : trackSynths)
            if(ti < (int)synths.size() && synths[ti])
                synths[ti]->set("shift", (float)td.shift);
    }

    // MONO button
    ImGui::SameLine(0, 14);
    {
        bool isMono = tc.monoMode;
        ImGui::PushStyleColor(ImGuiCol_Button,
            isMono ? ImVec4(acc.x*0.50f, acc.y*0.50f, acc.z*0.50f, 0.90f)
                   : ImVec4(0.18f, 0.20f, 0.26f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            isMono ? ImVec4(acc.x*0.65f, acc.y*0.65f, acc.z*0.65f, 1.f)
                   : ImVec4(0.25f, 0.28f, 0.36f, 1.f));
        if(ImGui::Button(isMono ? "MONO##mn" : "POLY##mn", {52.f, 20.f})) {
            tc.monoMode = !tc.monoMode;
            for(auto& [srv, synths] : trackSynths)
                if(ti < (int)synths.size() && synths[ti])
                    synths[ti]->set("mono", tc.monoMode ? 1.0f : 0.0f);
        }
        ImGui::PopStyleColor(2);
    }

    // LATCH button
    ImGui::SameLine(0, 5);
    {
        bool latch = tc.volLatch;
        ImGui::PushStyleColor(ImGuiCol_Button,
            latch ? ImVec4(acc.x*0.40f, acc.y*0.40f, acc.z*0.40f, 0.85f)
                  : ImVec4(0.18f, 0.20f, 0.26f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            latch ? ImVec4(acc.x*0.56f, acc.y*0.56f, acc.z*0.56f, 1.f)
                  : ImVec4(0.25f, 0.28f, 0.36f, 1.f));
        if(ImGui::Button(latch ? "LATCH##lt" : "CONT##lt", {58.f, 20.f})) {
            tc.volLatch = !tc.volLatch;
            for(auto& [srv, synths] : trackSynths)
                if(ti < (int)synths.size() && synths[ti])
                    synths[ti]->set("volLatch", tc.volLatch ? 1.0f : 0.0f);
        }
        ImGui::PopStyleColor(2);
    }

    // ── Disable step/tab content when muted ──────────────────────────────────
    if(tc.muted) ImGui::BeginDisabled(true);

    // ── Step row ──────────────────────────────────────────────────────────────
    int ns = tc.getNumSteps();
    float sw = std::max(4.0f, BEAT_W / (float)tc.stepsPerBeat);

    ImGui::Spacing();

    // Beat-group palette derived from the per-track accent color.
    // Even beat: full accent; Odd beat: accent lightened toward white.
    // Off states: near-black with a subtle accent hue for grouping clarity.
    auto mixColF = [](ImVec4 a, ImVec4 b, float t) -> ImVec4 {
        return {a.x*(1-t)+b.x*t, a.y*(1-t)+b.y*t, a.z*(1-t)+b.z*t, 1.f};
    };
    const ImVec4 baseDark0 = {0.09f, 0.10f, 0.13f, 1.f};
    const ImVec4 baseDark1 = {0.14f, 0.16f, 0.20f, 1.f};
    const ImVec4 accLight  = mixColF(acc, {1.f, 1.f, 1.f, 1.f}, 0.22f);
    struct BeatColors { ImVec4 off, on; };
    const BeatColors beatPalette[2] = {
        { mixColF(baseDark0, acc, 0.11f), acc       },  // even beat
        { mixColF(baseDark1, acc, 0.17f), accLight  },  // odd beat (lighter)
    };
    const ImVec4 playheadCol = {0.94f, 0.78f, 0.12f, 1.f};

    // Raw current SC step index = visual playhead position
    int visualPlayhead = (ti < (int)currentStep.size())
                         ? std::max(0, std::min(currentStep[ti], ns - 1))
                         : 0;

    // Step buttons — click+drag paints on/off across multiple steps.
    // stepPaintTrack tracks which track owns the active gesture so painting
    // on track N does not bleed into track M in the same frame.
    {
        bool mouseDown  = ImGui::IsMouseDown(0);
        bool stepChanged = false;

        for(int si = 0; si < ns; si++) {
            if(si > 0) ImGui::SameLine(0, STEP_GAP);  // uniform gap everywhere

            int ai = ((si - td.shift) % ns + ns) % ns;
            bool isOn       = (ai < (int)td.stepOn.size()) ? td.stepOn[ai] : false;
            bool isPlayhead = (si == visualPlayhead);
            int  beatGroup  = (si / tc.stepsPerBeat) % 2;

            ImVec2 pos = ImGui::GetCursorScreenPos();
            std::string bid = "##s" + ofToString(si);
            ImGui::InvisibleButton(bid.c_str(), ImVec2(sw, STEP_H));

            // Gesture start: latch paint value and claim this track
            if(ImGui::IsItemActive() && stepPaintTrack == -1) {
                stepPaintTrack = ti;
                stepPaintValue = !isOn;
            }

            // Paint only when this track owns the gesture
            if(stepPaintTrack == ti && mouseDown) {
                float mouseX = ImGui::GetIO().MousePos.x;
                if(mouseX >= pos.x && mouseX < pos.x + sw) {
                    if(ai < (int)td.stepOn.size() && td.stepOn[ai] != stepPaintValue) {
                        td.stepOn[ai] = stepPaintValue;
                        stepChanged = true;
                    }
                }
            }
            isOn = (ai < (int)td.stepOn.size()) ? td.stepOn[ai] : false;

            // ── Manual draw ───────────────────────────────────────────────────
            bool hovered  = ImGui::IsItemHovered();
            ImVec4 btnCol = isOn ? beatPalette[beatGroup].on : beatPalette[beatGroup].off;

            if(isPlayhead) btnCol = playheadCol;
            if(hovered) {
                btnCol.x = std::min(1.0f, btnCol.x + 0.08f);
                btnCol.y = std::min(1.0f, btnCol.y + 0.08f);
                btnCol.z = std::min(1.0f, btnCol.z + 0.08f);
            }

            ImU32  fillCol = ImGui::ColorConvertFloat4ToU32(btnCol);
            ImVec2 bmax    = ImVec2(pos.x + sw, pos.y + STEP_H);
            // Rounded corners, no stroke
            dl->AddRectFilled(pos, bmax, fillCol, STEP_ROUND);
            // Thin white playhead bar at top
            if(isPlayhead)
                dl->AddRectFilled(pos,
                    ImVec2(bmax.x, pos.y + 3.0f),
                    IM_COL32(255, 255, 255, 220), STEP_ROUND);
        }

        if(!mouseDown && stepPaintTrack == ti) stepPaintTrack = -1;
        if(stepChanged) sendStepDataToAll(ti);
    }

    ImGui::Spacing();

    // ── Tab row ───────────────────────────────────────────────────────────────
    // 0:VOL  1:PROB  2:PAN  3:CUT  4:RES  5:PITCH  6:WAV  7:ENV  8:EQ  9:EUC  10:REV  11:×
    const char* tabLabels[] = { "VOL","PROB","PAN","CUT","RES","PITCH","WAV","ENV","EQ","EUC","REV","\xc3\x97" };
    const int   nTabs = 12;
    const ImVec2 tabSz = {44.f, 23.f};
    for(int t = 0; t < nTabs; t++) {
        if(t > 0) ImGui::SameLine(0, 3);
        bool isActive = (t < nTabs - 1 && td.activeTab == t);
        ImGui::PushStyleColor(ImGuiCol_Button,
            isActive ? ImVec4(acc.x*0.38f, acc.y*0.38f, acc.z*0.38f, 0.95f)
                     : ImVec4(0.16f, 0.18f, 0.23f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            isActive ? ImVec4(acc.x*0.52f, acc.y*0.52f, acc.z*0.52f, 1.f)
                     : ImVec4(0.23f, 0.26f, 0.34f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
            ImVec4(acc.x*0.62f, acc.y*0.62f, acc.z*0.62f, 1.f));
        bool clicked = ImGui::Button(tabLabels[t], tabSz);
        // Active tab: accent-coloured underline at bottom of button
        if(isActive) {
            ImVec2 bmin = ImGui::GetItemRectMin();
            ImVec2 bmax = ImGui::GetItemRectMax();
            dl->AddRectFilled({bmin.x + 4.f, bmax.y - 3.f}, {bmax.x - 4.f, bmax.y - 1.f}, accU32);
        }
        ImGui::PopStyleColor(3);
        if(clicked) {
            if(t == nTabs - 1)         td.activeTab = -1;
            else if(td.activeTab == t) td.activeTab = -1;
            else                       td.activeTab = t;
        }
    }

    // ── Parameter slider rows (tabs 0-4: VOL PROB PAN CUT RES) ─────────────────
    if(td.activeTab >= 0 && td.activeTab <= 4) {
        struct SliderCfg { std::vector<float>* vec; float mn, mx, def; bool bipolar; };
        td.stepVol  .resize(ns, 1.0f);
        td.stepProb .resize(ns, 1.0f);
        td.stepPan  .resize(ns, 0.0f);
        td.stepCut  .resize(ns, 0.0f);
        td.stepRes  .resize(ns, 0.0f);

        SliderCfg cfgs[5] = {
            { &td.stepVol,  0.0f, 1.0f, 1.0f, false },
            { &td.stepProb, 0.0f, 1.0f, 1.0f, false },
            { &td.stepPan, -1.0f, 1.0f, 0.0f, true  },
            { &td.stepCut, -1.0f, 1.0f, 0.0f, true  },
            { &td.stepRes,  0.0f, 1.0f, 0.0f, false },
        };
        auto& cfg = cfgs[td.activeTab];

        // ── Global control row (VOL tab = track volume, PROB tab = track prob) ──
        if(td.activeTab == 0) {
            ImGui::Spacing();
            ImGui::TextUnformatted("Vol:");
            ImGui::SameLine(0, 4);
            ImGui::SetNextItemWidth(120.0f);
            float gv = tc.globalVol;
            if(ImGui::SliderFloat("##gvol", &gv, 0.0f, 1.0f, "%.2f")) {
                tc.globalVol = gv;
                auto v = globalVolP.get(); v.resize(numTracks, 1.0f);
                v[ti] = gv; globalVolP.set(v);  // fires listener → SC
            }
            if(ImGui::IsItemHovered())
                ImGui::SetTooltip("Track output volume (0..1)\nAlso editable via the Vol node parameter.");

            // ── Euclidean Accents ────────────────────────────────────────────
            ImGui::SameLine(0, 16);
            ImGui::TextUnformatted("Accents:");
            ImGui::SameLine(0, 4); ImGui::TextUnformatted("Steps:");
            ImGui::SameLine(0, 4); ImGui::SetNextItemWidth(40.0f);
            if(ImGui::DragInt("##as", &tc.accentSteps, 0.2f, 1, MAX_STEPS))
                tc.accentSteps = ofClamp(tc.accentSteps, 1, MAX_STEPS);
            if(ImGui::IsItemHovered())
                ImGui::SetTooltip("Pattern length. Cut or repeated to fill sequence steps.");
            ImGui::SameLine(0, 8); ImGui::TextUnformatted("Offset:");
            ImGui::SameLine(0, 4); ImGui::SetNextItemWidth(40.0f);
            if(ImGui::DragInt("##ao", &tc.accentOffset, 0.2f, 0, std::max(tc.accentSteps - 1, 0)))
                tc.accentOffset = ofClamp(tc.accentOffset, 0, std::max(tc.accentSteps - 1, 0));
            if(ImGui::IsItemHovered())
                ImGui::SetTooltip("Rotation offset applied to the accent pattern before filling.");
            ImGui::SameLine(0, 8); ImGui::TextUnformatted("Accents:");
            ImGui::SameLine(0, 4); ImGui::SetNextItemWidth(40.0f);
            if(ImGui::DragInt("##ak", &tc.accentPulses, 0.2f, 0, std::max(tc.accentSteps, 1)))
                tc.accentPulses = ofClamp(tc.accentPulses, 0, std::max(tc.accentSteps, 1));
            ImGui::SameLine(0, 8); ImGui::TextUnformatted("Hi:");
            ImGui::SameLine(0, 4); ImGui::SetNextItemWidth(50.0f);
            ImGui::DragFloat("##ahi", &tc.accentHi, 0.01f, 0.0f, 1.0f, "%.2f");
            ImGui::SameLine(0, 8); ImGui::TextUnformatted("Lo:");
            ImGui::SameLine(0, 4); ImGui::SetNextItemWidth(50.0f);
            ImGui::DragFloat("##alo", &tc.accentLo, 0.01f, 0.0f, 1.0f, "%.2f");
            ImGui::SameLine(0, 8); ImGui::TextUnformatted("BeatVar:");
            ImGui::SameLine(0, 4); ImGui::SetNextItemWidth(50.0f);
            ImGui::DragFloat("##abv", &tc.accentBeatVar, 0.005f, 0.0f, 0.3f, "%.3f");
            if(ImGui::IsItemHovered())
                ImGui::SetTooltip("+BeatVar on downbeat, -BeatVar on last subdivision.");
            ImGui::SameLine(0, 8);
            if(ImGui::SmallButton("Fill Accents")) {
                int patLen = std::max(tc.accentSteps, 1);
                auto accented = euclideanRhythm(tc.accentPulses, patLen);
                td.stepVol.resize(ns, 1.0f);
                int spb = std::max(tc.stepsPerBeat, 1);
                for(int si = 0; si < ns; si++) {
                    int pi = ((si - tc.accentOffset) % patLen + patLen) % patLen;
                    float base = accented[pi] ? tc.accentHi : tc.accentLo;
                    float posInBeat = (float)(si % spb);
                    float beatMod = (spb > 1) ? tc.accentBeatVar * (1.0f - 2.0f * posInBeat / (float)(spb - 1)) : 0.0f;
                    td.stepVol[si] = ofClamp(base + beatMod, 0.0f, 1.0f);
                }
                sendStepDataToAll(ti);
            }
            if(ImGui::IsItemHovered())
                ImGui::SetTooltip("Fill VOL sliders with euclidean accent pattern + beat gradient.");
        }
        if(td.activeTab == 1) {
            ImGui::Spacing();
            ImGui::TextUnformatted("Prob:");
            ImGui::SameLine(0, 4);
            ImGui::SetNextItemWidth(120.0f);
            float gp = tc.globalProb;
            if(ImGui::SliderFloat("##gprob", &gp, 0.0f, 1.0f, "%.2f")) {
                tc.globalProb = gp;
                auto v = globalProbP.get(); v.resize(numTracks, 1.0f);
                v[ti] = gp; globalProbP.set(v);  // fires listener → SC
            }
            if(ImGui::IsItemHovered())
                ImGui::SetTooltip("Track probability multiplier (0..1)\nMultiplies all per-step probabilities.\nAlso editable via the Prob node parameter.");
        }

        bool sliderChanged = false;
        ImGui::Spacing();

        for(int si = 0; si < ns; si++) {
            if(si > 0) ImGui::SameLine(0, STEP_GAP);
            int pai = ((si - td.shift) % ns + ns) % ns;
            float& val = (*cfg.vec)[pai];

            ImVec2 pos = ImGui::GetCursorScreenPos();
            std::string sid = "##sl" + ofToString(si);
            ImGui::InvisibleButton(sid.c_str(), ImVec2(sw, PARAM_H));

            // Click+drag to set value — paints any column the mouse X overlaps.
            // sliderPaintTrack ensures only the track that owns the gesture is painted.
            bool mouseDown = ImGui::IsMouseDown(0);
            if(ImGui::IsItemActive() && sliderPaintTrack == -1)
                sliderPaintTrack = ti;
            if(sliderPaintTrack == ti && mouseDown) {
                float mouseX = ImGui::GetIO().MousePos.x;
                float mouseY = ImGui::GetIO().MousePos.y;
                if(mouseX >= pos.x && mouseX < pos.x + sw) {
                    float t = 1.0f - ofClamp((mouseY - pos.y) / PARAM_H, 0.0f, 1.0f);
                    val = cfg.mn + t * (cfg.mx - cfg.mn);
                    sliderChanged = true;
                }
            }
            // Right-click to reset
            if(ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)) {
                val = cfg.def;
                sliderChanged = true;
            }

            // Background: beat-group off-color; playhead column = golden yellow
            int  slBeatGroup = (si / tc.stepsPerBeat) % 2;
            bool slIsPlayhead = (si == visualPlayhead);
            ImVec4 bgCol4 = slIsPlayhead ? playheadCol : beatPalette[slBeatGroup].off;
            ImU32  bgCol  = ImGui::ColorConvertFloat4ToU32(bgCol4);
            ImVec2 bmax   = ImVec2(pos.x + sw, pos.y + PARAM_H);
            dl->AddRectFilled(pos, bmax, bgCol, STEP_ROUND);
            // White top bar on playhead column (same as step buttons)
            if(slIsPlayhead)
                dl->AddRectFilled(pos, ImVec2(bmax.x, pos.y + 3.0f),
                                  IM_COL32(255, 255, 255, 220), STEP_ROUND);

            // Draw bar (no outline)
            float t = (val - cfg.mn) / (cfg.mx - cfg.mn);
            ImU32 barCol = slIsPlayhead
                           ? IM_COL32(255, 255, 255, 180)   // white bar on playhead
                           : ImGui::ColorConvertFloat4ToU32(beatPalette[slBeatGroup].on);
            if(cfg.bipolar) {
                float midY  = pos.y + PARAM_H * 0.5f;
                float valY  = pos.y + PARAM_H * (1.0f - t);
                float top   = std::min(valY, midY);
                float bot   = std::max(valY, midY);
                if(bot > top + 0.5f)
                    dl->AddRectFilled(ImVec2(pos.x + 1, top),
                                      ImVec2(pos.x + sw - 1, bot), barCol);
                // Center line
                dl->AddLine(ImVec2(pos.x, midY), ImVec2(pos.x + sw, midY),
                            IM_COL32(100, 100, 100, 120));
            } else {
                float barH = PARAM_H * t;
                if(barH > 0.5f)
                    dl->AddRectFilled(
                        ImVec2(pos.x + 1, pos.y + PARAM_H - barH),
                        ImVec2(pos.x + sw - 1, pos.y + PARAM_H),
                        barCol);
            }
        }

        if(sliderChanged) sendStepDataToAll(ti);
    }

    // ── PITCH tab (tab 5) ────────────────────────────────────────────────────
    if(td.activeTab == 5) {
        td.stepPitch.resize(ns, 0);

        ImGui::Spacing();

        // Track transpose control (mirrors node Transpose parameter)
        ImGui::TextUnformatted("Transpose:");
        ImGui::SameLine(0, 4);
        ImGui::SetNextItemWidth(80.0f);
        {
            float tp = tc.trackPitch;
            if(ImGui::DragFloat("##tpose", &tp, 0.1f, -24.0f, 24.0f, "%.1f st")) {
                auto v = transposeP.get(); v.resize(numTracks, 0.0f);
                v[ti] = tp; transposeP.set(v);  // fires listener → updates TrackConfig + SC
            }
        }
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Track transpose (-24..+24 semitones, float)\nMirrors the Transpose node parameter.\nPer-step pitch offsets stack on top of this.");

        ImGui::SameLine(0, 12);
        ImGui::TextDisabled("Per-step: right-click to reset to 0");

        ImGui::Spacing();

        bool pitchChanged = false;

        for(int si = 0; si < ns; si++) {
            if(si > 0) ImGui::SameLine(0, STEP_GAP);
            int pai = ((si - td.shift) % ns + ns) % ns;
            int& val = td.stepPitch[pai];

            ImVec2 pos = ImGui::GetCursorScreenPos();
            std::string sid = "##sp" + ofToString(si);
            ImGui::InvisibleButton(sid.c_str(), ImVec2(sw, PARAM_H));

            // Click+drag: map Y to semitone integer
            bool mouseDown = ImGui::IsMouseDown(0);
            if(ImGui::IsItemActive() && sliderPaintTrack == -1)
                sliderPaintTrack = ti;
            if(sliderPaintTrack == ti && mouseDown) {
                float mouseX = ImGui::GetIO().MousePos.x;
                float mouseY = ImGui::GetIO().MousePos.y;
                if(mouseX >= pos.x && mouseX < pos.x + sw) {
                    float t    = 1.0f - ofClamp((mouseY - pos.y) / PARAM_H, 0.0f, 1.0f);
                    int newVal = (int)std::round(-12.0f + t * 24.0f);
                    newVal     = ofClamp(newVal, -12, 12);
                    if(newVal != val) { val = newVal; pitchChanged = true; }
                }
            }
            // Right-click to reset
            if(ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)) {
                val = 0; pitchChanged = true;
            }

            // Background: beat-group off-color; playhead column = golden yellow
            int  ptBeatGroup = (si / tc.stepsPerBeat) % 2;
            bool ptIsPlayhead = (si == visualPlayhead);
            ImVec4 ptBgCol4 = ptIsPlayhead ? playheadCol : beatPalette[ptBeatGroup].off;
            ImU32  ptBgCol  = ImGui::ColorConvertFloat4ToU32(ptBgCol4);
            ImVec2 ptBmax   = ImVec2(pos.x + sw, pos.y + PARAM_H);
            dl->AddRectFilled(pos, ptBmax, ptBgCol, STEP_ROUND);
            if(ptIsPlayhead)
                dl->AddRectFilled(pos, ImVec2(ptBmax.x, pos.y + 3.0f),
                                  IM_COL32(255, 255, 255, 220), STEP_ROUND);

            // Center line (= semitone 0)
            float midY = pos.y + PARAM_H * 0.5f;
            dl->AddLine(ImVec2(pos.x, midY), ImVec2(pos.x + sw, midY),
                        IM_COL32(100, 100, 100, 120));

            // Bar from center to value (no outline)
            if(val != 0) {
                float t    = (float)(val + 12) / 24.0f;
                float valY = pos.y + PARAM_H * (1.0f - t);
                float top  = std::min(valY, midY);
                float bot  = std::max(valY, midY);
                ImU32 barCol = ptIsPlayhead
                               ? IM_COL32(255, 255, 255, 200)
                               : ImGui::ColorConvertFloat4ToU32(beatPalette[ptBeatGroup].on);
                if(bot > top + 0.5f)
                    dl->AddRectFilled(ImVec2(pos.x + 1, top),
                                      ImVec2(pos.x + sw - 1, bot), barCol);
            }

            // Value label centred in bar (only when wide enough)
            if(sw >= 14.0f && val != 0) {
                char lbl[8];
                snprintf(lbl, sizeof(lbl), "%+d", val);
                ImVec2 ts = ImGui::CalcTextSize(lbl);
                if(ts.x < sw - 2.0f) {
                    dl->AddText(ImVec2(pos.x + (sw - ts.x) * 0.5f,
                                       midY - ts.y * 0.5f),
                                IM_COL32(230, 230, 230, 220), lbl);
                }
            }
        }

        if(pitchChanged) sendStepDataToAll(ti);

        ImGui::Spacing();
    }

    // ── WAV tab (tab 6) ──────────────────────────────────────────────────────
    if(td.activeTab == 6) {
        // Compute the waveform display width to match the step-button grid
        int numBeatGaps = (ns > 1) ? (ns - 1) / tc.stepsPerBeat : 0;
        int numStepGaps = (ns > 1) ? (ns - 1) - numBeatGaps      : 0;
        float wavW = (float)ns * sw
                     + (float)numBeatGaps * 4.0f
                     + (float)numStepGaps * 1.0f;
        wavW = std::max(wavW, 120.0f);
        const float wavH = 80.0f;

        ImGui::Spacing();
        ImVec2 wavPos = ImGui::GetCursorScreenPos();

        // ── InvisibleButton captures mouse ───────────────────────────────────
        ImGui::InvisibleButton("##wav", ImVec2(wavW, wavH));
        bool wavActive  = ImGui::IsItemActive();
        bool wavHovered = ImGui::IsItemHovered();

        // Background
        dl->AddRectFilled(wavPos, ImVec2(wavPos.x + wavW, wavPos.y + wavH),
                          IM_COL32(28, 28, 28, 255));

        // Waveform bars (or placeholder when no sample is loaded)
        if(!waveformPeaks[ti].empty()) {
            int   bins  = (int)waveformPeaks[ti].size();
            float midY  = wavPos.y + wavH * 0.5f;
            for(int px = 0; px < (int)wavW; px++) {
                int bidx  = (int)((float)px / wavW * (float)bins);
                bidx      = std::min(std::max(bidx, 0), bins - 1);
                float hh  = waveformPeaks[ti][bidx] * wavH * 0.5f;
                if(hh < 0.5f) continue;
                dl->AddLine(ImVec2(wavPos.x + px, midY - hh),
                            ImVec2(wavPos.x + px, midY + hh),
                            IM_COL32(90, 170, 210, 200));
            }
        } else {
            // No sample loaded
            float midY = wavPos.y + wavH * 0.5f;
            dl->AddLine(ImVec2(wavPos.x, midY),
                        ImVec2(wavPos.x + wavW, midY),
                        IM_COL32(80, 80, 80, 180));
            const char* msg = "no sample";
            dl->AddText(ImVec2(wavPos.x + wavW * 0.5f - 28, midY - 7),
                        IM_COL32(110, 110, 110, 255), msg);
        }

        // ── Grid lines ───────────────────────────────────────────────────────
        if(tc.wavGrid > 0) {
            for(int g = 1; g < tc.wavGrid; g++) {
                float gx = wavPos.x + (float)g / (float)tc.wavGrid * wavW;
                dl->AddLine(ImVec2(gx, wavPos.y), ImVec2(gx, wavPos.y + wavH),
                            IM_COL32(160, 160, 160, 80), 1.0f);
            }
        }

        // Darken region outside in..out
        float inX  = wavPos.x + tc.inPoint  * wavW;
        float outX = wavPos.x + tc.outPoint * wavW;
        dl->AddRectFilled(wavPos,
                          ImVec2(inX,  wavPos.y + wavH), IM_COL32(0, 0, 0, 110));
        dl->AddRectFilled(ImVec2(outX, wavPos.y),
                          ImVec2(wavPos.x + wavW, wavPos.y + wavH), IM_COL32(0, 0, 0, 110));

        // In marker (green)
        dl->AddLine(ImVec2(inX, wavPos.y), ImVec2(inX, wavPos.y + wavH),
                    IM_COL32(80, 220, 100, 255), 2.0f);
        dl->AddTriangleFilled(
            ImVec2(inX - 5.0f, wavPos.y),
            ImVec2(inX + 5.0f, wavPos.y),
            ImVec2(inX,        wavPos.y + 10.0f),
            IM_COL32(80, 220, 100, 255));

        // Out marker (orange)
        dl->AddLine(ImVec2(outX, wavPos.y), ImVec2(outX, wavPos.y + wavH),
                    IM_COL32(220, 140, 60, 255), 2.0f);
        dl->AddTriangleFilled(
            ImVec2(outX - 5.0f, wavPos.y),
            ImVec2(outX + 5.0f, wavPos.y),
            ImVec2(outX,        wavPos.y + 10.0f),
            IM_COL32(220, 140, 60, 255));

        // Border
        dl->AddRect(wavPos, ImVec2(wavPos.x + wavW, wavPos.y + wavH),
                    IM_COL32(70, 70, 70, 200));

        // ── Drag interaction ─────────────────────────────────────────────────
        if(wavActive) {
            float mouseX = ImGui::GetIO().MousePos.x;
            float norm   = std::min(std::max((mouseX - wavPos.x) / wavW, 0.0f), 1.0f);

            // Snap to grid
            if(tc.wavGrid > 0) {
                float bestDist = 1e9f;
                for(int g = 0; g <= tc.wavGrid; g++) {
                    float gn = (float)g / (float)tc.wavGrid;
                    float d  = std::abs(norm - gn);
                    if(d < bestDist) { bestDist = d; norm = gn; }
                }
            }

            if(wavDragMode == WavDrag::None) {
                float distIn  = std::abs(mouseX - inX);
                float distOut = std::abs(mouseX - outX);
                wavDragMode   = (distIn <= distOut) ? WavDrag::In : WavDrag::Out;
            }

            bool changed = false;
            if(wavDragMode == WavDrag::In) {
                float newIn = std::min(norm, tc.outPoint - 0.01f);
                if(newIn != tc.inPoint) { tc.inPoint = newIn; changed = true; }
            } else {
                float newOut = std::max(norm, tc.inPoint + 0.01f);
                if(newOut != tc.outPoint) { tc.outPoint = newOut; changed = true; }
            }

            if(changed) {
                for(auto& [srv, synths] : trackSynths) {
                    if(ti < (int)synths.size() && synths[ti]) {
                        synths[ti]->set("inPoint",  tc.inPoint);
                        synths[ti]->set("outPoint", tc.outPoint);
                    }
                }
            }
        } else {
            wavDragMode = WavDrag::None;
        }

        if(wavHovered)
            ImGui::SetTooltip("In: %.3f  Out: %.3f", tc.inPoint, tc.outPoint);

        // ── Controls row ─────────────────────────────────────────────────────
        ImGui::Spacing();

        // LOOP toggle
        {
            bool loop = tc.loopEnabled;
            if(loop) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.25f, 1.0f));
            if(ImGui::SmallButton(loop ? "LOOP ON" : "LOOP")) {
                tc.loopEnabled = !tc.loopEnabled;
                for(auto& [srv, synths] : trackSynths)
                    if(ti < (int)synths.size() && synths[ti])
                        synths[ti]->set("loopSample", tc.loopEnabled ? 1.0f : 0.0f);
            }
            if(loop) ImGui::PopStyleColor();
        }

        // FIXDUR toggle
        ImGui::SameLine(0, 8);
        {
            bool fix = tc.fixDuration;
            if(fix) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.50f, 0.30f, 0.55f, 1.0f));
            if(ImGui::SmallButton(fix ? "FIXDUR ON" : "FIXDUR")) {
                tc.fixDuration = !tc.fixDuration;
                for(auto& [srv, synths] : trackSynths)
                    if(ti < (int)synths.size() && synths[ti])
                        synths[ti]->set("fixDuration", tc.fixDuration ? 1.0f : 0.0f);
            }
            if(fix) ImGui::PopStyleColor();
        }

        // Beats DragFloat
        ImGui::SameLine(0, 8);
        ImGui::TextUnformatted("Beats:");
        ImGui::SameLine(0, 4);
        ImGui::SetNextItemWidth(64.0f);
        float beats = tc.durationBeats;
        if(ImGui::DragFloat("##beats", &beats, 0.01f, 0.125f, 64.0f, "%.3f")) {
            tc.durationBeats = std::max(beats, 0.125f);
            for(auto& [srv, synths] : trackSynths)
                if(ti < (int)synths.size() && synths[ti])
                    synths[ti]->set("durationBeats", tc.durationBeats);
        }

        // Grid DragInt
        ImGui::SameLine(0, 8);
        ImGui::TextUnformatted("Grid:");
        ImGui::SameLine(0, 4);
        ImGui::SetNextItemWidth(48.0f);
        int wg = tc.wavGrid;
        if(ImGui::DragInt("##wavgrid", &wg, 0.1f, 0, 64, wg == 0 ? "off" : "%d"))
            tc.wavGrid = std::max(wg, 0);

        ImGui::Spacing();
    }

    // ── ENV tab (tab 7) ──────────────────────────────────────────────────────
    if(td.activeTab == 7) {
        ImGui::Spacing();

        // ── Enable toggle ─────────────────────────────────────────────────────
        {
            bool en = tc.envEnabled;
            if(en) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.45f, 0.15f, 1.0f));
            if(ImGui::SmallButton(en ? "ENV ON" : "ENV OFF")) {
                tc.envEnabled = !tc.envEnabled;
                for(auto& [srv, synths] : trackSynths)
                    if(ti < (int)synths.size() && synths[ti])
                        synths[ti]->set("envEnabled", tc.envEnabled ? 1.0f : 0.0f);
            }
            if(en) ImGui::PopStyleColor();
        }

        // ── Envelope shape visualisation ─────────────────────────────────────
        ImGui::Spacing();

        int numBeatGaps = (ns > 1) ? (ns - 1) / tc.stepsPerBeat : 0;
        int numStepGaps = (ns > 1) ? (ns - 1) - numBeatGaps      : 0;
        float envW = std::max((float)ns * sw
                              + (float)numBeatGaps * 4.0f
                              + (float)numStepGaps * 1.0f, 120.0f);
        const float envH = 64.0f;

        ImVec2 envPos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(envW, envH));

        // Background
        dl->AddRectFilled(envPos, ImVec2(envPos.x + envW, envPos.y + envH),
                          IM_COL32(28, 28, 28, 255));

        // Compute pixel positions for each segment boundary
        float tA = tc.envAttack;
        float tH = tc.envHold;
        float tD = tc.envDecay;
        float total = std::max(tA + tH + tD, 0.001f);

        float x0 = envPos.x;
        float x1 = envPos.x + (tA         / total) * envW;           // end attack
        float x2 = envPos.x + ((tA + tH)  / total) * envW;           // end hold
        float x3 = envPos.x + envW;                                    // end decay (= envelope end)
        float yTop = envPos.y + 6.0f;
        float yBot = envPos.y + envH - 6.0f;

        ImU32 fillCol = tc.envEnabled
                        ? IM_COL32(180, 130, 50, 55)
                        : IM_COL32(80,  80,  80, 35);
        ImU32 lineCol = tc.envEnabled
                        ? IM_COL32(220, 170, 70, 230)
                        : IM_COL32(110, 110, 110, 160);
        ImU32 segCol  = IM_COL32(90, 90, 90, 100);

        // SC-compatible curve interpolation: (exp(c*t)-1)/(exp(c)-1), fallback linear
        auto evalCurve = [](float t, float c) -> float {
            if(std::fabsf(c) < 0.001f) return t;
            return (std::expf(c * t) - 1.0f) / (std::expf(c) - 1.0f);
        };

        // Build a multi-point outline (attack + hold + decay + release).
        // CN = number of interior sample points per curved segment (excluding shared endpoints).
        constexpr int CN = 20;
        std::vector<ImVec2> outline;
        outline.reserve(CN + 1 + CN + 1);   // A(CN) + hold-end(1) + D(CN-1 extra) + R-end(1)

        // Attack: yBot → yTop, curve = envCurveA
        for(int i = 0; i < CN; i++) {
            float t  = float(i) / float(CN - 1);
            float ct = evalCurve(t, tc.envCurveA);
            outline.push_back({ x0 + (x1 - x0) * t,
                                 yBot + (yTop - yBot) * ct });
        }
        // Hold: flat at yTop, endpoint only (start shared with last attack point)
        outline.push_back({ x2, yTop });

        // Decay: yTop → yBot, curve = envCurveD (skip i=0, shared with hold end)
        for(int i = 1; i < CN; i++) {
            float t  = float(i) / float(CN - 1);
            float ct = evalCurve(t, tc.envCurveD);
            outline.push_back({ x2 + (x3 - x2) * t,
                                 yTop + (yBot - yTop) * ct });
        }
        // (No Release segment — envelope always ends at amplitude 0)

        // Fill: outline + baseline back to start
        std::vector<ImVec2> fillPoly = outline;
        fillPoly.push_back({ x0, yBot });
        dl->AddConvexPolyFilled(fillPoly.data(), (int)fillPoly.size(), fillCol);

        // Outline polyline
        dl->AddPolyline(outline.data(), (int)outline.size(), lineCol, false, 2.0f);

        // Segment boundary ticks (A-end, H-end only; D-end is the right edge)
        for(float xTick : {x1, x2}) {
            dl->AddLine(ImVec2(xTick, envPos.y),
                        ImVec2(xTick, envPos.y + envH), segCol, 1.0f);
        }

        // Segment labels (A H D centred in each segment)
        auto drawSegLabel = [&](float xFrom, float xTo, const char* lbl) {
            float cx = (xFrom + xTo) * 0.5f;
            ImVec2 ts = ImGui::CalcTextSize(lbl);
            if(xTo - xFrom > ts.x + 4.0f)
                dl->AddText(ImVec2(cx - ts.x * 0.5f, envPos.y + 4.0f),
                            IM_COL32(180, 180, 180, 200), lbl);
        };
        drawSegLabel(x0, x1, "A");
        drawSegLabel(x1, x2, "H");
        drawSegLabel(x2, x3, "D");

        // Border
        dl->AddRect(envPos, ImVec2(envPos.x + envW, envPos.y + envH),
                    IM_COL32(70, 70, 70, 200));

        // ── A / H / D / R DragFloat controls ─────────────────────────────────
        ImGui::Spacing();

        struct EnvSeg { const char* label; const char* id; float* val; float lo; float hi; };
        EnvSeg segs[3] = {
            { "A", "##envA", &tc.envAttack, 0.001f, 8.0f },
            { "H", "##envH", &tc.envHold,   0.000f, 8.0f },
            { "D", "##envD", &tc.envDecay,  0.001f, 8.0f },
        };

        bool envChanged = false;
        for(int e = 0; e < 3; e++) {
            if(e > 0) ImGui::SameLine(0, 8);
            ImGui::TextUnformatted(segs[e].label);
            ImGui::SameLine(0, 4);
            ImGui::SetNextItemWidth(68.0f);
            if(ImGui::DragFloat(segs[e].id, segs[e].val,
                                0.001f, segs[e].lo, segs[e].hi, "%.3fs"))
                envChanged = true;
        }

        if(envChanged) {
            for(auto& [srv, synths] : trackSynths) {
                if(ti < (int)synths.size() && synths[ti]) {
                    synths[ti]->set("envAttack", tc.envAttack);
                    synths[ti]->set("envHold",   tc.envHold);
                    synths[ti]->set("envDecay",  tc.envDecay);
                }
            }
        }

        // ── Curve tension row (A and D only; H=1→1 and R=0→0 are flat) ────────
        ImGui::Spacing();

        struct CurveSeg { const char* label; const char* id; float* val; };
        CurveSeg curveSegs[2] = {
            { "~A", "##crvA", &tc.envCurveA },
            { "~D", "##crvD", &tc.envCurveD },
        };

        bool curveChanged = false;
        // Align ~A under A and ~D under D by offsetting with the same widths
        // A col: label + 4 + 68 + 8 = ~90px.  H col: same ~90px. So ~D starts at 2*90.
        float colW = ImGui::CalcTextSize("A").x + 4.0f + 68.0f + 8.0f;
        for(int c = 0; c < 2; c++) {
            if(c == 0) {
                // no indent — ~A aligns with A
            } else {
                // ~D: skip H column width to align with D
                ImGui::SameLine(0, colW + 8.0f);
            }
            ImGui::TextUnformatted(curveSegs[c].label);
            ImGui::SameLine(0, 4);
            ImGui::SetNextItemWidth(68.0f);
            if(ImGui::DragFloat(curveSegs[c].id, curveSegs[c].val,
                                0.05f, -8.0f, 8.0f, "%.2f"))
                curveChanged = true;
        }

        if(curveChanged) {
            for(auto& [srv, synths] : trackSynths) {
                if(ti < (int)synths.size() && synths[ti]) {
                    synths[ti]->set("envCurveA", tc.envCurveA);
                    synths[ti]->set("envCurveD", tc.envCurveD);
                }
            }
        }

        ImGui::Spacing();
    }

    // ── EQ tab (tab 8) ───────────────────────────────────────────────────────
    if(td.activeTab == 8) {
        ImGui::Spacing();

        // Enable toggle
        {
            bool en = tc.eqEnabled;
            if(en) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
            if(ImGui::SmallButton(en ? "EQ ON" : "EQ OFF")) {
                tc.eqEnabled = !tc.eqEnabled;
                for(auto& [srv, synths] : trackSynths)
                    if(ti < (int)synths.size() && synths[ti])
                        synths[ti]->set("eqEnabled", tc.eqEnabled ? 1.0f : 0.0f);
            }
            if(en) ImGui::PopStyleColor();
        }

        // ── Frequency response curve ─────────────────────────────────────────
        ImGui::Spacing();
        float eqW = std::max((float)ns * sw + (float)(ns - 1) * STEP_GAP, 180.0f);
        const float eqH = 64.0f;
        ImVec2 eqPos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(eqW, eqH));

        dl->AddRectFilled(eqPos, ImVec2(eqPos.x + eqW, eqPos.y + eqH),
                          IM_COL32(22, 22, 22, 255));

        // Biquad helpers — all as lambdas
        struct Bq { float b0, b1, b2, a1, a2; };
        auto evalBq = [](const Bq& q, float f, float SR) -> float {
            float w  = 2.0f * (float)M_PI * f / SR;
            float cw = cosf(w), c2w = cosf(2*w);
            float sw = sinf(w), s2w = sinf(2*w);
            float nr = q.b0 + q.b1*cw + q.b2*c2w;
            float ni = q.b1*sw + q.b2*s2w;
            float dr = 1.0f + q.a1*cw + q.a2*c2w;
            float di = q.a1*sw + q.a2*s2w;
            return sqrtf(std::max((nr*nr + ni*ni) / std::max(dr*dr + di*di, 1e-20f), 1e-20f));
        };
        constexpr float EQ_SR = 44100.0f;
        float hpFreq = std::max(tc.eqHPFreq, 20.0f);
        float hpQ    = std::max(tc.eqHPQ,    0.01f);
        {
            float w0 = 2.0f*(float)M_PI*hpFreq/EQ_SR, cw = cosf(w0), sw_ = sinf(w0);
            float alpha = sw_ / (2.0f * hpQ), a0 = 1.0f + alpha;
            Bq hpBq = {(1+cw)*0.5f/a0, -(1+cw)/a0, (1+cw)*0.5f/a0, -2*cw/a0, (1-alpha)/a0};

            float pkFreq = std::max(tc.eqPeakFreq, 20.0f);
            float pkQ    = std::max(tc.eqPeakQ,    0.1f);
            float A = powf(10.0f, tc.eqPeakGain / 40.0f);
            float w0p = 2.0f*(float)M_PI*pkFreq/EQ_SR, cwp = cosf(w0p), swp = sinf(w0p);
            float alphap = swp / (2.0f * pkQ), a0p = 1.0f + alphap / A;
            Bq pkBq = {(1+alphap*A)/a0p, -2*cwp/a0p, (1-alphap*A)/a0p, -2*cwp/a0p, (1-alphap/A)/a0p};

            float lpFreq = std::max(tc.eqLPFreq, 20.0f);
            float lpQ    = std::max(tc.eqLPQ,    0.01f);
            float w0l = 2.0f*(float)M_PI*lpFreq/EQ_SR, cwl = cosf(w0l), swl = sinf(w0l);
            float alphal = swl / (2.0f * lpQ), a0l = 1.0f + alphal;
            Bq lpBq = {(1-cwl)*0.5f/a0l, (1-cwl)/a0l, (1-cwl)*0.5f/a0l, -2*cwl/a0l, (1-alphal)/a0l};

            constexpr int EQ_PTS  = 200;
            constexpr float FMIN  = 20.0f, FMAX = 20000.0f;
            constexpr float DB_RANGE = 18.0f;
            float midY = eqPos.y + eqH * 0.5f;

            // 0 dB grid line
            dl->AddLine(ImVec2(eqPos.x, midY), ImVec2(eqPos.x + eqW, midY),
                        IM_COL32(60, 60, 60, 150));

            // Frequency grid lines at 100, 1k, 10kHz
            for(float fg : {100.0f, 1000.0f, 10000.0f}) {
                float gx = eqPos.x + logf(fg/FMIN) / logf(FMAX/FMIN) * eqW;
                dl->AddLine(ImVec2(gx, eqPos.y), ImVec2(gx, eqPos.y + eqH),
                            IM_COL32(50, 50, 50, 120));
            }

            // Draw curve
            std::vector<ImVec2> pts;
            pts.reserve(EQ_PTS);
            for(int pi = 0; pi < EQ_PTS; pi++) {
                float t  = (float)pi / (EQ_PTS - 1);
                float f  = FMIN * powf(FMAX / FMIN, t);
                float db = 20.0f * log10f(
                    std::max(evalBq(hpBq, f, EQ_SR) * evalBq(pkBq, f, EQ_SR) * evalBq(lpBq, f, EQ_SR),
                             1e-6f));
                db = ofClamp(db, -DB_RANGE, DB_RANGE);
                float x = eqPos.x + t * eqW;
                float y = midY - (db / DB_RANGE) * (eqH * 0.5f - 4.0f);
                pts.push_back({x, y});
            }
            ImU32 curveCol = tc.eqEnabled
                             ? IM_COL32(100, 210, 120, 230)
                             : IM_COL32(100, 100, 100, 150);
            dl->AddPolyline(pts.data(), (int)pts.size(), curveCol, false, 2.0f);
        }
        dl->AddRect(eqPos, ImVec2(eqPos.x + eqW, eqPos.y + eqH),
                    IM_COL32(70, 70, 70, 200));

        // ── Controls ─────────────────────────────────────────────────────────
        ImGui::Spacing();
        bool eqChanged = false;

        // HP band
        ImGui::TextUnformatted("HP:");
        ImGui::SameLine(0, 4); ImGui::SetNextItemWidth(80.0f);
        if(ImGui::DragFloat("##hpf", &tc.eqHPFreq, 5.0f, 20.0f, 18000.0f, "%.0f Hz")) eqChanged = true;
        ImGui::SameLine(0, 8); ImGui::TextUnformatted("Q:");
        ImGui::SameLine(0, 4); ImGui::SetNextItemWidth(60.0f);
        if(ImGui::DragFloat("##hpq", &tc.eqHPQ, 0.01f, 0.1f, 4.0f, "%.2f")) eqChanged = true;

        // Peak band
        ImGui::SameLine(0, 16); ImGui::TextUnformatted("PK:");
        ImGui::SameLine(0, 4); ImGui::SetNextItemWidth(80.0f);
        if(ImGui::DragFloat("##pkf", &tc.eqPeakFreq, 5.0f, 20.0f, 20000.0f, "%.0f Hz")) eqChanged = true;
        ImGui::SameLine(0, 4); ImGui::SetNextItemWidth(60.0f);
        if(ImGui::DragFloat("##pkg", &tc.eqPeakGain, 0.2f, -24.0f, 24.0f, "%.1f dB")) eqChanged = true;
        ImGui::SameLine(0, 4); ImGui::SetNextItemWidth(60.0f);
        if(ImGui::DragFloat("##pkq", &tc.eqPeakQ, 0.05f, 0.1f, 8.0f, "Q%.2f")) eqChanged = true;

        // LP band
        ImGui::SameLine(0, 16); ImGui::TextUnformatted("LP:");
        ImGui::SameLine(0, 4); ImGui::SetNextItemWidth(80.0f);
        if(ImGui::DragFloat("##lpf", &tc.eqLPFreq, 20.0f, 200.0f, 20000.0f, "%.0f Hz")) eqChanged = true;
        ImGui::SameLine(0, 8); ImGui::TextUnformatted("Q:");
        ImGui::SameLine(0, 4); ImGui::SetNextItemWidth(60.0f);
        if(ImGui::DragFloat("##lpq", &tc.eqLPQ, 0.01f, 0.1f, 4.0f, "%.2f")) eqChanged = true;

        if(eqChanged) {
            for(auto& [srv, synths] : trackSynths) {
                if(ti < (int)synths.size() && synths[ti]) {
                    synths[ti]->set("eqEnabled",  tc.eqEnabled  ? 1.0f : 0.0f);
                    synths[ti]->set("eqHPFreq",   tc.eqHPFreq);
                    synths[ti]->set("eqHPRq",     1.0f / std::max(tc.eqHPQ,   0.01f));
                    synths[ti]->set("eqPeakFreq", tc.eqPeakFreq);
                    synths[ti]->set("eqPeakGain", tc.eqPeakGain);
                    synths[ti]->set("eqPeakRq",   1.0f / std::max(tc.eqPeakQ, 0.01f));
                    synths[ti]->set("eqLPFreq",   tc.eqLPFreq);
                    synths[ti]->set("eqLPRq",     1.0f / std::max(tc.eqLPQ,   0.01f));
                }
            }
        }

        ImGui::Spacing();
    }

    // ── EUC tab (tab 9) ──────────────────────────────────────────────────────
    if(td.activeTab == 9) {
        ImGui::Spacing();

        // Clamp euclSteps to a sensible range
        if(tc.euclSteps < 1) tc.euclSteps = ns;

        ImGui::TextUnformatted("Fills:");
        ImGui::SameLine(0, 4); ImGui::SetNextItemWidth(50.0f);
        if(ImGui::DragInt("##ek", &tc.euclPulses, 0.2f, 0, std::max(tc.euclSteps, 1)))
            tc.euclPulses = ofClamp(tc.euclPulses, 0, std::max(tc.euclSteps, 1));

        ImGui::SameLine(0, 10); ImGui::TextUnformatted("Steps:");
        ImGui::SameLine(0, 4); ImGui::SetNextItemWidth(50.0f);
        if(ImGui::DragInt("##es", &tc.euclSteps, 0.2f, 1, MAX_STEPS))
            tc.euclSteps = ofClamp(tc.euclSteps, 1, MAX_STEPS);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Length of the euclidean pattern.\nIf different from sequence steps, the pattern is cut or repeated.");

        ImGui::SameLine(0, 10); ImGui::TextUnformatted("Offset:");
        ImGui::SameLine(0, 4); ImGui::SetNextItemWidth(50.0f);
        if(ImGui::DragInt("##eo", &tc.euclOffset, 0.2f, 0, std::max(tc.euclSteps - 1, 0)))
            tc.euclOffset = ofClamp(tc.euclOffset, 0, std::max(tc.euclSteps - 1, 0));

        ImGui::SameLine(0, 12);
        if(ImGui::SmallButton("Fill Steps")) {
            int patLen = std::max(tc.euclSteps, 1);
            auto pattern = euclideanRhythm(tc.euclPulses, patLen);
            td.stepOn.resize(ns, false);
            for(int si = 0; si < ns; si++) {
                // Apply offset rotation within the pattern, then cut/repeat to fill ns
                int pi = ((si - tc.euclOffset) % patLen + patLen) % patLen;
                td.stepOn[si] = pattern[pi];
            }
            sendStepDataToAll(ti);
        }
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Distribute %d pulses in a %d-step euclidean pattern,\nthen cut/repeat to fill %d sequence steps.",
                              tc.euclPulses, std::max(tc.euclSteps, 1), ns);

        ImGui::Spacing();
    }

    // ── REV tab (tab 10) ─────────────────────────────────────────────────────
    if(td.activeTab == 10) {
        td.stepReverse.resize(ns, false);

        bool revChanged = false;
        ImGui::Spacing();

        for(int si = 0; si < ns; si++) {
            if(si > 0) ImGui::SameLine(0, STEP_GAP);  // same uniform gap as step buttons

            // Apply shift correction — same as all other tabs so that button at screen
            // position si controls the same data slot that SC reads when stepIdx == si.
            int  ri         = ((si - td.shift) % ns + ns) % ns;
            int  beatGroup  = (si / tc.stepsPerBeat) % 2;
            bool isPlayhead = (si == visualPlayhead);
            bool isRev      = (ri < (int)td.stepReverse.size()) && td.stepReverse[ri];

            ImVec2 pos  = ImGui::GetCursorScreenPos();
            std::string sid = "##rev" + ofToString(si);
            ImGui::InvisibleButton(sid.c_str(), ImVec2(sw, STEP_H));

            // Paint gesture — uses revPaintTrack/revPaintValue, independent of stepPaintTrack
            bool mouseDown = ImGui::IsMouseDown(0);
            if(ImGui::IsItemActivated()) {
                if(revPaintTrack == -1) {
                    revPaintTrack = ti;
                    revPaintValue = !isRev;
                }
            }
            if(revPaintTrack == ti && mouseDown && ImGui::IsItemHovered()) {
                if(ri < (int)td.stepReverse.size() && td.stepReverse[ri] != revPaintValue) {
                    td.stepReverse[ri] = revPaintValue;
                    revChanged = true;
                }
            }
            if(ImGui::IsItemClicked(1)) {
                if(ri < (int)td.stepReverse.size()) td.stepReverse[ri] = false;
                revChanged = true;
            }

            // Draw button
            ImU32 bgCol, textCol;
            if(isRev) {
                bgCol   = IM_COL32(180, 50, 50, 255);   // red when reversed
                textCol = IM_COL32(255, 255, 255, 255);
            } else {
                ImVec4 bg4 = isPlayhead ? playheadCol : beatPalette[beatGroup].off;
                bgCol   = ImGui::ColorConvertFloat4ToU32(bg4);
                textCol = IM_COL32(100, 100, 100, 180);
            }
            ImVec2 bmax = ImVec2(pos.x + sw, pos.y + STEP_H);
            dl->AddRectFilled(pos, bmax, bgCol, STEP_ROUND);
            if(isPlayhead && !isRev)
                dl->AddRectFilled(pos, ImVec2(bmax.x, pos.y + 3.0f),
                                  IM_COL32(255, 255, 255, 220), STEP_ROUND);

            // "R" label in center
            ImVec2 tpos = ImVec2(pos.x + sw * 0.5f - 4.0f, pos.y + STEP_H * 0.5f - 7.0f);
            dl->AddText(tpos, textCol, "R");
        }

        if(!ImGui::IsMouseDown(0) && revPaintTrack == ti) revPaintTrack = -1;
        if(revChanged) sendStepDataToAll(ti);

        ImGui::Spacing();
    }

    // Release slider paint gesture when mouse is released on this track
    if(!ImGui::IsMouseDown(0) && sliderPaintTrack == ti) sliderPaintTrack = -1;

    if(tc.muted) ImGui::EndDisabled();

    // ── Close card ────────────────────────────────────────────────────────────
    ImGui::Unindent(accentBarW + cardPadX);
    ImGui::Dummy({0.f, cardPadBot});
    ImVec2 cardMax = {cardMin.x + cardW, ImGui::GetCursorScreenPos().y};

    splitter.SetCurrentChannel(dl, 0);
    dl->AddRectFilled(cardMin, cardMax, IM_COL32(23, 26, 35, 255), 8.0f);
    dl->AddRectFilled(cardMin, {cardMin.x + accentBarW, cardMax.y}, accU32, 8.0f);
    dl->AddRect(cardMin, cardMax, IM_COL32(46, 50, 64, 200), 8.0f, 0, 1.0f);
    splitter.Merge(dl);

    ImGui::Spacing();
    ImGui::Spacing();
}

// ════════════════════════════════════════════════════════════════════════════
// Preset serialization
// ════════════════════════════════════════════════════════════════════════════

// Serialize/deserialize per-track global config (not per-slot)
static ofJson serializeTrackConfig(const fullStepSequencer::TrackConfig& tc) {
    ofJson j;
    j["name"]          = tc.name;
    j["numBeats"]      = tc.numBeats;
    j["stepsPerBeat"]  = tc.stepsPerBeat;
    j["monoMode"]      = tc.monoMode;
    j["volLatch"]      = tc.volLatch;
    j["inPoint"]       = tc.inPoint;
    j["outPoint"]      = tc.outPoint;
    j["wavGrid"]       = tc.wavGrid;
    j["loopEnabled"]   = tc.loopEnabled;
    j["fixDuration"]   = tc.fixDuration;
    j["durationBeats"] = tc.durationBeats;
    j["envEnabled"]    = tc.envEnabled;
    j["envAttack"]     = tc.envAttack;
    j["envHold"]       = tc.envHold;
    j["envDecay"]      = tc.envDecay;
    j["envCurveA"]     = tc.envCurveA;
    j["envCurveD"]     = tc.envCurveD;
    j["eqEnabled"]     = tc.eqEnabled;
    j["eqHPFreq"]      = tc.eqHPFreq;
    j["eqHPQ"]         = tc.eqHPQ;
    j["eqPeakFreq"]    = tc.eqPeakFreq;
    j["eqPeakGain"]    = tc.eqPeakGain;
    j["eqPeakQ"]       = tc.eqPeakQ;
    j["eqLPFreq"]      = tc.eqLPFreq;
    j["eqLPQ"]         = tc.eqLPQ;
    j["euclPulses"]    = tc.euclPulses;
    j["euclSteps"]     = tc.euclSteps;
    j["euclOffset"]    = tc.euclOffset;
    j["accentPulses"]  = tc.accentPulses;
    j["accentSteps"]   = tc.accentSteps;
    j["accentOffset"]  = tc.accentOffset;
    j["accentHi"]      = tc.accentHi;
    j["accentLo"]      = tc.accentLo;
    j["accentBeatVar"] = tc.accentBeatVar;
    j["trackPitch"]    = tc.trackPitch;
    j["globalVol"]     = tc.globalVol;
    j["globalProb"]    = tc.globalProb;
    j["muted"]         = tc.muted;
    return j;
}

static void deserializeTrackConfig(const ofJson& j, fullStepSequencer::TrackConfig& tc) {
    if(j.contains("name"))          tc.name          = j["name"].get<std::string>();
    if(j.contains("numBeats"))      tc.numBeats      = j["numBeats"].get<int>();
    if(j.contains("stepsPerBeat"))  tc.stepsPerBeat  = j["stepsPerBeat"].get<int>();
    if(j.contains("monoMode"))      tc.monoMode      = j["monoMode"].get<bool>();
    if(j.contains("volLatch"))      tc.volLatch       = j["volLatch"].get<bool>();
    if(j.contains("inPoint"))       tc.inPoint        = j["inPoint"].get<float>();
    if(j.contains("outPoint"))      tc.outPoint       = j["outPoint"].get<float>();
    if(j.contains("wavGrid"))       tc.wavGrid        = j["wavGrid"].get<int>();
    if(j.contains("loopEnabled"))   tc.loopEnabled    = j["loopEnabled"].get<bool>();
    if(j.contains("fixDuration"))   tc.fixDuration    = j["fixDuration"].get<bool>();
    if(j.contains("durationBeats")) tc.durationBeats  = j["durationBeats"].get<float>();
    if(j.contains("envEnabled"))    tc.envEnabled     = j["envEnabled"].get<bool>();
    if(j.contains("envAttack"))     tc.envAttack      = j["envAttack"].get<float>();
    if(j.contains("envHold"))       tc.envHold        = j["envHold"].get<float>();
    if(j.contains("envDecay"))      tc.envDecay       = j["envDecay"].get<float>();
    if(j.contains("envCurveA"))     tc.envCurveA      = j["envCurveA"].get<float>();
    if(j.contains("envCurveD"))     tc.envCurveD      = j["envCurveD"].get<float>();
    if(j.contains("eqEnabled"))     tc.eqEnabled      = j["eqEnabled"].get<bool>();
    if(j.contains("eqHPFreq"))      tc.eqHPFreq       = j["eqHPFreq"].get<float>();
    if(j.contains("eqHPQ"))         tc.eqHPQ          = j["eqHPQ"].get<float>();
    if(j.contains("eqPeakFreq"))    tc.eqPeakFreq     = j["eqPeakFreq"].get<float>();
    if(j.contains("eqPeakGain"))    tc.eqPeakGain     = j["eqPeakGain"].get<float>();
    if(j.contains("eqPeakQ"))       tc.eqPeakQ        = j["eqPeakQ"].get<float>();
    if(j.contains("eqLPFreq"))      tc.eqLPFreq       = j["eqLPFreq"].get<float>();
    if(j.contains("eqLPQ"))         tc.eqLPQ          = j["eqLPQ"].get<float>();
    if(j.contains("euclPulses"))    tc.euclPulses     = j["euclPulses"].get<int>();
    if(j.contains("euclSteps"))     tc.euclSteps      = j["euclSteps"].get<int>();
    if(j.contains("euclOffset"))    tc.euclOffset     = j["euclOffset"].get<int>();
    if(j.contains("accentPulses"))  tc.accentPulses   = j["accentPulses"].get<int>();
    if(j.contains("accentSteps"))   tc.accentSteps    = j["accentSteps"].get<int>();
    if(j.contains("accentOffset"))  tc.accentOffset   = j["accentOffset"].get<int>();
    if(j.contains("accentHi"))      tc.accentHi       = j["accentHi"].get<float>();
    if(j.contains("accentLo"))      tc.accentLo       = j["accentLo"].get<float>();
    if(j.contains("accentBeatVar")) tc.accentBeatVar  = j["accentBeatVar"].get<float>();
    if(j.contains("trackPitch"))    tc.trackPitch     = j["trackPitch"].get<float>();
    if(j.contains("globalVol"))     tc.globalVol      = j["globalVol"].get<float>();
    if(j.contains("globalProb"))    tc.globalProb     = j["globalProb"].get<float>();
    if(j.contains("muted"))         tc.muted          = j["muted"].get<bool>();
}

// Serialize/deserialize per-slot step/shift data only
static ofJson serializeTrackData(const fullStepSequencer::TrackData& td) {
    ofJson j;
    j["shift"]     = td.shift;
    j["activeTab"] = td.activeTab;

    ofJson on = ofJson::array(), vol = ofJson::array(), prob  = ofJson::array();
    ofJson pan = ofJson::array(), cut = ofJson::array(), res   = ofJson::array();
    ofJson spitch = ofJson::array(), srev = ofJson::array();
    for(bool  v : td.stepOn)      on    .push_back(v);
    for(float v : td.stepVol)     vol   .push_back(v);
    for(float v : td.stepProb)    prob  .push_back(v);
    for(float v : td.stepPan)     pan   .push_back(v);
    for(float v : td.stepCut)     cut   .push_back(v);
    for(float v : td.stepRes)     res   .push_back(v);
    for(int   v : td.stepPitch)   spitch.push_back(v);
    for(bool  v : td.stepReverse) srev  .push_back(v);
    j["stepOn"]      = on;
    j["stepVol"]     = vol;
    j["stepProb"]    = prob;
    j["stepPan"]     = pan;
    j["stepCut"]     = cut;
    j["stepRes"]     = res;
    j["stepPitch"]   = spitch;
    j["stepReverse"] = srev;
    return j;
}

static void deserializeTrackData(const ofJson& j, fullStepSequencer::TrackData& td,
                                 const fullStepSequencer::TrackConfig& tc) {
    if(j.contains("shift"))    td.shift    = j["shift"].get<int>();
    if(j.contains("activeTab"))td.activeTab= j["activeTab"].get<int>();

    td.resizeSteps();
    int n = tc.getNumSteps();

    auto loadArr = [&](const char* key, std::vector<float>& vec) {
        if(j.contains(key)) {
            auto& arr = j[key];
            for(int i = 0; i < n && i < (int)arr.size(); i++)
                vec[i] = arr[i].get<float>();
        }
    };
    if(j.contains("stepOn")) {
        auto& arr = j["stepOn"];
        for(int i = 0; i < n && i < (int)arr.size(); i++)
            td.stepOn[i] = arr[i].get<bool>();
    }
    loadArr("stepVol",  td.stepVol);
    loadArr("stepProb", td.stepProb);
    loadArr("stepPan",  td.stepPan);
    loadArr("stepCut",  td.stepCut);
    loadArr("stepRes",  td.stepRes);
    if(j.contains("stepPitch")) {
        auto& arr = j["stepPitch"];
        for(int i = 0; i < n && i < (int)arr.size(); i++)
            td.stepPitch[i] = arr[i].get<int>();
    }
    if(j.contains("stepReverse")) {
        auto& arr = j["stepReverse"];
        for(int i = 0; i < n && i < (int)arr.size(); i++)
            td.stepReverse[i] = arr[i].get<bool>();
    }
}

void fullStepSequencer::serializeSlots(ofJson& j) const {
    j["browseDir"]   = browseDir;
    j["currentSlot"] = currentSlotP.get();
    j["numTracks"]   = numTracks;
    j["swingAmount"] = swingP.get();

    // Per-track global config (shared across all slots)
    ofJson cfgArr = ofJson::array();
    for(int ti = 0; ti < numTracks; ti++)
        cfgArr.push_back(serializeTrackConfig(trackConfigs[ti]));
    j["trackConfigs"] = cfgArr;

    // Per-track sample paths (node-global)
    ofJson pathsArr = ofJson::array();
    for(int ti = 0; ti < MAX_TRACKS; ti++)
        pathsArr.push_back(samplePaths[ti]);
    j["samplePaths"] = pathsArr;

    // Per-slot step/shift data only
    ofJson slotsArr = ofJson::array();
    for(int s = 0; s < MAX_SLOTS; s++) {
        ofJson slotJ = ofJson::array();
        const auto& slot = slots[s];
        for(int ti = 0; ti < (int)slot.tracks.size(); ti++)
            slotJ.push_back(serializeTrackData(slot.tracks[ti]));
        slotsArr.push_back(slotJ);
    }
    j["slots"] = slotsArr;
}

void fullStepSequencer::deserializeSlots(const ofJson& j) {
    if(j.contains("browseDir")) {
        std::string d = j["browseDir"].get<std::string>();
        if(std::filesystem::exists(d)) refreshBrowse(d);
    }

    if(j.contains("numTracks")) {
        int n = j["numTracks"].get<int>();
        numTracks = ofClamp(n, 1, MAX_TRACKS);
    }

    // Load per-track sample paths
    if(j.contains("samplePaths")) {
        const auto& arr = j["samplePaths"];
        for(int ti = 0; ti < MAX_TRACKS && ti < (int)arr.size(); ti++)
            samplePaths[ti] = arr[ti].get<std::string>();
    } else if(j.contains("samplePath")) {
        samplePaths[0] = j["samplePath"].get<std::string>();
    } else if(j.contains("slots") && !j["slots"].empty() &&
              !j["slots"][0].empty() && j["slots"][0][0].contains("samplePath")) {
        samplePaths[0] = j["slots"][0][0]["samplePath"].get<std::string>();
    }

    initSlots();  // resets trackConfigs and slots to defaults

    // ── Load global swing ─────────────────────────────────────────────────────
    if(j.contains("swingAmount"))
        swingP.set(ofClamp(j["swingAmount"].get<float>(), 0.0f, 0.5f));

    // ── Load per-track global config ──────────────────────────────────────────
    if(j.contains("trackConfigs")) {
        const auto& cfgArr = j["trackConfigs"];
        for(int ti = 0; ti < numTracks && ti < (int)cfgArr.size(); ti++)
            deserializeTrackConfig(cfgArr[ti], trackConfigs[ti]);
    } else if(j.contains("slots") && !j["slots"].empty()) {
        // Backward compatibility: old format stored config inside slot[0] TrackData
        const auto& slot0 = j["slots"][0];
        for(int ti = 0; ti < numTracks && ti < (int)slot0.size(); ti++)
            deserializeTrackConfig(slot0[ti], trackConfigs[ti]);
        // Also migrate swingAmount from old TrackData
        if(numTracks > 0 && slot0.size() > 0 && slot0[0].contains("swingAmount"))
            swingP.set(ofClamp(slot0[0]["swingAmount"].get<float>(), 0.0f, 0.5f));
    }

    // ── Load per-slot step/shift data ─────────────────────────────────────────
    if(j.contains("slots")) {
        const auto& slotsArr = j["slots"];
        for(int s = 0; s < MAX_SLOTS && s < (int)slotsArr.size(); s++) {
            const auto& slotJ = slotsArr[s];
            for(int ti = 0; ti < (int)slotJ.size() && ti < (int)slots[s].tracks.size(); ti++)
                deserializeTrackData(slotJ[ti], slots[s].tracks[ti], trackConfigs[ti]);
        }
    }

    int slot = j.contains("currentSlot") ? j["currentSlot"].get<int>() : 0;
    slot = ofClamp(slot, 0, MAX_SLOTS - 1);
    for(int ti = 0; ti < numTracks; ti++)
        snprintf(nameEditBuf[ti], 64, "%s", trackConfigs[ti].name.c_str());
}

void fullStepSequencer::presetSave(ofJson& j) {
    serializeSlots(j);
    j["embedInProject"] = embedInProject.get();

    if(embedInProject.get()) {
        std::string presetPath = ofxOceanodeShared::getCurrentPresetPath();
        if(!presetPath.empty()) {
            std::string absRoot = ofToDataPath(presetPath, true);
            embedSamplesIntoJson(j, absRoot);
        }
    }
}

void fullStepSequencer::loadBeforeConnections(ofJson& j) {
    if(j.contains("browseDir")) {
        std::string d = j["browseDir"].get<std::string>();
        if(std::filesystem::exists(d)) refreshBrowse(d);
    }
    // Output ports (Out 1..N) must exist before Oceanode reconstructs connections.
    // numTracksP listener fires during presetRecallAfterSettingParameters which is
    // called AFTER connections are remade, so we must add the ports here instead.
    if(j.contains("numTracks")) {
        int n = ofClamp(j["numTracks"].get<int>(), 1, MAX_TRACKS);
        if(n != numTracks) setNumTracks(n);
    }
}

void fullStepSequencer::presetRecallAfterSettingParameters(ofJson& j) {
    if(j.contains("embedInProject"))
        embedInProject = j["embedInProject"].get<bool>();

    deserializeSlots(j);

    // ── 1. Load samples FIRST so getBufnum() is valid for everything below ──
    // IMPORTANT: freeAllSamples() → freeSampleForTrack() clears samplePaths[ti].
    // Snapshot paths before freeing so the reload loop still knows what to load.
    {
        std::vector<std::string> pathsToLoad = samplePaths;
        freeAllSamples();
        for(int ti = 0; ti < MAX_TRACKS; ti++) {
            if(pathsToLoad[ti].empty()) continue;
            std::string absPath = resolveToAbsolutePath(pathsToLoad[ti]);
            if(!std::filesystem::exists(absPath)) continue;
            loadSampleForTrack(ti, absPath);   // sets samplePaths[ti] = absPath at end
        }
    }

    // ── 2. Push all params (including correct bufnum) to already-running synths
    // reloadCurrentSlot sends timing, step arrays (/n_setn — correct regardless
    // of addCharArg vs addStringArg), env params, and bufnum to every running
    // synth.  This handles the toUpdateNodes case — nodes that were already
    // connected before this preset load and whose synths serverManager will NOT
    // recreate (it only calls createSynth for toCreateNodes).
    //
    // Bus routing is intentionally NOT touched here.  serverManager owns the
    // output-bus assignment.  After presetHasLoaded it runs recomputeGraph which:
    //   • toUpdateNodes: calls setOutputBus → sends /n_set out <correctBus> ✓
    //   • toCreateNodes: calls setOutputBus then createSynth with correct buses
    //     and correct getBufnum (samples already loaded above) ✓
    //   • disconnected nodes: calls free(server) → synths freed → no hardware leak ✓
    reloadCurrentSlot();
}

void fullStepSequencer::macroSave(ofJson& j, string path) {
    serializeSlots(j);
    j["embedInProject"] = embedInProject.get();

    if(embedInProject.get()) {
        // Local macro (under Presets/): anchor to the shared preset root so the
        // samples path survives macro duplication. Global macro: use path directly.
        std::string absFolderPath      = ofToDataPath(path, true);
        std::string sharedPresetPath   = ofxOceanodeShared::getCurrentPresetPath();
        std::string absSharedPreset    = sharedPresetPath.empty() ? ""
                                         : ofToDataPath(sharedPresetPath, true);

        std::string absRoot;
        if(!absSharedPreset.empty() && absFolderPath.find("/Presets/") != std::string::npos)
            absRoot = absSharedPreset;
        else
            absRoot = absFolderPath;

        embedSamplesIntoJson(j, absRoot);
    }
}

void fullStepSequencer::macroLoad(ofJson& j, string /*path*/) {
    if(j.contains("embedInProject"))
        embedInProject = j["embedInProject"].get<bool>();
    presetRecallAfterSettingParameters(j);
}

// ════════════════════════════════════════════════════════════════════════════
// Sample embed helpers
// ════════════════════════════════════════════════════════════════════════════

void fullStepSequencer::embedSamplesIntoJson(ofJson& j, const std::string& folderRootAbs) {
    if(!j.contains("samplePaths")) return;

    std::string samplesDir = folderRootAbs + "/samples";
    try {
        std::filesystem::create_directories(samplesDir);
    } catch(const std::exception& e) {
        ofLogError("fullStepSequencer") << "embedSamples: mkdir failed: " << e.what();
        return;
    }

    auto& pathsArr = j["samplePaths"];
    for(int ti = 0; ti < (int)pathsArr.size(); ti++) {
        std::string storedPath = pathsArr[ti].get<std::string>();
        if(storedPath.empty()) continue;

        std::string absPath = resolveToAbsolutePath(storedPath);
        if(!std::filesystem::exists(absPath)) continue;

        std::string filename = std::filesystem::path(absPath).filename().string();
        std::string destAbs  = samplesDir + "/" + filename;

        try {
            auto srcCanon  = std::filesystem::weakly_canonical(std::filesystem::path(absPath));
            auto destCanon = std::filesystem::weakly_canonical(std::filesystem::path(destAbs));
            if(srcCanon != destCanon) {
                std::filesystem::copy_file(absPath, destAbs,
                    std::filesystem::copy_options::overwrite_existing);
            }
        } catch(const std::exception& e) {
            ofLogWarning("fullStepSequencer") << "embedSamples: copy failed for track " << ti << ": " << e.what();
            continue;
        }

        // Rewrite JSON path to data-relative embedded path
        std::string relPath = computeDataRelativePath(folderRootAbs) + "/samples/" + filename;
        pathsArr[ti] = relPath;
    }
}

std::string fullStepSequencer::resolveToAbsolutePath(const std::string& inputPath) const {
    std::string s = inputPath;
    if(s.size() >= 2 && s[0] == '.' && s[1] == '/') s = s.substr(2);

    if(!s.empty() && s[0] == '/') return s;  // already absolute

    bool isKnownRelative = (s.rfind("Macros/", 0) == 0) ||
                           (s.rfind("Presets/", 0) == 0) ||
                           (s.rfind("Supercollider/", 0) == 0);
    if(isKnownRelative) return ofToDataPath(s, true);

    std::string dataRel = ofToDataPath(s, true);
    if(std::filesystem::exists(dataRel)) return dataRel;

    return ofToDataPath("Supercollider/Samples/" + s, true);
}

std::string fullStepSequencer::computeDataRelativePath(const std::string& absPath) const {
    size_t p = absPath.find("/Macros/");
    if(p != std::string::npos) return absPath.substr(p + 1);

    p = absPath.find("/Presets/");
    if(p != std::string::npos) return absPath.substr(p + 1);

    std::string dataDir = ofToDataPath("", true);
    if(absPath.find(dataDir) == 0) {
        std::string rel = absPath.substr(dataDir.size());
        if(!rel.empty() && rel[0] == '/') rel = rel.substr(1);
        return rel;
    }
    return std::filesystem::path(absPath).filename().string();
}
