//
//  scChordBox.h
//  ofxOceanodeSuperCollider
//
//  ChordBox: chord progressions (the Chord Sequence node of ofxSantiNodes,
//  every feature), locked to the transport and played sample accurately.
//
//  • Steps: a chord (library), a scale, a cypher symbol ("Dm7", "G7b9"), a
//    scale degree or a functional chord (tonic / subdominant / dominant
//    variants of the key's scale), each with its length in beats, transpose,
//    inversion and diatonic deviation. Import from chord text, the saved
//    progressions or the jazz standards.
//  • Order: Index (the Index input picks the step: values, or an audio
//    cable, sample accurately), Ascending, Descending, Random, Markov (a
//    weight per step pair). The timed orders follow the position: a seek or
//    a loop plays the chord that belongs there; Reset starts step 1 now.
//  • Outputs (1..16): each shapes the step's notes (source, octave,
//    transpose, inversion, voicing, spread, size / expand, bass, rootless,
//    fold, range, voice leading, random octave / chromatic / detune, glide).
//    Each has its vector (OutputN) and an audio Pitch N (16 channels).
//  • Variation: a random transpose / inversion every N steps or laps, and the
//    transposition sequencer. Every random draw is seeded (Seed; 0: fresh
//    draws on every pass).
//  • Audio (ChordBoxClock / ChordBoxVoices, synthdefs/Defaults/ChordBox):
//    Trig on each change, Root AR, ChPh AR, ProgPh AR, Pitch N (glide in SC).
//    The vectors (OutputN, Root, ChPh, ProgPh) stay for control-rate nodes.
//  • Presets of the old node: tools/migrate_chordsequence.py. Its state
//    (the "state" JSON) is the same, so its snapshots import too.
//

#pragma once

#include "../../../ofxOceanodeSuperColliderConfig.h"


#include "../../../scNode.h"
#include "../../../ui/scTimelineMenu.h"
#include "../shared/scAudioParam.h"
#include "../../../timing/scTransportSync.h"
#include "scChordBoxEngine.h"
#include "../shared/scChordLink.h"
#include "../shared/scSnapshots.h"
#include "../../../ui/scDropdowns.h"
#include "ofxSCSynth.h"
#include "ofxSCGroup.h"
#include "ofxSCBuffer.h"
#include "ofxSCBus.h"
#include "ofxSCServer.h"
#include "ofxOceanodeContainer.h"
#include "imgui.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

class scChordBox : public scNode {
public:
    // its window (SC Mixer Pro's "Go to origin synth window" opens and focuses it)
    std::string getWindowTitle() override { return (canvasID == "Canvas" ? "" : canvasID + "/") + "ChordBox " + ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return &showWindow; }
private:
public:
    static constexpr int TPB = 48, CHUNK = 8 * TPB, SLOTS = 4, CLOCKED = SLOTS * CHUNK;
    static constexpr int RING = 256, INDEX_ROW = 256, LIVE_ROW = 288, ROWS = 290, NV = chordbox::VOICES;
    static constexpr int FIXED_PORTS = 4;     // Trig, Root AR, ChPh AR, ProgPh AR; then Pitch N
    static constexpr int64_t NONE = INT64_MIN;
    static constexpr int NUM_SNAPS = 128;

    scChordBox() : scNode("ChordBox") {}
    ~scChordBox() override {
        listeners.unsubscribeAll();
        for(auto& v : outVecs) chordlink::registry().erase(v.get());
        chordlink::registry().erase(&rootOut);
        for(auto& [server, st] : servers) releaseServer(server, st, true);
        servers.clear();
    }

    // ════════════════════════════════════════════════════════════════════════
    // Setup
    // ════════════════════════════════════════════════════════════════════════

    void setup() override {
        timeline.setup(this, [this](const std::string& k){ publish(k); }, [this](const std::string& k){ unpublish(k); },
                       [this](){ return publishedKeys; });
        description = "Chord progressions locked to the transport: chords, scales, cypher symbols, degrees or functional "
                      "harmony per step; Index, ascending, descending, random or Markov order; each output voiced, "
                      "voice led and shaped. Vector outputs and sample-accurate audio (Pitch per output, Trig, Root, "
                      "phasors). Show opens the timeline and every control; right-click a control to type a value or publish it.";
        indexIn.attach(addParameter(indexP.set("Index", 0, 0, 32)), availableInputs, [this](){ refreshPorts(); });
        addParameter(numChordsP.set("NumChords", 4, 1, chordbox::MAX_STEPS));
        addParameter(transposeP.set("Transpose", 0, -48, 48));
        addParameter(bendP.set("Pitchbend", 0.0f, -24.0f, 24.0f));
        addParameter(inversionP.set("Inversion", 0, -16, 16));
        snaps.setupParameters();
        addParameter(scaleIn.set("Scale", {0, 2, 4, 5, 7, 9, 11}, {-127.0f}, {127.0f}));
        addParameter(resetP.set("Reset"));
        addOutputParameter(rootOut.set("Root", 0.0f, 0.0f, 11.0f));
        {   // a node's Root input on this one reads the root at each beat too (scChordLink.h)
            chordlink::Provider pr;
            pr.timed = [this](){ return timed(); };
            pr.notesAt = [this](double beat){ return std::vector<float>{engine.eventAt(beat).root}; };
            pr.rootAt = [this](double beat){ return engine.eventAt(beat).root; };
            pr.chordStartAt = [this](double beat){ return engine.eventAt(beat).start; };
            pr.revision = [this](){ return revision; };
            chordlink::registry()[&rootOut] = pr;
        }
        addOutputParameter(chphOut.set("ChPh", 0.0f, 0.0f, 1.0f));
        addOutputParameter(progphOut.set("ProgPh", 0.0f, 0.0f, 1.0f));
        addParameter(showWindow.set("Show", false));
        addParameter(seedP.set("Seed", 1, 0, 99999));
        for(const char* n : {"Trig", "Root AR", "ChPh AR", "ProgPh AR"}) scNode::addOutput(n);

        engine.loadLibraries(pitchclassDir());
        loadImportSources();
        engine.externalScale = scaleIn.get();
        engine.seed = effectiveSeed();
        syncOutputs();

        listeners.push(indexP.newListener([this](int&){ if(engine.progressionOrder == chordbox::ORDER_INDEX) indexDirty = true; }));
        listeners.push(numChordsP.newListener([this](int& n){
            if(n == (int)engine.progression.size()) return;
            engine.resize(n);
            changed();
        }));
        listeners.push(transposeP.newListener([this](int& v){ if(v != engine.globalTranspose){ engine.globalTranspose = v; changed(); } }));
        listeners.push(bendP.newListener([this](float& v){ if(v != engine.globalPitchBend){ engine.globalPitchBend = v; changed(); } }));
        listeners.push(inversionP.newListener([this](int& v){ if(v != engine.globalInvert){ engine.globalInvert = v; changed(); } }));
        listeners.push(scaleIn.newListener([this](std::vector<float>& v){
            if(v == engine.externalScale) return;
            engine.externalScale = v;
            if(engine.useExternalScale) changed();
        }));
        listeners.push(resetP.newListener([this](){ doReset(); }));
        listeners.push(seedP.newListener([this](int&){ changed(); }));
        listeners.push(resendParams.newListener([this](){
            for(auto& [server, st] : servers) sendStatic(server, st);
            follower.requestAnchor();
        }));
        snapshotHandle = addParameter(snaps.snapshotP);   // recalls by number (1..): a dropdown of their names
        {
            scSnapshots::Host h;
            h.folder = "ChordBox";
            h.title = "ChordBox";
            h.capture = [this](){ return ofJson{{"state", engine.toJson()}}; };
            // a progression does not blend: the state switches at the switch point
            h.apply = [this](const ofJson& a, const ofJson& b, float, bool sw){
                const ofJson& s = sw ? b : a;
                if(!s.contains("state") || s["state"] == appliedSnapshotState) return;
                appliedSnapshotState = s["state"];
                loadState(appliedSnapshotState);
                changed();
            };
            h.importLegacy = [this](){   // the list this node kept before (Snapshots.json)
                const std::string old = ofToDataPath("nodeSnapshots/ChordBox/Snapshots.json", true);
                if(!ofFile(old).exists()) return false;
                bool any = false;
                try {
                    const ofJson j = ofLoadJson(old);
                    if(j.contains("snapshots") && j["snapshots"].is_array())
                        for(const auto& o : j["snapshots"])
                            if(o.is_object()) any = snaps.appendSnapshot({{"name", o.value("name", std::string("Snapshot"))},
                                                                           {"state", o.value("state", ofJson::object())}}) || any;
                } catch(...) {}
                return any;
            };
            h.bpm = [this](){ return std::max(1.0f, getFrameTransportState().current.bpm); };
            snaps.setup(h);
        }
        rebuildControls();
        changed();
    }

    // ════════════════════════════════════════════════════════════════════════
    // Frame
    // ════════════════════════════════════════════════════════════════════════

    void update(ofEventArgs&) override {
        timeline.update();
        scDropdowns::setNames(snapshotHandle, snaps.slotNames());
        const float now = ofGetElapsedTimef();
        for(auto& [server, st] : servers){
            if(st.allocPending && (!server->isNRTSyncPending() || now - st.allocTime > 2.0f)){
                st.allocPending = false;
                invalidateServer(st);
            }
        }
        snaps.update();
        watchTransport();
        if(dirty){ dirty = false; rebuild(); }
        syncPublishedProxies();
        updateControlOutputs();
        fillBuffers();
        sendTransportAnchors();
    }

    void draw(ofEventArgs&) override { if(showWindow) drawWindow(); }

    // ════════════════════════════════════════════════════════════════════════
    // scNode: a group (the clock, then one voices synth per output)
    // ════════════════════════════════════════════════════════════════════════

    void buildSynth(ofxSCServer* server) override {
        if(!server) return;
        ServerState& st = servers[server];
        st.server = server;
        releaseServer(server, st, false);
        st.group = new ofxSCGroup(server);
        ensureResources(server, st);
    }
    void createSynth(ofxSCServer* server) override {
        auto it = servers.find(server);
        if(it == servers.end() || !it->second.group) return;
        auto& st = it->second;
        st.group->create(0, 1);
        st.created = true;
        if(!getActive()) st.group->run(false);
        st.clock = new ofxSCSynth("ChordBoxClock", server);
        sendStatic(server, st);
        st.clock->set("run", 0.0f);
        st.clock->createAndRun(1, st.group->nodeID, true);
        syncVoices(server, st);
        forceHardAnchor = true;
        follower.requestAnchor();
        st.liveCount++;
        if(engine.progressionOrder == chordbox::ORDER_INDEX) writeLiveRow(st);
    }
    void moveSynthBefore(ofxSCServer* server, int nodeID) override {
        auto it = servers.find(server);
        if(it == servers.end() || !it->second.created) return;
        sendStatic(server, it->second);
        it->second.group->moveBefore(nodeID);
    }
    void free(ofxSCServer* server) override {
        auto it = servers.find(server);
        if(it == servers.end()) return;
        releaseServer(server, it->second, false);
        it->second.outputBuses.clear();
    }
    int getLastSynthID(ofxSCServer* server) override {
        auto it = servers.find(server);
        return it != servers.end() && it->second.created && it->second.group ? it->second.group->nodeID : -1;
    }
    void setOutputBus(ofxSCServer* server, int index, int bus) override {
        if(!server || index < 0) return;
        ServerState& st = servers[server];
        st.outputBuses[index] = bus;
        sendOutputBus(st, index);
    }
    int getOutputNumChannels(int index) override {
        return index >= 0 && index < (int)engine.outputs.size() ? std::max(1, engine.outputs[index].outputSize) : -1;
    }
    int getOutputBusIndex(ofxSCServer* server, int index) override {
        auto it = servers.find(server);
        if(it == servers.end()) return -1;
        auto b = it->second.outputBuses.find(index);
        return b != it->second.outputBuses.end() ? b->second : -1;
    }
    void setInputBus(ofxSCServer* server, scNode* node, int bus) override {
        if(indexIn.take(server, node, bus)){ auto& st = servers[server]; st.server = server; sendIndexInput(st); }
    }
    void resetInputBusses(ofxSCServer* server, int) override {
        indexIn.reset(server);
        auto& st = servers[server];
        st.server = server;
        sendIndexInput(st);
    }
    void activate() override {
        for(auto& [server, st] : servers) if(st.group && st.created) st.group->run(true);
        forceHardAnchor = true;
        follower.requestAnchor();
    }
    void deactivate() override { for(auto& [server, st] : servers) if(st.group && st.created) st.group->run(false); }
    // The recorder writes the patch's state at score time zero: the tables too
    void resendParametersForNRT() override {
        resendParams.notify();
        for(auto& [server, st] : servers){ invalidateServer(st); st.liveCount++; }
        fillBuffers();
    }

