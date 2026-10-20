//
//  scEuclideanBox.h
//  ofxOceanodeSuperCollider
//
//  EuclideanBox: up to 8 euclidean tracks locked to the global transport,
//  each with Trig, Gate (x the hit's level) and Pitch outputs, Voices channels
//  wide, plus Gates: every track's gate, one channel each (track n = channel
//  n, for a SampleBox kit's pads). Drag a track (its tab or its timeline
//  label) onto another to move it there.
//
//  • A track spreads Hits over Steps as evenly as possible (E(k, n)), turned
//    by Rotate, one step = Step (1 bar .. 1/32). Swing delays every second
//    step; Chance drops hits (seeded per step).
//  • Accents and durations follow euclidean rules of their own (as the
//    Polyphonic Arpeggiator's): E(Accents, Accent Len) turned by Accent Rotate,
//    stepped by hit (By Hit) or by step: accented hits play at Accent Level,
//    the others at Level; E(Long Hits, Long Len) the same way: those hits last
//    Long Gate steps, the others Gate.
//  • Pitch: a Scale (the node's input, semitones) from Root; every step, hit
//    or accent (Advance) the degree moves by Interval (negative: down), kept
//    within Range degrees (Wrap or Bounce), counted from the start, from each
//    period or from each cycle (Reset). Pitch outputs (audio) and Note <n>
//    (Oceanode vectors, one value per voice).
//  • Voices (the node's): each output is that many channels. Round robin:
//    each hit to the next voice (a polyphonic synth plays them all); All: every
//    hit to every voice (a stereo / unison synth).
//  • Randomize: every N cycles / bars / beats / steps (a "period") a track
//    can draw new Hits, Rotate, Accents, Accent Rotate, Long Hits, Long Rotate
//    and Steps from the ranges set, with a Chance per period (else the base
//    values play). Seeded (node Seed + track Seed + the period's index): the
//    same position always plays the same, a seek or a loop replays it, the
//    window shows exactly what will play. Seed 0 (the node's or the track's):
//    no seed, every pass through a period draws again (a loop plays something
//    new each time; the window still shows what is coming). Restart: the
//    pattern starts again at every period (else it runs on, polymetric
//    against the bars).
//  • Synth: EuclideanBoxPlayer<Voices> (synthdefs/Defaults/EuclideanBox), the
//    MetroBox clock: the node writes the onsets (amplitude, duration, pitch,
//    voice per tick, 48 ticks per beat) of the chunks around the playhead into
//    a buffer; the player follows the transport with scTransportSync anchors.
//  • GUI: rings (each track's cycle now: hits, accents, long notes, the
//    playhead step), a scrolling timeline and a tab per track in columns.
//    Right-click any control: type a value, or publish it ("T<n> <Setting>").
//

#pragma once

#include "ofxOceanodeSuperColliderConfig.h"

#if OFXOCEANODESC_HAS_TRANSPORT

#include "scNode.h"
#include "scTimelineMenu.h"
#include "scSnapshots.h"
#include "scDropdowns.h"
#include "scTransportSync.h"
#include "scMetroBoxEngine.h"     // hashOf, unit, floorDiv, positiveMod64, tickAt
#include "ofxSCSynth.h"
#include "scSynthControlBatch.h"
#include "ofxSCBuffer.h"
#include "ofxSCServer.h"
#include "ofxOceanodeShared.h"
#if OFXOCEANODESC_HAS_TIMELINE
#include "ofxOceanodeContainer.h"
#include "ofxOceanodeTimeline.h"
#endif
#include "imgui.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

class scEuclideanBox : public scNode {
public:
    // its window (SC Mixer Pro's "Go to origin synth window" opens and focuses it)
    std::string getWindowTitle() override { return (canvasID == "Canvas" ? "" : canvasID + "/") + "EuclideanBox " + ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return &showWindow; }
private:
public:
    static constexpr int MAX_TRACKS = 8;          // the player's tracks
    static constexpr int MAX_VOICES = 16;
    static constexpr int MAX_STEPS = 64;
    static constexpr int TPB = metrobox::TICKS_PER_BEAT;
    static constexpr int SLOTS = 4;                // chunks held in the buffer per track
    static constexpr int MAX_CHUNK_TICKS = metrobox::MAX_CYCLE_TICKS;   // the player's buffer layout
    static constexpr int CHUNK = 8 * TPB;          // ticks per written chunk (any rhythm length)
    static constexpr int OUTPUTS_PER_TRACK = 3;    // Trig, Gate (x the hit's level), Pitch
    static constexpr int NUM_OUTPUTS = 1 + MAX_TRACKS * OUTPUTS_PER_TRACK;   // port 0: Gates
    static constexpr int64_t NO_CHUNK = INT64_MIN;
    static constexpr int BUF_CH = 4;               // per tick: amplitude, duration, pitch, voice

    scEuclideanBox() : scNode("EuclideanBox") {}

    ~scEuclideanBox() override {
        listeners.unsubscribeAll();
        for(auto& [server, st] : servers){
            if(st.synth){ st.synth->free(); delete st.synth; }
            if(st.buffer){ if(bufferLive(server, st)) st.buffer->free(); delete st.buffer; }
        }
        servers.clear();
    }

    // ════════════════════════════════════════════════════════════════════════
    // Setup
    // ════════════════════════════════════════════════════════════════════════

    void setup() override {
        timeline.setup(this, [this](const std::string& k){ publish(k); }, [this](const std::string& k){ unpublish(k); },
                       [this](){ return publishedKeys; });
        description = "Euclidean sequencer locked to the transport: up to 8 tracks of hits spread over steps, "
                      "with euclidean accents and long notes, swing, chance and a pitch progression over a scale. "
                      "Seeded randomization every N cycles / bars / beats (Seed 0: unseeded, new every pass). Each track has "
                      "Trig, Gate and Pitch outputs (Gate carries the hit's level), Voices channels wide (round robin or all "
                      "voices); Gates carries every track (track n = channel n: wire it to a SampleBox kit). Show opens the "
                      "rings, the timeline and every control; drag a track's tab to reorder; right-click a control there to "
                      "type a value or publish it here.";
        addParameter(showWindow.set("Show", false));
        addParameter(numTracks.set("Tracks", 2, 1, MAX_TRACKS));
        addParameter(globalSeed.set("Seed", 1, 0, 99999));
        addParameter(trigAmp.set("Trig Amp", false));
        addParameter(numVoices.set("Voices", 1, 1, MAX_VOICES));
        addParameter(scale.set("Scale", {0, 2, 4, 5, 7, 9, 11}, {0}, {24}));

        // A first groove: a 4-on-the-floor kick and a tresillo-ish rim
        static const int defSteps[MAX_TRACKS] = {16, 16, 16, 12, 16, 8, 16, 16};
        static const int defHits[MAX_TRACKS]  = {4, 5, 7, 5, 3, 3, 9, 2};
        for(int ti = 0; ti < MAX_TRACKS; ti++){
            Track& T = tracks[ti];
            const std::string p = "T" + ofToString(ti + 1) + " ";
            T.steps.set(p + "Steps", defSteps[ti], 1, MAX_STEPS);
            T.hits.set(p + "Hits", defHits[ti], 0, MAX_STEPS);
            T.rotate.set(p + "Rotate", 0, 0, MAX_STEPS - 1);
            T.division.set(p + "Step", 5, 0, (int)divisions().size() - 1);
            T.chance.set(p + "Chance", 1.0f, 0.0f, 1.0f);
            T.mute.set(p + "Mute", false);
            T.voiceMode.set(p + "Voice Mode", 0, 0, 1);
            // accents: a euclidean rule of their own
            T.accLen.set(p + "Accent Len", 4, 1, MAX_STEPS);
            T.accents.set(p + "Accents", 1, 0, MAX_STEPS);
            T.accRotate.set(p + "Accent Rotate", 0, 0, MAX_STEPS - 1);
            T.accByHit.set(p + "Accent By Hit", true);
            T.level.set(p + "Level", 0.6f, 0.0f, 1.0f);
            T.accLevel.set(p + "Accent Level", 1.0f, 0.0f, 1.0f);
            // durations: another
            T.gate.set(p + "Gate", 0.5f, 0.05f, 16.0f);
            T.longLen.set(p + "Long Len", 3, 1, MAX_STEPS);
            T.longHits.set(p + "Long Hits", 0, 0, MAX_STEPS);
            T.longRotate.set(p + "Long Rotate", 0, 0, MAX_STEPS - 1);
            T.longByHit.set(p + "Long By Hit", true);
            T.longGate.set(p + "Long Gate", 2.0f, 0.05f, 16.0f);
            T.swing.set(p + "Swing", 0.0f, 0.0f, 0.75f);
            T.restart.set(p + "Restart", false);
            // pitch
            T.pitchAdvance.set(p + "Advance", ADV_HIT, 0, 3);
            T.root.set(p + "Root", 48.0f, 0.0f, 127.0f);
            T.interval.set(p + "Interval", 0, -12, 12);
            T.range.set(p + "Range", 8, 1, 64);
            T.bounce.set(p + "Bounce", false);
            T.octave.set(p + "Octave", 0, -4, 4);
            T.pitchReset.set(p + "Pitch Reset", RESET_NEVER, 0, 2);
            // randomize
            T.every.set(p + "Every", 1, 1, 64);
            T.unit.set(p + "Unit", UNIT_BARS, 0, (int)unitNames().size() - 1);
            T.rndChance.set(p + "Random Chance", 1.0f, 0.0f, 1.0f);
            T.rndHits.set(p + "Random Hits", false);
            T.hitsMin.set(p + "Hits Min", 1, 0, MAX_STEPS);
            T.hitsMax.set(p + "Hits Max", 8, 0, MAX_STEPS);
            T.rndRotate.set(p + "Random Rotate", false);
            T.rndAccents.set(p + "Random Accents", false);
            T.accMin.set(p + "Accents Min", 0, 0, MAX_STEPS);
            T.accMax.set(p + "Accents Max", 3, 0, MAX_STEPS);
            T.rndAccRotate.set(p + "Random Accent Rotate", false);
            T.rndLong.set(p + "Random Long", false);
            T.longMin.set(p + "Long Min", 0, 0, MAX_STEPS);
            T.longMax.set(p + "Long Max", 2, 0, MAX_STEPS);
            T.rndLongRotate.set(p + "Random Long Rotate", false);
            T.rndSteps.set(p + "Random Steps", false);
            T.stepsMin.set(p + "Steps Min", 8, 1, MAX_STEPS);
            T.stepsMax.set(p + "Steps Max", 16, 1, MAX_STEPS);
            T.seed.set(p + "Seed", ti + 1, 0, 9999);

            regI(ti, T.steps); regI(ti, T.hits); regI(ti, T.rotate); regI(ti, T.division, divisionLabels());
            regF(ti, T.chance); regB(ti, T.mute); regI(ti, T.voiceMode, voiceModeNames());
            regI(ti, T.accLen); regI(ti, T.accents); regI(ti, T.accRotate); regB(ti, T.accByHit);
            regF(ti, T.level); regF(ti, T.accLevel);
            regF(ti, T.gate); regI(ti, T.longLen); regI(ti, T.longHits); regI(ti, T.longRotate); regB(ti, T.longByHit);
            regF(ti, T.longGate); regF(ti, T.swing); regB(ti, T.restart);
            regI(ti, T.pitchAdvance, advanceNames()); regF(ti, T.root); regI(ti, T.interval); regI(ti, T.range);
            regB(ti, T.bounce); regI(ti, T.octave); regI(ti, T.pitchReset, resetNames());
            regI(ti, T.every); regI(ti, T.unit, unitNames()); regF(ti, T.rndChance);
            regB(ti, T.rndHits); regI(ti, T.hitsMin); regI(ti, T.hitsMax); regB(ti, T.rndRotate);
            regB(ti, T.rndAccents); regI(ti, T.accMin); regI(ti, T.accMax); regB(ti, T.rndAccRotate);
            regB(ti, T.rndLong); regI(ti, T.longMin); regI(ti, T.longMax); regB(ti, T.rndLongRotate);
            regB(ti, T.rndSteps); regI(ti, T.stepsMin); regI(ti, T.stepsMax); regI(ti, T.seed);
            listeners.push(T.mute.newListener([this](bool&){ for(auto& [s, st] : servers) sendStaticControls(st); }));
            notes[ti].set("Note " + ofToString(ti + 1), {48.0f}, {0.0f}, {127.0f});
        }
        syncOutputPorts();
        bpb = beatsPerBar();

        listeners.push(numTracks.newListener([this](int& n){
            syncOutputPorts();
            invalidate(-1);
            for(auto& [server, st] : servers) if(st.synth) st.synth->set("layers", (float)n);
        }));
        listeners.push(globalSeed.newListener([this](int&){ invalidate(-1); }));
        listeners.push(scale.newListener([this](std::vector<float>&){ invalidate(-1); }));
        listeners.push(numVoices.newListener([this](int&){ invalidate(-1); voicesDirty = true; }));
        listeners.push(trigAmp.newListener([this](bool&){
            for(auto& [server, st] : servers) if(st.synth) st.synth->set("trigAmp", trigAmp ? 1.0f : 0.0f);
        }));
        listeners.push(resendParams.newListener([this](){
            for(auto& [server, st] : servers) sendStaticControls(st);
            follower.requestAnchor();
        }));

        // Snapshots (the shared system): every track control, and the node's
        // Tracks, Seed, Scale and Trig Amp (not Voices: the outputs' width)
        snaps.setupParameters();
        for(auto* p : {&snaps.snapshotP, &snaps.morphAP, &snaps.morphBP, &snaps.morphCurveP, &snaps.morphSwitchP}) controls.push_back(plainControl(*p));
        for(auto* p : {&snaps.morphP, &snaps.morphTimeP}) controls.push_back(plainControl(*p));
        for(auto* p : {&snaps.morphBeatsP, &snaps.morphGlideP}) controls.push_back(plainControl(*p));
        snapExtras = {plainControl(numTracks), plainControl(globalSeed), plainControl(scale), plainControl(trigAmp)};
        initial = captureSnapshot();
        scSnapshots::Host h;
        h.folder = "EuclideanBox"; h.title = "EuclideanBox";
        h.capture = [this](){ return captureSnapshot(); };
        h.apply = [this](const ofJson& a, const ofJson& b, float t, bool sw){ applySnapshot(a, b, t, sw); };
        h.factory = [this](){ return factorySnapshots(); };
        h.bpm = [this](){ return currentBpm; };
        h.paramMenu = [this](const std::string& k){ controlMenu(k); };
        snaps.setup(h);
    }

