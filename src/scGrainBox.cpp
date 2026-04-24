//
//  scGrainBox.cpp
//  ofxOceanodeSuperCollider
//

#include "scGrainBox.h"
#include "ofxSuperCollider.h"
#include <algorithm>
#include <cmath>
#include <fstream>

// Static member definitions
const char* scGrainBox::LFO_TARGET_NAMES[6] = {"Pos","Dur","Pitch","Amp","Pan","TrRt"};

// ════════════════════════════════════════════════════════════════════════════
// Constructor / Destructor
// ════════════════════════════════════════════════════════════════════════════

scGrainBox::scGrainBox(vector<serverManager*> servers)
    : scNode("GrainBox"), allServers(servers)
{
    color = ofColor(255, 140, 60);

    for(auto* sm : allServers) {
        if(sm && sm->getServer()) {
            previewServer = sm->getServer();
            break;
        }
    }

    envData.resize(ENV_BUFFER_SIZE, 0.0f);
    waveformPeaks.resize(WAVEFORM_BINS, 0.0f);

    browseDir = ofToDataPath("Supercollider/Samples", true);
    if(!std::filesystem::exists(browseDir))
        browseDir = ofToDataPath("", true);
}

scGrainBox::~scGrainBox() {
    try {
        nodeListeners.unsubscribeAll();
        stopPreview();

        for(auto& [srv, s] : synths) {
            if(s) { s->free(); delete s; }
        }
        synths.clear();

        for(auto& [srv, bufs] : sampleBufs) {
            for(auto* b : bufs) { if(b) { b->free(); delete b; } }
        }
        sampleBufs.clear();

        for(auto& [srv, b] : envBufs) {
            if(b) { b->free(); delete b; }
        }
        envBufs.clear();

        for(auto& [srv, b] : privateBuses) {
            if(b) { b->free(); delete b; }
        }
        privateBuses.clear();
    } catch(const std::exception& e) {
        ofLogError("scGrainBox") << "Destructor: " << e.what();
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Setup
// ════════════════════════════════════════════════════════════════════════════

void scGrainBox::setup() {
    addSeparator("GrainBox");
    addParameter(showWindow.set("Show", false));
    addParameter(numChannelsP.set("N Chan", 1, 1, MAX_CHANNELS));

    addSeparator("Trigger");
    addParameter(triggerP.set("Trigger", {0}, {0}, {1}));
    addParameter(autoTrigP.set("AutoTrig", false));
    addParameter(autoTrigBeatDivP.set("AutoTrigBDiv", {1.0f}, {0.125f}, {64.0f}));
    addParameter(chanceP.set("Chance", {1.0f}, {0.0f}, {1.0f}));
    addParameter(uniqueTrigP.set("UniqueTrig", false));
    addParameter(syncGateP.set("SyncGate", 0, 0, 1));

    addSeparator("Grain");
    addParameter(ampP.set("Amp", {1.0f}, {0.0f}, {1.0f}));
    addParameter(pitchP.set("Pitch", {0.0f}, {-48.0f}, {48.0f}));
    addParameter(dynamicDurP.set("DynamicDur", false));
    addParameter(trigDurP.set("TrigDur", false));
    addParameter(durationP.set("Duration", {0.1f}, {0.0f}, {1.0f}));
    addParameter(positionP.set("Position", {0.0f}, {0.0f}, {1.0f}));
    addParameter(panAzP.set("PanAz", {0.0f}, {0.0f}, {2.0f}));

    addSeparator("Jitter");
    addParameter(uniqueJitP.set("UniqueJit", false));
    addParameter(posJitP.set("PosJit", {0.0f}, {0.0f}, {1.0f}));
    addParameter(pitchJitP.set("PitchJit", {0.0f}, {0.0f}, {12.0f}));
    addParameter(durJitP.set("DurJit", {0.0f}, {0.0f}, {1.0f}));
    addParameter(ampJitP.set("AmpJit", {0.0f}, {0.0f}, {1.0f}));
    addParameter(panAzJitP.set("PanAzJit", {0.0f}, {0.0f}, {2.0f}));

    addSeparator("Region");
    addParameter(inPointP.set("InPoint", 0.0f, 0.0f, 1.0f));
    addParameter(outPointP.set("OutPoint", 1.0f, 0.0f, 1.0f));
    addOutputParameter(sampleMsP.set("SampleMs", 0.0f, 0.0f, 3600000.0f));

    addSeparator("Envelope");
    addParameter(envAttackP.set("EnvAttack",  0.1f, 0.0f, 1.0f));
    addParameter(envReleaseP.set("EnvRelease", 0.1f, 0.0f, 1.0f));
    addParameter(envTensionP.set("EnvTension", 0.0f, -1.0f, 1.0f));

    addSeparator("Levels");
    addParameter(levelsP.set("Levels", {1.0f}, {0.0f}, {1.0f}));

    // ── LFO groups (6 targets × 5 params each + per-target unique toggles) ──
    static const char* lfoNames[6] = {"Pos","Dur","Pitch","Amp","Pan","TrRt"};
    for(int t = 0; t < NUM_LFO; t++)
        addParameter(uniqueLfoTargetP[t].set("UniqueLfo"+std::string(lfoNames[t]), false));
    static const char* lfoSCShape[6]  = {"lfoShapePos","lfoShapeDur","lfoShapePitch","lfoShapeAmp","lfoShapePan","lfoShapeTrRt"};
    static const char* lfoSCSpeed[6]  = {"lfoSpeedPos","lfoSpeedDur","lfoSpeedPitch","lfoSpeedAmp","lfoSpeedPan","lfoSpeedTrRt"};
    static const char* lfoSCPhase[6]  = {"lfoPhasePos","lfoPhaseDur","lfoPhasePitch","lfoPhaseAmp","lfoPhasePan","lfoPhaseTrRt"};
    static const char* lfoSCQuant[6]  = {"lfoQuantPos","lfoQuantDur","lfoQuantPitch","lfoQuantAmp","lfoQuantPan","lfoQuantTrRt"};
    static const char* lfoSCStr[6]    = {"lfoStrPos",  "lfoStrDur",  "lfoStrPitch",  "lfoStrAmp",  "lfoStrPan",  "lfoStrTrRt"};
    // Strength ranges differ per target:
    //   Pos/Dur/Amp: 0..1   Pan: 0..2   Pitch: 0..48 semitones   TrRt: 0..32 beat-divisions
    static const float lfoStrMax[6] = {1.0f, 1.0f, 48.0f, 1.0f, 2.0f, 32.0f};
    for(int t = 0; t < NUM_LFO; t++) {
        std::string n = lfoNames[t];
        addInspectorParameter(lfoGroups[t].shape.set   ("LFOShape"+n,    {0.0f}, {0.0f},   {5.0f}));
        addInspectorParameter(lfoGroups[t].speed.set   ("LFOSpeed"+n,    {1.0f}, {0.125f}, {128.0f}));
        addInspectorParameter(lfoGroups[t].phase.set   ("LFOPhase"+n,    {0.0f}, {0.0f},   {1.0f}));
        addInspectorParameter(lfoGroups[t].quant.set   ("LFOQuant"+n,    {0.0f}, {0.0f},   {32.0f}));
        addInspectorParameter(lfoGroups[t].strength.set("LFOStr"+n,      {0.0f}, {0.0f},   {lfoStrMax[t]}));
    }

    // Output port
    scNode::addOutput("Out");

    // ── Pre-allocate env buffers for all servers (must happen BEFORE any
    //    setBLatency bundle so /b_alloc is processed by SC's NRT thread
    //    before the synth is created) ────────────────────────────────────
    computeEnvData();
    for(auto* sm : allServers) {
        if(!sm || !sm->getServer()) continue;
        auto* srv = sm->getServer();
        if(!envBufs.count(srv) || !envBufs[srv]) {
            envBufs[srv] = new ofxSCBuffer(ENV_BUFFER_SIZE, 1, srv);
            envBufs[srv]->alloc();
        }
    }
    // Schedule env data upload for next update() — alloc is async so we can't send immediately
    envNeedsUpdate = true;

    // ── Listeners ─────────────────────────────────────────────────────────

    // numChannels: recreate synths with new SynthDef variant
    nodeListeners.push(numChannelsP.newListener([this](int& n) {
        if(n < 1 || n > MAX_CHANNELS) return;
        if(n == oldNumChannels) return;
        oldNumChannels = n;
        for(auto* sm : allServers) {
            if(!sm || !sm->getServer()) continue;
            auto* srv = sm->getServer();
            if(!synths.count(srv) || !synths[srv]) continue;
            int oldNodeID = synths[srv]->nodeID;
            int oldBus    = outputBuses.count(srv) ? outputBuses[srv] : 0;
            // Delete the C++ object WITHOUT sending /n_free — createAndRun with
            // action=4 (replace) will atomically free the old SC node.
            // Sending /n_free first causes the replace to target a dead node,
            // making the new synth land in the wrong graph position.
            delete synths[srv];
            synths[srv] = nullptr;
            // Build new synth object and queue all params
            buildSynth(srv);
            if(synths[srv]) {
                synths[srv]->set("out", oldBus);
                synths[srv]->createAndRun(4, oldNodeID, true);
                envNeedsUpdate = true;  // re-upload env data for new synth
            }
        }
    }));

    // Envelope params → recompute and upload
    auto envListener = [this](float&) {
        computeEnvData();
        envNeedsUpdate = true;
    };
    nodeListeners.push(envAttackP.newListener(envListener));
    nodeListeners.push(envReleaseP.newListener(envListener));
    nodeListeners.push(envTensionP.newListener(envListener));

    // Scalar synth params
    nodeListeners.push(autoTrigP.newListener([this](bool& v) {
        for(auto& [srv, s] : synths) if(s) s->set("autotrig", (int)v);
    }));
    nodeListeners.push(dynamicDurP.newListener([this](bool& v) {
        for(auto& [srv, s] : synths) if(s) s->set("dynamicdur", (int)v);
    }));
    nodeListeners.push(trigDurP.newListener([this](bool& v) {
        for(auto& [srv, s] : synths) if(s) s->set("trigdur", (int)v);
    }));
    nodeListeners.push(uniqueTrigP.newListener([this](bool& v) {
        for(auto& [srv, s] : synths) if(s) s->set("uniquetrig", (int)v);
    }));
    nodeListeners.push(uniqueJitP.newListener([this](bool& v) {
        for(auto& [srv, s] : synths) if(s) s->set("uniquejit", (int)v);
    }));
    {
        for(int t = 0; t < NUM_LFO; t++) {
            nodeListeners.push(uniqueLfoTargetP[t].newListener([this, t](bool& v) {
                static const char* sa[6] = {"uniquelfoPos","uniquelfoDur","uniquelfoPitch","uniquelfoAmp","uniquelfoPan","uniquelfoTrRt"};
                for(auto& [srv, s] : synths) if(s) s->set(sa[t], (int)v);
            }));
        }
    }
    auto updateSampleMs = [this]() {
        float ms = (outPointP.get() - inPointP.get()) * bufferDurationSecs * 1000.0f;
        sampleMsP.set(std::max(0.0f, ms));
    };

    nodeListeners.push(inPointP.newListener([this, updateSampleMs](float& v) {
        for(auto& [srv, s] : synths) if(s) s->set("inpoint", v);
        updateSampleMs();
    }));
    nodeListeners.push(outPointP.newListener([this, updateSampleMs](float& v) {
        for(auto& [srv, s] : synths) if(s) s->set("outpoint", v);
        updateSampleMs();
    }));

    // Vector params — expand to numChannels so a scalar input reaches every voice
    auto expandF = [this](const vector<float>& v) {
        int n = numChannelsP.get();
        if((int)v.size() >= n || v.empty()) return v;
        vector<float> out(n);
        for(int i = 0; i < n; i++) out[i] = v[i % (int)v.size()];
        return out;
    };
    auto expandI = [this](const vector<int>& v) {
        int n = numChannelsP.get();
        if((int)v.size() >= n || v.empty()) return v;
        vector<int> out(n);
        for(int i = 0; i < n; i++) out[i] = v[i % (int)v.size()];
        return out;
    };

    auto vfListener = [this, expandF](const std::string& name, ofParameter<vector<float>>& p) {
        nodeListeners.push(p.newListener([this, name, expandF](vector<float>& v) {
            auto ev = expandF(v);
            for(auto& [srv, s] : synths) if(s) s->set(name, ev);
        }));
    };
    auto viListener = [this, expandI](const std::string& name, ofParameter<vector<int>>& p) {
        nodeListeners.push(p.newListener([this, name, expandI](vector<int>& v) {
            auto ev = expandI(v);
            for(auto& [srv, s] : synths) if(s) s->set(name, ev);
        }));
    };

    viListener("trigger",          triggerP);
    vfListener("amp",              ampP);
    vfListener("pitch",            pitchP);
    vfListener("duration",         durationP);
    vfListener("position",         positionP);
    vfListener("panaz",            panAzP);
    vfListener("autotrigbeatdiv",  autoTrigBeatDivP);
    vfListener("chance",           chanceP);
    vfListener("posjit",           posJitP);
    vfListener("pitchjit",      pitchJitP);
    vfListener("durjit",        durJitP);
    vfListener("ampjit",        ampJitP);
    vfListener("panazjit",      panAzJitP);
    vfListener("levels",        levelsP);

    // SyncGate (int scalar 0/1)
    nodeListeners.push(syncGateP.newListener([this](int& v) {
        for(auto& [srv, s] : synths) if(s) s->set("syncgate", v);
        if(v == 1) syncGateReset = true;
    }));

    // LFO listeners — same expand-and-broadcast pattern
    static const char* lfoSCShapeL[6]  = {"lfoShapePos","lfoShapeDur","lfoShapePitch","lfoShapeAmp","lfoShapePan","lfoShapeTrRt"};
    static const char* lfoSCSpeedL[6]  = {"lfoSpeedPos","lfoSpeedDur","lfoSpeedPitch","lfoSpeedAmp","lfoSpeedPan","lfoSpeedTrRt"};
    static const char* lfoSCPhaseL[6]  = {"lfoPhasePos","lfoPhaseDur","lfoPhasePitch","lfoPhaseAmp","lfoPhasePan","lfoPhaseTrRt"};
    static const char* lfoSCQuantL[6]  = {"lfoQuantPos","lfoQuantDur","lfoQuantPitch","lfoQuantAmp","lfoQuantPan","lfoQuantTrRt"};
    static const char* lfoSCStrL[6]    = {"lfoStrPos",  "lfoStrDur",  "lfoStrPitch",  "lfoStrAmp",  "lfoStrPan",  "lfoStrTrRt"};
    for(int t = 0; t < NUM_LFO; t++) {
        vfListener(lfoSCShapeL[t], lfoGroups[t].shape);
        vfListener(lfoSCSpeedL[t], lfoGroups[t].speed);
        vfListener(lfoSCPhaseL[t], lfoGroups[t].phase);
        vfListener(lfoSCQuantL[t], lfoGroups[t].quant);
        vfListener(lfoSCStrL[t],   lfoGroups[t].strength);
    }

    // (computeEnvData already called above in the pre-alloc block)

    refreshBrowse(browseDir);
}

// ════════════════════════════════════════════════════════════════════════════
// scNode lifecycle
// ════════════════════════════════════════════════════════════════════════════

// buildSynth: create the ofxSCSynth object and queue all init args.
// serverManager calls buildSynth → setOutputBus → createSynth, all within
// the same setBLatency bundle.  By creating the synth object here (before
// setOutputBus fires), the "out" bus value gets stored as an init arg
// (created=false) and is baked into the /s_new message via createAndRun.
void scGrainBox::buildSynth(ofxSCServer* srv) {
    if(!srv) return;
    bool hadSampleBufs = sampleBufs.count(srv) && !sampleBufs[srv].empty();
    ofLogNotice("scGrainBox") << "buildSynth: sampleBufs=" << hadSampleBufs
        << " samplePath=" << (samplePath.empty() ? "<empty>" : samplePath)
        << " numSampleCh=" << numSampleChannels;

    // Free any stale synth
    if(synths.count(srv) && synths[srv]) {
        synths[srv]->free();
        delete synths[srv];
        synths[srv] = nullptr;
    }

    // Ensure env buffer exists (pre-allocated in setup, guard here for late servers)
    if(!envBufs.count(srv) || !envBufs[srv]) {
        envBufs[srv] = new ofxSCBuffer(ENV_BUFFER_SIZE, 1, srv);
        envBufs[srv]->alloc();
    }

    synths[srv] = new ofxSCSynth(getSynthdefName(), srv);

    // Subscribe to /grainTrig SendReply messages for exact-timing visualization.
    // SC sends: /grainTrig [nodeID, channelIdx, pos(0..1), dur(s), amp(0..1)]
    grainFeedbackListeners[srv] = synths[srv]->newFeedbackMessage.newListener(
        [this, srv](ofxOscMessage& msg) {
            if(msg.getAddress() != "/grainTrig") return;
            if(msg.getNumArgs() < 5) return;
            if(!synths.count(srv) || !synths[srv]) return;
            if(msg.getArgAsInt(0) != synths[srv]->nodeID) return;

            int   ch  = msg.getArgAsInt(1);
            float pos = msg.getArgAsFloat(2);
            float dur = msg.getArgAsFloat(3);
            float amp = std::max(0.0f, msg.getArgAsFloat(4));

            GrainHighlight gh;
            gh.position = pos;
            gh.duration = dur;
            gh.amp      = amp;
            gh.channel  = ch;
            gh.maxLife  = std::max(dur, 0.033f);
            gh.lifeTime = gh.maxLife;
            grainHighlights.push_back(gh);
        });

    // Set per-channel sample buffers (bufnum0..bufnum{K-1})
    if(sampleBufs.count(srv)) {
        for(int ch = 0; ch < (int)sampleBufs[srv].size(); ch++) {
            if(sampleBufs[srv][ch])
                synths[srv]->set("bufnum" + ofToString(ch), sampleBufs[srv][ch]->index);
        }
    }

    // Wire up custom envelope buffer
    synths[srv]->set("envbuf", envBufs[srv]->index);

    // Set scalar params
    synths[srv]->set("dynamicdur",   (int)dynamicDurP.get());
    synths[srv]->set("trigdur",      (int)trigDurP.get());
    synths[srv]->set("autotrig",     (int)autoTrigP.get());
    synths[srv]->set("uniquetrig",   (int)uniqueTrigP.get());
    synths[srv]->set("uniquejit",    (int)uniqueJitP.get());
    {
        static const char* sa[6] = {"uniquelfoPos","uniquelfoDur","uniquelfoPitch","uniquelfoAmp","uniquelfoPan","uniquelfoTrRt"};
        for(int t = 0; t < NUM_LFO; t++) synths[srv]->set(sa[t], (int)uniqueLfoTargetP[t].get());
    }
    synths[srv]->set("bpm",          currentBpm);
    synths[srv]->set("syncgate",     syncGateP.get());
    synths[srv]->set("inpoint",      inPointP.get());
    synths[srv]->set("outpoint",     outPointP.get());

    // Set vector params — expand to numChannels so a scalar covers every voice
    int nch = numChannelsP.get();
    auto xf = [nch](const vector<float>& v) -> vector<float> {
        if((int)v.size() >= nch || v.empty()) return v;
        vector<float> out(nch); for(int i=0;i<nch;i++) out[i]=v[i%(int)v.size()]; return out;
    };
    synths[srv]->set("amp",              xf(ampP.get()));
    synths[srv]->set("pitch",            xf(pitchP.get()));
    synths[srv]->set("duration",         xf(durationP.get()));
    synths[srv]->set("position",         xf(positionP.get()));
    synths[srv]->set("panaz",            xf(panAzP.get()));
    synths[srv]->set("autotrigbeatdiv",  xf(autoTrigBeatDivP.get()));
    synths[srv]->set("chance",           xf(chanceP.get()));
    synths[srv]->set("posjit",           xf(posJitP.get()));
    synths[srv]->set("pitchjit",         xf(pitchJitP.get()));
    synths[srv]->set("durjit",           xf(durJitP.get()));
    synths[srv]->set("ampjit",           xf(ampJitP.get()));
    synths[srv]->set("panazjit",         xf(panAzJitP.get()));
    synths[srv]->set("levels",           xf(levelsP.get()));

    // LFO params
    static const char* lbShape[6] = {"lfoShapePos","lfoShapeDur","lfoShapePitch","lfoShapeAmp","lfoShapePan","lfoShapeTrRt"};
    static const char* lbSpeed[6] = {"lfoSpeedPos","lfoSpeedDur","lfoSpeedPitch","lfoSpeedAmp","lfoSpeedPan","lfoSpeedTrRt"};
    static const char* lbPhase[6] = {"lfoPhasePos","lfoPhaseDur","lfoPhasePitch","lfoPhaseAmp","lfoPhasePan","lfoPhaseTrRt"};
    static const char* lbQuant[6] = {"lfoQuantPos","lfoQuantDur","lfoQuantPitch","lfoQuantAmp","lfoQuantPan","lfoQuantTrRt"};
    static const char* lbStr[6]   = {"lfoStrPos",  "lfoStrDur",  "lfoStrPitch",  "lfoStrAmp",  "lfoStrPan",  "lfoStrTrRt"};
    for(int t = 0; t < NUM_LFO; t++) {
        synths[srv]->set(lbShape[t], xf(lfoGroups[t].shape.get()));
        synths[srv]->set(lbSpeed[t], xf(lfoGroups[t].speed.get()));
        synths[srv]->set(lbPhase[t], xf(lfoGroups[t].phase.get()));
        synths[srv]->set(lbQuant[t], xf(lfoGroups[t].quant.get()));
        synths[srv]->set(lbStr[t],   xf(lfoGroups[t].strength.get()));
    }

    // If we have a sample path but no buffers loaded yet for this server, load them now
    if(!samplePath.empty() && (!sampleBufs.count(srv) || sampleBufs[srv].empty())) {
        ofLogWarning("scGrainBox") << "buildSynth: lazy-loading sample INSIDE b_latency window — "
            << "buffer and synth scheduled at same T+200ms (may cause silence)";
        for(int ch = 0; ch < numSampleChannels; ch++) {
            auto* buf = new ofxSCBuffer(0, 0, srv);
            ofxOscMessage m;
            m.setAddress("/b_allocReadChannel");
            m.addIntArg(buf->index);
            m.addStringArg(samplePath);
            m.addIntArg(0);   // startFrame
            m.addIntArg(-1);  // numFrames (-1 = all)
            m.addIntArg(ch);  // channel ch → mono slice
            srv->sendMsg(m);
            sampleBufs[srv].push_back(buf);
            synths[srv]->set("bufnum" + ofToString(ch), buf->index);
            ofLogNotice("scGrainBox") << "buildSynth: lazy-load ch=" << ch << " bufIdx=" << buf->index;
        }
    } else if(!samplePath.empty()) {
        ofLogNotice("scGrainBox") << "buildSynth: using existing sampleBufs count=" << sampleBufs[srv].size();
        for(int ch = 0; ch < (int)sampleBufs[srv].size(); ch++)
            if(sampleBufs[srv][ch])
                ofLogNotice("scGrainBox") << "  ch=" << ch << " bufIdx=" << sampleBufs[srv][ch]->index;
    }
}

// createSynth: send /s_new.  All args (including "out") were already queued
// during buildSynth / setOutputBus while created=false.
void scGrainBox::createSynth(ofxSCServer* srv) {
    if(!srv) return;
    // If buildSynth wasn't called (edge case), fall back to building now
    if(!synths.count(srv) || !synths[srv])
        buildSynth(srv);
    if(!synths[srv]) return;
    ofLogNotice("scGrainBox") << "createSynth: synthdefName=" << getSynthdefName()
        << " sampleBufs=" << (sampleBufs.count(srv) ? (int)sampleBufs[srv].size() : 0)
        << " envbuf=" << (envBufs.count(srv) && envBufs[srv] ? envBufs[srv]->index : -1)
        << " outBus=" << (outputBuses.count(srv) ? outputBuses[srv] : -1)
        << " autotrig=" << (int)autoTrigP.get()
        << " numCh=" << numChannelsP.get();
    synths[srv]->createAndRun(0, 1, true);
    ofLogNotice("scGrainBox") << "createSynth AFTER: nodeID=" << synths[srv]->nodeID;
    // Env buffer alloc is async — upload data now that the synth is live
    // so GrainBuf reads the correct envelope from the first grain onward.
    envNeedsUpdate = true;
}

void scGrainBox::free(ofxSCServer* srv) {
    if(!srv) return;
    ofLogNotice("scGrainBox") << "free: called. synth=" << (synths.count(srv) && synths[srv] ? synths[srv]->nodeID : -1)
        << " sampleBufs=" << (sampleBufs.count(srv) ? (int)sampleBufs[srv].size() : 0);
    if(synths.count(srv) && synths[srv]) {
        synths[srv]->free();
        delete synths[srv];
        synths.erase(srv);
    }
    // sampleBufs intentionally NOT freed here — disk-loaded buffers must persist
    // across recomputeGraph cycles so buildSynth can reference them without
    // re-issuing /b_allocReadChannel inside the b_latency window (which would
    // schedule both buffer load and /s_new at the same T+200ms, reading from an
    // unloaded buffer → silence).  Freed in loadSample() on sample change and
    // in the destructor.
    //
    // envBufs likewise NOT freed here — re-allocating inside b_latency schedules
    // /b_alloc at T+200ms, but uploadEnvBuffers() (called from update()) sends
    // /b_setn at T+0ms, arriving before the buffer exists → env reads zeros →
    // silent grains on disconnect/reconnect.  Env buffer is cheap per-server
    // state; keep it alive across recomputes.  Freed only in destructor.
    if(privateBuses.count(srv) && privateBuses[srv]) {
        privateBuses[srv]->free();
        delete privateBuses[srv];
        privateBuses.erase(srv);
    }
    outputBuses.erase(srv);
    grainFeedbackListeners.erase(srv);
}

void scGrainBox::setOutputBus(ofxSCServer* srv, int /*idx*/, int bus) {
    if(!srv) return;
    outputBuses[srv] = bus;
    ofLogNotice("scGrainBox") << "setOutputBus: bus=" << bus
        << " synth=" << (synths.count(srv) && synths[srv] ? synths[srv]->nodeID : -1);
    // synth object exists (created in buildSynth, not yet /s_new'd) — set "out"
    // so it becomes an init arg baked into the /s_new call in createSynth.
    if(synths.count(srv) && synths[srv])
        synths[srv]->set("out", bus);
}

int scGrainBox::getOutputBusIndex(ofxSCServer* srv, int idx) {
    int result = outputBuses.count(srv) ? outputBuses.at(srv) : -1;
    ofLogNotice("scGrainBox") << "getOutputBusIndex: idx=" << idx << " return=" << result;
    return result;
}

void scGrainBox::moveSynthBefore(ofxSCServer* srv, int nodeID) {
    if(!synths.count(srv) || !synths[srv]) {
        ofLogWarning("scGrainBox") << "moveSynthBefore: no synth for server, skipping";
        return;
    }
    ofLogNotice("scGrainBox") << "moveSynthBefore: myNodeID=" << synths[srv]->nodeID
        << " beforeNodeID=" << nodeID
        << " outBus=" << (outputBuses.count(srv) ? outputBuses[srv] : -1);
    // Resend all params then move — mirrors scFM7Drone/scRhythmBox pattern.
    // Do NOT rebuild the synth here; just refresh state and reposition.
    sendAllParams();
    if(outputBuses.count(srv))
        synths[srv]->set("out", outputBuses[srv]);
    synths[srv]->moveBefore(nodeID);
}

int scGrainBox::getLastSynthID(ofxSCServer* srv) {
    if(synths.count(srv) && synths[srv]) return synths[srv]->nodeID;
    return -1;
}

void scGrainBox::activate() {
    for(auto& [srv, s] : synths) {
        if(s) { s->set("active", 1.0f); s->run(true); }
    }
}

void scGrainBox::deactivate() {
    for(auto& [srv, s] : synths) {
        if(s) { s->set("active", 0.0f); s->run(false); }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Update
// ════════════════════════════════════════════════════════════════════════════

void scGrainBox::update(ofEventArgs& /*args*/) {
    float dt = (float)ofGetLastFrameTime();

    // ── Reset syncGate to 0 one frame after it was pulsed to 1 ──────────────
    if(syncGateReset) {
        syncGateP.set(0);
        syncGateReset = false;
    }

    // ── Upload envelope buffer if dirty ──────────────────────────────────────
    if(envNeedsUpdate) {
        uploadEnvBuffers();
        envNeedsUpdate = false;
    }

    // ── Update grain highlight lifetimes ─────────────────────────────────────
    for(auto it = grainHighlights.begin(); it != grainHighlights.end();) {
        it->lifeTime -= dt;
        if(it->lifeTime <= 0.0f)
            it = grainHighlights.erase(it);
        else
            ++it;
    }

    // Grain highlights are spawned by the /grainTrig OSC callback in buildSynth
    // (SendReply.ar fires at the exact trigger sample in SC — no C++ approximation needed)

    // Limit highlight buffer size to avoid accumulation at very high trigger rates
    while(grainHighlights.size() > 2048)
        grainHighlights.pop_front();
}

// ════════════════════════════════════════════════════════════════════════════
// Draw
// ════════════════════════════════════════════════════════════════════════════

void scGrainBox::draw(ofEventArgs& /*args*/) {
    if(showWindow.get())
        drawGrainBoxWindow();
}

// ════════════════════════════════════════════════════════════════════════════
// Envelope buffer
// ════════════════════════════════════════════════════════════════════════════

void scGrainBox::computeEnvData() {
    if((int)envData.size() != ENV_BUFFER_SIZE)
        envData.resize(ENV_BUFFER_SIZE, 0.0f);

    float rawAtk = envAttackP.get();   // 0..1
    float rawRel = envReleaseP.get();  // 0..1
    float tension = envTensionP.get(); // -1..1

    // Normalise so attack + release never exceed 1.0
    float total = rawAtk + rawRel;
    float atk = (total > 1.0f) ? rawAtk / total : rawAtk;
    float rel = (total > 1.0f) ? rawRel / total : rawRel;

    // Tension → exponent: <0 = fast start/concave, 0 = linear, >0 = slow start/convex
    float exponent = std::pow(4.0f, tension);

    auto applyTension = [&](float seg) -> float {
        return std::pow(std::max(0.0f, seg), exponent);
    };

    for(int i = 0; i < ENV_BUFFER_SIZE; i++) {
        float t = (float)i / (float)(ENV_BUFFER_SIZE - 1);
        float val;
        if(atk > 0.0f && t < atk) {
            val = applyTension(t / atk);
        } else if(rel > 0.0f && t > (1.0f - rel)) {
            val = applyTension((1.0f - t) / rel);
        } else {
            val = 1.0f;
        }
        envData[i] = std::max(0.0f, std::min(1.0f, val));
    }
}

void scGrainBox::uploadEnvBuffers() {
    for(auto& [srv, buf] : envBufs) {
        if(!buf || !srv) continue;
        ofLogNotice("scGrainBox") << "uploadEnvBuffers: bufIdx=" << buf->index
            << " samples=" << (int)envData.size();
        ofxOscMessage m;
        m.setAddress("/b_setn");
        m.addIntArg(buf->index);
        m.addIntArg(0);
        m.addIntArg((int)envData.size());
        for(float v : envData) m.addFloatArg(v);
        srv->sendMsg(m);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Sample loading
// ════════════════════════════════════════════════════════════════════════════

void scGrainBox::loadSample(const std::string& path) {
    if(path.empty()) return;
    samplePath = path;

    int prevSampleChannels = numSampleChannels;
    loadWaveformData(path);   // sets numSampleChannels and bufferDurationSecs from WAV header
    sampleMsP.set(std::max(0.0f, (outPointP.get() - inPointP.get()) * bufferDurationSecs * 1000.0f));

    for(auto* sm : allServers) {
        if(!sm || !sm->getServer()) continue;
        auto* srv = sm->getServer();

        // Free old per-channel buffers
        if(sampleBufs.count(srv)) {
            for(auto* b : sampleBufs[srv]) { if(b) { b->free(); delete b; } }
            sampleBufs[srv].clear();
        }

        // Allocate one mono buffer per sample channel
        for(int ch = 0; ch < numSampleChannels; ch++) {
            auto* buf = new ofxSCBuffer(0, 0, srv);
            ofxOscMessage m;
            m.setAddress("/b_allocReadChannel");
            m.addIntArg(buf->index);
            m.addStringArg(path);
            m.addIntArg(0);   // startFrame
            m.addIntArg(-1);  // numFrames (-1 = all)
            m.addIntArg(ch);  // channel ch → mono slice
            srv->sendMsg(m);
            sampleBufs[srv].push_back(buf);
        }

        // If numSampleChannels changed, the synthdef variant changes too — replace synth
        if(numSampleChannels != prevSampleChannels && synths.count(srv) && synths[srv]) {
            int oldNodeID = synths[srv]->nodeID;
            int oldBus    = outputBuses.count(srv) ? outputBuses[srv] : 0;
            delete synths[srv];
            synths[srv] = nullptr;
            buildSynth(srv);
            if(synths[srv]) {
                synths[srv]->set("out", oldBus);
                synths[srv]->createAndRun(4, oldNodeID, true);
                envNeedsUpdate = true;
            }
        } else if(synths.count(srv) && synths[srv]) {
            // Same channel count — just update bufnum params
            for(int ch = 0; ch < numSampleChannels; ch++) {
                if(sampleBufs[srv][ch])
                    synths[srv]->set("bufnum" + ofToString(ch), sampleBufs[srv][ch]->index);
            }
        }
    }

    oldNumSampleChannels = numSampleChannels;
}

void scGrainBox::loadWaveformData(const std::string& path) {
    if(path.empty()) return;
    waveformPeaks.assign(WAVEFORM_BINS, 0.0f);
    bufferDurationSecs = 0.0f;

    std::ifstream f(path, std::ios::binary);
    if(!f.is_open()) return;

    // Read raw PCM bytes; this is a simplified peak extraction.
    // Assumes 16-bit signed PCM. For real use the WAV header must be skipped.
    // We seek past the first 44 bytes (standard WAV header).
    f.seekg(0, std::ios::end);
    auto fileSize = f.tellg();
    f.seekg(0, std::ios::beg);
    if(fileSize < 46) return;

    // Read WAV header to find audio data offset and bit depth
    char riff[4]; f.read(riff, 4);
    if(std::string(riff, 4) != "RIFF") return;
    f.seekg(22, std::ios::beg);
    int16_t numCh; f.read((char*)&numCh, 2);
    numSampleChannels = std::max(1, std::min((int)numCh, MAX_CHANNELS));
    int32_t sampleRate = 44100;
    f.seekg(24, std::ios::beg);
    f.read((char*)&sampleRate, 4);
    f.seekg(34, std::ios::beg);
    int16_t bitDepth; f.read((char*)&bitDepth, 2);

    // Find "data" chunk
    f.seekg(12, std::ios::beg);
    int dataOffset = -1;
    int dataSize   = 0;
    while(f.tellg() < (std::streampos)((std::streamoff)fileSize - 8)) {
        char id[4]; f.read(id, 4);
        int32_t sz; f.read((char*)&sz, 4);
        if(std::string(id, 4) == "data") {
            dataOffset = (int)f.tellg();
            dataSize   = sz;
            break;
        }
        f.seekg(sz, std::ios::cur);
    }
    if(dataOffset < 0 || dataSize <= 0) return;

    f.seekg(dataOffset, std::ios::beg);
    int bytesPerSample = (bitDepth == 16) ? 2 : (bitDepth == 24 ? 3 : 4);
    int totalFrames    = dataSize / (bytesPerSample * std::max(1, (int)numCh));
    if(totalFrames <= 0) return;

    bufferDurationSecs = (sampleRate > 0) ? (float)totalFrames / (float)sampleRate : 0.0f;

    int framesPerBin = std::max(1, totalFrames / WAVEFORM_BINS);
    std::vector<char> buf(framesPerBin * bytesPerSample * std::max(1, (int)numCh));

    for(int bin = 0; bin < WAVEFORM_BINS; bin++) {
        int framePos = bin * framesPerBin;
        if(framePos >= totalFrames) { waveformPeaks[bin] = 0.0f; continue; }
        f.seekg(dataOffset + framePos * bytesPerSample * std::max(1, (int)numCh), std::ios::beg);
        int frames = std::min(framesPerBin, totalFrames - framePos);
        f.read(buf.data(), (std::streamsize)(frames * bytesPerSample * std::max(1, (int)numCh)));
        float peak = 0.0f;
        for(int s = 0; s < frames; s++) {
            float sample = 0.0f;
            if(bitDepth == 16) {
                int16_t v = *(int16_t*)(buf.data() + s * bytesPerSample * std::max(1, (int)numCh));
                sample = std::abs(v / 32768.0f);
            } else if(bitDepth == 24) {
                int32_t v = 0;
                memcpy(&v, buf.data() + s * 3 * std::max(1, (int)numCh), 3);
                if(v & 0x800000) v |= 0xFF000000;
                sample = std::abs(v / 8388608.0f);
            } else {
                float v; memcpy(&v, buf.data() + s * bytesPerSample * std::max(1, (int)numCh), 4);
                sample = std::abs(v);
            }
            peak = std::max(peak, sample);
        }
        waveformPeaks[bin] = std::min(1.0f, peak);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Preview
// ════════════════════════════════════════════════════════════════════════════

void scGrainBox::triggerPreview(const std::string& path) {
    stopPreview();
    if(!previewServer || path.empty()) return;
    previewBuf = new ofxSCBuffer(0, 0, previewServer);
    previewBuf->read(path);
    previewSynth = new ofxSCSynth("BufferBrowserPreview", previewServer);
    previewSynth->set("bufnum", previewBuf->index);
    previewSynth->set("out",    0);
    previewSynth->set("gain",   0.7f);
    previewSynth->addToTail();
}

void scGrainBox::stopPreview() {
    if(previewSynth) { previewSynth->free(); delete previewSynth; previewSynth = nullptr; }
    if(previewBuf)   { previewBuf->free();   delete previewBuf;   previewBuf   = nullptr; }
}

// ════════════════════════════════════════════════════════════════════════════
// File browser
// ════════════════════════════════════════════════════════════════════════════

void scGrainBox::refreshBrowse(const std::string& dir) {
    browseEntries.clear();
    browserSel = -1;
    if(!std::filesystem::exists(dir)) return;
    browseDir = dir;

    try {
        std::vector<BrowseEntry> dirs, files;
        for(auto& e : std::filesystem::directory_iterator(dir)) {
            BrowseEntry be;
            be.isDir   = e.is_directory();
            be.name    = e.path().filename().string();
            be.fullPath = e.path().string();
            if(be.isDir) dirs.push_back(be);
            else {
                std::string ext = e.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if(ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac")
                    files.push_back(be);
            }
        }
        std::sort(dirs.begin(),  dirs.end(),  [](auto& a, auto& b){ return a.name < b.name; });
        std::sort(files.begin(), files.end(), [](auto& a, auto& b){ return a.name < b.name; });
        for(auto& e : dirs)  browseEntries.push_back(e);
        for(auto& e : files) browseEntries.push_back(e);
    } catch(...) {}
}

// ════════════════════════════════════════════════════════════════════════════
// Helpers
// ════════════════════════════════════════════════════════════════════════════

std::string scGrainBox::getSynthdefName() const {
    return "GrainBox_" + ofToString(numSampleChannels) + "_" + ofToString(numChannelsP.get());
}

void scGrainBox::sendAllParams() {
    int nch = numChannelsP.get();
    auto xf = [nch](const vector<float>& v) -> vector<float> {
        if((int)v.size() >= nch || v.empty()) return v;
        vector<float> out(nch); for(int i=0;i<nch;i++) out[i]=v[i%(int)v.size()]; return out;
    };
    for(auto& [srv, s] : synths) {
        if(!s) continue;
        if(sampleBufs.count(srv)) {
            for(int ch = 0; ch < (int)sampleBufs[srv].size(); ch++)
                if(sampleBufs[srv][ch])
                    s->set("bufnum" + ofToString(ch), sampleBufs[srv][ch]->index);
        }

        s->set("dynamicdur",    (int)dynamicDurP.get());
        s->set("trigdur",       (int)trigDurP.get());
        s->set("autotrig",      (int)autoTrigP.get());
        s->set("uniquetrig",    (int)uniqueTrigP.get());
        s->set("uniquejit",     (int)uniqueJitP.get());
        s->set("bpm",           currentBpm);
        s->set("inpoint",       inPointP.get());
        s->set("outpoint",      outPointP.get());

        s->set("amp",              xf(ampP.get()));
        s->set("pitch",            xf(pitchP.get()));
        s->set("duration",         xf(durationP.get()));
        s->set("position",         xf(positionP.get()));
        s->set("panaz",            xf(panAzP.get()));
        s->set("autotrigbeatdiv",  xf(autoTrigBeatDivP.get()));
        s->set("chance",           xf(chanceP.get()));
        s->set("posjit",           xf(posJitP.get()));
        s->set("pitchjit",         xf(pitchJitP.get()));
        s->set("durjit",           xf(durJitP.get()));
        s->set("ampjit",           xf(ampJitP.get()));
        s->set("panazjit",         xf(panAzJitP.get()));
        s->set("levels",           xf(levelsP.get()));

        static const char* saShape[6]  = {"lfoShapePos","lfoShapeDur","lfoShapePitch","lfoShapeAmp","lfoShapePan","lfoShapeTrRt"};
        static const char* saSpeed[6]  = {"lfoSpeedPos","lfoSpeedDur","lfoSpeedPitch","lfoSpeedAmp","lfoSpeedPan","lfoSpeedTrRt"};
        static const char* saPhase[6]  = {"lfoPhasePos","lfoPhaseDur","lfoPhasePitch","lfoPhaseAmp","lfoPhasePan","lfoPhaseTrRt"};
        static const char* saQuant[6]  = {"lfoQuantPos","lfoQuantDur","lfoQuantPitch","lfoQuantAmp","lfoQuantPan","lfoQuantTrRt"};
        static const char* saStr[6]    = {"lfoStrPos",  "lfoStrDur",  "lfoStrPitch",  "lfoStrAmp",  "lfoStrPan",  "lfoStrTrRt"};
        static const char* saUnique[6] = {"uniquelfoPos","uniquelfoDur","uniquelfoPitch","uniquelfoAmp","uniquelfoPan","uniquelfoTrRt"};
        for(int t = 0; t < NUM_LFO; t++) {
            s->set(saShape[t],   xf(lfoGroups[t].shape.get()));
            s->set(saSpeed[t],   xf(lfoGroups[t].speed.get()));
            s->set(saPhase[t],   xf(lfoGroups[t].phase.get()));
            s->set(saQuant[t],   xf(lfoGroups[t].quant.get()));
            s->set(saStr[t],     xf(lfoGroups[t].strength.get()));
            s->set(saUnique[t],  (int)uniqueLfoTargetP[t].get());
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// BPM
// ════════════════════════════════════════════════════════════════════════════

void scGrainBox::setBpm(float bpm) {
    currentBpm = bpm;
    for(auto& [srv, s] : synths) if(s) s->set("bpm", bpm);
}

// Validate and reshape vector parameters to match current channel count
void scGrainBox::validateAndReshapeVectorParams() {
    int nch = numChannelsP.get();
    auto reshape = [nch](ofParameter<vector<float>>& p) {
        auto v = p.get();
        if((int)v.size() != nch) {
            if(v.empty()) v.assign(nch, 0.0f);
            else if((int)v.size() > nch) v.resize(nch);
            else v.insert(v.end(), nch - v.size(), v[0]);
            p.set(v);
        }
    };
    auto reshapeI = [nch](ofParameter<vector<int>>& p) {
        auto v = p.get();
        if((int)v.size() != nch) {
            if(v.empty()) v.assign(nch, 0);
            else if((int)v.size() > nch) v.resize(nch);
            else v.insert(v.end(), nch - v.size(), v[0]);
            p.set(v);
        }
    };
    reshapeI(triggerP);
    reshape(chanceP);
    reshape(ampP);
    reshape(pitchP);
    reshape(durationP);
    reshape(positionP);
    reshape(panAzP);
    reshape(posJitP);
    reshape(pitchJitP);
    reshape(durJitP);
    reshape(ampJitP);
    reshape(panAzJitP);
    reshape(levelsP);
    reshape(autoTrigBeatDivP);
}

// ════════════════════════════════════════════════════════════════════════════
// Preset
// ════════════════════════════════════════════════════════════════════════════

void scGrainBox::presetSave(ofJson& j) {
    j["samplePath"]  = samplePath;
    j["inpoint"]     = inPointP.get();
    j["outpoint"]    = outPointP.get();
    j["envAttack"]   = envAttackP.get();
    j["envRelease"]  = envReleaseP.get();
    j["envTension"]  = envTensionP.get();
    j["waveZoom"]    = waveZoom;
    j["waveScroll"]  = waveScroll;
    j["browserW"]    = browserW;

    // Audio vector params — framework auto-serialization is unreliable for
    // vector<float>; save the first element explicitly (broadcast on restore).
    auto get0 = [](const ofParameter<vector<float>>& p) -> float {
        return p.get().empty() ? 0.0f : p.get()[0];
    };
    j["amp"]         = get0(ampP);
    j["pitch"]       = get0(pitchP);
    j["duration"]    = get0(durationP);
    j["position"]    = get0(positionP);
    j["panaz"]       = get0(panAzP);
    j["chance"]      = get0(chanceP);
    j["atrigbdiv"]   = get0(autoTrigBeatDivP);
    j["posjit"]      = get0(posJitP);
    j["pitchjit"]    = get0(pitchJitP);
    j["durjit"]      = get0(durJitP);
    j["ampjit"]      = get0(ampJitP);
    j["panazjit"]    = get0(panAzJitP);
    j["levels"]      = get0(levelsP);

    // LFO group parameters — registered with addInspectorParameter so not
    // included in the automatic preset serialization; save them explicitly.
    static const char* lfoNames[6] = {"Pos","Dur","Pitch","Amp","Pan","TrRt"};
    for(int t = 0; t < NUM_LFO; t++) {
        std::string n = lfoNames[t];
        j["lfoShape" + n] = get0(lfoGroups[t].shape);
        j["lfoSpeed" + n] = get0(lfoGroups[t].speed);
        j["lfoPhase" + n] = get0(lfoGroups[t].phase);
        j["lfoQuant" + n] = get0(lfoGroups[t].quant);
        j["lfoStr"   + n] = get0(lfoGroups[t].strength);
    }
}

void scGrainBox::presetRecallAfterSettingParameters(ofJson& j) {
    if(j.contains("samplePath") && !j["samplePath"].get<std::string>().empty())
        loadSample(j["samplePath"].get<std::string>());
    if(j.contains("inpoint"))    inPointP.set(j["inpoint"].get<float>());
    if(j.contains("outpoint"))   outPointP.set(j["outpoint"].get<float>());
    if(j.contains("envAttack"))  envAttackP.set(j["envAttack"].get<float>());
    if(j.contains("envRelease")) envReleaseP.set(j["envRelease"].get<float>());
    if(j.contains("envTension")) envTensionP.set(j["envTension"].get<float>());
    if(j.contains("waveZoom"))   waveZoom   = j["waveZoom"].get<float>();
    if(j.contains("waveScroll")) waveScroll = j["waveScroll"].get<float>();
    if(j.contains("browserW"))   browserW   = j["browserW"].get<float>();
    computeEnvData();
    envNeedsUpdate = true;

    // Restore audio vector params and LFO params — all saved as scalars,
    // restored as uniform vectors of current channel count.
    int nch = numChannelsP.get();
    auto setVf = [nch](ofParameter<vector<float>>& p, const ofJson& root,
                       const std::string& key) {
        if(!root.contains(key)) return;
        float v = root[key].get<float>();
        p.set(vector<float>(nch, v));
    };

    setVf(ampP,             j, "amp");
    setVf(pitchP,           j, "pitch");
    setVf(durationP,        j, "duration");
    setVf(positionP,        j, "position");
    setVf(panAzP,           j, "panaz");
    setVf(chanceP,          j, "chance");
    setVf(autoTrigBeatDivP, j, "atrigbdiv");
    setVf(posJitP,          j, "posjit");
    setVf(pitchJitP,        j, "pitchjit");
    setVf(durJitP,          j, "durjit");
    setVf(ampJitP,          j, "ampjit");
    setVf(panAzJitP,        j, "panazjit");
    setVf(levelsP,          j, "levels");

    static const char* lfoNames[6] = {"Pos","Dur","Pitch","Amp","Pan","TrRt"};
    for(int t = 0; t < NUM_LFO; t++) {
        std::string n = lfoNames[t];
        setVf(lfoGroups[t].shape,    j, "lfoShape" + n);
        setVf(lfoGroups[t].speed,    j, "lfoSpeed" + n);
        setVf(lfoGroups[t].phase,    j, "lfoPhase" + n);
        setVf(lfoGroups[t].quant,    j, "lfoQuant" + n);
        setVf(lfoGroups[t].strength, j, "lfoStr"   + n);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// GUI — main window
// ════════════════════════════════════════════════════════════════════════════

void scGrainBox::drawGrainBoxWindow() {
    float zoom = ofxOceanodeShared::getZoomLevel();
    float m    = 6.0f * zoom;   // general margin / gap

    ImGui::SetNextWindowSize(ImVec2(900 * zoom, 506 * zoom), ImGuiCond_FirstUseEver);
    std::string winTitle = "GrainBox " + ofToString(getNumIdentifier());
    bool open = showWindow.get();
    if(!ImGui::Begin(winTitle.c_str(), &open,
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        if(!open) showWindow = false;
        ImGui::End();
        return;
    }
    if(!open) showWindow = false;

    float totalW  = ImGui::GetContentRegionAvail().x;
    float totalH  = ImGui::GetContentRegionAvail().y;
    float bw      = browserW * zoom;
    float dragW   = 5.0f * zoom;
    float rw      = totalW - bw - dragW;

    // Init waveH on first open
    if(waveH <= 0.0f) waveH = totalH * 0.52f;

    // ── Left: file browser ────────────────────────────────────────────────
    ImGui::BeginChild("##gbBrowser", ImVec2(bw, totalH), true);
    drawBrowser(bw, totalH);
    ImGui::EndChild();

    // ── Browser resize handle ─────────────────────────────────────────────
    ImGui::SameLine(0.0f, 0.0f);
    {
        ImVec2 hPos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##gbBrowserResize", ImVec2(dragW, totalH));
        bool hov = ImGui::IsItemHovered();
        bool act = ImGui::IsItemActive();
        ImU32 hcol = (hov || act) ? IM_COL32(120, 170, 255, 220) : IM_COL32(55, 55, 70, 130);
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(hPos.x + 1.0f * zoom, hPos.y),
            ImVec2(hPos.x + dragW - 1.0f * zoom, hPos.y + totalH), hcol, 2.0f * zoom);
        if(hov || act) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if(act && ImGui::IsMouseDragging(0)) {
            float newBW = browserW + ImGui::GetIO().MouseDelta.x / zoom;
            browserW = std::max(80.0f, std::min(totalW / zoom * 0.5f, newBW));
            bw = browserW * zoom;
            rw = totalW - bw - dragW;
        }
    }
    ImGui::SameLine(0.0f, 0.0f);

    // ── Right panel (single scrollable column) ────────────────────────────
    ImGui::BeginChild("##gbRight", ImVec2(rw, totalH), false,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    float rightW = rw - ImGui::GetStyle().ScrollbarSize - m;

    // ── Waveform (resizable height) ───────────────────────────────────────
    {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##gbWave", ImVec2(rightW, waveH));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        drawWaveformPanel(dl, pos, rightW, waveH);
    }

    // ── Zoom + scroll controls strip ──────────────────────────────────────
    {
        float vis = 1.0f / waveZoom;
        if(ImGui::Button("-##wz")) {
            waveZoom   = std::max(1.0f, waveZoom / 1.5f);
            vis        = 1.0f / waveZoom;
            waveScroll = std::min(waveScroll, 1.0f - vis);
        }
        ImGui::SameLine(0, m * 0.5f);
        ImGui::Text("x%.1f", waveZoom);
        ImGui::SameLine(0, m * 0.5f);
        if(ImGui::Button("+##wz")) {
            float pivot = waveScroll + vis * 0.5f;
            waveZoom    = std::min(64.0f, waveZoom * 1.5f);
            vis         = 1.0f / waveZoom;
            waveScroll  = std::max(0.0f, std::min(1.0f - vis, pivot - vis * 0.5f));
        }
        ImGui::SameLine(0, m * 0.5f);
        if(ImGui::Button("fit##wz")) { waveZoom = 1.0f; waveScroll = 0.0f; vis = 1.0f; }
        ImGui::SameLine(0, m);
        float scrollMax = std::max(0.0f, 1.0f - vis);
        ImGui::SetNextItemWidth(rightW - ImGui::GetCursorPos().x - m);
        ImGui::SliderFloat("##wscr", &waveScroll, 0.0f, scrollMax);
    }

    // ── Resize handle ─────────────────────────────────────────────────────
    {
        float  handleH = 5.0f * zoom;
        ImVec2 hpos    = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##gbWaveResize", ImVec2(rightW, handleH));
        bool hov = ImGui::IsItemHovered();
        bool act = ImGui::IsItemActive();
        ImU32 hcol = (hov || act) ? IM_COL32(120, 170, 255, 220) : IM_COL32(55, 55, 70, 180);
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(hpos.x, hpos.y + 1.0f * zoom),
            ImVec2(hpos.x + rightW, hpos.y + handleH - 1.0f * zoom), hcol, 2.0f * zoom);
        if(hov || act) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if(act && ImGui::IsMouseDragging(0))
            waveH = std::max(40.0f * zoom, waveH + ImGui::GetIO().MouseDelta.y);
    }

    // ── Bottom row: 4 equal cells separated by margins ────────────────────
    //   cell 0 = envelope  |  cells 1-3 = controls (3 columns)
    float cellW   = (rightW - 3.0f * m) / 4.0f;
    float bottomH = 240.0f * zoom;   // fixed height — not derived from window size

    // Cell 0 — envelope (wrapped in child so SameLine works correctly)
    ImGui::BeginChild("##gbEnvCell", ImVec2(cellW, bottomH), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawEnvelopePanel(cellW, bottomH);
    ImGui::EndChild();
    ImGui::SameLine(0.0f, m);

    // Cells 1-3 — grain controls (3 columns)
    drawControlsPanel(rightW - cellW - m, bottomH);

    // ── Modulation row ────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    float modH = 220.0f * zoom;
    drawModRow(rightW, modH);

    ImGui::EndChild();
    ImGui::End();
}

// ── File browser ─────────────────────────────────────────────────────────────

void scGrainBox::drawBrowser(float /*w*/, float /*h*/) {
    // Navigation bar
    if(ImGui::Button("...")) {
        auto res = ofSystemLoadDialog("Select Samples Folder", true, browseDir);
        if(res.bSuccess) refreshBrowse(res.getPath());
    }
    ImGui::SameLine();
    {
        std::string dirName = std::filesystem::path(browseDir).filename().string();
        if(dirName.empty()) dirName = browseDir;
        ImGui::TextUnformatted(dirName.c_str());
    }

    if(ImGui::Button("^ ..")) {
        auto parent = std::filesystem::path(browseDir).parent_path().string();
        if(!parent.empty() && parent != browseDir)
            refreshBrowse(parent);
    }
    ImGui::Separator();

    int n = (int)browseEntries.size();
    browserSel = std::min(browserSel, n - 1);

    ImGui::BeginChild("##gbBrowserList", ImVec2(0, 0), false);

    // Keyboard navigation
    if(ImGui::IsWindowFocused() || ImGui::IsWindowHovered()) {
        if(ImGui::IsKeyPressed(ImGuiKey_DownArrow) && n > 0) {
            browserSel = std::min(browserSel + 1, n - 1);
            if(browserSel >= 0 && !browseEntries[browserSel].isDir)
                triggerPreview(browseEntries[browserSel].fullPath);
        }
        if(ImGui::IsKeyPressed(ImGuiKey_UpArrow) && n > 0) {
            browserSel = std::max(browserSel - 1, 0);
            if(browserSel >= 0 && !browseEntries[browserSel].isDir)
                triggerPreview(browseEntries[browserSel].fullPath);
        }
        if(ImGui::IsKeyPressed(ImGuiKey_Enter) && browserSel >= 0 && browserSel < n) {
            if(browseEntries[browserSel].isDir) {
                std::string navPath = browseEntries[browserSel].fullPath;
                browserSel = -1;
                refreshBrowse(navPath);
                n = 0;
            } else {
                loadSample(browseEntries[browserSel].fullPath);
                stopPreview();
                // Browser stays on the current folder — no navigation needed.
            }
        }
    }

    bool navigated = false;
    for(int i = 0; i < n && !navigated; i++) {
        auto& e = browseEntries[i];
        ImGui::PushID(i);

        std::string lbl = (e.isDir ? "[D] " : "    ") + e.name;
        bool selected = (i == browserSel);

        if(ImGui::Selectable(lbl.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
            if(e.isDir) {
                std::string navPath = e.fullPath;
                browserSel = -1;
                ImGui::PopID();
                refreshBrowse(navPath);
                navigated = true;
                break;
            } else {
                browserSel = i;
                triggerPreview(e.fullPath);
                if(ImGui::IsMouseDoubleClicked(0)) {
                    loadSample(e.fullPath);
                    stopPreview();
                }
            }
        }

        if(selected) ImGui::SetScrollHereY(0.5f);

        if(!e.isDir) {
            if(ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload("GB_SAMPLE", e.fullPath.c_str(), e.fullPath.size() + 1);
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

// ── Waveform panel ────────────────────────────────────────────────────────────

void scGrainBox::drawWaveformPanel(ImDrawList* dl, ImVec2 pos, float w, float h) {
    float zoom = ofxOceanodeShared::getZoomLevel();

    // Background
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), IM_COL32(22, 22, 26, 255));
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), IM_COL32(70, 70, 80, 255));

    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();
    ImVec2 mouse = ImGui::GetIO().MousePos;

    // ── Setup variables ───────────────────────────────────────────────────
    float visStart = waveScroll;
    float visEnd   = waveScroll + (1.0f / waveZoom);
    float midY     = pos.y + h * 0.5f;

    // ── Grain highlights ──────────────────────────────────────────────────
    int n = numChannelsP.get();
    float inP  = inPointP.get();
    float outP = outPointP.get();
    float span = std::max(0.001f, outP - inP);

    for(const auto& gh : grainHighlights) {
        // gh.position is already an absolute buffer position (0..1) from SC's usedPos
        float normPos = (gh.position - visStart) / (visEnd - visStart);
        if(normPos < -0.1f || normPos > 1.1f) continue;

        // grain duration (seconds) → fraction of buffer → fraction of visible window → pixels
        float bufDur = (bufferDurationSecs > 0.0f) ? bufferDurationSecs : 1.0f;
        float grainBufFraction = gh.duration / bufDur;
        float visWindow        = std::max(0.0001f, visEnd - visStart);
        float grainWidthPx     = (grainBufFraction / visWindow) * w;
        grainWidthPx = std::max(2.0f * zoom, std::min(w, grainWidthPx));

        float x = pos.x + normPos * w;
        float fade = gh.lifeTime / gh.maxLife;
        float alpha = fade * gh.amp;

        ofColor c; c.setHsb((gh.channel * 40 + 20) % 256, 200, 255);
        ImU32 col = IM_COL32(c.r, c.g, c.b, (int)(alpha * 180));
        dl->AddRectFilled(
            ImVec2(x, pos.y),
            ImVec2(x + grainWidthPx, pos.y + h),
            col
        );
    }

    // ── Waveform (redraw on top with semi-transparency) ───────────────────
    // This ensures the waveform is always visible, even with dense grain highlights
    if(!waveformPeaks.empty()) {
        int bins = (int)waveformPeaks.size();
        for(int i = 0; i < (int)w; i++) {
            float norm    = visStart + ((float)i / w) * (visEnd - visStart);
            int   binIdx  = std::min((int)(norm * bins), bins - 1);
            float peak    = (binIdx >= 0 && binIdx < bins) ? waveformPeaks[binIdx] : 0.0f;
            float px      = pos.x + (float)i;
            float py      = midY - peak * (h * 0.45f);
            float py2     = midY + peak * (h * 0.45f);
            dl->AddLine(ImVec2(px, py), ImVec2(px, py2), IM_COL32(70, 160, 110, 100));
        }
    }

    // ── Per-channel position cursors ──────────────────────────────────────
    const auto& positions = positionP.get();
    for(int i = 0; i < n; i++) {
        float p    = positions.empty() ? 0.0f : positions[i % (int)positions.size()];
        float absP = inP + p * span;
        float normP = (absP - visStart) / (visEnd - visStart);
        if(normP < 0.0f || normP > 1.0f) continue;
        float x = pos.x + normP * w;
        ofColor c; c.setHsb((i * 40 + 20) % 256, 255, 255);
        dl->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + h), IM_COL32(c.r, c.g, c.b, 200), 1.5f * zoom);
    }

    // ── InPoint / OutPoint handles ────────────────────────────────────────
    float inNorm  = (inP  - visStart) / (visEnd - visStart);
    float outNorm = (outP - visStart) / (visEnd - visStart);
    float inX     = pos.x + inNorm  * w;
    float outX    = pos.x + outNorm * w;

    // Shaded overlay for inaccessible region
    if(inNorm > 0.0f)
        dl->AddRectFilled(pos, ImVec2(pos.x + inNorm * w, pos.y + h), IM_COL32(0, 0, 0, 100));
    if(outNorm < 1.0f)
        dl->AddRectFilled(ImVec2(pos.x + outNorm * w, pos.y), ImVec2(pos.x + w, pos.y + h), IM_COL32(0, 0, 0, 100));

    // In/out lines
    dl->AddLine(ImVec2(inX,  pos.y), ImVec2(inX,  pos.y + h), IM_COL32(255, 220, 60, 220), 2.0f * zoom);
    dl->AddLine(ImVec2(outX, pos.y), ImVec2(outX, pos.y + h), IM_COL32(255, 100, 60, 220), 2.0f * zoom);

    // Drag handles (triangles at top)
    dl->AddTriangleFilled(
        ImVec2(inX, pos.y), ImVec2(inX + 8*zoom, pos.y), ImVec2(inX, pos.y + 8*zoom),
        IM_COL32(255, 220, 60, 255));
    dl->AddTriangleFilled(
        ImVec2(outX, pos.y), ImVec2(outX - 8*zoom, pos.y), ImVec2(outX, pos.y + 8*zoom),
        IM_COL32(255, 100, 60, 255));

    // Hit-test for handle dragging
    constexpr float HANDLE_R = 12.0f;
    bool nearIn  = (std::abs(mouse.x - inX)  < HANDLE_R * zoom && mouse.y > pos.y && mouse.y < pos.y + h);
    bool nearOut = (std::abs(mouse.x - outX) < HANDLE_R * zoom && mouse.y > pos.y && mouse.y < pos.y + h);

    if(active) {
        if(wavDragMode == WavDrag::None) {
            if(ImGui::IsMouseClicked(0)) {
                if(nearIn)  wavDragMode = WavDrag::InPoint;
                else if(nearOut) wavDragMode = WavDrag::OutPoint;
            }
        }
        if(ImGui::IsMouseDragging(0)) {
            float norm = std::max(0.0f, std::min(1.0f, (mouse.x - pos.x) / w));
            float absVal = visStart + norm * (visEnd - visStart);
            absVal = std::max(0.0f, std::min(1.0f, absVal));
            if(wavDragMode == WavDrag::InPoint)
                inPointP.set(std::min(absVal, outPointP.get() - 0.001f));
            else if(wavDragMode == WavDrag::OutPoint)
                outPointP.set(std::max(absVal, inPointP.get() + 0.001f));
        }
    }
    if(!ImGui::IsMouseDown(0))
        wavDragMode = WavDrag::None;

    // Labels
    dl->AddText(ImVec2(pos.x + 4*zoom, pos.y + 4*zoom),
        IM_COL32(200, 200, 200, 180),
        samplePath.empty() ? "— no file loaded —" : std::filesystem::path(samplePath).filename().c_str());
}

// ── Envelope panel ────────────────────────────────────────────────────────────
// Self-contained: curve display (top) + Attack/Release/Shape sliders (bottom).

void scGrainBox::drawEnvelopePanel(float w, float h) {
    float zoom = ofxOceanodeShared::getZoomLevel();
    float m    = 8.0f * zoom;   // inner margin
    // Clamp to actual available content width so drawing never overflows the right margin
    w = std::min(w, ImGui::GetContentRegionAvail().x);

    // Fixed curve height — envelope display is non-resizable
    float lineH   = ImGui::GetTextLineHeightWithSpacing();
    float sliderH = 3.0f * (lineH + 2.0f * zoom) + m;
    float curveH  = 100.0f * zoom;   // fixed, not derived from available h

    // ── Curve display ─────────────────────────────────────────────────────
    ImVec2 pos = ImGui::GetCursorScreenPos();
    // Inset the curve area by margin
    ImVec2 cPos = ImVec2(pos.x + m, pos.y + m);
    float  cW   = w - 2.0f * m;
    float  cH   = curveH - m;

    ImGui::InvisibleButton("##gbEnv", ImVec2(w, curveH));
    bool   active = ImGui::IsItemActive();
    ImVec2 mouse  = ImGui::GetIO().MousePos;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background (full cell, then inset curve)
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + curveH), IM_COL32(22, 22, 28, 255));
    dl->AddRectFilled(cPos, ImVec2(cPos.x + cW, cPos.y + cH), IM_COL32(28, 28, 38, 255));
    dl->AddRect(cPos, ImVec2(cPos.x + cW, cPos.y + cH), IM_COL32(70, 70, 80, 200));

    // Curve
    if(!envData.empty()) {
        int N = (int)envData.size();
        for(int i = 0; i < N - 1; i++) {
            float x1 = cPos.x + ((float)i       / (N - 1)) * cW;
            float x2 = cPos.x + ((float)(i + 1) / (N - 1)) * cW;
            float y1 = cPos.y + cH - envData[i]     * (cH - 2.0f * zoom);
            float y2 = cPos.y + cH - envData[i + 1] * (cH - 2.0f * zoom);
            dl->AddRectFilled(ImVec2(x1, y1), ImVec2(x2, cPos.y + cH), IM_COL32(255, 140, 60, 45));
            dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(255, 160, 80, 220), 1.5f * zoom);
        }
    }

    // Attack / Release guide lines
    float rawAtk = envAttackP.get();   // 0..1
    float rawRel = envReleaseP.get();  // 0..1
    float total  = rawAtk + rawRel;
    float atk    = (total > 1.0f) ? rawAtk / total : rawAtk;
    float rel    = (total > 1.0f) ? rawRel / total : rawRel;
    float tensionNorm = (envTensionP.get() + 1.0f) * 0.5f;  // -1..1 → 0..1
    float atkX = cPos.x + atk * cW;
    float relX = cPos.x + (1.0f - rel) * cW;
    float tenY = cPos.y + (1.0f - tensionNorm) * cH;
    dl->AddLine(ImVec2(atkX, cPos.y), ImVec2(atkX, cPos.y + cH), IM_COL32(100, 200, 255, 120), 1.0f * zoom);
    dl->AddLine(ImVec2(relX, cPos.y), ImVec2(relX, cPos.y + cH), IM_COL32(100, 200, 255, 120), 1.0f * zoom);

    // Drag handles
    constexpr float HR = 5.0f;
    auto drawHandle = [&](ImVec2 p, ImU32 col) {
        dl->AddCircleFilled(p, HR * zoom, col);
        dl->AddCircle(p, HR * zoom, IM_COL32(255, 255, 255, 160), 16, 1.0f * zoom);
    };
    drawHandle(ImVec2(atkX, cPos.y + cH * 0.25f), IM_COL32(100, 200, 255, 200));
    drawHandle(ImVec2(relX, cPos.y + cH * 0.25f), IM_COL32(100, 200, 255, 200));
    drawHandle(ImVec2(cPos.x + cW * 0.5f, tenY),  IM_COL32(255, 200, 80, 200));

    // Handle dragging
    static int dragHandle = -1;
    if(active && ImGui::IsMouseClicked(0)) {
        auto dist2 = [](ImVec2 a, ImVec2 b) {
            return (a.x-b.x)*(a.x-b.x) + (a.y-b.y)*(a.y-b.y);
        };
        float thr = (HR * zoom + 5.0f) * (HR * zoom + 5.0f);
        if     (dist2(mouse, ImVec2(atkX, cPos.y + cH * 0.25f)) < thr) dragHandle = 0;
        else if(dist2(mouse, ImVec2(relX, cPos.y + cH * 0.25f)) < thr) dragHandle = 1;
        else if(dist2(mouse, ImVec2(cPos.x + cW * 0.5f, tenY))  < thr) dragHandle = 2;
    }
    if(!ImGui::IsMouseDown(0)) dragHandle = -1;
    if(dragHandle >= 0 && ImGui::IsMouseDragging(0)) {
        float nx = std::max(0.f, std::min(1.f, (mouse.x - cPos.x) / cW));
        float ny = 1.0f - std::max(0.f, std::min(1.f, (mouse.y - cPos.y) / cH));
        if(dragHandle == 0)      envAttackP.set(nx);
        else if(dragHandle == 1) envReleaseP.set(1.0f - nx);
        else                     envTensionP.set(ny * 2.0f - 1.0f);  // 0..1 → -1..1
    }

    // Label
    dl->AddText(ImVec2(cPos.x + 3*zoom, cPos.y + 2*zoom),
        IM_COL32(160, 160, 160, 140), "Envelope");

    // ── Sliders below the curve ───────────────────────────────────────────
    ImGui::SetCursorScreenPos(ImVec2(pos.x + m, pos.y + curveH + m * 0.5f));

    float sw      = w - 2.0f * m;
    float envLblW = 48.0f * zoom;   // fixed label column width within envelope panel
    auto envSlider = [&](const char* label, const char* id, ofParameter<float>& p,
                         float mn, float mx) {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(envLblW);
        ImGui::SetNextItemWidth(sw - envLblW);
        float v = p.get();
        if(ImGui::SliderFloat(id, &v, mn, mx)) p.set(v);
    };

    envSlider("Atk",     "##envAtk", envAttackP,  0.0f, 1.0f);
    envSlider("Rel",     "##envRel", envReleaseP, 0.0f, 1.0f);
    envSlider("Tension", "##envTen", envTensionP, -1.0f, 1.0f);
}

// ── Controls panel ────────────────────────────────────────────────────────────

void scGrainBox::drawControlsPanel(float w, float h) {
    float zoom   = ofxOceanodeShared::getZoomLevel();
    int   n      = numChannelsP.get();
    float gap    = 6.0f * zoom;
    float colW   = (w - 2.0f * gap) / 3.0f;
    float m      = 4.0f * zoom;  // inner margin
    // Fixed label column width — keeps sliders aligned within each column
    float labelW = 58.0f * zoom;

    // Scalar value → broadcast to all n channels
    auto setAllF = [&](ofParameter<vector<float>>& p, float val) {
        p.set(vector<float>(n, val));
    };
    auto getF = [](const ofParameter<vector<float>>& p) -> float {
        return p.get().empty() ? 0.f : p.get()[0];
    };

    // Label left | slider right — no overflow, respects margins
    auto sliderF = [&](const char* label, const char* id,
                       ofParameter<vector<float>>& p, float mn, float mx) {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(colW - labelW - m);
        float v = getF(p);
        if(ImGui::SliderFloat(id, &v, mn, mx)) setAllF(p, v);
    };
    auto sliderScalar = [&](const char* label, const char* id,
                             ofParameter<float>& p, float mn, float mx) {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(colW - labelW - m);
        float v = p.get();
        if(ImGui::SliderFloat(id, &v, mn, mx)) p.set(v);
    };
    auto toggle = [&](const char* label, ofParameter<bool>& p) {
        bool v = p.get();
        if(ImGui::Checkbox(label, &v)) p.set(v);
    };

    // Filled section header: draws a dark rounded rect with bold label text.
    // Must be called inside a child window so GetWindowDrawList() is scoped.
    auto secHdr = [&](const char* label) {
        ImDrawList* dl   = ImGui::GetWindowDrawList();
        ImVec2      cur  = ImGui::GetCursorScreenPos();
        float       lh   = ImGui::GetTextLineHeight();
        float       padV = 3.0f * zoom;
        float       padH = 6.0f * zoom;
        float       bh   = lh + padV * 2.0f;
        dl->AddRectFilled(
            ImVec2(cur.x, cur.y),
            ImVec2(cur.x + colW - m * 0.5f, cur.y + bh),
            IM_COL32(55, 60, 90, 230), 3.0f * zoom);
        dl->AddText(
            ImVec2(cur.x + padH, cur.y + padV),
            IM_COL32(200, 215, 255, 255), label);
        ImGui::Dummy(ImVec2(colW, bh));
        ImGui::SetCursorScreenPos(ImVec2(cur.x, cur.y + bh + m * 0.5f));
    };

    // ── Column 0: Trigger + Region ───────────────────────────────────────────
    ImGui::BeginChild("##gbCol0", ImVec2(colW, h), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(m, m));
    secHdr("Trigger");
    toggle("AutoTrig##c", autoTrigP);
    ImGui::SameLine(0, m);
    toggle("Uniq##ct", uniqueTrigP);
    sliderF("BDiv",   "##abdiv",   autoTrigBeatDivP, 0.125f, 64.f);
    sliderF("Chance", "##chance",  chanceP,          0.0f,   1.0f);
    if(ImGui::Button("Sync##c")) syncGateP.set(1);
    ImGui::Spacing();
    secHdr("Region");
    sliderScalar("In",   "##inp",  inPointP,  0.f, 1.f);
    sliderScalar("Out",  "##outp", outPointP, 0.f, 1.f);
    ImGui::PopStyleVar();
    ImGui::EndChild();

    ImGui::SameLine(0.0f, gap);

    // ── Column 1: Grain ──────────────────────────────────────────────────────
    ImGui::BeginChild("##gbCol1", ImVec2(colW, h), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(m, m));
    secHdr("Grain");
    toggle("DynDur##c", dynamicDurP);
    ImGui::SameLine(0, m);
    toggle("TrgDur##c", trigDurP);
    sliderF ("Amp",    "##amp", ampP,       0.f,  1.f);
    sliderF ("Pitch",  "##pit", pitchP,   -48.f, 48.f);
    sliderF ("Dur",    "##dur", durationP,  0.f,  1.f);
    sliderF ("Pos",    "##pos", positionP,  0.f,  1.f);
    sliderF ("PanAz",  "##pan", panAzP,     0.f,  2.f);
    ImGui::Spacing();
    secHdr("Levels");
    sliderF ("Levels", "##lvl", levelsP,    0.f,  1.f);
    ImGui::PopStyleVar();
    ImGui::EndChild();

    ImGui::SameLine(0.0f, gap);

    // ── Column 2: Jitter ─────────────────────────────────────────────────────
    ImGui::BeginChild("##gbCol2", ImVec2(colW, h), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(m, m));
    secHdr("Jitter");
    toggle("Uniq##cj", uniqueJitP);
    sliderF ("Pos",   "##pjit",  posJitP,   0.f,  1.f);
    sliderF ("Pitch", "##ijit",  pitchJitP, 0.f,  12.f);
    sliderF ("Dur",   "##djit",  durJitP,   0.f,  1.f);
    sliderF ("Amp",   "##ajit",  ampJitP,   0.f,  1.f);
    sliderF ("PanAz", "##pnjit", panAzJitP, 0.f,  2.f);
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

// ── LFO preview canvas ────────────────────────────────────────────────────────

void scGrainBox::drawLFOPreview(ImDrawList* dl, ImVec2 pos, float w, float h,
                                 int shape, float phase, float quant, float strength, float barDiv) {
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), IM_COL32(18, 18, 26, 255));
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), IM_COL32(55, 55, 70, 200));

    // ── Tempo grid (behind waveform) ─────────────────────────────────────────
    // Display = 1 LFO cycle = 1/barDiv bars = 4/barDiv beats.
    // Beat lines at phase k*barDiv/4, bar lines at phase k*barDiv.
    {
        float safeBarDiv = std::max(0.001f, barDiv);
        // Beat lines (faint) — cap at 128 to avoid overdraw
        for(int k = 1; k < 128; k++) {
            float frac = k * safeBarDiv / 4.0f;
            if(frac >= 1.0f) break;
            float xPx = pos.x + frac * w;
            dl->AddLine(ImVec2(xPx, pos.y), ImVec2(xPx, pos.y + h), IM_COL32(70, 70, 120, 110), 1.0f);
        }
        // Bar lines (brighter)
        for(int k = 1; k < 128; k++) {
            float frac = k * safeBarDiv;
            if(frac >= 1.0f) break;
            float xPx = pos.x + frac * w;
            dl->AddLine(ImVec2(xPx, pos.y), ImVec2(xPx, pos.y + h), IM_COL32(120, 120, 220, 200), 1.5f);
        }
    }

    // Zero-strength: draw baseline only
    if(strength < 0.001f) {
        float y = pos.y + h - 1.0f;
        dl->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + w, y), IM_COL32(80, 80, 100, 180));
        // Still draw phase line so user sees where phase is set
        if(phase > 0.001f) {
            float xPx = pos.x + phase * w;
            dl->AddLine(ImVec2(xPx, pos.y), ImVec2(xPx, pos.y + h), IM_COL32(255, 200, 60, 160), 1.5f);
        }
        return;
    }

    // Fixed random values for deterministic randStep / randCurve preview
    static const float rv[5] = {0.30f, 0.80f, 0.15f, 0.65f, 0.45f};

    auto lfoAt = [&](float ph) -> float {
        ph = std::fmod(ph, 1.0f);
        float raw = 0.5f;
        switch(shape) {
            case 0: raw = std::sin(ph * 2.0f * float(M_PI)) * 0.5f + 0.5f; break;
            case 1: raw = 1.0f - std::abs(ph * 2.0f - 1.0f); break;
            case 2: raw = ph; break;
            case 3: raw = 1.0f - ph; break;
            case 4: { // randomStep
                int seg = std::min(3, (int)(ph * 4));
                raw = rv[seg];
            } break;
            case 5: { // randomCurve
                float idx = ph * 4.0f;
                int   i0  = std::min(3, (int)idx);
                float frac = idx - (float)i0;
                raw = rv[i0] * (1.0f - frac) + rv[i0 + 1] * frac;
            } break;
        }
        // Quantize: N steps 0, 1/N, ..., (N-1)/N
        if(quant >= 1.0f) {
            int qi = (int)quant;
            raw = std::floor(raw * qi);
            raw = std::min(raw, (float)(qi - 1));
            raw /= qi;
        }
        return raw;
    };

    // ── Waveform (1 cycle, phase-offset applied) ──────────────────────────────
    int    N    = (int)w;
    ImVec2 prev = {-1.0f, -1.0f};
    for(int i = 0; i < N; i++) {
        // Map pixel i → waveform phase, offset by initPhase
        float t   = std::fmod((float)i / (float)N + phase, 1.0f);
        float val = lfoAt(t);
        float x   = pos.x + (float)i;
        float y   = pos.y + h - val * h;
        y = std::max(pos.y + 1.0f, std::min(pos.y + h - 1.0f, y));
        if(prev.x >= 0) {
            if(shape == 4) {
                dl->AddLine(ImVec2(prev.x, prev.y), ImVec2(x, prev.y), IM_COL32(100,200,255,210), 1.5f);
                dl->AddLine(ImVec2(x, prev.y),      ImVec2(x, y),      IM_COL32(100,200,255,100), 1.0f);
            } else {
                dl->AddLine(prev, ImVec2(x, y), IM_COL32(100, 200, 255, 210), 1.5f);
            }
        }
        prev = {x, y};
    }

    // ── Baseline ──────────────────────────────────────────────────────────────
    float by = pos.y + h - 1.0f;
    dl->AddLine(ImVec2(pos.x, by), ImVec2(pos.x + w, by), IM_COL32(80, 80, 100, 100));

    // ── Phase marker (where the cycle starts / sync-reset target) ────────────
    // Drawn last so it's always visible on top of the waveform.
    // Phase=0 is at the left edge (no line needed); draw for any meaningful offset.
    if(phase > 0.001f) {
        float xPx = pos.x + phase * w;
        dl->AddLine(ImVec2(xPx, pos.y), ImVec2(xPx, pos.y + h), IM_COL32(255, 200, 60, 220), 1.5f);
        // Small tick at bottom
        dl->AddTriangleFilled(
            ImVec2(xPx, pos.y + h),
            ImVec2(xPx - 3.0f, pos.y + h - 5.0f),
            ImVec2(xPx + 3.0f, pos.y + h - 5.0f),
            IM_COL32(255, 200, 60, 200));
    }
}

// ── Modulation row ────────────────────────────────────────────────────────────

void scGrainBox::drawModRow(float w, float h) {
    float zoom  = ofxOceanodeShared::getZoomLevel();
    float gap   = 4.0f * zoom;
    float colW  = (w - gap * (NUM_LFO - 1)) / NUM_LFO;
    float m     = 4.0f * zoom;
    float lw    = 44.0f * zoom;   // label column width inside each cell

    static const char* shapeLabels[6] = {"sin","tri","saw","iSw","rSt","rCv"};
    // Strength max per target (matches SC OceanodeParameter range)
    static const float lfoStrMax[6] = {1.0f, 1.0f, 48.0f, 1.0f, 2.0f, 32.0f};

    auto getF0 = [](const ofParameter<vector<float>>& p) -> float {
        return p.get().empty() ? 0.f : p.get()[0];
    };
    auto setAllF0 = [this](ofParameter<vector<float>>& p, float v) {
        int n = numChannelsP.get();
        p.set(vector<float>(n, v));
    };

    for(int t = 0; t < NUM_LFO; t++) {
        if(t > 0) ImGui::SameLine(0.0f, gap);

        ImGui::PushID(t);
        ImGui::BeginChild("##gbMod", ImVec2(colW, h), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(m * 0.5f, m * 0.5f));

        // Header: filled background label + Unique toggle on the right
        {
            ImDrawList* hdl  = ImGui::GetWindowDrawList();
            ImVec2      hcur = ImGui::GetCursorScreenPos();
            float       lh   = ImGui::GetTextLineHeight();
            float       padV = 3.0f * zoom;
            float       bh   = lh + padV * 2.0f;
            hdl->AddRectFilled(
                ImVec2(hcur.x, hcur.y),
                ImVec2(hcur.x + colW - m * 0.5f, hcur.y + bh),
                IM_COL32(55, 60, 90, 230), 3.0f * zoom);
            hdl->AddText(ImVec2(hcur.x + 5.0f * zoom, hcur.y + padV),
                         IM_COL32(200, 215, 255, 255), LFO_TARGET_NAMES[t]);
            // Unique checkbox drawn over the right portion of the header
            float chkW = 50.0f * zoom;
            ImGui::SetCursorScreenPos(ImVec2(hcur.x + colW - m * 0.5f - chkW, hcur.y + padV * 0.5f));
            {
                bool uv = uniqueLfoTargetP[t].get();
                if(ImGui::Checkbox("Uniq##lfu", &uv)) uniqueLfoTargetP[t].set(uv);
            }
            ImGui::SetCursorScreenPos(ImVec2(hcur.x, hcur.y + bh + m * 0.5f));
        }

        // Shape selector — 6 small toggle buttons in two rows of 3
        float btnW = (colW - 2.0f * m - 5.0f * gap * 0.5f) / 6.0f;
        int curShape = (int)std::round(getF0(lfoGroups[t].shape));
        for(int s = 0; s < 6; s++) {
            if(s > 0) ImGui::SameLine(0.0f, gap * 0.5f);
            bool active = (s == curShape);
            if(active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f,0.55f,0.85f,1.0f));
            ImGui::PushID(s);
            if(ImGui::Button(shapeLabels[s], ImVec2(btnW, 0))) {
                setAllF0(lfoGroups[t].shape, (float)s);
            }
            ImGui::PopID();
            if(active) ImGui::PopStyleColor();
        }

        // Speed slider (bar divisions: 0.125=1/8 bar cycle, 1=1/bar, 4=4×/bar)
        {
            ImGui::TextUnformatted("Spd");
            ImGui::SameLine(lw);
            ImGui::SetNextItemWidth(colW - lw - m);
            float v = getF0(lfoGroups[t].speed);
            if(ImGui::SliderFloat("##spd", &v, 0.125f, 128.0f, "%.3f"))
                setAllF0(lfoGroups[t].speed, v);
        }

        // Phase slider (initial phase / sync reset target)
        {
            ImGui::TextUnformatted("Phs");
            ImGui::SameLine(lw);
            ImGui::SetNextItemWidth(colW - lw - m);
            float v = getF0(lfoGroups[t].phase);
            if(ImGui::SliderFloat("##phs", &v, 0.0f, 1.0f, "%.2f"))
                setAllF0(lfoGroups[t].phase, v);
        }

        // Quant slider (integer)
        {
            ImGui::TextUnformatted("Qnt");
            ImGui::SameLine(lw);
            ImGui::SetNextItemWidth(colW - lw - m);
            float v = getF0(lfoGroups[t].quant);
            if(ImGui::SliderFloat("##qnt", &v, 0.0f, 32.0f, "%.0f"))
                setAllF0(lfoGroups[t].quant, std::round(v));
        }

        // Strength slider (range depends on target)
        {
            ImGui::TextUnformatted("Str");
            ImGui::SameLine(lw);
            ImGui::SetNextItemWidth(colW - lw - m);
            float v = getF0(lfoGroups[t].strength);
            const char* fmt = (t == 2) ? "%.1fst" : (t == 5) ? "%.2fbd" : "%.2f";  // pitch=semitones, TrRt=beatdiv
            if(ImGui::SliderFloat("##str", &v, 0.0f, lfoStrMax[t], fmt))
                setAllF0(lfoGroups[t].strength, v);
        }

        // LFO preview
        float previewH = h - ImGui::GetCursorPosY() - m;
        if(previewH > 16.0f * zoom) {
            ImVec2 pPos = ImGui::GetCursorScreenPos();
            float  pW   = colW - 2.0f * m;
            ImGui::Dummy(ImVec2(pW, previewH));
            drawLFOPreview(ImGui::GetWindowDrawList(), pPos, pW, previewH,
                           curShape,
                           getF0(lfoGroups[t].phase),
                           getF0(lfoGroups[t].quant),
                           getF0(lfoGroups[t].strength),
                           getF0(lfoGroups[t].speed));
        }

        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::PopID();
    }
}