    // ════════════════════════════════════════════════════════════════════════
    // Presets (the old node's "state", its outputs before the connections)
    // ════════════════════════════════════════════════════════════════════════

    void presetSave(ofJson& json) override {
        json["state"] = engine.toJson();
        json["ChordBoxPublished"] = publishedKeys;
        timeline.save(json, "ChordBoxTimeline");
        json["ChordBoxTab"] = selectedTab;
        json["ChordBoxSnapshots"] = snaps.settingsJson();
    }
    void loadBeforeConnections(ofJson& json) override {
        int n = 1;
        if(json.contains("state")){
            const auto& s = json["state"];
            if(s.contains("outputs") && s["outputs"].is_array()) n = std::max(1, (int)s["outputs"].size());
            else n = std::max(1, s.value("numOutputs", 1));
        }
        engine.outputs.resize(std::clamp(n, 1, chordbox::MAX_OUTPUTS));
        syncOutputs();
        rebuildControls();
        std::vector<std::string> wanted;
        if(json.contains("ChordBoxPublished") && json["ChordBoxPublished"].is_array())
            for(const auto& k : json["ChordBoxPublished"]) if(k.is_string()) wanted.push_back(k.get<std::string>());
        const auto old = publishedKeys;
        for(const auto& k : old) if(std::find(wanted.begin(), wanted.end(), k) == wanted.end() && !hasConnection(k)) unpublish(k);
        for(const auto& k : wanted) publish(k);
            timeline.load(json, "ChordBoxTimeline");
}
    void presetRecallBeforeSettingParameters(ofJson& json) override { loadBeforeConnections(json); }
    void presetRecallAfterSettingParameters(ofJson& json) override {
        if(json.contains("state") && json["state"].is_object()) loadState(json["state"]);
        if(json.contains("ChordBoxTab") && json["ChordBoxTab"].is_number_integer()) selectedTab = json["ChordBoxTab"].get<int>();
        if(json.contains("ChordBoxSnapshots")) snaps.loadSettings(json["ChordBoxSnapshots"]);
        engine.originBeat = 0.0;
        newSalt();
        changed();
    }
    void presetHasLoaded() override { changed(); }

private:
    // ════════════════════════════════════════════════════════════════════════
    // State
    // ════════════════════════════════════════════════════════════════════════

    static std::string pitchclassDir(){ return ofToDataPath("Supercollider/Pitchclass/", true); }

    // Something changed: everything derived again
    void changed(){ dirty = true; }
    void rebuild(){
        engine.seed = effectiveSeed();
        engine.sanitize();
        engine.invalidate();
        revision++;
        live = chordbox::Engine::Live();
        indexDirty = true;
        lastEventK = NONE;
        syncOutputs();
        syncNodeParams();
        if(controlsOutputs != (int)engine.outputs.size() || controlsSteps != (int)engine.progression.size()) rebuildControls();
        for(auto& [server, st] : servers){ invalidateServer(st); sendStatic(server, st); syncVoices(server, st); }
    }
    void loadState(const ofJson& state){
        engine.fromJson(state);
        engine.externalScale = scaleIn.get();
        syncOutputs();
        rebuildControls();
    }
    void syncNodeParams(){
        auto setIf = [](auto& p, auto v){ if(p.get() != v) p.setWithoutEventNotifications(v); };
        setIf(numChordsP, (int)engine.progression.size());
        setIf(transposeP, engine.globalTranspose);
        setIf(bendP, engine.globalPitchBend);
        setIf(inversionP, engine.globalInvert);
        for(size_t o = 0; o < outSizes.size() && o < engine.outputs.size(); o++) setIf(*outSizes[o], engine.outputs[o].outputSize);
    }
    uint64_t effectiveSeed() const { return seedP.get() != 0 ? (uint64_t)seedP.get() : salt; }
    void newSalt(){
        static std::mt19937_64 rng((uint64_t)ofGetElapsedTimeMicros() ^ 0xC40B0Cull);
        salt = rng() | (1ull << 40);
    }
    bool timed() const { return engine.progressionOrder != chordbox::ORDER_INDEX; }
    double beatNow() const { return getFrameTransportState().current.beatPosition; }
    // Reset: step 1 starts now (the next tick); Index: the variations start again
    void doReset(){
        if(seedP.get() == 0) newSalt();
        engine.originBeat = std::ceil(beatNow() * TPB - 1e-6) / TPB;
        changed();
    }
    // Seed 0: fresh draws on every jump and loop pass
    void watchTransport(){
        const auto t = getFrameTransportState();
        const bool jumped = t.current.generation != lastGeneration || (t.current.isPlaying && !lastPlaying) || t.current.loopCount != lastLoopCount;
        lastGeneration = t.current.generation;
        lastPlaying = t.current.isPlaying;
        lastLoopCount = t.current.loopCount;
        bpb = beatsPerBar();
        if(jumped && seedP.get() == 0 && timed()){ newSalt(); changed(); }
    }

    // ── The node's outputs: OutputN and outNsize, and the Pitch N ports ─────
    void syncOutputs(){
        const int n = std::clamp((int)engine.outputs.size(), 1, chordbox::MAX_OUTPUTS);
        while((int)outVecs.size() < n){
            const int i = (int)outVecs.size();
            auto vec = std::make_shared<ofParameter<std::vector<float>>>();
            vec->set("Output" + ofToString(i + 1), std::vector<float>{0.0f}, std::vector<float>{-FLT_MAX}, std::vector<float>{FLT_MAX});
            vec->setSerializable(false);
            addOutputParameter(*vec);
            outVecs.push_back(vec);
            auto size = std::make_shared<ofParameter<int>>();
            size->set("out" + ofToString(i + 1) + "size", engine.outputs[i].outputSize, 1, 32);
            addParameter(*size);
            outSizes.push_back(size);
            listeners.push(size->newListener([this, i](int& v){
                if(i < (int)engine.outputs.size() && engine.outputs[i].outputSize != v){ engine.outputs[i].outputSize = std::max(1, v); changed(); }
            }));
            scNode::addOutput("Pitch " + ofToString(i + 1));
            glides.emplace_back();
            // what an ArpeggiatorBox reads through a connection to this output (scChordLink.h)
            chordlink::Provider pr;
            pr.timed = [this](){ return timed(); };
            pr.notesAt = [this, i](double beat){
                const auto& ev = engine.eventAt(beat);
                return i < (int)ev.outs.size() ? ev.outs[i] : std::vector<float>{0.0f};
            };
            pr.rootAt = [this](double beat){ return engine.eventAt(beat).root; };
            pr.chordStartAt = [this](double beat){ return engine.eventAt(beat).start; };
            pr.revision = [this](){ return revision; };
            chordlink::registry()[vec.get()] = pr;
        }
        while((int)outVecs.size() > n){
            const int i = (int)outVecs.size() - 1;
            chordlink::registry().erase(outVecs[i].get());
            removeParameter(outVecs[i]->getEscapedName());
            removeParameter(outSizes[i]->getEscapedName());
            outVecs.pop_back();
            outSizes.pop_back();
            scNode::removeOutput(FIXED_PORTS + i);
            for(auto& [server, st] : servers) st.outputBuses.erase(FIXED_PORTS + i);
            glides.pop_back();
        }
    }
    void refreshPorts(){ for(auto& output : scNode::outputs) output = output; }

    // ════════════════════════════════════════════════════════════════════════
    // Control-rate outputs (the old node's: OutputN with its glide, Root, phasors)
    // ════════════════════════════════════════════════════════════════════════

    struct Glide { std::vector<float> from, to, shown; float start = 0.0f, time = 0.0f; };
    std::vector<Glide> glides;
    int64_t lastEventK = NONE;
    chordbox::Engine::Live live;
    bool indexDirty = true;
    int playingEntry = 0;
    double playingStart = 0.0, playingDur = 1.0;
    chordbox::Mods playingMods;