    // ════════════════════════════════════════════════════════════════════════
    // Frame
    // ════════════════════════════════════════════════════════════════════════

    void update(ofEventArgs&) override {
        timeline.update();
        snaps.update();
        {   // the snapshot names, to a published Snapshot / Morph A / B dropdown
            const auto names = snaps.slotNames();
            for(const auto& key : snaps.slotKeys()) { auto it = published.find(key); if(it != published.end()) scDropdowns::setNames(it->second, names); }
        }
        const float now = ofGetElapsedTimef();
        for(auto& [server, st] : servers){
            if(st.allocPending && (!server->isNRTSyncPending() || now - st.allocTime > 2.0f)){
                st.allocPending = false;
                for(auto& s : st.slotChunk) s.fill(NO_CHUNK);
            }
        }
        // Periods in bars follow the timeline's meter
        const double b = beatsPerBar();
        if(std::abs(b - bpb) > 1e-9){ bpb = b; invalidate(-1); }
        if(voicesDirty){ voicesDirty = false; replaceSynths(); }
        advancePasses();
        fillBuffers();
        sendTransportAnchors();
        updateNotes();
    }

    void draw(ofEventArgs&) override { if(showWindow) drawWindow(); }
    void setBpm(float bpm) override { currentBpm = std::max(1.0f, bpm); }   // Morph Time in beats

    // ════════════════════════════════════════════════════════════════════════
    // scNode
    // ════════════════════════════════════════════════════════════════════════

    void buildSynth(ofxSCServer* server) override {
        if(!server) return;
        ServerState& st = servers[server];
        if(st.synth){ st.synth->free(); delete st.synth; }
        st.synth = new ofxSCSynth(playerDef(), server);
        st.builtVoices = voices();
        if(!bufferLive(server, st)){
            delete st.buffer;
            st.buffer = new ofxSCBuffer(MAX_TRACKS * SLOTS * MAX_CHUNK_TICKS, BUF_CH, server);
            st.buffer->alloc();
            st.allocPending = true;
            st.allocTime = ofGetElapsedTimef();
            server->requestNRTSync();
            for(auto& s : st.slotChunk) s.fill(NO_CHUNK);
        }
    }
    void createSynth(ofxSCServer* server) override {
        auto it = servers.find(server);
        if(it == servers.end() || !it->second.synth) return;
        resendParams.notify();
        it->second.synth->set("run", 0.0f);   // holds until the next update's hard anchor
        forceHardAnchor = true;
        follower.requestAnchor();
        it->second.synth->createAndRun(0, 1, getActive());
    }
    void moveSynthBefore(ofxSCServer* server, int nodeID) override {
        auto it = servers.find(server);
        if(it == servers.end() || !it->second.synth) return;
        sendStaticControls(it->second);
        it->second.synth->moveBefore(nodeID);
    }
    // The buffer stays for the next synth (freed with the node)
    void free(ofxSCServer* server) override {
        auto it = servers.find(server);
        if(it == servers.end()) return;
        if(it->second.synth){ it->second.synth->free(); delete it->second.synth; it->second.synth = nullptr; }
        it->second.outputBuses.clear();
    }
    void setOutputBus(ofxSCServer* server, int index, int bus) override {
        if(!server || index < 0 || index >= NUM_OUTPUTS) return;
        ServerState& st = servers[server];
        st.outputBuses[index] = bus;
        if(st.synth) st.synth->set(busName(index), bus);
    }
    int getOutputBusIndex(ofxSCServer* server, int index) override {
        auto it = servers.find(server);
        if(it == servers.end()) return -1;
        auto b = it->second.outputBuses.find(index);
        return b != it->second.outputBuses.end() ? b->second : -1;
    }
    int getLastSynthID(ofxSCServer* server) override {
        auto it = servers.find(server);
        return it != servers.end() && it->second.synth ? it->second.synth->nodeID : -1;
    }
    // Gates: channel n is track n
    std::string getOutputChannelName(int outputIndex, int channel) override {
        if(outputIndex != 0 || channel < 0 || channel >= numTracks.get()) return "";
        return "Track " + ofToString(channel + 1);
    }
    void setInputBus(ofxSCServer*, scNode*, int) override {}
    void resetInputBusses(ofxSCServer*, int) override {}
    void activate() override {
        for(auto& [server, st] : servers) if(st.synth) st.synth->run(true);
        forceHardAnchor = true;   // re-positioned without firing the missed ticks
        follower.requestAnchor();
    }
    void deactivate() override { for(auto& [server, st] : servers) if(st.synth) st.synth->run(false); }
    // The recorder writes the patch's state at score time zero: the onset
    // buffer too (written while it was arming, it never reached the score)
    void resendParametersForNRT() override { resendParams.notify(); invalidate(-1); fillBuffers(); }

    // ════════════════════════════════════════════════════════════════════════
    // Presets
    // ════════════════════════════════════════════════════════════════════════

    void presetSave(ofJson& json) override {
        auto& c = json["EuclideanBox"] = ofJson::object();
        for(const auto& ctl : controls) if(!published.count(ctl.key)) c[ctl.key] = ctl.save();
        json["EuclideanBoxPublished"] = publishedKeys;
        timeline.save(json, "EuclideanBoxTimeline");
        json["EuclideanBoxSelected"] = selectedTrack;
    }
    void loadBeforeConnections(ofJson& json) override {
        if(!json.contains("EuclideanBoxPublished") || !json["EuclideanBoxPublished"].is_array()) return;
        std::vector<std::string> wanted;
        for(const auto& k : json["EuclideanBoxPublished"]) if(k.is_string()) wanted.push_back(k.get<std::string>());
        const auto old = publishedKeys;
        for(const auto& k : old)
            if(std::find(wanted.begin(), wanted.end(), k) == wanted.end() && !hasConnection(k)) unpublish(k);
        for(const auto& k : wanted) publish(k);
            timeline.load(json, "EuclideanBoxTimeline");
}
    void presetRecallBeforeSettingParameters(ofJson& json) override { loadBeforeConnections(json); }
    void presetRecallAfterSettingParameters(ofJson& json) override {
        if(json.contains("EuclideanBox") && json["EuclideanBox"].is_object()){
            const auto& c = json["EuclideanBox"];
            for(const auto& ctl : controls) if(c.contains(ctl.key)) ctl.load(c[ctl.key]);
        }
        if(json.contains("EuclideanBoxSelected") && json["EuclideanBoxSelected"].is_number_integer())
            selectedTrack = std::clamp(json["EuclideanBoxSelected"].get<int>(), 0, MAX_TRACKS - 1);
        invalidate(-1);
        for(auto& [server, st] : servers) sendStaticControls(st);
    }

private:
    // ════════════════════════════════════════════════════════════════════════
    // Tracks
    // ════════════════════════════════════════════════════════════════════════

    enum Unit : int { UNIT_CYCLES = 0, UNIT_BARS, UNIT_BEATS, UNIT_STEPS };
    struct Division { int ticks; const char* label; };
    static const std::vector<Division>& divisions(){
        static const std::vector<Division> d = {{192, "1 bar"}, {96, "1/2"}, {48, "1/4"}, {24, "1/8"},
                                                {16, "1/8 T"}, {12, "1/16"}, {8, "1/16 T"}, {6, "1/32"}};
        return d;
    }
    static const std::vector<std::string>& divisionLabels(){
        static std::vector<std::string> l;
        if(l.empty()) for(const auto& d : divisions()) l.push_back(d.label);
        return l;
    }
    static const std::vector<std::string>& unitNames(){
        static const std::vector<std::string> n = {"Cycles", "Bars", "Beats", "Steps"};
        return n;
    }

    enum Advance : int { ADV_OFF = 0, ADV_STEP, ADV_HIT, ADV_ACCENT };
    enum Reset : int { RESET_NEVER = 0, RESET_PERIOD, RESET_CYCLE };
    static const std::vector<std::string>& voiceModeNames(){ static const std::vector<std::string> n = {"Round robin", "All voices"}; return n; }
    static const std::vector<std::string>& advanceNames(){ static const std::vector<std::string> n = {"Off", "Every step", "Every hit", "Every accent"}; return n; }
    static const std::vector<std::string>& resetNames(){ static const std::vector<std::string> n = {"Never", "Each period", "Each cycle"}; return n; }

    struct Track {
        ofParameter<int> steps, hits, rotate, division, voiceMode, accLen, accents, accRotate, longLen, longHits, longRotate;
        ofParameter<int> pitchAdvance, interval, range, octave, pitchReset;
        ofParameter<int> every, unit, hitsMin, hitsMax, accMin, accMax, longMin, longMax, stepsMin, stepsMax, seed;
        ofParameter<float> chance, level, accLevel, gate, longGate, swing, root, rndChance;
        ofParameter<bool> mute, restart, accByHit, longByHit, bounce;
        ofParameter<bool> rndHits, rndRotate, rndAccents, rndAccRotate, rndLong, rndLongRotate, rndSteps;
        bool anyRandom() const { return rndHits || rndRotate || rndAccents || rndAccRotate || rndLong || rndLongRotate || rndSteps; }
    };
    // What a track plays in one period (base values, or the period's draw)
    struct Pattern {
        int steps = 16, hits = 4, rotate = 0, accents = 0, accRotate = 0, longHits = 0, longRotate = 0;
        bool randomized = false;
    };
    struct Onset { int64_t tick; float amp; int dur; bool accent, isLong; float pitch; int voice; };
    // The Note outputs revisit the same 16-beat history every frame. Keep the
    // three intersecting chunks, bounded independently for each track.
    struct NoteHistoryChunk { int64_t chunk = NO_CHUNK; std::vector<Onset> events; };
    std::array<std::array<NoteHistoryChunk, 3>, MAX_TRACKS> noteHistory;


    int voices() const { return std::clamp(numVoices.get(), 1, MAX_VOICES); }
    std::string playerDef() const { return "EuclideanBoxPlayer" + ofToString(voices()); }
    int stepTicks(int ti) const {
        return divisions()[std::clamp(tracks[ti].division.get(), 0, (int)divisions().size() - 1)].ticks;
    }
    int64_t periodTicks(int ti) const {
        const Track& T = tracks[ti];
        int64_t u = TPB;
        switch(T.unit.get()){
            case UNIT_CYCLES: u = (int64_t)T.steps.get() * stepTicks(ti); break;
            case UNIT_BARS:   u = std::max<int64_t>(1, std::llround(bpb * TPB)); break;
            case UNIT_BEATS:  u = TPB; break;
            default:          u = stepTicks(ti); break;
        }
        return std::max<int64_t>(1, u * std::max(1, T.every.get()));
    }
    static int randIn(int a, int b, uint64_t h){
        if(a > b) std::swap(a, b);
        return a + std::min(b - a, (int)(metrobox::unit(h) * (b - a + 1)));
    }
    Pattern patternAt(int ti, int64_t period) const {
        const Track& T = tracks[ti];
        Pattern p;
        p.steps = std::clamp(T.steps.get(), 1, MAX_STEPS);
        p.hits = T.hits.get();
        p.rotate = T.rotate.get();
        p.accents = T.accents.get();
        p.accRotate = T.accRotate.get();
        p.longHits = T.longHits.get();
        p.longRotate = T.longRotate.get();
        if(T.anyRandom()){
            const uint64_t key = periodKey(ti, period);
            auto h = [&](uint64_t salt){ return metrobox::hashOf(key, (uint64_t)period, salt, 0xE0C11Du); };
            if(metrobox::unit(h(1)) < T.rndChance.get()){
                p.randomized = true;
                if(T.rndSteps) p.steps = std::clamp(randIn(T.stepsMin, T.stepsMax, h(2)), 1, MAX_STEPS);
                if(T.rndHits) p.hits = randIn(T.hitsMin, T.hitsMax, h(3));
                if(T.rndRotate) p.rotate = randIn(0, p.steps - 1, h(4));
                if(T.rndAccents) p.accents = randIn(T.accMin, T.accMax, h(5));
                if(T.rndAccRotate) p.accRotate = randIn(0, std::max(0, T.accLen.get() - 1), h(6));
                if(T.rndLong) p.longHits = randIn(T.longMin, T.longMax, h(7));
                if(T.rndLongRotate) p.longRotate = randIn(0, std::max(0, T.longLen.get() - 1), h(8));
            }
        }
        p.hits = std::clamp(p.hits, 0, p.steps);
        p.accents = std::clamp(p.accents, 0, std::max(1, T.accLen.get()));
        p.longHits = std::clamp(p.longHits, 0, std::max(1, T.longLen.get()));
        return p;
    }
    // The random key of a track's period. Seeded: node Seed x track Seed (not
    // the track's position, so a moved track plays the same). Seed 0 (either):
    // a key per pass, so the period draws again each time it comes back.
    bool unseeded(int ti) const { return globalSeed.get() == 0 || tracks[ti].seed.get() == 0; }
    uint64_t periodKey(int ti, int64_t period) const {
        if(!unseeded(ti)) return metrobox::hashOf((uint64_t)(uint32_t)globalSeed.get(), (uint64_t)(uint32_t)tracks[ti].seed.get(), 0xEB0Bu);
        const auto it = passes[ti].find(period);
        return metrobox::hashOf(sessionSalt, (uint64_t)period, it == passes[ti].end() ? 0 : (uint64_t)it->second, 0xF5EEu);
    }
    // Unseeded: a period the playhead left gets a new pass (its next draw is
    // new); the chunks are rewritten so the next time round plays it
    void advancePasses(){
        const int64_t now = metrobox::tickAt(getFrameTransportState().current.beatPosition);
        for(int ti = 0; ti < numTracks.get(); ti++){
            const int64_t period = metrobox::floorDiv(now, periodTicks(ti));
            if(!unseeded(ti)){ lastPeriod[ti] = period; continue; }
            if(lastPeriod[ti] != NO_CHUNK && period != lastPeriod[ti]){
                passes[ti][lastPeriod[ti]]++;
                if(passes[ti].size() > 4096) passes[ti].clear();
                invalidate(ti);
            }
            lastPeriod[ti] = period;
        }
    }