    float glideSeconds(const chordbox::OutputConfig& c) const {
        if(c.glideBeats) return (float)(c.glideMs * 60.0 / std::max(1.0f, getFrameTransportState().current.bpm));
        return c.glideMs / 1000.0f;
    }
    void glideTo(int o, const std::vector<float>& target, bool instant){
        if(o < 0 || o >= (int)glides.size()) return;
        auto& g = glides[o];
        const float t = glideSeconds(engine.outputs[o]);
        if(instant || t <= 0.0005f || g.shown.empty()){ g.from = g.to = g.shown = target; g.time = 0.0f; }
        else { g.from = g.shown; g.to = target; g.start = ofGetElapsedTimef(); g.time = t; }
    }
    void updateControlOutputs(){
        const int n = (int)engine.outputs.size();
        if(timed()){
            const double beat = beatNow();
            const auto& ev = engine.eventAt(beat);
            if(ev.k != lastEventK){
                const bool first = lastEventK == NONE;
                lastEventK = ev.k;
                for(int o = 0; o < n && o < (int)ev.outs.size(); o++) glideTo(o, ev.outs[o], first);
                rootOut = ev.root;
                playingEntry = ev.entry;
                playingStart = ev.start;
                playingDur = ev.dur;
                playingMods = ev.mods;
            }
            chphOut = (float)std::clamp((beat - ev.start) / std::max(0.001, ev.dur), 0.0, 1.0);
            progphOut = ev.lapLen > 0.0 ? (float)std::clamp(std::fmod(std::max(0.0, beat - ev.lapStart), ev.lapLen) / ev.lapLen, 0.0, 1.0) : 0.0f;
        }else{
            if(indexDirty){
                const bool force = live.lastIndex < 0;
                const int index = indexP.get();
                const bool changedIndex = live.lastIndex != chordbox::wrapIndex(index, (int)engine.progression.size());
                if(changedIndex || force){
                    const auto outs = engine.playIndex(live, index, force);
                    for(int o = 0; o < n && o < (int)outs.size(); o++) glideTo(o, outs[o], force);
                    playingEntry = live.lastIndex;
                    playingMods = engine.lastIndexMods;
                    rootOut = engine.rootPitchClass(engine.progression[playingEntry], playingMods);
                    for(auto& [server, st] : servers){ st.liveCount++; writeLiveRow(st); }
                }
                indexDirty = false;
            }
            chphOut = 0.0f;
            progphOut = 0.0f;
        }
        const float now = ofGetElapsedTimef();
        for(int o = 0; o < n && o < (int)glides.size(); o++){
            auto& g = glides[o];
            if(g.time > 0.0f){
                const float p = std::clamp((now - g.start) / g.time, 0.0f, 1.0f);
                const size_t m = std::max(g.from.size(), g.to.size());
                g.shown.assign(m, 0.0f);
                for(size_t i = 0; i < m; i++) g.shown[i] = ofLerp(chordbox::sampleVector(g.from, i), chordbox::sampleVector(g.to, i), p);
                if(p >= 1.0f){ g.time = 0.0f; g.shown = g.to; }
            }
            const auto shown = g.shown.empty() ? std::vector<float>{0.0f} : g.shown;
            if(o < (int)outVecs.size() && outVecs[o]->get() != shown) *outVecs[o] = shown;
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // The players' tables
    // ════════════════════════════════════════════════════════════════════════

    struct ServerState {
        ofxSCServer* server = nullptr;
        ofxSCGroup* group = nullptr;
        bool created = false;
        ofxSCSynth* clock = nullptr;
        std::vector<ofxSCSynth*> voices;
        ofxSCBuffer* marks = nullptr;
        ofxSCBuffer* info = nullptr;
        std::vector<ofxSCBuffer*> rows;
        ofxSCBus* rowBus = nullptr;
        bool allocPending = false;
        float allocTime = 0.0f;
        std::map<int, int> outputBuses;
        std::array<int64_t, SLOTS> slotChunk;
        std::array<int64_t, RING> rowEvent;
        bool indexTable = false;
        int liveCount = 0;
        ServerState(){ slotChunk.fill(NONE); rowEvent.fill(NONE); }
    };
    std::map<ofxSCServer*, ServerState> servers;

    static bool bufferLive(ofxSCServer* s, ofxSCBuffer* b){
        return s && b && b->index >= 0 && b->index < (int)s->buffers.size() && s->buffers[b->index] == b;
    }
    static bool audioBusLive(ofxSCServer* s, ofxSCBus* b){
        return s && b && b->index >= 0 && b->index < (int)s->audioBusses.size() && s->audioBusses[b->index] == b;
    }
    void invalidateServer(ServerState& st){ st.slotChunk.fill(NONE); st.rowEvent.fill(NONE); st.indexTable = false; }
    ofxSCBuffer* allocBuffer(ofxSCServer* server, ServerState& st, int frames, int channels){
        auto* b = new ofxSCBuffer(frames, channels, server);
        b->alloc();
        st.allocPending = true;
        st.allocTime = ofGetElapsedTimef();
        return b;
    }
    void ensureResources(ofxSCServer* server, ServerState& st){
        bool any = false;
        if(!bufferLive(server, st.marks)){ delete st.marks; st.marks = allocBuffer(server, st, CLOCKED, 4); any = true; }
        if(!bufferLive(server, st.info)){ delete st.info; st.info = allocBuffer(server, st, ROWS, 4); any = true; }
        while(st.rows.size() < engine.outputs.size()) st.rows.push_back(nullptr);
        for(size_t o = 0; o < engine.outputs.size(); o++)
            if(!bufferLive(server, st.rows[o])){ delete st.rows[o]; st.rows[o] = allocBuffer(server, st, ROWS, NV); any = true; }
        if(!audioBusLive(server, st.rowBus)){
            delete st.rowBus;
            st.rowBus = new ofxSCBus(RATE_AUDIO, 1, server);
            if(!audioBusLive(server, st.rowBus)){ st.rowBus->free(); delete st.rowBus; st.rowBus = nullptr; }
        }
        if(any){ server->requestNRTSync(); invalidateServer(st); }
    }
    void releaseServer(ofxSCServer* server, ServerState& st, bool resources){
        if(st.group && st.created) st.group->free();
        delete st.clock; st.clock = nullptr;
        for(auto* v : st.voices) delete v;
        st.voices.clear();
        delete st.group; st.group = nullptr;
        st.created = false;
        if(resources){
            auto freeBuf = [&](ofxSCBuffer*& b){ if(b){ if(bufferLive(server, b)) b->free(); delete b; b = nullptr; } };
            freeBuf(st.marks); freeBuf(st.info);
            for(auto*& b : st.rows) freeBuf(b);
            if(st.rowBus){ if(audioBusLive(server, st.rowBus)) st.rowBus->free(); delete st.rowBus; st.rowBus = nullptr; }
        }
    }
    // One voices synth per output, after the clock
    void syncVoices(ofxSCServer* server, ServerState& st){
        if(!st.created || !st.clock) return;
        ensureResources(server, st);
        const size_t n = engine.outputs.size();
        while(st.voices.size() < n){
            auto* v = new ofxSCSynth("ChordBoxVoices", server);
            st.voices.push_back(v);
            sendVoice(st, (int)st.voices.size() - 1);
            v->createAndRun(1, st.group->nodeID, true);
        }
        while(st.voices.size() > n){ st.voices.back()->free(); delete st.voices.back(); st.voices.pop_back(); }
        for(size_t o = 0; o < st.voices.size(); o++) sendVoice(st, (int)o);
    }
    void sendVoice(ServerState& st, int o){
        if(o < 0 || o >= (int)st.voices.size()) return;
        auto* v = st.voices[o];
        if(st.rowBus) v->set("rowbus", st.rowBus->index);
        if(o < (int)st.rows.size() && st.rows[o]) v->set("rows", st.rows[o]->index);
        v->set("glide", (double)glideSeconds(engine.outputs[o]));
        auto b = st.outputBuses.find(FIXED_PORTS + o);
        if(b != st.outputBuses.end()) v->set("pitchout", b->second);
    }
    void sendOutputBus(ServerState& st, int index){
        static const char* names[FIXED_PORTS] = {"trigout", "rootout", "chphout", "progphout"};
        auto b = st.outputBuses.find(index);
        if(b == st.outputBuses.end()) return;
        if(index < FIXED_PORTS){ if(st.clock) st.clock->set(names[index], b->second); }
        else sendVoice(st, index - FIXED_PORTS);
    }
    void sendIndexInput(ServerState& st){
        if(!st.clock) return;
        const int b = st.server ? indexIn.bus(st.server) : -1;
        if(b >= 0){ st.clock->set("index_sel", 1.0f); st.clock->mapan("index_ar", b, 1); }
        else st.clock->set("index_sel", 0.0f);
    }
    void sendStatic(ofxSCServer* server, ServerState& st){
        if(!st.clock) return;
        if(st.marks) st.clock->set("marks", st.marks->index);
        if(st.info) st.clock->set("info", st.info->index);
        if(st.rowBus) st.clock->set("rowbus", st.rowBus->index);
        st.clock->set("mode", timed() ? 1.0f : 0.0f);
        st.clock->set("steps", (float)engine.progression.size());
        st.clock->set("liveRow", (float)LIVE_ROW);
        st.clock->set("liveCount", (float)st.liveCount);
        for(int i = 0; i < FIXED_PORTS; i++) sendOutputBus(st, i);
        sendIndexInput(st);
        for(size_t o = 0; o < st.voices.size(); o++) sendVoice(st, (int)o);
    }

    // /b_setn runs (up to ~2000 values a message)
    struct Writer {
        ofxSCBuffer* buf;
        ofxOscMessage m;
        int values = 0;
        explicit Writer(ofxSCBuffer* b) : buf(b) { begin(); }
        void begin(){ m.clear(); m.setAddress("/b_setn"); m.addIntArg(buf->index); values = 0; }
        void run(int start, const float* data, int count){
            if(values + count > 2000) flush();
            m.addIntArg(start);
            m.addIntArg(count);
            for(int i = 0; i < count; i++) m.addFloatArg(data[i]);
            values += count + 2;
        }
        void flush(){ if(values > 0) buf->server->sendMsg(m); begin(); }
    };
    // A row: its info (root, ticks) and each output's 16 notes
    void writeRow(ServerState& st, int row, float root, float stepTicks, float lapTicks, int entry,
                  const std::vector<std::vector<float>>& outs){
        if(!st.info) return;
        const float info[4] = {root, stepTicks, lapTicks, (float)entry};
        Writer wi(st.info);
        wi.run(row * 4, info, 4);
        wi.flush();
        for(size_t o = 0; o < st.rows.size() && o < outs.size(); o++){
            if(!st.rows[o]) continue;
            float notes[NV];
            for(int v = 0; v < NV; v++) notes[v] = outs[o].empty() ? 0.0f : chordbox::sampleVector(outs[o], v);
            Writer w(st.rows[o]);
            w.run(row * NV, notes, NV);
            w.flush();
        }
    }
    void writeLiveRow(ServerState& st){
        if(st.allocPending || !bufferLive(st.server, st.info) || live.outs.empty()) return;
        writeRow(st, LIVE_ROW, rootOut.get(), 0.0f, 0.0f, playingEntry, live.outs);
        if(st.clock) st.clock->set("liveCount", (float)st.liveCount);
    }
    std::vector<int64_t> wantedChunks(){
        std::vector<int64_t> out;
        const auto t = getFrameTransportState().current;
        const int64_t c = chordbox::floorDiv((int64_t)std::floor(t.beatPosition * TPB), CHUNK);
        auto atStartOf = [&](int64_t chunk){
            double beat = (double)chunk * CHUNK / TPB;
            if(t.loopActive() && beat >= t.loopEndBeat - 1e-9){
                const double len = t.loopEndBeat - t.loopStartBeat;
                beat = t.loopStartBeat + std::fmod(beat - t.loopEndBeat, len);
            }
            return chordbox::floorDiv((int64_t)std::floor(beat * TPB), CHUNK);
        };
        for(int64_t w : {c, atStartOf(c + 1), c - 1, atStartOf(c + 2)})
            if(std::find(out.begin(), out.end(), w) == out.end()) out.push_back(w);
        return out;
    }
    // A chunk of ticks: each tick's row (+1), step start, ticks into the step / lap
    void writeChunk(ServerState& st, int64_t chunk){
        const int slot = (int)(((chunk % SLOTS) + SLOTS) % SLOTS);
        std::vector<float> data(CHUNK * 4, 0.0f);
        for(int i = 0; i < CHUNK; i++){
            const int64_t tick = chunk * CHUNK + i;
            const double beat = (double)tick / TPB;
            if(beat + 1e-9 < engine.originBeat) continue;
            const auto& ev = engine.eventAt(beat + 1e-9);
            const int row = (int)(((ev.k % RING) + RING) % RING);
            const int64_t startTick = (int64_t)std::llround(ev.start * TPB);
            const int64_t lapTick = (int64_t)std::llround(ev.lapStart * TPB);
            data[i * 4] = (float)(row + 1);
            data[i * 4 + 1] = tick == startTick ? 1.0f : 0.0f;
            data[i * 4 + 2] = (float)(tick - startTick);
            data[i * 4 + 3] = ev.lapLen > 0.0 ? (float)(tick - lapTick) : 0.0f;
            if(st.rowEvent[row] != ev.k){
                writeRow(st, row, ev.root, (float)(ev.dur * TPB), (float)(ev.lapLen * TPB), ev.entry, ev.outs);
                st.rowEvent[row] = ev.k;
            }
        }
        Writer w(st.marks);
        for(int off = 0; off < (int)data.size(); off += 1024)
            w.run(slot * CHUNK * 4 + off, data.data() + off, std::min(1024, (int)data.size() - off));
        w.flush();
        st.slotChunk[slot] = chunk;
    }
    void fillBuffers(){
        for(auto& [server, st] : servers){
            if(st.allocPending || !bufferLive(server, st.marks) || !bufferLive(server, st.info)) continue;
            bool rowsLive = true;
            for(size_t o = 0; o < engine.outputs.size(); o++) if(o >= st.rows.size() || !bufferLive(server, st.rows[o])) rowsLive = false;
            if(!rowsLive){ ensureResources(server, st); continue; }
            if(timed()){
                std::array<bool, SLOTS> claimed{};
                for(int64_t w : wantedChunks()){
                    const int slot = (int)(((w % SLOTS) + SLOTS) % SLOTS);
                    if(claimed[slot]) continue;
                    claimed[slot] = true;
                    if(st.slotChunk[slot] != w) writeChunk(st, w);
                }
            }else if(!st.indexTable){
                // the Index cable's table: each step's notes
                const auto table = engine.indexTable();
                const chordbox::Mods m = engine.modsFor(1, 1, 0);
                for(int i = 0; i < (int)table.size(); i++)
                    writeRow(st, INDEX_ROW + i, engine.rootPitchClass(engine.progression[i], m), 0.0f, 0.0f, i, table[i]);
                st.indexTable = true;
                writeLiveRow(st);
            }
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Transport (as EuclideanBox / ArpeggiatorBox)
    // ════════════════════════════════════════════════════════════════════════

    void sendTransportAnchors(){
        const auto anchors = follower.poll(getFrameTransportState());
        if(anchors.empty()) return;
        const int64_t period = (int64_t)std::llround(stepClock.getPeriod());
        for(const auto& a : anchors){
            const double tickPos = a.beat * TPB;
            const scTransportSync::StepAnchor sa = stepClock.make(tickPos, a.discontinuity, a.playing);
            const bool hard = sa.hard || forceHardAnchor;
            forceHardAnchor = false;
            const int64_t k = (int64_t)std::floor(tickPos);
            int64_t delta = (int64_t)std::floor(sa.pos) - (((k % period) + period) % period);
            if(delta > period / 2) delta -= period;
            else if(delta < -period / 2) delta += period;
            const int64_t absTick = k + delta;
            const float base = (float)(((absTick % CLOCKED) + CLOCKED) % CLOCKED);
            anchorIdCounter = anchorIdCounter % (1 << 20) + 1;
            if(hard) anchorHardCounter = anchorHardCounter % (1 << 20) + 1;
            bool pending = false;
            scTransportSync::AnchorScope scope(a);
            for(auto& [server, st] : servers){
                auto* s = st.clock;
                if(!s) continue;
                if(!s->isCreated()){ pending = true; continue; }
                s->set("bpm", a.bpm);
                s->set("run", a.playing ? 1.0f : 0.0f);
                s->set("anchorPos", (float)sa.pos);
                s->set("anchorFire", sa.fire ? 1.0f : 0.0f);
                s->set("base", base);
                if(hard) s->set("anchorHard", (float)anchorHardCounter);
                s->set("anchorId", (float)anchorIdCounter);
            }
            if(pending){ forceHardAnchor = true; follower.requestAnchor(); }
        }
    }
    double beatsPerBar() const {
        if(auto* container = getHostContainer()) {
            if(auto* extension = container->getAutomationExtension())
                return std::max(0.25, extension->getBeatsPerBar());
        }
        return 4.0;
    }

    // ════════════════════════════════════════════════════════════════════════
    // Controls: typed values and publishing (by key: "Key", "Out1 Octave", "Step2 Beats")
    // ════════════════════════════════════════════════════════════════════════

    struct Control {
        std::string key;
        char kind = 'f';                       // 'i' int, 'f' float, 'b' bool
        float lo = 0.0f, hi = 1.0f;
        std::vector<std::string> options;      // ints: a dropdown
        std::function<float()> get;
        std::function<void(float)> set;
    };
    std::map<std::string, Control> controls;
    std::vector<std::string> controlOrder;
    int controlsOutputs = -1, controlsSteps = -1;
    std::map<std::string, std::shared_ptr<ofxOceanodeAbstractParameter>> published;
    std::map<std::string, std::shared_ptr<ofParameter<int>>> proxyI;
    std::map<std::string, std::shared_ptr<ofParameter<float>>> proxyF;
    std::map<std::string, std::shared_ptr<ofParameter<bool>>> proxyB;
    std::vector<std::string> publishedKeys;
    scTimelineMenu timeline;              // "Add to Timeline" from the window
    bool publishedSeparator = false;

    void addControl(const std::string& key, char kind, float lo, float hi, std::function<float()> get, std::function<void(float)> set,
                    std::vector<std::string> options = {}){
        Control c{key, kind, lo, hi, std::move(options), std::move(get), [this, set](float v){ set(v); changed(); }};
        if(!controls.count(key)) controlOrder.push_back(key);
        controls[key] = c;
    }
    template<typename T>
    void ctlI(const std::string& key, T& field, int lo, int hi, std::vector<std::string> options = {}){
        addControl(key, 'i', (float)lo, (float)hi, [&field](){ return (float)field; },
                   [&field, lo, hi](float v){ field = std::clamp((int)std::lround(v), lo, hi); }, options);
    }
    void ctlF(const std::string& key, float& field, float lo, float hi){
        addControl(key, 'f', lo, hi, [&field](){ return field; }, [&field, lo, hi](float v){ field = std::clamp(v, lo, hi); });
    }
    void ctlB(const std::string& key, bool& field){
        addControl(key, 'b', 0, 1, [&field](){ return field ? 1.0f : 0.0f; }, [&field](float v){ field = v > 0.5f; });
    }
    static const std::vector<std::string>& orderNames(){ static const std::vector<std::string> n = {"Index", "Ascending", "Descending", "Random", "Markov"}; return n; }
    static const std::vector<std::string>& modeNames(){ static const std::vector<std::string> n = {"Chord", "Scale", "Cypher", "Degree", "Functional"}; return n; }
    static const std::vector<std::string>& sourceNames(){ static const std::vector<std::string> n = {"Chord", "Scale", "Root", "Key", "Chord sum"}; return n; }
    static const std::vector<std::string>& voicingNames(){ static const std::vector<std::string> n = {"None", "Close", "Open", "Drop 2", "Drop 3", "Shell"}; return n; }
    static const std::vector<std::string>& leadingNames(){ static const std::vector<std::string> n = {"Off", "Classic", "Smooth gravity (C anchor)", "Smooth gravity (free)"}; return n; }
    static std::vector<std::string> keyList(){ std::vector<std::string> k; for(int i = 0; i < 12; i++) k.push_back(chordbox::keyNames()[i]); return k; }

    // The controls for the current outputs and steps (the published ones kept)
    void rebuildControls(){
        controls.clear();
        controlOrder.clear();
        auto& e = engine;
        ctlI("Order", e.progressionOrder, 0, 4, orderNames());
        ctlI("Key", e.globalKey, 0, 11, keyList());
        ctlI("Scale Index", e.globalScaleIndex, 0, std::max(0, (int)e.scaleLibrary.size() - 1));
        ctlB("External Scale", e.useExternalScale);
        ctlI("Global Transpose", e.globalTranspose, -48, 48);
        ctlI("Global Inversion", e.globalInvert, -16, 16);
        ctlF("Global Bend", e.globalPitchBend, -24.0f, 24.0f);
        ctlI("Rnd Transpose Range", e.transposeRandomRange, 0, 48);
        ctlI("Rnd Transpose Quant", e.transposeRandomQuantization, 1, 12);
        ctlB("Rnd Transpose Per Step", e.transposeRandomStep);
        ctlI("Rnd Transpose Every", e.transposeRandomCycles, 1, 64);
        ctlI("Rnd Inversion Range", e.inversionRandomRange, 0, 16);
        ctlI("Rnd Inversion Quant", e.inversionRandomQuantization, 1, 8);
        ctlB("Rnd Inversion Per Step", e.inversionRandomStep);
        ctlI("Rnd Inversion Every", e.inversionRandomCycles, 1, 64);
        ctlI("Seq Every", e.transposeSequencerCycles, 1, 64);
        ctlI("Seq Mod", e.transposeSequencerMod, 0, 48);
        ctlB("Seq Per Step", e.transposeSequencerStepMode);
        for(int o = 0; o < (int)e.outputs.size(); o++){
            auto& c = e.outputs[o];
            const std::string p = "Out" + ofToString(o + 1) + " ";
            ctlI(p + "Source", c.sourceMode, 0, 4, sourceNames());
            ctlI(p + "Octave", c.octave, -4, 8);
            ctlI(p + "Transpose", c.transpose, -48, 48);
            ctlF(p + "Bend", c.pitchBend, -24.0f, 24.0f);
            ctlI(p + "Inversion", c.inversion, -16, 16);
            ctlI(p + "Voicing", c.voicingMode, 0, 5, voicingNames());
            ctlF(p + "Spread", c.voicingSpread, 0.0f, 24.0f);
            ctlI(p + "Size", c.outputSize, 1, 32);
            ctlB(p + "Expand", c.expandOutput);
            ctlB(p + "Sort", c.sortOutput);
            ctlB(p + "Add Bass", c.addBass);
            ctlI(p + "Bass Octave", c.bassOct, -4, 4);
            ctlB(p + "Rootless", c.rootLess);
            ctlB(p + "Fold 12", c.fold12);
            ctlI(p + "Voice Leading", c.voiceLeadingMode, 0, 3, leadingNames());
            ctlF(p + "Gravity", c.gravityAnchorWeight, 0.0f, 8.0f);
            ctlI(p + "Min Note", c.minNote, -12, 128);
            ctlI(p + "Max Note", c.maxNote, -12, 128);
            ctlF(p + "Octave Chance", c.octaveRandomProbability, 0.0f, 100.0f);
            ctlI(p + "Octave Range", c.octaveRandomRange, 0, 4);
            ctlF(p + "Chromatic Chance", c.chromaticDeviationProbability, 0.0f, 100.0f);
            ctlI(p + "Chromatic Range", c.chromaticDeviationRange, 0, 12);
            ctlF(p + "Detune", c.perNoteDetune, 0.0f, 1.0f);
            ctlF(p + "Glide", c.glideMs, 0.0f, 4000.0f);
            ctlB(p + "Glide Beats", c.glideBeats);
        }
        for(int s = 0; s < (int)e.progression.size(); s++){
            auto& en = e.progression[s];
            const std::string p = "Step" + ofToString(s + 1) + " ";
            ctlI(p + "Mode", en.mode, 0, 4, modeNames());
            ctlF(p + "Beats", en.beatDuration, 0.0625f, 64.0f);
            ctlI(p + "Transpose", en.transpose, -48, 48);
            ctlI(p + "Inversion", en.inversion, -16, 16);
            ctlI(p + "Degree", en.degree, 1, 14);
            ctlI(p + "Size", en.chordSize, 1, 12);
            ctlI(p + "Interval", en.stepInterval, 1, 7);
            ctlI(p + "Item", en.itemIndex, 0, 999);
            ctlI(p + "Function", en.functionalGroup, 0, 2, {"Tonic", "Subdominant", "Dominant"});
            ctlI(p + "Variant", en.functionalVariantIndex, 0, 16);
            ctlF(p + "Diatonic Chance", en.diatonicDeviationProbability, 0.0f, 100.0f);
            ctlI(p + "Diatonic Range", en.diatonicDeviationRange, 0, 7);
        }
        controlsOutputs = (int)e.outputs.size();
        controlsSteps = (int)e.progression.size();
        // published controls whose output / step is gone
        for(const auto& k : std::vector<std::string>(publishedKeys)) if(!controls.count(k) && !hasConnection(k)) unpublish(k);
    }
    bool hasConnection(const std::string& key) const {
        auto it = published.find(key);
        return it != published.end() && it->second && (it->second->hasInConnection() || it->second->hasOutConnections());
    }
    void publish(const std::string& key){
        if(published.count(key)) return;
        auto it = controls.find(key);
        if(it == controls.end()) return;
        const Control& c = it->second;
        if(!publishedSeparator){ addSeparator("Published", ofColor(200)); publishedSeparator = true; }
        if(c.kind == 'i'){
            auto p = std::make_shared<ofParameter<int>>();
            p->set(key, (int)std::lround(c.get()), (int)c.lo, (int)c.hi);
            auto h = addParameter(*p);
            if(!c.options.empty()) h->setDropdownOptions(c.options);
            listeners.push(p->newListener([this, key](int& v){ auto f = controls.find(key); if(f != controls.end() && (int)std::lround(f->second.get()) != v) f->second.set((float)v); }));
            proxyI[key] = p;
            published[key] = h;
        }else if(c.kind == 'f'){
            auto p = std::make_shared<ofParameter<float>>();
            p->set(key, c.get(), c.lo, c.hi);
            published[key] = addParameter(*p);
            listeners.push(p->newListener([this, key](float& v){ auto f = controls.find(key); if(f != controls.end() && f->second.get() != v) f->second.set(v); }));
            proxyF[key] = p;
        }else{
            auto p = std::make_shared<ofParameter<bool>>();
            p->set(key, c.get() > 0.5f);
            published[key] = addParameter(*p);
            listeners.push(p->newListener([this, key](bool& v){ auto f = controls.find(key); if(f != controls.end() && (f->second.get() > 0.5f) != v) f->second.set(v ? 1.0f : 0.0f); }));
            proxyB[key] = p;
        }
        publishedKeys.push_back(key);
        parameterGroupChanged.notify(this);
    }
    void unpublish(const std::string& key){
        auto it = published.find(key);
        if(it == published.end() || hasConnection(key)) return;
        removeParameter(it->second->getEscapedName());
        published.erase(it);
        proxyI.erase(key); proxyF.erase(key); proxyB.erase(key);
        publishedKeys.erase(std::remove(publishedKeys.begin(), publishedKeys.end(), key), publishedKeys.end());
        if(published.empty() && publishedSeparator){ removeSeparator("Published"); publishedSeparator = false; }
        parameterGroupChanged.notify(this);
    }
    // The published parameters show the window's values (unless something drives them)
    void syncPublishedProxies(){
        for(auto& [key, p] : proxyI){ auto c = controls.find(key); if(c != controls.end() && !published[key]->hasInConnection()){ const int v = (int)std::lround(c->second.get()); if(p->get() != v) p->setWithoutEventNotifications(v); } }
        for(auto& [key, p] : proxyF){ auto c = controls.find(key); if(c != controls.end() && !published[key]->hasInConnection()){ const float v = c->second.get(); if(p->get() != v) p->setWithoutEventNotifications(v); } }
        for(auto& [key, p] : proxyB){ auto c = controls.find(key); if(c != controls.end() && !published[key]->hasInConnection()){ const bool v = c->second.get() > 0.5f; if(p->get() != v) p->setWithoutEventNotifications(v); } }
    }
    void controlMenu(const std::string& key){
        auto it = controls.find(key);
        if(it == controls.end()) return;
        timeline.mark(key);
        if(published.count(key) && !timeline.isHidden(key)){
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y - 1), ImVec2(b.x, b.y - 1), ImGui::GetColorU32(ImGuiCol_Text));
        }
        if(!ImGui::BeginPopupContextItem(("##cbMenu" + key).c_str())) return;
        const Control& c = it->second;
        ImGui::TextDisabled("%s", key.c_str());
        ImGui::Separator();
        static std::map<std::string, std::array<char, 64>> buffers;
        auto& buf = buffers[key];
        const bool appearing = ImGui::IsWindowAppearing();
        if(appearing) std::snprintf(buf.data(), buf.size(), c.kind == 'f' ? "%.3f" : "%.0f", c.get());
        ImGui::TextDisabled("Value");
        ImGui::SetNextItemWidth(160.0f);
        if(appearing) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##cbTyped", buf.data(), buf.size(), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if(enter || ImGui::IsItemDeactivatedAfterEdit()){ c.set(std::clamp(ofToFloat(std::string(buf.data())), c.lo, c.hi)); if(enter) ImGui::CloseCurrentPopup(); }
        ImGui::Separator();
        if(timeline.isHidden(key)){
            if(ImGui::MenuItem("Publish to Node GUI")) timeline.reveal(key);
        }else if(published.count(key)){
            if(hasConnection(key)) ImGui::TextDisabled("Disconnect it to unpublish");
            else if(ImGui::MenuItem("Unpublish from Node")) timeline.unpublish(key);
        }else if(ImGui::MenuItem("Publish to Node GUI")) publish(key);
        timeline.menuItems(key);
        ImGui::EndPopup();
    }
    // Widgets on controls (a label's '%' is text, not a format)
    static std::string fmtLabel(const char* label){
        std::string out;
        for(const char* c = label; *c; c++){ out += *c; if(*c == '%') out += '%'; }
        return out;
    }
    bool wI(const std::string& key, const char* label, int lo = INT_MIN, int hi = INT_MIN){
        auto it = controls.find(key);
        if(it == controls.end()) return false;
        int v = (int)std::lround(it->second.get());
        const int mn = lo == INT_MIN ? (int)it->second.lo : lo, mx = hi == INT_MIN ? (int)it->second.hi : hi;
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool ch = ImGui::SliderInt(("##" + key).c_str(), &v, mn, std::max(mn, mx), (fmtLabel(label) + "  %d").c_str());
        if(ch) it->second.set((float)v);
        controlMenu(key);
        return ch;
    }
    bool wF(const std::string& key, const char* label, const char* fmt = "%.2f", bool logarithmic = false){
        auto it = controls.find(key);
        if(it == controls.end()) return false;
        float v = it->second.get();
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool ch = ImGui::SliderFloat(("##" + key).c_str(), &v, it->second.lo, it->second.hi, (fmtLabel(label) + "  " + fmt).c_str(),
                                           logarithmic ? ImGuiSliderFlags_Logarithmic : 0);
        if(ch) it->second.set(v);
        controlMenu(key);
        return ch;
    }
    bool wB(const std::string& key, const char* label){
        auto it = controls.find(key);
        if(it == controls.end()) return false;
        bool v = it->second.get() > 0.5f;
        const bool ch = ImGui::Checkbox((std::string(label) + "##" + key).c_str(), &v);
        if(ch) it->second.set(v ? 1.0f : 0.0f);
        controlMenu(key);
        return ch;
    }
    bool wCombo(const std::string& key, const char* label, const std::vector<std::string>& options){
        auto it = controls.find(key);
        if(it == controls.end() || options.empty()) return false;
        const int v = std::clamp((int)std::lround(it->second.get()), 0, (int)options.size() - 1);
        bool ch = false;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if(ImGui::BeginCombo(("##" + key).c_str(), (std::string(label) + ": " + options[v]).c_str(), ImGuiComboFlags_HeightLarge)){
            for(int i = 0; i < (int)options.size(); i++) if(ImGui::Selectable(options[i].c_str(), i == v)){ it->second.set((float)i); ch = true; }
            ImGui::EndCombo();
        }
        controlMenu(key);
        return ch;
    }

    // ════════════════════════════════════════════════════════════════════════
    // Import sources (the saved progressions, the jazz standards)
    // ════════════════════════════════════════════════════════════════════════

    ofJson progressionsDb, jazzDb;
    std::vector<std::string> progressionNames, jazzNames;
    int selectedProgression = 0, selectedJazz = 0;
    std::array<char, 2048> importText{};
    std::array<char, 128> importName{};
    std::string importMessage;

    void loadImportSources(){
        progressionsDb = ofJson::object();
        progressionsDb["progressions"] = ofJson::object();
        const std::string pp = pitchclassDir() + "chord_progressions.json";
        if(ofFile(pp).exists()){
            try { progressionsDb = ofLoadJson(pp); } catch(...) {}
            if(!progressionsDb.contains("progressions") || !progressionsDb["progressions"].is_object()) progressionsDb["progressions"] = ofJson::object();
        }
        progressionNames.clear();
        for(auto it = progressionsDb["progressions"].begin(); it != progressionsDb["progressions"].end(); ++it)
            progressionNames.push_back(it.value().value("name", "Progression " + it.key()));
        jazzDb = ofJson();
        jazzNames.clear();
        const std::string jp = pitchclassDir() + "JazzStandards.json";
        if(ofFile(jp).exists()){
            try { jazzDb = ofLoadJson(jp); } catch(...) {}
            if(jazzDb.is_array()) for(const auto& song : jazzDb) jazzNames.push_back(song.value("Title", "Untitled"));
        }
    }
    std::vector<std::string> jazzChords(int i) const {
        std::vector<std::string> out;
        if(!jazzDb.is_array() || i < 0 || i >= (int)jazzDb.size()) return out;
        const auto& song = jazzDb[i];
        if(!song.contains("Sections") || !song["Sections"].is_array()) return out;
        for(const auto& sec : song["Sections"]){
            if(sec.contains("MainSegment") && sec["MainSegment"].contains("Chords")){
                const auto c = chordbox::Engine::parseChordString(sec["MainSegment"]["Chords"].get<std::string>());
                out.insert(out.end(), c.begin(), c.end());
                for(int r = 0; r < sec.value("Repeats", 0); r++) out.insert(out.end(), c.begin(), c.end());
            }
            if(sec.contains("Endings") && sec["Endings"].is_array() && !sec["Endings"].empty() && sec["Endings"][0].contains("Chords")){
                const auto c = chordbox::Engine::parseChordString(sec["Endings"][0]["Chords"].get<std::string>());
                out.insert(out.end(), c.begin(), c.end());
            }
        }
        return out;
    }
    void applyChords(const std::vector<std::string>& chords){
        if(chords.empty()) return;
        const int n = std::min((int)chords.size(), chordbox::MAX_STEPS);
        engine.progression.clear();
        for(int i = 0; i < n; i++) engine.progression.push_back(engine.entryFromToken(chords[i]));
        importMessage = ofToString(n) + " steps" + ((int)chords.size() > n ? " (the first " + ofToString(n) + " of " + ofToString(chords.size()) + ")" : "");
        changed();
    }
    void appendProgression(const std::string& name, const std::string& text){
        const auto chords = chordbox::Engine::parseChordString(text);
        if(chords.empty()) return;
        int next = 0;
        for(auto it = progressionsDb["progressions"].begin(); it != progressionsDb["progressions"].end(); ++it){
            try { next = std::max(next, std::stoi(it.key()) + 1); } catch(...) {}
        }
        std::vector<std::string> timing(chords.size(), "1");
        progressionsDb["progressions"][ofToString(next)] = {{"name", name.empty() ? std::string("user") : name},
                                                             {"chords", ofJoinString(chords, ", ")}, {"timing", ofJoinString(timing, ", ")}};
        ofSavePrettyJson(pitchclassDir() + "chord_progressions.json", progressionsDb);
        loadImportSources();
        selectedProgression = std::max(0, (int)progressionNames.size() - 1);
        importMessage = "Saved \"" + (name.empty() ? std::string("user") : name) + "\"";
    }

    // ════════════════════════════════════════════════════════════════════════
    // Snapshots (shared by every ChordBox; the old node's import)
    // ════════════════════════════════════════════════════════════════════════

    // Snapshots: scSnapshots (shared by the synths), a snapshot = {"name", "state"}
    scSnapshots snaps;
    std::shared_ptr<ofxOceanodeAbstractParameter> snapshotHandle;   // Snapshot on the node (names: update)
    ofJson appliedSnapshotState;      // the last state a recall / morph loaded
    int importOldSnapshots(){
        const std::string dirPath = ofToDataPath("nodeSnapshots/ChordSequence/", true);
        ofDirectory dir(dirPath);
        if(!dir.exists()) return 0;
        dir.allowExt("json");
        dir.listDir();
        std::vector<std::pair<int, std::string>> files;
        for(size_t i = 0; i < dir.size(); i++){
            const std::string n = dir.getName(i);
            if(n.rfind("snapshot_", 0) == 0) files.push_back({ofToInt(n.substr(9)), dir.getPath(i)});
        }
        std::sort(files.begin(), files.end());
        int count = 0;
        for(const auto& [slot, path] : files){
            ofJson j;
            try { j = ofLoadJson(path); } catch(...) { continue; }
            if(!j.is_object() || !j.contains("entries")) continue;
            if(!snaps.appendSnapshot({{"name", j.value("name", std::string("Chord Sequence ") + ofToString(slot + 1))}, {"state", j}})) break;
            count++;
        }
        if(count) snaps.saveLibrary();
        return count;
    }

    // ════════════════════════════════════════════════════════════════════════
    // Window
    // ════════════════════════════════════════════════════════════════════════

    static ImU32 stepColor(int i, float alpha = 1.0f){
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(std::fmod(0.36f + i * 0.0618f * 3.0f, 1.0f), 0.45f, 0.85f, r, g, b);
        return ImGui::GetColorU32(ImVec4(r, g, b, alpha));
    }
    static ImU32 outputColor(int o, float alpha = 1.0f){
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(std::fmod(0.08f + o * 0.17f, 1.0f), 0.6f, 0.98f, r, g, b);
        return ImGui::GetColorU32(ImVec4(r, g, b, alpha));
    }
    // A keyboard with the notes lit
    static void keyboard(const char* id, const std::vector<float>& notes, float width, float height, ImU32 lit){
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id, ImVec2(std::max(10.0f, width), height));
        int lo = 127, hi = 0;
        for(float v : notes){ lo = std::min(lo, (int)std::floor(v)); hi = std::max(hi, (int)std::ceil(v)); }
        if(lo > hi){ lo = 48; hi = 72; }
        lo = (int)std::floor(lo / 12.0f) * 12; hi = std::max(hi, lo + 23); hi = ((int)std::floor(hi / 12.0f) + 1) * 12 - 1;
        const int n = hi - lo + 1;
        const float kw = width / n;
        auto isLit = [&](int p){ for(float v : notes) if(std::abs(v - p) < 0.5f) return true; return false; };
        for(int pass = 0; pass < 2; pass++)
            for(int p = lo; p <= hi; p++){
                const int pc = chordbox::wrapIndex(p, 12);
                const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
                if((pass == 0) == black) continue;
                const float xa = o.x + (p - lo) * kw, xb = xa + kw - 1;
                const float yb = o.y + (black ? height * 0.62f : height);
                dl->AddRectFilled(ImVec2(xa, o.y), ImVec2(xb, yb), black ? IM_COL32(32, 34, 42, 255) : IM_COL32(226, 222, 214, 255), 2.0f);
                if(isLit(p)) dl->AddRectFilled(ImVec2(xa + 1, o.y + 1), ImVec2(xb - 1, yb - 1), lit, 2.0f);
                if(pc == 0 && !black) dl->AddText(ImVec2(xa + 1, o.y + height - 14), IM_COL32(70, 70, 80, 255), ("C" + ofToString(p / 12 - 1)).c_str());
            }
    }
    static std::string notesText(const std::vector<float>& v){
        std::string s;
        for(float x : v){
            const int n = (int)std::lround(x);
            s += std::string(chordbox::keyNames()[chordbox::wrapIndex(n, 12)]) + ofToString(n / 12 - 1) + " ";
        }
        return s;
    }

    void drawWindow(){
        const std::string title = getWindowTitle();
        bool open = showWindow.get();
        ImGui::SetNextWindowSize(ImVec2(1180.0f, 860.0f), ImGuiCond_FirstUseEver);
        if(ImGui::Begin(title.c_str(), &open)){
            ImGui::PushID(this);
            drawHeader();
            drawTimeline(ImGui::GetContentRegionAvail().x, 120.0f);
            drawNowPlaying();
            ImGui::Spacing();
            if(ImGui::BeginTabBar("##cbTabs")){
                const char* tabs[] = {"Steps", "Harmony", "Outputs", "Variation", "Import", "Snapshots"};
                for(int i = 0; i < 6; i++){
                    if(ImGui::BeginTabItem(tabs[i])){
                        selectedTab = i;
                        switch(i){
                            case 0: drawStepsTab(); break;
                            case 1: drawHarmonyTab(); break;
                            case 2: drawOutputsTab(); break;
                            case 3: drawVariationTab(); break;
                            case 4: drawImportTab(); break;
                            default: drawSnapshotsTab(); break;
                        }
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
            ImGui::PopID();
        }
        ImGui::End();
        if(open != showWindow.get()) showWindow = open;
    }
    static void heading(const char* t){ ImGui::TextDisabled("%s", t); ImGui::Separator(); }
    void columns(int count, const std::function<void(int)>& body, float minWidth = 0.0f){
        const float avail = ImGui::GetContentRegionAvail().x;
        const float gap = 14.0f, cw = std::max(minWidth, (avail - gap * (count - 1)) / count);
        for(int k = 0; k < count; k++){
            if(k) ImGui::SameLine(0, gap);
            ImGui::BeginChild(("##col" + ofToString(k)).c_str(), ImVec2(cw, 0), false);
            body(k);
            ImGui::EndChild();
        }
    }

    void drawHeader(){
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("CHORDBOX");
        ImGui::SameLine(0, 16);
        ImGui::SetNextItemWidth(170);
        wCombo("Order", "Order", orderNames());
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Index: the Index input picks the step (values or an audio cable)\n"
                                                     "The others: the steps follow the transport (each its Beats); a seek or a loop plays what belongs there");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        wCombo("Key", "Key", keyList());
        ImGui::SameLine();
        {
            ImGui::SetNextItemWidth(190);
            const std::string cur = engine.useExternalScale ? "External (Scale input)" : engine.globalScaleName;
            if(ImGui::BeginCombo("##cbScale", ("Scale: " + cur).c_str(), ImGuiComboFlags_HeightLarge)){
                if(ImGui::Selectable("External (Scale input)", engine.useExternalScale)){ engine.useExternalScale = true; changed(); }
                for(int i = 0; i < (int)engine.scaleLibrary.size(); i++)
                    if(ImGui::Selectable(engine.scaleLibrary[i].name.c_str(), !engine.useExternalScale && i == engine.globalScaleIndex)){
                        engine.useExternalScale = false; engine.globalScaleIndex = i; engine.globalScaleName = engine.scaleLibrary[i].name; changed();
                    }
                ImGui::EndCombo();
            }
        }
        ImGui::SameLine(0, 16);
        int sd = seedP.get();
        ImGui::SetNextItemWidth(100);
        if(ImGui::InputInt("##cbSeed", &sd)) seedP = std::clamp(sd, 0, 99999);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Seed: every random draw (Random / Markov order, random transpose / inversion, deviations, detune)\n0: fresh draws on every pass");
        ImGui::SameLine(0, 4);
        if(ImGui::SmallButton("Dice")){ static std::mt19937 rng((unsigned)ofGetElapsedTimeMicros()); seedP = 1 + (int)(rng() % 99999); }
        ImGui::SameLine(0, 16);
        if(ImGui::Button("Reset")) doReset();
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Step 1 starts now (on the next tick); the variations start again");
        ImGui::SameLine(0, 16);
        ImGui::SetNextItemWidth(100);
        ImGui::SliderInt("##cbView", &viewBeats, 4, 64, "View %d beats");
    }

    // The progression over time (timed orders), or its steps (Index)
    void drawTimeline(float width, float height){
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##cbTimeline", ImVec2(std::max(50.0f, width), height));
        const bool hovered = ImGui::IsItemHovered(), clicked = ImGui::IsItemClicked();
        dl->AddRectFilled(o, ImVec2(o.x + width, o.y + height), ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
        const ImVec2 m = ImGui::GetIO().MousePos;
        auto block = [&](float xa, float xb, int entry, bool now, bool past, int64_t k){
            const float y0 = o.y + 18, y1 = o.y + height - 4;
            dl->AddRectFilled(ImVec2(xa + 1, y0), ImVec2(std::max(xa + 2, xb - 1), y1), stepColor(entry, now ? 0.95f : (past ? 0.28f : 0.55f)), 4.0f);
            if(entry == selectedStep) dl->AddRect(ImVec2(xa + 1, y0), ImVec2(std::max(xa + 2, xb - 1), y1), IM_COL32(255, 255, 255, 200), 4.0f, 0, 2.0f);
            if(xb - xa > 26){
                const std::string label = engine.entryLabel(engine.progression[std::clamp(entry, 0, (int)engine.progression.size() - 1)]);
                dl->PushClipRect(ImVec2(xa + 2, y0), ImVec2(xb - 2, y1), true);
                dl->AddText(ImVec2(xa + 6, y0 + 4), IM_COL32(15, 20, 18, 255), label.c_str());
                dl->AddText(ImVec2(xa + 6, y0 + 22), IM_COL32(15, 20, 18, 160), ofToString(entry + 1).c_str());
                dl->PopClipRect();
            }
            if(hovered && m.x >= xa && m.x < xb && m.y >= y0){
                ImGui::SetTooltip("Step %d: %s%s", entry + 1, engine.entryLabel(engine.progression[entry]).c_str(),
                                  k >= 0 ? ("  (#" + ofToString(k + 1) + ")").c_str() : "");
                if(clicked) selectedStep = entry;
            }
        };
        if(timed()){
            const double now = beatNow(), span = viewBeats;
            const double start = now - span * 0.25, end = start + span;
            auto X = [&](double b){ return o.x + (float)((b - start) / span) * width; };
            for(double b = std::ceil(start); b <= end; b += 1.0){
                const double inBar = std::fmod(std::fmod(b, bpb) + bpb, bpb);
                const bool bar = inBar < 1e-6 || bpb - inBar < 1e-6;
                dl->AddLine(ImVec2(X(b), o.y), ImVec2(X(b), o.y + 16), ImGui::GetColorU32(ImGuiCol_Border, bar ? 0.9f : 0.35f));
                if(bar && b >= 0) dl->AddText(ImVec2(X(b) + 3, o.y + 1), ImGui::GetColorU32(ImGuiCol_TextDisabled), ofToString((int)std::lround(b / bpb) + 1).c_str());
            }
            dl->PushClipRect(o, ImVec2(o.x + width, o.y + height), true);
            const double first = std::max(start, engine.originBeat);
            if(first < end){
                const auto& e0 = engine.eventAt(first);
                for(int64_t k = e0.k; k < e0.k + 512; k++){
                    const auto& ev = engine.event(k);
                    if(ev.start >= end) break;
                    block(X(ev.start), X(ev.start + ev.dur), ev.entry, now >= ev.start && now < ev.start + ev.dur, ev.start + ev.dur <= now, ev.k);
                    if(ev.lapLen > 0.0 && std::abs(ev.lapStart - ev.start) < 1e-9)
                        dl->AddTriangleFilled(ImVec2(X(ev.start) - 5, o.y + 18), ImVec2(X(ev.start) + 5, o.y + 18), ImVec2(X(ev.start), o.y + 25), IM_COL32(255, 255, 255, 170));
                }
            }
            const float px = X(now);
            dl->AddLine(ImVec2(px, o.y), ImVec2(px, o.y + height), ImGui::GetColorU32(ImGuiCol_PlotLinesHovered), 1.5f);
            dl->PopClipRect();
        }else{
            const int n = (int)engine.progression.size();
            float total = 0.0f;
            for(const auto& e : engine.progression) total += std::max(0.25f, e.beatDuration);
            float x = o.x;
            for(int i = 0; i < n; i++){
                const float w = width * std::max(0.25f, engine.progression[i].beatDuration) / total;
                block(x, x + w, i, i == playingEntry, false, -1);
                x += w;
            }
            dl->AddText(ImVec2(o.x + 6, o.y + 2), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                        indexIn.cabled() ? "Index: an audio cable (sample accurate)" : ("Index " + ofToString(indexP.get())).c_str());
        }
    }
    // Each output's notes now
    void drawNowPlaying(){
        const int n = (int)engine.outputs.size();
        const float avail = ImGui::GetContentRegionAvail().x;
        const float w = std::max(160.0f, (avail - 10.0f * (n - 1)) / n);
        for(int o = 0; o < n; o++){
            if(o) ImGui::SameLine(0, 10);
            ImGui::BeginGroup();
            const auto notes = o < (int)outVecs.size() ? outVecs[o]->get() : std::vector<float>{};
            ImGui::TextColored(ImColor(outputColor(o)), "Output %d", o + 1);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", notesText(notes).c_str());
            keyboard(("##now" + ofToString(o)).c_str(), notes, w, 44.0f, outputColor(o, 0.85f));
            ImGui::EndGroup();
        }
        ImGui::TextDisabled("Step %d  %s   root %s   transpose %+d (random %+d, sequencer %+d)   inversion %+d",
                            playingEntry + 1, engine.entryLabel(engine.progression[std::clamp(playingEntry, 0, (int)engine.progression.size() - 1)]).c_str(),
                            chordbox::keyNames()[std::clamp((int)std::lround(rootOut.get()), 0, 11)], playingMods.transpose,
                            playingMods.randomTranspose, playingMods.sequencer, playingMods.invert);
    }

    // ── Steps: a column each (scrolls sideways) ─────────────────────────────
    void drawStepEditor(int s, float w){
        auto& en = engine.progression[s];
        const std::string p = "Step" + ofToString(s + 1) + " ";
        const bool now = s == playingEntry;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, (ImVec4)ImColor(stepColor(s, now ? 0.22f : 0.10f)));
        ImGui::BeginChild(("##step" + ofToString(s)).c_str(), ImVec2(w, 0), true);
        ImGui::TextColored(ImColor(stepColor(s)), "%d  %s", s + 1, engine.entryLabel(en).c_str());
        if(ImGui::IsItemClicked()) selectedStep = s;
        wCombo(p + "Mode", "Mode", modeNames());
        if(en.mode == chordbox::MODE_CHORD || en.mode == chordbox::MODE_SCALE){
            const auto& lib = engine.libraryFor(en.mode);
            const std::string itemLabel = std::string(en.mode == chordbox::MODE_SCALE ? "Scale: " : "Chord: ") +
                                          (lib.empty() ? std::string("-") : lib[std::clamp(en.itemIndex, 0, (int)lib.size() - 1)].name);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if(ImGui::BeginCombo(("##item" + ofToString(s)).c_str(), itemLabel.c_str(), ImGuiComboFlags_HeightLarge)){
                for(int i = 0; i < (int)lib.size(); i++) if(ImGui::Selectable(lib[i].name.c_str(), i == en.itemIndex)){ en.itemIndex = i; en.itemName = lib[i].name; changed(); }
                ImGui::EndCombo();
            }
            controlMenu(p + "Item");
            wI(p + "Transpose", en.mode == chordbox::MODE_SCALE ? "Root" : "Root");
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("The root, in semitones from C");
        }else if(en.mode == chordbox::MODE_CYPHER){
            std::array<char, 64> buf{};
            std::snprintf(buf.data(), buf.size(), "%s", en.itemName.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if(ImGui::InputText(("##cy" + ofToString(s)).c_str(), buf.data(), buf.size(), ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()){
                if(en.itemName != buf.data()){ en.itemName = buf.data(); changed(); }
            }
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("A chord symbol: Dm7, G7b9, Fmaj7#11, Bbsus, C/E...");
            wI(p + "Transpose", "Transpose");
        }else{
            if(en.mode == chordbox::MODE_FUNCTIONAL){
                wCombo(p + "Function", "Function", {"Tonic", "Subdominant", "Dominant"});
                const auto& v = engine.functionalVariants(en.functionalGroup);
                std::vector<std::string> labels;
                for(const auto& x : v) labels.push_back(x.label + " (" + ofToString(x.degree) + ")");
                if(labels.empty()) ImGui::TextDisabled("(no functions for this scale)");
                else{
                    // variants are matched by label: set both
                    const int cur = std::clamp(en.functionalVariantIndex, 0, (int)labels.size() - 1);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if(ImGui::BeginCombo(("##var" + ofToString(s)).c_str(), ("Chord: " + labels[cur]).c_str())){
                        for(int i = 0; i < (int)labels.size(); i++)
                            if(ImGui::Selectable(labels[i].c_str(), i == cur)){ en.functionalVariantIndex = i; en.functionalVariantLabel = v[i].label; changed(); }
                        ImGui::EndCombo();
                    }
                    controlMenu(p + "Variant");
                }
            }else wI(p + "Degree", "Degree");
            wI(p + "Size", "Notes");
            wI(p + "Interval", "Every");
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Scale steps between the notes (2: thirds)");
            wI(p + "Transpose", "Transpose");
        }
        wF(p + "Beats", "Beats", "%.3g", true);
        wI(p + "Inversion", "Inversion");
        if(chordbox::supportsDiatonicDeviation(en.mode)){
            wF(p + "Diatonic Chance", "Diatonic %", "%.0f");
            wI(p + "Diatonic Range", "Range");
        }
        // its notes (output 1's shaping aside)
        const auto preview = engine.entryPreview(en);
        std::vector<float> shown;
        for(float v : preview) shown.push_back(v + 60.0f);
        keyboard(("##sk" + ofToString(s)).c_str(), shown, ImGui::GetContentRegionAvail().x, 34.0f, stepColor(s, 0.9f));
        if(engine.progression.size() > 1 && ImGui::SmallButton(("Remove##rm" + ofToString(s)).c_str())){
            engine.progression.erase(engine.progression.begin() + s);
            changed();
        }
        ImGui::SameLine();
        if((int)engine.progression.size() < chordbox::MAX_STEPS && ImGui::SmallButton(("Duplicate##dp" + ofToString(s)).c_str())){
            engine.progression.insert(engine.progression.begin() + s + 1, en);
            changed();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    void drawStepsTab(){
        const int n = (int)engine.progression.size();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%d steps", n);
        ImGui::SameLine();
        if(n < chordbox::MAX_STEPS && ImGui::SmallButton("+ Step")){ engine.progression.push_back(engine.progression.back()); changed(); }
        ImGui::SameLine();
        ImGui::TextDisabled("  the progression is %.3g beats", engine.lapBeats());
        const bool markov = engine.progressionOrder == chordbox::ORDER_MARKOV;
        const float h = std::max(200.0f, ImGui::GetContentRegionAvail().y - (markov ? 190.0f : 0.0f));
        ImGui::BeginChild("##cbSteps", ImVec2(0, h), false, ImGuiWindowFlags_HorizontalScrollbar);
        const float w = 175.0f;
        for(int s = 0; s < (int)engine.progression.size(); s++){
            if(s) ImGui::SameLine(0, 8);
            drawStepEditor(s, w);
        }
        ImGui::EndChild();
        if(markov) drawMarkov();
    }
    // Markov: from (rows) -> to (columns), drag a cell
    void drawMarkov(){
        heading("Markov: the chance of each next step (rows: from, columns: to; drag up / down)");
        const int n = (int)engine.progression.size();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float cell = std::min(28.0f, (ImGui::GetContentRegionAvail().x - 40.0f) / std::max(1, n));
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##cbMarkov", ImVec2(40.0f + cell * n, cell * n + 4));
        const ImVec2 m = ImGui::GetIO().MousePos;
        for(int r = 0; r < n; r++){
            dl->AddText(ImVec2(o.x + 4, o.y + r * cell + cell * 0.25f), ImGui::GetColorU32(ImGuiCol_TextDisabled), ofToString(r + 1).c_str());
            const auto& w = engine.progression[r].markovWeights;
            float sum = 0.0f;
            for(float x : w) sum += x;
            for(int c = 0; c < n; c++){
                const float v = c < (int)w.size() ? w[c] : 0.0f;
                const float t = sum > 0 ? v / sum : 0.0f;
                const ImVec2 a(o.x + 40 + c * cell, o.y + r * cell), b(a.x + cell - 2, a.y + cell - 2);
                dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_FrameBg), 2.0f);
                dl->AddRectFilled(ImVec2(a.x, b.y - (cell - 2) * t), b, stepColor(c, 0.85f), 2.0f);
                if(ImGui::IsItemActive() && m.x >= a.x && m.x < b.x + 2 && m.y >= a.y && m.y < b.y + 2){
                    const float dy = -ImGui::GetIO().MouseDelta.y * 0.02f;
                    auto& wr = engine.progression[r].markovWeights;
                    if(c < (int)wr.size() && dy != 0.0f){ wr[c] = std::clamp(wr[c] + dy, 0.0f, 1.0f); changed(); }
                }
                if(ImGui::IsItemHovered() && m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y)
                    ImGui::SetTooltip("%d -> %d: %.0f%%", r + 1, c + 1, t * 100.0f);
            }
        }
        if(ImGui::SmallButton("Uniform")){ for(auto& e : engine.progression) std::fill(e.markovWeights.begin(), e.markovWeights.end(), 1.0f); changed(); }
        ImGui::SameLine();
        if(ImGui::SmallButton("Cycle")){
            for(int r = 0; r < n; r++){ auto& w = engine.progression[r].markovWeights; std::fill(w.begin(), w.end(), 0.0f); w[(r + 1) % n] = 1.0f; }
            changed();
        }
        ImGui::SameLine();
        if(ImGui::SmallButton("Random")){
            static std::mt19937 rng(11);
            for(auto& e : engine.progression) for(auto& w : e.markovWeights) w = (float)(rng() % 100) / 100.0f;
            changed();
        }
    }

    // ── Harmony ─────────────────────────────────────────────────────────────
    void drawHarmonyTab(){
        columns(3, [&](int k){
            if(k == 0){
                heading("Key and scale");
                wCombo("Key", "Key", keyList());
                wB("External Scale", "Scale from the Scale input");
                if(!engine.useExternalScale){
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if(ImGui::BeginCombo("##cbScale2", ("Scale: " + engine.globalScaleName).c_str(), ImGuiComboFlags_HeightLarge)){
                        for(int i = 0; i < (int)engine.scaleLibrary.size(); i++)
                            if(ImGui::Selectable(engine.scaleLibrary[i].name.c_str(), i == engine.globalScaleIndex)){
                                engine.globalScaleIndex = i; engine.globalScaleName = engine.scaleLibrary[i].name; changed();
                            }
                        ImGui::EndCombo();
                    }
                    controlMenu("Scale Index");
                }
                std::vector<float> sc;
                for(float v : engine.globalScaleValues()) sc.push_back(v + engine.globalKey + 60.0f);
                keyboard("##cbScaleKeys", sc, ImGui::GetContentRegionAvail().x, 46.0f, IM_COL32(120, 200, 150, 220));
                ImGui::TextDisabled("Degree and Functional steps use this scale");
                const auto* groups = engine.functionalGroups();
                if(groups){
                    ImGui::Spacing();
                    heading("Functions in this scale");
                    for(int g = 0; g < 3; g++){
                        std::string s;
                        for(const auto& v : (*groups)[g]) s += v.label + " ";
                        ImGui::TextWrapped("%s: %s", chordbox::functionalGroupLabel(g), s.c_str());
                    }
                }
            }else if(k == 1){
                heading("Global");
                wI("Global Transpose", "Transpose");
                wI("Global Inversion", "Inversion");
                wF("Global Bend", "Pitch bend", "%.2f st");
                ImGui::TextDisabled("(the node's Transpose / Inversion / Pitchbend)");
                ImGui::Spacing();
                heading("Steps");
                int n = (int)engine.progression.size();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if(ImGui::SliderInt("##cbSteps2", &n, 1, chordbox::MAX_STEPS, "Steps  %d")){ engine.resize(n); changed(); }
                if(ImGui::SmallButton("All 1 beat")){ for(auto& e : engine.progression) e.beatDuration = 1.0f; changed(); }
                ImGui::SameLine();
                if(ImGui::SmallButton("All 1 bar")){ for(auto& e : engine.progression) e.beatDuration = (float)bpb; changed(); }
                ImGui::SameLine();
                if(ImGui::SmallButton("All 2 bars")){ for(auto& e : engine.progression) e.beatDuration = (float)(2 * bpb); changed(); }
            }else{
                heading("Order");
                wCombo("Order", "Order", orderNames());
                ImGui::TextWrapped("%s", engine.progressionOrder == chordbox::ORDER_INDEX
                    ? "The Index input picks the step (an audio cable into Index: sample accurate)."
                    : "Each step lasts its Beats from the origin (Reset, or the transport's start). A seek or a loop plays what belongs there.");
                if(timed()){
                    ImGui::Text("Origin: beat %.2f", engine.originBeat);
                    if(ImGui::SmallButton("Origin at 0")){ engine.originBeat = 0.0; changed(); }
                }
            }
        });
    }

    // ── Outputs: a column each ──────────────────────────────────────────────
    void drawOutputEditor(int o, float w){
        auto& c = engine.outputs[o];
        const std::string p = "Out" + ofToString(o + 1) + " ";
        ImGui::BeginChild(("##out" + ofToString(o)).c_str(), ImVec2(w, 0), true);
        ImGui::TextColored(ImColor(outputColor(o)), "Output %d", o + 1);
        if(engine.outputs.size() > 1){
            ImGui::SameLine(w - 70);
            if(ImGui::SmallButton(("Remove##ro" + ofToString(o)).c_str())){ engine.outputs.erase(engine.outputs.begin() + o); changed(); ImGui::EndChild(); return; }
        }
        const auto notes = o < (int)outVecs.size() ? outVecs[o]->get() : std::vector<float>{};
        keyboard(("##ok" + ofToString(o)).c_str(), notes, ImGui::GetContentRegionAvail().x, 40.0f, outputColor(o, 0.85f));
        wCombo(p + "Source", "Source", sourceNames());
        const bool scl = chordbox::scaleLike(c.sourceMode);
        heading("Register");
        wI(p + "Octave", "Octave");
        wI(p + "Transpose", "Transpose");
        wF(p + "Bend", "Bend", "%.2f st");
        wI(p + "Min Note", "Lowest");
        wI(p + "Max Note", "Highest");
        heading("Shape");
        if(!scl){
            wI(p + "Inversion", "Inversion");
            wCombo(p + "Voicing", "Voicing", voicingNames());
            wF(p + "Spread", "Spread", "%.2f st");
        }
        wI(p + "Size", "Notes");
        wB(p + "Expand", "Expand up");
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("More notes than the chord: the chord again, octaves up (else repeated)");
        ImGui::SameLine();
        wB(p + "Sort", "Sort");
        if(!scl){
            wB(p + "Add Bass", "Bass");
            if(c.addBass){ ImGui::SameLine(); ImGui::SetNextItemWidth(-FLT_MIN); wI(p + "Bass Octave", "oct"); }
        }
        wB(p + "Rootless", "Rootless");
        ImGui::SameLine();
        wB(p + "Fold 12", "Fold to one octave");
        heading("Voice leading");
        wCombo(p + "Voice Leading", "Mode", leadingNames());
        if(c.voiceLeadingMode == chordbox::VL_GRAVITY) wF(p + "Gravity", "Pull to C", "%.2f");
        heading("Randomness (seeded)");
        wF(p + "Octave Chance", "Octave %", "%.0f");
        wI(p + "Octave Range", "Octaves");
        wF(p + "Chromatic Chance", "Chromatic %", "%.0f");
        wI(p + "Chromatic Range", "Semitones");
        wF(p + "Detune", "Detune", "%.3f st");
        heading("Glide");
        wF(p + "Glide", c.glideBeats ? "Glide (beats/1000)" : "Glide", c.glideBeats ? "%.0f" : "%.0f ms", true);
        wB(p + "Glide Beats", "In beats");
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Glide in beats (the value / 1000: 500 = half a beat)");
        ImGui::EndChild();
    }
    void drawOutputsTab(){
        const int n = (int)engine.outputs.size();
        if(n < chordbox::MAX_OUTPUTS && ImGui::SmallButton("+ Output")){ engine.outputs.push_back(engine.outputs.back()); changed(); }
        ImGui::SameLine();
        ImGui::TextDisabled("Each output: OutputN (vector) and Pitch N (audio, 16 channels)");
        ImGui::BeginChild("##cbOuts", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
        const float avail = ImGui::GetContentRegionAvail().x;
        const float w = std::max(230.0f, (avail - 8.0f * (n - 1)) / std::max(1, std::min(n, 4)));
        for(int o = 0; o < (int)engine.outputs.size(); o++){
            if(o) ImGui::SameLine(0, 8);
            drawOutputEditor(o, w);
        }
        ImGui::EndChild();
    }

    // ── Variation ───────────────────────────────────────────────────────────
    void drawVariationTab(){
        columns(3, [&](int k){
            if(k == 0){
                heading("Random transpose");
                wI("Rnd Transpose Range", "Up to");
                wI("Rnd Transpose Quant", "In steps of");
                wI("Rnd Transpose Every", "Every");
                wB("Rnd Transpose Per Step", "Count steps (else laps)");
                ImGui::Text("Now: %+d", playingMods.randomTranspose);
            }else if(k == 1){
                heading("Random inversion");
                wI("Rnd Inversion Range", "Up to");
                wI("Rnd Inversion Quant", "In steps of");
                wI("Rnd Inversion Every", "Every");
                wB("Rnd Inversion Per Step", "Count steps (else laps)");
                ImGui::Text("Now: %+d", playingMods.randomInvert);
                ImGui::Spacing();
                ImGui::TextWrapped("A lap: whenever step 1 comes back. Draws follow the Seed (header).");
            }else{
                heading("Transposition sequencer");
                std::array<char, 128> buf{};
                std::string s;
                for(size_t i = 0; i < engine.transposeSequencerSteps.size(); i++) s += (i ? ", " : "") + ofToString(engine.transposeSequencerSteps[i]);
                std::snprintf(buf.data(), buf.size(), "%s", s.c_str());
                ImGui::SetNextItemWidth(-FLT_MIN);
                if(ImGui::InputTextWithHint("##cbSeqSteps", "steps: 5, 7, -2", buf.data(), buf.size(), ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()){
                    std::vector<int> v;
                    for(const auto& t : ofSplitString(buf.data(), ",", true, true)){ try { v.push_back(std::stoi(t)); } catch(...) {} }
                    if(v.empty()) v = {0};
                    if(v != engine.transposeSequencerSteps){ engine.transposeSequencerSteps = v; changed(); }
                }
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("Each trigger adds the next value (in turn), wrapped by Mod");
                wI("Seq Every", "Every");
                wB("Seq Per Step", "Count steps (else laps)");
                wI("Seq Mod", "Mod");
                ImGui::Text("Now: %+d", playingMods.sequencer);
            }
        });
    }

    // ── Import ──────────────────────────────────────────────────────────────
    void drawImportTab(){
        columns(3, [&](int k){
            if(k == 0){
                heading("Chord symbols");
                ImGui::InputTextMultiline("##cbImport", importText.data(), importText.size(), ImVec2(-FLT_MIN, 110));
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("Dm7, G7, Cmaj7 | Am7, D7 ... (commas or bars)");
                if(ImGui::Button("Use as the steps")) applyChords(chordbox::Engine::parseChordString(importText.data()));
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputTextWithHint("##cbImportName", "name", importName.data(), importName.size());
                if(ImGui::Button("Save to the progressions")) appendProgression(importName.data(), importText.data());
                if(!importMessage.empty()) ImGui::TextDisabled("%s", importMessage.c_str());
            }else if(k == 1){
                heading("Saved progressions");
                if(progressionNames.empty()) ImGui::TextDisabled("(none)");
                else{
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    selectedProgression = std::clamp(selectedProgression, 0, (int)progressionNames.size() - 1);
                    if(ImGui::BeginCombo("##cbProg", progressionNames[selectedProgression].c_str(), ImGuiComboFlags_HeightLarge)){
                        for(int i = 0; i < (int)progressionNames.size(); i++) if(ImGui::Selectable(progressionNames[i].c_str(), i == selectedProgression)) selectedProgression = i;
                        ImGui::EndCombo();
                    }
                    int i = 0;
                    for(auto it = progressionsDb["progressions"].begin(); it != progressionsDb["progressions"].end(); ++it, ++i){
                        if(i != selectedProgression) continue;
                        const std::string chords = it.value().value("chords", std::string());
                        ImGui::TextWrapped("%s", chords.c_str());
                        if(ImGui::Button("Load##prog")) applyChords(chordbox::Engine::parseChordString(chords));
                    }
                }
            }else{
                heading("Jazz standards");
                if(jazzNames.empty()) ImGui::TextDisabled("(JazzStandards.json not found)");
                else{
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    selectedJazz = std::clamp(selectedJazz, 0, (int)jazzNames.size() - 1);
                    if(ImGui::BeginCombo("##cbJazz", jazzNames[selectedJazz].c_str(), ImGuiComboFlags_HeightLarge)){
                        for(int i = 0; i < (int)jazzNames.size(); i++) if(ImGui::Selectable(jazzNames[i].c_str(), i == selectedJazz)) selectedJazz = i;
                        ImGui::EndCombo();
                    }
                    const auto chords = jazzChords(selectedJazz);
                    ImGui::TextWrapped("%s", ofJoinString(std::vector<std::string>(chords.begin(), chords.begin() + std::min<size_t>(chords.size(), 24)), " ").c_str());
                    if(ImGui::Button("Load##jazz")) applyChords(chords);
                    ImGui::TextDisabled("Up to %d steps", chordbox::MAX_STEPS);
                }
            }
        });
    }

    // ── Snapshots ───────────────────────────────────────────────────────────
    void drawSnapshotsTab(){
        columns(2, [&](int k){
            if(k == 0){
                heading("Snapshots (shared by every ChordBox)");
                snaps.drawBar(ImGui::GetContentRegionAvail().x, false, false);   // in its own tab: no line
                ImGui::TextDisabled("Click a number to recall it, drag it to move it; type to rename.");
                snaps.drawList();
            }else{
                heading("Recall");
                snaps.drawMorph(ImGui::GetContentRegionAvail().x);
                ImGui::TextWrapped("A snapshot is the whole progression and its outputs: a recall or a morph switches it "
                                   "at the switch point (...). The node's Snapshot input recalls by number (1..).");
                ImGui::Spacing();
                heading("Import");
                if(ImGui::Button("Import the Chord Sequence node's snapshots"))
                    snapMessage = "Imported " + ofToString(importOldSnapshots()) + " snapshot(s)";
                if(!snapMessage.empty()) ImGui::TextDisabled("%s", snapMessage.c_str());
            }
        });
    }

    // ════════════════════════════════════════════════════════════════════════
    // Members
    // ════════════════════════════════════════════════════════════════════════

    chordbox::Engine engine;
    bool dirty = true;
    uint64_t revision = 1;
    uint64_t salt = 0xC4B0;
    ofParameter<int> indexP, numChordsP, transposeP, inversionP, seedP;
    scAudioParam indexIn;
    ofParameter<float> bendP, rootOut, chphOut, progphOut;
    ofParameter<std::vector<float>> scaleIn;
    ofParameter<void> resetP;
    ofParameter<bool> showWindow;
    std::vector<std::shared_ptr<ofParameter<std::vector<float>>>> outVecs;
    std::vector<std::shared_ptr<ofParameter<int>>> outSizes;
    ofEvent<void> resendParams;
    ofEventListeners listeners;

    uint64_t lastGeneration = 0, lastLoopCount = 0;
    bool lastPlaying = false;
    double bpb = 4.0;
    scTransportSync::Follower follower;
    scTransportSync::StepClock stepClock;
    int anchorIdCounter = 0, anchorHardCounter = 0;
    bool forceHardAnchor = true;

    int selectedTab = 0, selectedStep = 0, viewBeats = 16;
    std::string snapMessage;
};