    // E(k, n): hit at i when (i * k) mod n < k (the first on step 0)
    static bool euclid(int64_t i, int k, int n){ return n > 0 && k > 0 && (metrobox::positiveMod64(i, n) * k) % n < k; }
    // a step of the pattern (after Rotate) is a hit?
    static bool isHit(const Pattern& p, int64_t pos){ return euclid(pos - p.rotate, p.hits, p.steps); }
    // hits among n steps from position a of the pattern
    static int64_t hitsIn(const Pattern& p, int64_t a, int64_t n){
        if(n <= 0 || p.hits <= 0) return 0;
        int64_t count = (n / p.steps) * p.hits;
        for(int64_t k = 0; k < n % p.steps; k++) if(isHit(p, a + k)) count++;
        return count;
    }
    // accents of a rule E(k, len) among its first n indices (rotated)
    static int64_t ruleCount(int64_t n, int k, int len, int rot){
        if(n <= 0 || k <= 0 || len <= 0) return 0;
        int64_t count = (n / len) * k;
        for(int64_t i = 0; i < n % len; i++) if(euclid(i - rot, k, len)) count++;
        return count;
    }
    // A step's place: its period, the pattern, the position, and the counters
    // (steps / hits since the start, the period, the cycle)
    struct StepInfo { int64_t period; Pattern pattern; int pos; int64_t first, cycleStart; };
    StepInfo stepInfo(int ti, int64_t s) const {
        const int st = stepTicks(ti);
        const int64_t pl = periodTicks(ti);
        StepInfo info;
        info.period = metrobox::floorDiv(s * st, pl);
        info.pattern = patternAt(ti, info.period);
        info.first = metrobox::floorDiv(info.period * pl + st - 1, st);   // the period's first step
        const int64_t rel = tracks[ti].restart ? s - info.first : s;
        info.pos = (int)metrobox::positiveMod64(rel, info.pattern.steps);
        info.cycleStart = s - info.pos;
        return info;
    }
    struct Counters { int64_t steps[3], hits[3]; };       // [never, period, cycle]
    Counters counters(int ti, int64_t s, const StepInfo& info) const {
        const Track& T = tracks[ti];
        const Pattern& p = info.pattern;
        Counters c;
        c.steps[RESET_NEVER] = s;
        c.steps[RESET_PERIOD] = s - info.first;
        c.steps[RESET_CYCLE] = info.pos;
        c.hits[RESET_CYCLE] = hitsIn(p, 0, info.pos);            // hits before it in its cycle
        const int64_t firstPos = T.restart ? 0 : metrobox::positiveMod64(info.first, p.steps);
        c.hits[RESET_PERIOD] = hitsIn(p, firstPos, s - info.first);
        // from the start: exact while the pattern never changes; with random
        // draws (or Restart), counted from the period's start
        if(T.anyRandom() || T.restart) c.hits[RESET_NEVER] = c.hits[RESET_PERIOD];
        else c.hits[RESET_NEVER] = metrobox::floorDiv(s, p.steps) * p.hits + c.hits[RESET_CYCLE];
        return c;
    }
    float degreePitch(int ti, int64_t count) const {
        const Track& T = tracks[ti];
        const int R = std::max(1, T.range.get());
        int64_t d = count * T.interval.get();
        if(T.bounce && R > 1){
            const int64_t m = metrobox::positiveMod64(d, 2 * (R - 1));
            d = m < R ? m : 2 * (R - 1) - m;
        }else d = metrobox::positiveMod64(d, R);
        const auto& sc = scale.get();
        const int n = (int)sc.size();
        const float semis = n == 0 ? (float)d : sc[d % n] + 12.0f * (float)(d / n);
        return ofClamp(T.root.get() + 12.0f * T.octave.get() + semis, 0.0f, 135.0f);
    }
    // Every onset of a track with its tick in [from, to)
    std::vector<Onset> onsets(int ti, int64_t from, int64_t to) const {
        std::vector<Onset> out;
        const Track& T = tracks[ti];
        const int st = stepTicks(ti);
        const int swingTicks = (int)std::lround(T.swing.get() * st);
        const int64_t s0 = metrobox::floorDiv(from - swingTicks, st) - 1, s1 = metrobox::floorDiv(to, st) + 1;
        const int V = voices();
        for(int64_t s = s0; s <= s1; s++){
            const int64_t tick = s * st + ((s & 1) ? swingTicks : 0);
            if(tick < from || tick >= to) continue;
            const StepInfo info = stepInfo(ti, s);
            const Pattern& p = info.pattern;
            if(!isHit(p, info.pos)) continue;
            const Counters c = counters(ti, s, info);
            const int reset = std::clamp(T.pitchReset.get(), 0, 2);
            // accents and long notes: their own euclidean rules, by hit or by step
            const int64_t accIdx = T.accByHit ? c.hits[RESET_NEVER] : c.steps[RESET_NEVER];
            const int64_t longIdx = T.longByHit ? c.hits[RESET_NEVER] : c.steps[RESET_NEVER];
            const bool acc = euclid(accIdx - p.accRotate, p.accents, T.accLen.get());
            const bool lng = euclid(longIdx - p.longRotate, p.longHits, T.longLen.get());
            if(T.chance.get() < 1.0f){
                const uint64_t h = metrobox::hashOf(periodKey(ti, info.period), (uint64_t)s, 0xC4A2CEu);
                if(metrobox::unit(h) >= T.chance.get()) continue;
            }
            Onset o;
            o.tick = tick;
            o.accent = acc;
            o.isLong = lng;
            o.amp = std::max(0.001f, acc ? T.accLevel.get() : T.level.get());   // 0 would be "no onset"
            o.dur = std::max(1, (int)std::lround((lng ? T.longGate.get() : T.gate.get()) * st));
            // pitch: the advances before this hit
            int64_t advances = 0;
            switch(T.pitchAdvance.get()){
                case ADV_STEP: advances = c.steps[reset]; break;
                case ADV_HIT: advances = c.hits[reset]; break;
                case ADV_ACCENT: {
                    const int64_t base = T.accByHit ? c.hits[reset] : c.steps[reset];
                    advances = ruleCount(base, p.accents, T.accLen.get(), p.accRotate);
                    break;
                }
                default: break;
            }
            o.pitch = T.pitchAdvance.get() == ADV_OFF ? ofClamp(T.root.get() + 12.0f * T.octave.get(), 0.0f, 135.0f)
                                                      : degreePitch(ti, advances);
            o.voice = (T.voiceMode.get() == 1 || V == 1) ? -1 : (int)metrobox::positiveMod64(c.hits[RESET_NEVER], V);
            out.push_back(o);
        }
        return out;
    }
    // Note <n>: each voice's pitch now (its latest onset), for synths that take Oceanode pitches
    void updateNotes(){
        const int64_t now = metrobox::tickAt(getFrameTransportState().current.beatPosition);
        const int V = voices();
        for(int ti = 0; ti < numTracks.get(); ti++){
            std::vector<float> v(V, notes[ti].get().empty() ? 48.0f : notes[ti].get()[0]);
            const auto& cur = notes[ti].get();
            for(int k = 0; k < V && k < (int)cur.size(); k++) v[k] = cur[k];
            bool any = false;
            const int64_t from = now - 2 * CHUNK;
            const int64_t lastChunk = metrobox::floorDiv(now, CHUNK);
            for(int64_t chunk = metrobox::floorDiv(from, CHUNK); chunk <= lastChunk; chunk++){
                auto& cached = noteHistory[ti][metrobox::positiveMod64(chunk, 3)];
                if(cached.chunk != chunk){
                    cached.events = onsets(ti, chunk * CHUNK, (chunk + 1) * CHUNK);
                    cached.chunk = chunk;
                }
                for(const auto& o : cached.events){
                    if(o.tick < from || o.tick > now) continue;
                    if(o.voice < 0) std::fill(v.begin(), v.end(), o.pitch); else v[o.voice] = o.pitch;
                    any = true;
                }
            }
            if(any && v != cur) notes[ti] = v;
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Controls: typed values and publishing ("T<n> <Setting>")
    // ════════════════════════════════════════════════════════════════════════

    struct Control {
        std::string key;
        int track = 0;
        std::function<std::shared_ptr<ofxOceanodeAbstractParameter>()> add;
        std::function<ofJson()> save;
        std::function<void(const ofJson&)> load;
        std::function<std::string()> text;
        std::function<void(const std::string&)> apply;
    };
    std::vector<Control> controls;
    std::vector<Control> snapExtras;      // the node's parameters a snapshot holds (Tracks, Seed...)
    scSnapshots snaps;                    // the shared snapshots: library, morph, presets
    ofJson initial;                       // the groove at setup (the factory's base)
    float currentBpm = 120.0f;
    std::map<std::string, std::shared_ptr<ofxOceanodeAbstractParameter>> published;
    std::vector<std::string> publishedKeys;
    scTimelineMenu timeline;              // "Add to Timeline" from the window
    bool publishedSeparator = false;

    void regI(int ti, ofParameter<int>& p, const std::vector<std::string>& dropdown = {}){
        Control c;
        c.key = p.getEscapedName();
        c.track = ti;
        c.add = [this, &p, dropdown](){ auto h = addParameter(p); if(!dropdown.empty()) h->setDropdownOptions(dropdown); return h; };
        c.save = [&p](){ return ofJson(p.get()); };
        c.load = [&p](const ofJson& j){ try { p.set(std::clamp((int)std::lround(j.get<double>()), p.getMin(), p.getMax())); } catch(...) {} };
        c.text = [&p](){ return ofToString(p.get()); };
        c.apply = [&p](const std::string& t){ p.set(std::clamp((int)std::lround(ofToFloat(t)), p.getMin(), p.getMax())); };
        controls.push_back(std::move(c));
        listeners.push(p.newListener([this, ti](int&){ invalidate(ti); }));
    }
    void regF(int ti, ofParameter<float>& p){
        Control c;
        c.key = p.getEscapedName();
        c.track = ti;
        c.add = [this, &p](){ return addParameter(p); };
        c.save = [&p](){ return ofJson(p.get()); };
        c.load = [&p](const ofJson& j){ try { p.set(ofClamp(j.get<float>(), p.getMin(), p.getMax())); } catch(...) {} };
        c.text = [&p](){ return ofToString(p.get(), 3); };
        c.apply = [&p](const std::string& t){ p.set(ofClamp(ofToFloat(t), p.getMin(), p.getMax())); };
        controls.push_back(std::move(c));
        listeners.push(p.newListener([this, ti](float&){ invalidate(ti); }));
    }
    void regB(int ti, ofParameter<bool>& p){
        Control c;
        c.key = p.getEscapedName();
        c.track = ti;
        c.add = [this, &p](){ return addParameter(p); };
        c.save = [&p](){ return ofJson(p.get()); };
        c.load = [&p](const ofJson& j){ try { p.set(j.is_boolean() ? j.get<bool>() : j.get<double>() > 0.5); } catch(...) {} };
        c.text = [&p](){ return std::string(p.get() ? "1" : "0"); };
        c.apply = [&p](const std::string& t){ const std::string l = ofToLower(ofTrim(t)); p.set(l == "1" || l == "on" || l == "true" || l == "yes"); };
        controls.push_back(std::move(c));
        listeners.push(p.newListener([this, ti](bool&){ invalidate(ti); }));
    }
    // A control with no track and nothing to invalidate (snapshot settings,
    // the node's own parameters as snapshot values)
    template<typename T>
    Control plainControl(ofParameter<T>& p){
        Control c;
        c.key = p.getEscapedName();
        c.track = -1;
        c.add = [this, &p](){ return addParameter(p); };
        c.save = [&p](){ return ofJson(p.get()); };
        c.load = [&p](const ofJson& j){ try { p.set(j.get<T>()); } catch(...) {} };
        c.text = [&p](){ return ofToString(p.get()); };
        c.apply = [&p](const std::string& t){ if constexpr(!std::is_same_v<T, std::vector<float>>) p.set(ofFromString<T>(t)); };
        return c;
    }
    ofJson captureSnapshot() const {
        auto none = [](const std::string&){ return false; };
        ofJson j = scSnapshots::captureControls(controls, none);
        j.update(scSnapshots::captureControls(snapExtras, none));
        return j;
    }
    void applySnapshot(const ofJson& a, const ofJson& b, float t, bool sw){
        auto driven = [this](const std::string& k){ auto it = published.find(k); return it != published.end() && it->second->hasInConnection(); };
        scSnapshots::blendControls(snapExtras, a, b, t, sw, [](const std::string&){ return false; });   // Tracks first
        scSnapshots::blendControls(controls, a, b, t, sw, driven);
    }
    // Five starting grooves (the rest: the defaults)
    std::vector<ofJson> factorySnapshots() const {
        return scSnapshots::presetsFrom(initial, R"JSON([
 {"name": "Init"},
 {"name": "Four on the Floor", "set": {"Tracks": 3,
   "T1 Steps": 16, "T1 Hits": 4, "T1 Rotate": 0,
   "T2 Steps": 16, "T2 Hits": 2, "T2 Rotate": 4,
   "T3 Steps": 16, "T3 Hits": 8, "T3 Rotate": 1, "T3 Level": 0.4}},
 {"name": "Tresillo and Clave", "set": {"Tracks": 3,
   "T1 Steps": 8, "T1 Hits": 3, "T2 Steps": 16, "T2 Hits": 5, "T2 Rotate": 2,
   "T3 Steps": 16, "T3 Hits": 7, "T3 Level": 0.45}},
 {"name": "Afro 12-8", "set": {"Tracks": 3,
   "T1 Step": 4, "T1 Steps": 12, "T1 Hits": 4,
   "T2 Step": 4, "T2 Steps": 12, "T2 Hits": 7, "T2 Rotate": 2,
   "T3 Step": 4, "T3 Steps": 12, "T3 Hits": 5, "T3 Rotate": 4, "T3 Level": 0.5}},
 {"name": "Random Drift", "set": {"Tracks": 4,
   "T1 Random Hits": true, "T1 Hits Min": 2, "T1 Hits Max": 6,
   "T2 Chance": 0.7, "T3 Random Rotate": true,
   "T4 Steps": 7, "T4 Hits": 3, "T4 Swing": 0.2, "T4 Level": 0.5}}
])JSON");
    }
    const Control* findControl(const std::string& key) const {
        for(const auto& c : controls) if(c.key == key) return &c;
        return nullptr;
    }
    bool hasConnection(const std::string& key) const {
        auto it = published.find(key);
        return it != published.end() && it->second && (it->second->hasInConnection() || it->second->hasOutConnections());
    }
    void publish(const std::string& key){
        if(published.count(key)) return;
        const Control* c = findControl(key);
        if(!c) return;
        if(!publishedSeparator){ addSeparator("Published", ofColor(200)); publishedSeparator = true; }
        published[key] = c->add();
        publishedKeys.push_back(key);
        parameterGroupChanged.notify(this);
    }
    void unpublish(const std::string& key){
        auto it = published.find(key);
        if(it == published.end() || hasConnection(key)) return;
        removeParameter(it->second->getEscapedName());
        published.erase(it);
        publishedKeys.erase(std::remove(publishedKeys.begin(), publishedKeys.end(), key), publishedKeys.end());
        if(published.empty() && publishedSeparator){ removeSeparator("Published"); publishedSeparator = false; }
        parameterGroupChanged.notify(this);
    }
    void controlMenu(const std::string& key){
        timeline.mark(key);
        if(published.count(key) && !timeline.isHidden(key)){
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y - 1), ImVec2(b.x, b.y - 1), ImGui::GetColorU32(ImGuiCol_Text));
        }
        if(!ImGui::BeginPopupContextItem(("##ebPublish" + key).c_str())) return;
        ImGui::TextDisabled("%s", key.c_str());
        ImGui::Separator();
        if(const Control* c = findControl(key)){
            static std::map<std::string, std::array<char, 128>> buffers;
            auto& buf = buffers[key];
            const bool appearing = ImGui::IsWindowAppearing();
            if(appearing) std::snprintf(buf.data(), buf.size(), "%s", c->text().c_str());
            ImGui::TextDisabled("Value");
            ImGui::SetNextItemWidth(160.0f);
            if(appearing) ImGui::SetKeyboardFocusHere();
            const bool enter = ImGui::InputText("##ebTyped", buf.data(), buf.size(),
                                                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if(enter || ImGui::IsItemDeactivatedAfterEdit()){ c->apply(std::string(buf.data())); if(enter) ImGui::CloseCurrentPopup(); }
            ImGui::Separator();
            if(timeline.isHidden(key)){
                if(ImGui::MenuItem("Publish to Node GUI")) timeline.reveal(key);
            }else if(published.count(key)){
                if(hasConnection(key)) ImGui::TextDisabled("Disconnect it to unpublish");
                else if(ImGui::MenuItem("Unpublish from Node")) timeline.unpublish(key);
            }else if(ImGui::MenuItem("Publish to Node GUI")) publish(key);
            timeline.menuItems(key);
        }
        ImGui::EndPopup();
    }

    // ════════════════════════════════════════════════════════════════════════
    // Ports
    // ════════════════════════════════════════════════════════════════════════

    static std::string busName(int index){
        static const char* kinds[] = {"trigout", "gateout", "pitchout"};
        if(index == 0) return "gatesout";
        return std::string(kinds[(index - 1) % OUTPUTS_PER_TRACK]) + ofToString((index - 1) / OUTPUTS_PER_TRACK + 1);
    }
    void syncOutputPorts(){
        const int n = std::clamp(numTracks.get(), 1, MAX_TRACKS);
        if(getNumOutputs() == 0) scNode::addOutput("Gates");
        while(portTracks < n){
            const std::string t = ofToString(portTracks + 1);
            scNode::addOutput("Trig " + t);
            scNode::addOutput("Gate " + t);
            scNode::addOutput("Pitch " + t);
            addOutputParameter(notes[portTracks]);
            portTracks++;
        }
        while(portTracks > n){
            const int first = 1 + (portTracks - 1) * OUTPUTS_PER_TRACK;
            removeParameter(notes[portTracks - 1].getEscapedName());
            for(int k = OUTPUTS_PER_TRACK - 1; k >= 0; k--) scNode::removeOutput(first + k);
            // the removed ports' buses may go to another node: forgotten (the synth silences the track)
            for(auto& [server, st] : servers) for(int k = 0; k < OUTPUTS_PER_TRACK; k++) st.outputBuses.erase(first + k);
            portTracks--;
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // Onset buffer (MetroBoxPlayer's layout: track regions of SLOTS chunks)
    // ════════════════════════════════════════════════════════════════════════

    struct ServerState {
        ofxSCSynth* synth = nullptr;
        ofxSCBuffer* buffer = nullptr;
        bool allocPending = false;
        float allocTime = 0.0f;
        std::map<int, int> outputBuses;
        int builtVoices = 0;
        std::array<std::array<int64_t, SLOTS>, MAX_TRACKS> slotChunk;
        ServerState(){ for(auto& s : slotChunk) s.fill(NO_CHUNK); }
    };
    std::map<ofxSCServer*, ServerState> servers;

    bool bufferLive(ofxSCServer* server, const ServerState& st) const {
        return server && st.buffer && st.buffer->index >= 0 && st.buffer->index < (int)server->buffers.size()
            && server->buffers[st.buffer->index] == st.buffer;
    }
    void invalidate(int ti){
        for(int i = 0; i < MAX_TRACKS; i++) if(ti < 0 || i == ti)
            for(auto& cached : noteHistory[i]) cached.chunk = NO_CHUNK;
        for(auto& [server, st] : servers)
            for(int i = 0; i < MAX_TRACKS; i++) if(ti < 0 || i == ti) st.slotChunk[i].fill(NO_CHUNK);
    }
    // The chunks the player may read soon: now, next, previous, the one after
    // (after the loop end: from the loop start)
    std::vector<int64_t> wantedChunks(const ofxOceanodeTransportState& t) const {
        const int64_t c = metrobox::floorDiv(metrobox::tickAt(t.beatPosition), CHUNK);
        auto atStartOf = [&](int64_t chunk){
            double beat = (double)chunk * CHUNK / TPB;
            if(t.loopActive() && beat >= t.loopEndBeat - 1e-9){
                const double len = t.loopEndBeat - t.loopStartBeat;
                beat = t.loopStartBeat + std::fmod(beat - t.loopEndBeat, len);
            }
            return metrobox::floorDiv(metrobox::tickAt(beat), CHUNK);
        };
        std::vector<int64_t> out;
        for(int64_t w : {c, atStartOf(c + 1), c - 1, atStartOf(c + 2)})
            if(std::find(out.begin(), out.end(), w) == out.end()) out.push_back(w);
        return out;
    }
    void writeSlot(ServerState& st, int ti, int64_t chunk){
        const int slot = (int)metrobox::positiveMod64(chunk, SLOTS);
        std::vector<float> data(CHUNK * BUF_CH, 0.0f);
        if(ti < numTracks.get() && !tracks[ti].mute.get()){
            const int64_t from = chunk * CHUNK;
            for(const auto& o : onsets(ti, from, from + CHUNK)){
                const int local = (int)(o.tick - from);
                data[local * BUF_CH] = o.amp;
                data[local * BUF_CH + 1] = (float)o.dur;
                data[local * BUF_CH + 2] = o.pitch;
                data[local * BUF_CH + 3] = (float)o.voice;
            }
        }
        const int first = (ti * SLOTS * MAX_CHUNK_TICKS + slot * CHUNK) * BUF_CH;
        constexpr int perMessage = 2048;
        for(int offset = 0; offset < (int)data.size(); offset += perMessage){
            const int count = std::min(perMessage, (int)data.size() - offset);
            ofxOscMessage m;
            m.setAddress("/b_setn");
            m.addIntArg(st.buffer->index);
            m.addIntArg(first + offset);
            m.addIntArg(count);
            for(int i = 0; i < count; i++) m.addFloatArg(data[offset + i]);
            st.buffer->server->sendMsg(m);
        }
        st.slotChunk[ti][slot] = chunk;
    }
    void fillBuffers(){
        const auto t = getFrameTransportState().current;
        const auto wanted = wantedChunks(t);
        for(auto& [server, st] : servers){
            if(st.allocPending || !bufferLive(server, st)) continue;
            for(int ti = 0; ti < MAX_TRACKS; ti++){
                std::array<bool, SLOTS> claimed{};
                for(int64_t w : wanted){
                    const int slot = (int)metrobox::positiveMod64(w, SLOTS);
                    if(claimed[slot]) continue;
                    claimed[slot] = true;
                    if(st.slotChunk[ti][slot] != w) writeSlot(st, ti, w);
                }
            }
        }
    }
    void sendStaticControls(ServerState& st){
        if(!st.synth) return;
        if(st.buffer) st.synth->set("buf", st.buffer->index);
        st.synth->set("trigAmp", trigAmp ? 1.0f : 0.0f);
        st.synth->set("layers", (float)numTracks.get());
        std::vector<float> mutes(MAX_TRACKS);
        for(int i = 0; i < MAX_TRACKS; i++) mutes[i] = tracks[i].mute ? 1.0f : 0.0f;
        st.synth->set("mute", mutes);
        for(auto& [index, bus] : st.outputBuses)
            if(index >= 0 && index < NUM_OUTPUTS) st.synth->set(busName(index), bus);
    }

    // ════════════════════════════════════════════════════════════════════════
    // Transport (as MetroBox: \base = the absolute tick at floor(anchorPos)
    // modulo each track's buffer period, computed here in 64-bit)
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
            int64_t delta = (int64_t)std::floor(sa.pos) - metrobox::positiveMod64(k, period);
            if(delta > period / 2) delta -= period;
            else if(delta < -period / 2) delta += period;
            const int64_t absTick = k + delta;
            std::vector<float> chunkTicks(MAX_TRACKS, (float)CHUNK), bases(MAX_TRACKS);
            for(int i = 0; i < MAX_TRACKS; i++) bases[i] = (float)metrobox::positiveMod64(absTick, (int64_t)CHUNK * SLOTS);
            anchorIdCounter = anchorIdCounter % (1 << 20) + 1;
            if(hard) anchorHardCounter = anchorHardCounter % (1 << 20) + 1;
            bool pending = false;
            scTransportSync::AnchorScope scope(a);
            for(auto& [server, st] : servers){
                auto* s = st.synth;
                if(!s) continue;
                // sets stored on a synth not confirmed yet would go out later without the timetag
                if(!s->isCreated()){ pending = true; continue; }
                scSynthControlBatch controls(s, server);
                controls.set("bpm", a.bpm);
                controls.set("run", a.playing ? 1.0f : 0.0f);
                controls.set("anchorPos", (float)sa.pos);
                controls.set("anchorFire", sa.fire ? 1.0f : 0.0f);
                controls.set("cycleTicks", chunkTicks);
                controls.set("base", bases);
                controls.set("anchorHard", (float)anchorHardCounter);
                controls.set("anchorId", (float)anchorIdCounter);   // last: starts the new anchor
                controls.send();
            }
            if(pending){ forceHardAnchor = true; follower.requestAnchor(); }
        }
    }

    // Voices changed: the player replaced in place (/s_new action 4), re-anchored
    void replaceSynths(){
        for(auto& [server, st] : servers){
            if(!st.synth || st.builtVoices == voices()) continue;
            auto* old = st.synth;
            st.synth = new ofxSCSynth(playerDef(), server);
            st.builtVoices = voices();
            sendStaticControls(st);
            st.synth->set("run", 0.0f);
            if(old->isCreated()) st.synth->createAndRun(4, old->nodeID, getActive());
            delete old;
        }
        forceHardAnchor = true;
        follower.requestAnchor();
    }

    double beatsPerBar() const {
#if OFXOCEANODESC_HAS_TIMELINE
        if(auto* container = getHostContainer())
            return std::max(0.25, container->getTimelineManager().getBeatsPerBar());
#endif
        return 4.0;
    }

    // ════════════════════════════════════════════════════════════════════════
    // Window
    // ════════════════════════════════════════════════════════════════════════

    static ImU32 trackColor(int i, float alpha = 1.0f){
        static const float hues[] = {0.58f, 0.02f, 0.13f, 0.33f, 0.78f, 0.48f, 0.90f, 0.20f};
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(hues[i % 8], 0.55f, 0.95f, r, g, b);
        return ImGui::GetColorU32(ImVec4(r, g, b, alpha));
    }

    void drawWindow(){
        const std::string title = getWindowTitle();
        bool open = showWindow.get();
        ImGui::SetNextWindowSize(ImVec2(1160.0f, 820.0f), ImGuiCond_FirstUseEver);
        if(ImGui::Begin(title.c_str(), &open)){
            ImGui::PushID(this);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("EUCLIDEANBOX");
            ImGui::SameLine(0, 20);
            int n = numTracks.get();
            ImGui::SetNextItemWidth(120);
            if(ImGui::SliderInt("##ebTracks", &n, 1, MAX_TRACKS, "Tracks %d")) numTracks = n;
            ImGui::SameLine();
            int seed = globalSeed.get();
            ImGui::SetNextItemWidth(130);
            if(ImGui::InputInt("Seed##ebSeed", &seed)) globalSeed = std::clamp(seed, 0, 99999);
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Every track's random choices (Randomize, Chance).\n0 = no seed: new choices every pass (a loop never repeats exactly)");
            ImGui::SameLine();
            bool ta = trigAmp.get();
            if(ImGui::Checkbox("Trig Amp", &ta)) trigAmp = ta;
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Trig outputs carry the hit's level instead of 1");
            ImGui::SameLine(0, 20);
            int nv = numVoices.get();
            ImGui::SetNextItemWidth(110);
            if(ImGui::SliderInt("##ebVoices", &nv, 1, MAX_VOICES, "Voices %d")) numVoices = nv;
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Each output is this many channels (the synth's voices)");
            ImGui::SameLine();
            {   // the scale: presets (the node's Scale input takes any)
                static const std::vector<std::pair<const char*, std::vector<float>>> scales = {
                    {"Major", {0, 2, 4, 5, 7, 9, 11}}, {"Minor", {0, 2, 3, 5, 7, 8, 10}}, {"Dorian", {0, 2, 3, 5, 7, 9, 10}},
                    {"Phrygian", {0, 1, 3, 5, 7, 8, 10}}, {"Lydian", {0, 2, 4, 6, 7, 9, 11}}, {"Mixolydian", {0, 2, 4, 5, 7, 9, 10}},
                    {"Harmonic minor", {0, 2, 3, 5, 7, 8, 11}}, {"Pentatonic", {0, 2, 4, 7, 9}}, {"Minor pentatonic", {0, 3, 5, 7, 10}},
                    {"Whole tone", {0, 2, 4, 6, 8, 10}}, {"Chromatic", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}}};
                std::string cur = "Custom";
                for(const auto& sc : scales) if(sc.second == scale.get()) cur = sc.first;
                ImGui::SetNextItemWidth(150);
                if(ImGui::BeginCombo("##ebScale", ("Scale: " + cur).c_str())){
                    for(const auto& sc : scales) if(ImGui::Selectable(sc.first, cur == sc.first)) scale = sc.second;
                    ImGui::EndCombo();
                }
            }
            ImGui::SameLine(0, 20);
            ImGui::SetNextItemWidth(110);
            ImGui::SliderInt("##ebBars", &viewBars, 1, 16, "Bars shown %d");
            // snapshots, named presets, morph (scSnapshots), then the tracks
            snaps.drawBar(std::max(1.0f, ImGui::GetContentRegionAvail().x));

            const float w = std::max(300.0f, ImGui::GetContentRegionAvail().x);
            const float viewH = std::max(240.0f, std::min(380.0f, 60.0f + numTracks.get() * 44.0f));
            drawRings(viewH);
            ImGui::SameLine(0, 12);
            drawTimeline(w - viewH - 12, viewH);

            ImGui::Spacing();
            selectedTrack = std::clamp(selectedTrack, 0, numTracks.get() - 1);
            if(ImGui::BeginTabBar("##ebTabs")){
                for(int ti = 0; ti < numTracks.get(); ti++){
                    ImGui::PushStyleColor(ImGuiCol_Text, trackColor(ti));
                    const bool openTab = ImGui::BeginTabItem(("Track " + ofToString(ti + 1) + "###ebTab" + ofToString(ti)).c_str(), nullptr,
                                                             pendingSelect == ti ? ImGuiTabItemFlags_SetSelected : 0);
                    ImGui::PopStyleColor();
                    if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::GetDragDropPayload())
                        ImGui::SetTooltip("Drag onto another track's tab to move it there (its outputs and its Gates channel move with it)");
                    trackDragSource(ti);
                    trackDropTarget(ti);
                    if(openTab){ selectedTrack = ti; drawTrackEditor(ti); ImGui::EndTabItem(); }
                }
                ImGui::EndTabBar();
            }
            pendingSelect = -1;
            ImGui::PopID();
        }
        ImGui::End();
        if(open != showWindow.get()) showWindow = open;
        if(pendingMoveFrom >= 0){
            moveTrack(pendingMoveFrom, pendingMoveTo);
            pendingMoveFrom = pendingMoveTo = -1;
        }
    }

    // ── Moving tracks: every setting of the track goes to the new position
    //    (published controls stay with their position: "T2 Hits" is track 2's) ──
    struct TrackDrag { const void* node; int track; };
    void trackDragSource(int ti){
        if(!ImGui::BeginDragDropSource()) return;
        const TrackDrag d{this, ti};
        ImGui::SetDragDropPayload("EuclideanBoxTrack", &d, sizeof(d));
        ImGui::Text("Move track %d", ti + 1);
        ImGui::EndDragDropSource();
    }
    void trackDropTarget(int ti){
        if(!ImGui::BeginDragDropTarget()) return;
        if(const ImGuiPayload* p = ImGui::AcceptDragDropPayload("EuclideanBoxTrack")){
            const TrackDrag d = *(const TrackDrag*)p->Data;
            if(d.node == this && d.track != ti){ pendingMoveFrom = d.track; pendingMoveTo = ti; }
        }
        ImGui::EndDragDropTarget();
    }
    void moveTrack(int from, int to){
        const int n = numTracks.get();
        if(from < 0 || to < 0 || from >= n || to >= n || from == to) return;
        // each track's values, in registration order (the same for every track)
        std::array<std::vector<ofJson>, MAX_TRACKS> values;
        for(const auto& c : controls) if(c.track >= 0 && c.track < MAX_TRACKS) values[c.track].push_back(c.save());
        auto move = [&](auto& a){
            auto v = a[from];
            if(from < to) for(int i = from; i < to; i++) a[i] = a[i + 1];
            else for(int i = from; i > to; i--) a[i] = a[i - 1];
            a[to] = v;
        };
        move(values);
        move(passes);
        move(lastPeriod);
        std::array<std::vector<float>, MAX_TRACKS> noteValues;
        for(int i = 0; i < MAX_TRACKS; i++) noteValues[i] = notes[i].get();
        move(noteValues);
        std::array<size_t, MAX_TRACKS> next{};
        for(const auto& c : controls)
            if(c.track >= 0 && c.track < MAX_TRACKS && next[c.track] < values[c.track].size()) c.load(values[c.track][next[c.track]++]);
        for(int i = 0; i < MAX_TRACKS; i++) if(notes[i].get() != noteValues[i]) notes[i] = noteValues[i];
        invalidate(-1);
        for(auto& [server, st] : servers) sendStaticControls(st);
        if(selectedTrack == from) { selectedTrack = to; pendingSelect = to; }
    }

    // Concentric rings, track 1 outside: each track's pattern at the playhead
    void drawRings(float size){
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##ebRings", ImVec2(size, size));
        dl->AddRectFilled(o, ImVec2(o.x + size, o.y + size), ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
        const ImVec2 c(o.x + size * 0.5f, o.y + size * 0.5f);
        const int n = numTracks.get();
        const float outer = size * 0.5f - 14.0f, inner = std::max(18.0f, size * 0.12f);
        const float gap = n > 1 ? (outer - inner) / (n - 1) : 0.0f;
        const double tick = getFrameTransportState().current.beatPosition * TPB;
        int hovered = -1;
        for(int ti = 0; ti < n; ti++){
            const Track& T = tracks[ti];
            const float r = outer - ti * gap;
            const float dim = T.mute ? 0.3f : 1.0f;
            const int64_t s = metrobox::floorDiv((int64_t)std::floor(tick), stepTicks(ti));
            const StepInfo info = stepInfo(ti, s);
            const Pattern& p = info.pattern;
            dl->AddCircle(c, r, ImGui::GetColorU32(ImGuiCol_Border, 0.35f * dim), 64, 1.0f);
            // the hits joined: the euclidean polygon
            std::vector<ImVec2> poly;
            auto at = [&](int pos){
                const float a = -(float)PI * 0.5f + 2.0f * (float)PI * pos / p.steps;
                return ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
            };
            for(int pos = 0; pos < p.steps; pos++) if(isHit(p, pos)) poly.push_back(at(pos));
            if(poly.size() >= 3) dl->AddConvexPolyFilled(poly.data(), (int)poly.size(), trackColor(ti, 0.08f * dim));
            if(poly.size() >= 2) dl->AddPolyline(poly.data(), (int)poly.size(), trackColor(ti, 0.35f * dim), ImDrawFlags_Closed, 1.0f);
            // this cycle's onsets as they play: accents bigger, long notes with an arc
            const int st = stepTicks(ti);
            std::vector<const Onset*> byPos(p.steps, nullptr);
            const auto cyc = onsets(ti, info.cycleStart * st, (info.cycleStart + p.steps) * st + st);
            for(const auto& on : cyc){
                const int64_t ps = metrobox::floorDiv(on.tick, st) - info.cycleStart;
                if(ps >= 0 && ps < p.steps) byPos[ps] = &on;
            }
            for(int pos = 0; pos < p.steps; pos++){
                const ImVec2 q = at(pos);
                const bool now = pos == info.pos;
                if(!isHit(p, pos)) dl->AddCircleFilled(q, 2.0f, ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.6f * dim));
                else if(!byPos[pos]) dl->AddCircle(q, 4.0f, trackColor(ti, 0.4f * dim), 12, 1.0f);   // dropped by Chance
                else{
                    const Onset& on = *byPos[pos];
                    dl->AddCircleFilled(q, on.accent ? 6.0f : 4.0f, trackColor(ti, (on.accent ? 1.0f : 0.65f) * dim));
                    if(on.accent) dl->AddCircle(q, 8.5f, trackColor(ti, 0.6f * dim), 16, 1.0f);
                    if(on.isLong){   // an arc along the ring, as long as the note
                        const float a0 = -(float)PI * 0.5f + 2.0f * (float)PI * pos / p.steps;
                        const float a1 = a0 + 2.0f * (float)PI * std::min(1.0f, (float)on.dur / (st * p.steps));
                        dl->PathArcTo(c, r + 6.0f, a0, a1, 16);
                        dl->PathStroke(trackColor(ti, 0.8f * dim), 0, 2.0f);
                    }
                }
                if(now) dl->AddCircle(q, 7.0f, IM_COL32(255, 255, 255, (int)(230 * dim)), 16, 2.0f);
            }
            const float dx = ImGui::GetIO().MousePos.x - c.x, dy = ImGui::GetIO().MousePos.y - c.y;
            if(ImGui::IsItemHovered() && std::abs(std::sqrt(dx * dx + dy * dy) - r) < std::max(5.0f, gap * 0.5f)) hovered = ti;
        }
        if(hovered >= 0){
            const int64_t s = metrobox::floorDiv((int64_t)std::floor(tick), stepTicks(hovered));
            const Pattern p = stepInfo(hovered, s).pattern;
            ImGui::SetTooltip("Track %d: %d hits / %d steps, rotate %d; accents E(%d, %d) rotate %d; long E(%d, %d)%s\nClick: edit it",
                              hovered + 1, p.hits, p.steps, p.rotate, p.accents, tracks[hovered].accLen.get(), p.accRotate,
                              p.longHits, tracks[hovered].longLen.get(), p.randomized ? "  (randomized)" : "");
            if(ImGui::IsItemClicked()){ selectedTrack = hovered; pendingSelect = hovered; }
        }
    }

    // Scrolling timeline: the playhead stays put, the hits move left
    void drawTimeline(float width, float height){
        const auto t = getFrameTransportState().current;
        const double viewBeats = viewBars * bpb;
        const double viewStart = t.beatPosition - viewBeats * 0.25, viewEnd = viewStart + viewBeats;
        const int n = numTracks.get();
        constexpr float labelW = 120.0f, rulerH = 18.0f;
        const float rowH = std::max(24.0f, (height - rulerH) / std::max(1, n));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        // the rhythm area (the labels are their own buttons, below)
        ImGui::SetCursorScreenPos(ImVec2(origin.x + labelW, origin.y));
        ImGui::InvisibleButton("##ebTimeline", ImVec2(std::max(50.0f, width - labelW), height));
        const float x0 = origin.x + labelW, w = std::max(50.0f, width - labelW);
        const float yRows = origin.y + rulerH, yEnd = yRows + n * rowH;
        auto X = [&](double beat){ return x0 + (float)((beat - viewStart) / viewBeats) * w; };
        const ImU32 beatLine = ImGui::GetColorU32(ImGuiCol_Border, 0.35f), barLine = ImGui::GetColorU32(ImGuiCol_Border, 1.0f);
        const ImU32 textDim = ImGui::GetColorU32(ImGuiCol_TextDisabled), text = ImGui::GetColorU32(ImGuiCol_Text);
        dl->AddRectFilled(ImVec2(x0, yRows), ImVec2(x0 + w, yEnd), ImGui::GetColorU32(ImGuiCol_FrameBg));
        dl->PushClipRect(ImVec2(x0, origin.y), ImVec2(x0 + w, yEnd), true);
        for(double beat = std::ceil(viewStart); beat <= viewEnd; beat += 1.0){
            const double inBar = std::fmod(std::fmod(beat, bpb) + bpb, bpb);
            const bool bar = inBar < 1e-6 || bpb - inBar < 1e-6;
            dl->AddLine(ImVec2(X(beat), yRows), ImVec2(X(beat), yEnd), bar ? barLine : beatLine);
            if(bar && beat >= 0) dl->AddText(ImVec2(X(beat) + 3, origin.y + 1), textDim, ofToString((int)std::lround(beat / bpb) + 1).c_str());
        }
        const int64_t fromTick = metrobox::tickAt(viewStart), toTick = metrobox::tickAt(viewEnd) + 1;
        for(int ti = 0; ti < n; ti++){
            const Track& T = tracks[ti];
            const float y0 = yRows + ti * rowH, yB = y0 + rowH - 4.0f;
            const float dim = T.mute ? 0.3f : 1.0f;
            // period starts (where a new random draw lands)
            if(T.anyRandom() || T.restart){
                const int64_t pl = periodTicks(ti);
                for(int64_t p = metrobox::floorDiv(fromTick, pl); p * pl < toTick && p - metrobox::floorDiv(fromTick, pl) < 512; p++){
                    const double pb = (double)(p * pl) / TPB;
                    if(pb < viewStart) continue;
                    const float px = X(pb);
                    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text, 0.3f);
                    dl->AddLine(ImVec2(px, y0 + 1), ImVec2(px, y0 + rowH - 1), col);
                    dl->AddTriangleFilled(ImVec2(px - 4, y0 + 1), ImVec2(px + 4, y0 + 1), ImVec2(px, y0 + 6), col);
                }
            }
            for(const auto& o : onsets(ti, fromTick - 16 * TPB, toTick)){
                const double ob = (double)o.tick / TPB, oe = ob + (double)o.dur / TPB;
                if(oe < viewStart || ob >= viewEnd) continue;
                const float alpha = (ob < t.beatPosition ? 0.45f : 1.0f) * dim;
                const float x = X(ob), xe = X(oe);
                dl->AddRectFilled(ImVec2(x, yB - 4), ImVec2(std::max(x + 1, xe), yB), trackColor(ti, 0.35f * alpha));
                const float top = yB - o.amp * (rowH - 10);
                dl->AddLine(ImVec2(x, yB), ImVec2(x, top), trackColor(ti, alpha), o.accent ? 2.5f : 1.2f);
                if(o.accent) dl->AddCircleFilled(ImVec2(x, top), 2.5f, trackColor(ti, alpha));
            }
            dl->AddLine(ImVec2(x0, y0 + rowH), ImVec2(x0 + w, y0 + rowH), beatLine);
        }
        const float px = X(t.beatPosition);
        dl->AddLine(ImVec2(px, origin.y), ImVec2(px, yEnd), ImGui::GetColorU32(ImGuiCol_PlotLinesHovered), 1.5f);
        dl->PopClipRect();
        // labels (a click selects the track)
        for(int ti = 0; ti < n; ti++){
            const float y0 = yRows + ti * rowH;
            const Track& T = tracks[ti];
            if(ti == selectedTrack) dl->AddRectFilled(ImVec2(origin.x, y0), ImVec2(x0 - 4, y0 + rowH - 2), ImGui::GetColorU32(ImGuiCol_Header));
            dl->AddRectFilled(ImVec2(origin.x + 4, y0 + 5), ImVec2(origin.x + 8, y0 + rowH - 6), trackColor(ti, T.mute ? 0.3f : 1.0f));
            const int64_t s = metrobox::floorDiv(metrobox::tickAt(t.beatPosition), stepTicks(ti));
            const Pattern p = stepInfo(ti, s).pattern;
            dl->AddText(ImVec2(origin.x + 14, y0 + 4), T.mute ? textDim : text,
                        ("E(" + ofToString(p.hits) + "," + ofToString(p.steps) + ")" + (p.randomized ? " *" : "")).c_str());
            if(rowH >= 34)
                dl->AddText(ImVec2(origin.x + 14, y0 + 19), textDim,
                            (std::string(divisions()[std::clamp(T.division.get(), 0, 7)].label) + (T.mute ? "  muted" : "")).c_str());
        }
        // the labels: a click selects the track, a drag moves it
        const ImVec2 back = ImGui::GetCursorScreenPos();
        for(int ti = 0; ti < n; ti++){
            const float y0 = yRows + ti * rowH;
            ImGui::SetCursorScreenPos(ImVec2(origin.x, y0));
            ImGui::PushID(ti);
            if(ImGui::InvisibleButton("##ebLabel", ImVec2(labelW - 4, rowH - 2)) && ImGui::GetIO().MouseDragMaxDistanceSqr[0] < 36.0f){
                selectedTrack = ti;
                pendingSelect = ti;
            }
            if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::GetDragDropPayload())
                ImGui::SetTooltip("Click: edit this track\nDrag onto another track: move it there");
            trackDragSource(ti);
            trackDropTarget(ti);
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(back);
    }

    // ── A track's controls: columns ─────────────────────────────────────────
    bool sliderI(ofParameter<int>& p, const char* label, int lo = INT_MIN, int hi = INT_MIN){
        int v = p.get();
        const int mn = lo == INT_MIN ? p.getMin() : lo, mx = hi == INT_MIN ? p.getMax() : hi;
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool ch = ImGui::SliderInt(("##" + p.getEscapedName()).c_str(), &v, mn, std::max(mn, mx), (std::string(label) + "  %d").c_str());
        if(ch) p = std::clamp(v, p.getMin(), p.getMax());
        controlMenu(p.getEscapedName());
        return ch;
    }
    bool sliderF(ofParameter<float>& p, const char* label, const char* fmt = "%.2f", bool logarithmic = false){
        float v = p.get();
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool ch = ImGui::SliderFloat(("##" + p.getEscapedName()).c_str(), &v, p.getMin(), p.getMax(),
                                           (std::string(label) + "  " + fmt).c_str(), logarithmic ? ImGuiSliderFlags_Logarithmic : 0);
        if(ch) p = v;
        controlMenu(p.getEscapedName());
        return ch;
    }
    bool check(ofParameter<bool>& p, const char* label){
        bool v = p.get();
        const bool ch = ImGui::Checkbox((std::string(label) + "##" + p.getEscapedName()).c_str(), &v);
        if(ch) p = v;
        controlMenu(p.getEscapedName());
        return ch;
    }
    bool combo(ofParameter<int>& p, const char* label, const std::vector<std::string>& options){
        const int v = std::clamp(p.get(), 0, (int)options.size() - 1);
        bool ch = false;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if(ImGui::BeginCombo(("##" + p.getEscapedName()).c_str(), (std::string(label) + ": " + options[v]).c_str())){
            for(int i = 0; i < (int)options.size(); i++) if(ImGui::Selectable(options[i].c_str(), i == v)){ p = i; ch = true; }
            ImGui::EndCombo();
        }
        controlMenu(p.getEscapedName());
        return ch;
    }
    static void heading(const char* t){ ImGui::TextDisabled("%s", t); ImGui::Separator(); }

    void drawTrackEditor(int ti){
        Track& T = tracks[ti];
        ImGui::PushID(ti);
        const float avail = ImGui::GetContentRegionAvail().x;
        const float gap = 14.0f, cw = (avail - gap * 4) / 5;
        auto column = [&](int k, const std::function<void()>& body){
            if(k) ImGui::SameLine(0, gap);
            ImGui::BeginChild(("##col" + ofToString(k)).c_str(), ImVec2(cw, 0), false);
            body();
            ImGui::EndChild();
        };
        column(0, [&](){
            heading("Pattern");
            sliderI(T.steps, "Steps");
            sliderI(T.hits, "Hits", 0, T.steps.get());
            sliderI(T.rotate, "Rotate", 0, std::max(0, T.steps.get() - 1));
            combo(T.division, "Step", divisionLabels());
            sliderF(T.swing, "Swing");
            sliderF(T.chance, "Chance");
            combo(T.voiceMode, "Voices", voiceModeNames());
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Round robin: each hit to the next voice (Voices: the node's)\nAll voices: every hit to every voice");
            check(T.mute, "Mute");
            ImGui::SameLine();
            if(ImGui::SmallButton("Invert hits")) T.hits = std::clamp(T.steps.get() - T.hits.get(), 0, T.steps.get());
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Hits = Steps - Hits (the complementary rhythm)");
            check(T.restart, "Restart each period");
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("On: the pattern starts again at every period (Randomize: Every).\n"
                                                         "Off: it runs on (polymeter against the bars).");
        });
        column(1, [&](){
            heading("Accents");
            sliderI(T.accents, "Accents", 0, T.accLen.get());
            sliderI(T.accLen, "Len");
            sliderI(T.accRotate, "Rotate", 0, std::max(0, T.accLen.get() - 1));
            check(T.accByHit, "By hit");
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("On: the accent rule steps with each hit\nOff: with each step");
            sliderF(T.level, "Level");
            sliderF(T.accLevel, "Accent Level");
            ImGui::TextDisabled("E(Accents, Len): the Gate's level");
        });
        column(2, [&](){
            heading("Durations");
            sliderF(T.gate, "Gate (steps)", "%.2f", true);
            sliderI(T.longHits, "Long", 0, T.longLen.get());
            sliderI(T.longLen, "Len");
            sliderI(T.longRotate, "Rotate", 0, std::max(0, T.longLen.get() - 1));
            check(T.longByHit, "By hit");
            sliderF(T.longGate, "Long Gate", "%.2f", true);
            ImGui::TextDisabled("E(Long, Len): those hits last Long Gate steps");
        });
        column(3, [&](){
            heading("Pitch");
            combo(T.pitchAdvance, "Advance", advanceNames());
            sliderF(T.root, "Root", "%.0f");
            sliderI(T.interval, "Interval");
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Scale degrees per advance (negative: down)");
            sliderI(T.range, "Range");
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Degrees before it wraps (or bounces back)");
            check(T.bounce, "Bounce");
            sliderI(T.octave, "Octave");
            combo(T.pitchReset, "Reset", resetNames());
            ImGui::TextDisabled("Scale: the node's input (header)");
        });
        column(4, [&](){
            heading("Randomize");
            sliderI(T.every, "Every");
            combo(T.unit, "Unit", unitNames());
            sliderF(T.rndChance, "Chance");
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("The chance a period draws new values (else the base values play)");
            auto range = [&](ofParameter<bool>& on, const char* label, ofParameter<int>* lo, ofParameter<int>* hi){
                check(on, label);
                if(lo && hi && on.get()){
                    ImGui::PushItemWidth(-FLT_MIN);
                    const float half = (ImGui::GetContentRegionAvail().x - 6) * 0.5f;
                    int a = lo->get(), b = hi->get();
                    ImGui::SetNextItemWidth(half);
                    if(ImGui::SliderInt(("##" + lo->getEscapedName()).c_str(), &a, lo->getMin(), lo->getMax(), "min %d")) *lo = a;
                    controlMenu(lo->getEscapedName());
                    ImGui::SameLine(0, 6);
                    ImGui::SetNextItemWidth(half);
                    if(ImGui::SliderInt(("##" + hi->getEscapedName()).c_str(), &b, hi->getMin(), hi->getMax(), "max %d")) *hi = b;
                    controlMenu(hi->getEscapedName());
                    ImGui::PopItemWidth();
                }
            };
            range(T.rndHits, "Hits", &T.hitsMin, &T.hitsMax);
            range(T.rndRotate, "Rotate", nullptr, nullptr);
            range(T.rndAccents, "Accents", &T.accMin, &T.accMax);
            range(T.rndAccRotate, "Accent Rotate", nullptr, nullptr);
            range(T.rndLong, "Long", &T.longMin, &T.longMax);
            range(T.rndLongRotate, "Long Rotate", nullptr, nullptr);
            range(T.rndSteps, "Steps", &T.stepsMin, &T.stepsMax);
            ImGui::Spacing();
            int sd = T.seed.get();
            ImGui::SetNextItemWidth(cw - 60);
            if(ImGui::InputInt(("Seed##" + T.seed.getEscapedName()).c_str(), &sd, 1, 10)) T.seed = std::clamp(sd, 0, 9999);
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("This track's random choices (Randomize, Chance): the same seed always plays the same.\n0 = no seed: new choices every pass");
            controlMenu(T.seed.getEscapedName());
            ImGui::SameLine();
            if(ImGui::SmallButton("Dice")){
                static std::mt19937 rng((unsigned)ofGetElapsedTimeMicros());
                T.seed = (int)(rng() % 10000);
            }
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("A new seed: other random draws (the same seed always plays the same)");
            if(unseeded(ti)) ImGui::TextDisabled("Unseeded: new draws every pass");
            // when the next draw lands
            const auto t = getFrameTransportState().current;
            if(T.anyRandom()){
                const int64_t pl = periodTicks(ti);
                const int64_t now = metrobox::tickAt(t.beatPosition);
                const double left = (double)((metrobox::floorDiv(now, pl) + 1) * pl - now) / TPB;
                ImGui::TextDisabled("Next draw in %.2f beats", left);
            }
        });
        ImGui::PopID();
    }

    // ════════════════════════════════════════════════════════════════════════
    // Members
    // ════════════════════════════════════════════════════════════════════════

    ofParameter<bool> showWindow;
    ofParameter<int> numTracks, globalSeed;
    ofParameter<bool> trigAmp;
    ofParameter<int> numVoices;
    ofParameter<std::vector<float>> scale;
    std::array<ofParameter<std::vector<float>>, MAX_TRACKS> notes;   // Note <n> outputs
    bool voicesDirty = false;
    std::array<Track, MAX_TRACKS> tracks;
    ofEvent<void> resendParams;
    ofEventListeners listeners;

    double bpb = 4.0;
    int portTracks = 0;
    int selectedTrack = 0;
    int pendingSelect = -1;                      // a tab to bring to the front (after a click or a move)
    int pendingMoveFrom = -1, pendingMoveTo = -1;
    // Unseeded tracks: how many times the playhead has left each period
    std::array<std::map<int64_t, int>, MAX_TRACKS> passes;
    std::array<int64_t, MAX_TRACKS> lastPeriod = [](){ std::array<int64_t, MAX_TRACKS> a; a.fill(NO_CHUNK); return a; }();
    const uint64_t sessionSalt = ((uint64_t)std::random_device{}() << 32) ^ std::random_device{}();
    int viewBars = 2;

    scTransportSync::Follower follower;
    scTransportSync::StepClock stepClock;
    int anchorIdCounter = 0, anchorHardCounter = 0;
    bool forceHardAnchor = true;
};

#endif // OFXOCEANODESC_HAS_TRANSPORT
