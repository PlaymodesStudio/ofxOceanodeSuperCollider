//
//  scHarmonyBox.h
//  ofxOceanodeSuperCollider
//
//  HarmonyBox: a bank of chords that work together, played by Index. It does
//  not run in time itself: sequence it from outside (a counter, MetroBox, the
//  timeline) and connect Pitch to an instrument, Scale / Root to an
//  ArpeggiatorBox or a melody.
//
//  • Generate: a progression in a style (Functional, Jazz, Glass, Reich,
//    Neo-Riemann, Neo-Soul, Modal Field) and a key, shaped by Color (chromatic adventure),
//    Extensions (triads .. thirteenths), Tension Level and Shape. Locked chords
//    stay; New on a card draws only that one, to fit its neighbours.
//    Seed 0 draws fresh every time; Seed > 0 is one progression per number,
//    and Generate moves to the next number.
//  • Each chord is typed or edited as a symbol (Dm9, F/C, G7alt, D(2,4,5)),
//    reordered by dragging, voiced with the least movement from the chord
//    before (Voices, Low / High, Spread, Bass).
//  • AI: describe a progression ("Glass in A minor with a pedal") or ask for
//    a variation of the current one.
//  • Index 1..N plays chord 1..N (wrapping); Index 0 is no chord: Gate 0,
//    the outputs keep the last chord (its release). Holding a card plays it
//    too, over whatever Index says.
//  • Chord Gates: one gate per chord (component k plays chord k+1), over
//    Index; the last one opened wins and its value is the Gate level. The
//    timeline writes it with a Chords lane (node menu "Add to Timeline as
//    Chord Track", or Chord Track in the window): a row per chord, named
//    and coloured by tension, drag in a row to hold that chord.
//  • Outputs for the chord playing: Pitch (voiced notes), Gate (1 per voice
//    while a chord plays), Bass, Root, Scale (relative to Root: the notes to
//    play over it), Tension (0..1).
//  • Audio: an Index cable picks the chord sample accurately (Index 0 there
//    reads the last chord: Pitch AR does not hold; Gate is control rate); Trig, Root AR
//    and Pitch AR (16 channels, the voicing repeated, Glide) come from
//    ChordBox's players (ChordBoxClock in Index mode + ChordBoxVoices,
//    synthdefs/Defaults/ChordBox) reading two tables: no SynthDef of its own.
//  • Window controls are node parameters hidden from the node: right-click
//    one to type a value, publish it to the node GUI, add it to the timeline
//    or bind MIDI (scTimelineMenu).
//  • Engine and tests: scHarmonyBoxEngine.h, tools/harmonybox_test.
//

#pragma once

#include "../../../ofxOceanodeSuperColliderConfig.h"
#include "../../../scNode.h"
#include "../../../audio/scSynthControlBatch.h"
#include "../../../ui/scTimelineMenu.h"
#include "../shared/scAudioParam.h"
#include "ofxSCSynth.h"
#include "ofxSCGroup.h"
#include "ofxSCBuffer.h"
#include "ofxSCBus.h"
#include "ofxSCServer.h"
#include "ofxOceanodeShared.h"
#include "ofxOceanodeAIAssistant.h"
#include "scHarmonyBoxEngine.h"
#include "../shared/scSnapshots.h"
#include "../../../ui/scDropdowns.h"
#include "imgui.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

class scHarmonyBox : public scNode {
public:
    static constexpr int INDEX_ROW = 256, LIVE_ROW = 288, ROWS = 290, NV = 16;   // ChordBoxClock's table layout
    enum Port { PORT_TRIG, PORT_ROOT, PORT_PITCH, NUM_PORTS };

    scHarmonyBox() : scNode("HarmonyBox") {}
    ~scHarmonyBox() override {
        listeners.unsubscribeAll();
        for(auto& [server, st] : servers) releaseServer(server, st, true);
        servers.clear();
    }

    // its window (SC Mixer Pro's "Go to origin synth window" opens and focuses it)
    std::string getWindowTitle() override { return (canvasID == "Canvas" ? "" : canvasID + "/") + "HarmonyBox " + ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return &showWindow; }

    // ════════════════════════════════════════════════════════════════════════
    // Setup
    // ════════════════════════════════════════════════════════════════════════

    void setup() override {
        using namespace harmonybox;
        description = "A bank of chords that work together, played by Index: Generate a progression in a style "
                      "(functional, jazz, Glass, Reich, neo-Riemannian) and key, lock the chords you like, type "
                      "your own, or ask the AI. Index 1..N plays a chord (0: none); outputs it voiced (Pitch, Bass), "
                      "a Gate, its Root, the Scale to play over it and its Tension. Show opens the chord bank: hold a card to play it.";
        addParameter(showWindow.set("Show", false));
        indexIn.attach(addParameter(indexP.set("Index", 0, 0, 127)), availableInputs, [this](){ refreshPorts(); });
        chordGatesHandle = addParameter(chordGatesP.set("Chord Gates", {0.0f}, {0.0f}, {1.0f}));
        addParameter(generateP.set("Generate"));
        addParameterDropdown(styleP, "Style", STYLE_FUNCTIONAL, styleNames());
        addParameterDropdown(keyP, "Key", 0, keyNames());
        addParameterDropdown(modeP, "Mode", MODE_MAJOR, modeNames());
        addParameter(numChordsP.set("NumChords", 4, 1, MAX_CHORDS));
        addParameter(seedP.set("Seed", 1, 0, 99999));
        addParameter(transposeP.set("Transpose", 0, -24, 24));
        snaps.setupParameters();
        snapshotHandle = addParameter(snaps.snapshotP);   // recalls by number (1..): a dropdown of their names
        addOutputParameter(pitchOut.set("Pitch", {60.0f}, {0.0f}, {127.0f}));
        addOutputParameter(bassOut.set("Bass", 48.0f, 0.0f, 127.0f));
        addOutputParameter(rootOut.set("Root", 0.0f, 0.0f, 11.0f));
        addOutputParameter(scaleOut.set("Scale", {0, 2, 4, 5, 7, 9, 11}, {0.0f}, {11.0f}));
        addOutputParameter(tensionOut.set("Tension", 0.0f, 0.0f, 1.0f));
        addOutputParameter(gateOut.set("Gate", {0.0f}, {0.0f}, {1.0f}));

        for(const char* n : {"Trig", "Root AR", "Pitch AR"}) scNode::addOutput(n);

        // the window's controls: node parameters without a widget until published
        const Settings d;
        const auto hidden = HIDDEN;
        windowOnly(addParameter(colorP.set("Color", d.color, 0.0f, 1.0f), hidden));
        windowOnly(addParameter(extensionsP.set("Extensions", d.extensions, 0.0f, 1.0f), hidden));
        windowOnly(addParameter(tensionP.set("Tension Level", d.tension, 0.0f, 1.0f), hidden));
        windowOnly(addParameterDropdown(shapeP, "Shape", d.shape, shapeNames(), hidden));
        windowOnly(addParameter(voicesP.set("Voices", d.voices, 1, MAX_VOICES), hidden));
        windowOnly(addParameter(lowP.set("Low", d.low, 12, 96), hidden));
        windowOnly(addParameter(highP.set("High", d.high, 24, 120), hidden));
        windowOnly(addParameterDropdown(spreadP, "Spread", d.spread, spreadNames(), hidden));
        windowOnly(addParameter(bassP.set("Bass Voice", d.bass), hidden));
        windowOnly(addParameter(leadP.set("Voice Leading", d.voiceLeading), hidden));
        windowOnly(addParameter(glideP.set("Glide", 0.0f, 0.0f, 4.0f), hidden));
        timeline.setup(this, [this](const std::string& k){ publish(k); }, [this](const std::string& k){ unpublish(k); },
                       [this](){ return std::vector<std::string>(published.begin(), published.end()); });

        pushSettings();
        engine.generate((uint64_t)seedP.get());

        auto regenerate = [this](){ if(!quiet()) generateNow(false); };
        listeners.push(indexP.newListener([this](int&){ updateOutputs(); }));
        listeners.push(chordGatesP.newListener([this](std::vector<float>& v){ chordGatesChanged(v); }));
        listeners.push(transposeP.newListener([this](int&){ updateOutputs(); }));
        listeners.push(generateP.newListener([this](){ generateNow(true); }));
        listeners.push(styleP.newListener([this, regenerate](int& v){ engine.s.style = v; regenerate(); }));
        listeners.push(modeP.newListener([this, regenerate](int& v){
            if(v == engine.s.mode) return;
            engine.s.mode = v;
            engine.changed();
            regenerate();
        }));
        listeners.push(keyP.newListener([this](int& v){
            if(syncing) return;
            engine.setKey(v);   // the chords move with the key
        }));
        listeners.push(numChordsP.newListener([this](int& v){
            if(quiet() || v == engine.size()) return;
            engine.setNumChords(v, drawSeed());
        }));
        listeners.push(seedP.newListener([this](int& v){
            if(quiet() || v <= 0) return;
            engine.generate((uint64_t)v);
        }));
        listeners.push(colorP.newListener([this](float& v){ engine.s.color = v; }));
        listeners.push(extensionsP.newListener([this](float& v){ engine.s.extensions = v; }));
        listeners.push(tensionP.newListener([this](float& v){ engine.s.tension = v; }));
        listeners.push(shapeP.newListener([this](int& v){ engine.s.shape = v; }));
        auto voicing = [this](){ if(syncing) return; pushSettings(); engine.changed(); };
        listeners.push(voicesP.newListener([voicing](int&){ voicing(); }));
        listeners.push(lowP.newListener([voicing](int&){ voicing(); }));
        listeners.push(highP.newListener([voicing](int&){ voicing(); }));
        listeners.push(spreadP.newListener([voicing](int&){ voicing(); }));
        listeners.push(bassP.newListener([voicing](bool&){ voicing(); }));
        listeners.push(leadP.newListener([voicing](bool&){ voicing(); }));
        listeners.push(glideP.newListener([this](float&){ for(auto& [server, st] : servers) sendVoices(st); }));

        {
            scSnapshots::Host h;
            h.folder = "HarmonyBox";
            h.title = "HarmonyBox";
            h.capture = [this](){ return ofJson{{"state", engine.toJson()}}; };
            // a chord bank does not blend: the state switches at the switch point
            h.apply = [this](const ofJson& a, const ofJson& b, float, bool sw){
                const ofJson& s = sw ? b : a;
                if(!s.contains("state") || s["state"] == appliedSnapshotState) return;
                appliedSnapshotState = s["state"];
                engine.fromJson(appliedSnapshotState);
                pullSettings();
            };
            h.bpm = [this](){ return std::max(1.0f, getFrameTransportState().current.bpm); };
            snaps.setup(h);
        }
        setupAI();
        updateOutputs();
    }

    // ════════════════════════════════════════════════════════════════════════
    // Frame
    // ════════════════════════════════════════════════════════════════════════

    void update(ofEventArgs&) override {
        timeline.update();
        scDropdowns::setNames(snapshotHandle, snaps.slotNames());
        snaps.update();
        ai.update();
        if(engine.revision != shownRevision){
            // the bank changed (generate, edit, AI, snapshot): the node's controls follow it
            if(numChordsP.get() != engine.size() || keyP.get() != engine.s.key || modeP.get() != engine.s.mode) pullSettings();
            updateOutputs();
        }
        const float now = ofGetElapsedTimef();
        for(auto& [server, st] : servers)
            if(st.allocPending && (!server->isNRTSyncPending() || now - st.allocTime > 2.0f)){ st.allocPending = false; st.tableWritten = false; }
        fillBuffers();
    }

    void draw(ofEventArgs&) override { if(showWindow) drawWindow(); }

    // ════════════════════════════════════════════════════════════════════════
    // Presets
    // ════════════════════════════════════════════════════════════════════════

    void presetSave(ofJson& json) override {
        json["HarmonyBox"] = engine.toJson();
        json["HarmonyBoxSnapshots"] = snaps.settingsJson();
        json["HarmonyBoxPublished"] = std::vector<std::string>(published.begin(), published.end());
        timeline.save(json, "HarmonyBoxTimeline");
        ai.save(json);
    }
    // the published window controls show before the connections are made
    void loadBeforeConnections(ofJson& json) override {
        std::set<std::string> wanted;
        if(json.contains("HarmonyBoxPublished") && json["HarmonyBoxPublished"].is_array())
            for(const auto& k : json["HarmonyBoxPublished"]) if(k.is_string()) wanted.insert(k.get<std::string>());
        for(const auto& k : std::set<std::string>(published)) if(!wanted.count(k)) unpublish(k);
        for(const auto& k : wanted) publish(k);
        timeline.load(json, "HarmonyBoxTimeline");
    }
    void presetRecallBeforeSettingParameters(ofJson& json) override { loadBeforeConnections(json); }
    void presetRecallAfterSettingParameters(ofJson& json) override {
        if(json.contains("HarmonyBox") && json["HarmonyBox"].is_object()) engine.fromJson(json["HarmonyBox"]);
        if(json.contains("HarmonyBoxSnapshots")) snaps.loadSettings(json["HarmonyBoxSnapshots"]);
        ai.load(json);
        pullSettings();
    }

    // ════════════════════════════════════════════════════════════════════════
    // scNode: a group with ChordBoxClock (Index mode) and ChordBoxVoices
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
        st.voices = new ofxSCSynth("ChordBoxVoices", server);
        sendStatic(st);
        st.clock->createAndRun(1, st.group->nodeID, true);
        st.voices->createAndRun(1, st.group->nodeID, true);
        st.tableWritten = false;
        st.liveCount++;
    }
    void moveSynthBefore(ofxSCServer* server, int nodeID) override {
        auto it = servers.find(server);
        if(it == servers.end() || !it->second.created) return;
        sendStatic(it->second);
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
        st.server = server;
        st.outputBuses[index] = bus;
        sendOutputBuses(st);
    }
    int getOutputNumChannels(int index) override { return index == PORT_PITCH ? NV : index >= 0 && index < NUM_PORTS ? 1 : -1; }
    std::string getOutputChannelName(int index, int channel) override {
        return index == PORT_PITCH ? "Voice " + ofToString(channel + 1) : "";
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
    void activate() override { for(auto& [server, st] : servers) if(st.group && st.created) st.group->run(true); }
    void deactivate() override { for(auto& [server, st] : servers) if(st.group && st.created) st.group->run(false); }
    // The recorder writes the patch's state at score time zero: the tables too
    void resendParametersForNRT() override {
        for(auto& [server, st] : servers){ sendStatic(st); st.tableWritten = false; st.liveCount++; }
        fillBuffers();
    }

private:
    // ════════════════════════════════════════════════════════════════════════
    // The players and their tables (rows as ChordBox's: INDEX_ROW + slot for
    // a cable, LIVE_ROW for the Index value)
    // ════════════════════════════════════════════════════════════════════════

    struct ServerState {
        ofxSCServer* server = nullptr;
        ofxSCGroup* group = nullptr;
        bool created = false;
        ofxSCSynth* clock = nullptr;
        ofxSCSynth* voices = nullptr;
        ofxSCBuffer* info = nullptr;     // per row: root, 0, 0, slot
        ofxSCBuffer* rows = nullptr;     // per row: 16 notes
        ofxSCBus* rowBus = nullptr;
        bool allocPending = false;
        float allocTime = 0.0f;
        std::map<int, int> outputBuses;
        bool tableWritten = false;
        int liveCount = 0;
    };
    std::map<ofxSCServer*, ServerState> servers;
    scAudioParam indexIn;
    uint64_t liveKey = UINT64_MAX;   // the live row's chord, transpose and bank

    static bool bufferLive(ofxSCServer* s, ofxSCBuffer* b){
        return s && b && b->index >= 0 && b->index < (int)s->buffers.size() && s->buffers[b->index] == b;
    }
    static bool audioBusLive(ofxSCServer* s, ofxSCBus* b){
        return s && b && b->index >= 0 && b->index < (int)s->audioBusses.size() && s->audioBusses[b->index] == b;
    }
    ofxSCBuffer* allocBuffer(ofxSCServer* server, ServerState& st, int frames, int channels){
        auto* b = new ofxSCBuffer(frames, channels, server);
        b->alloc();
        st.allocPending = true;
        st.allocTime = ofGetElapsedTimef();
        return b;
    }
    void ensureResources(ofxSCServer* server, ServerState& st){
        bool any = false;
        if(!bufferLive(server, st.info)){ delete st.info; st.info = allocBuffer(server, st, ROWS, 4); any = true; }
        if(!bufferLive(server, st.rows)){ delete st.rows; st.rows = allocBuffer(server, st, ROWS, NV); any = true; }
        if(!audioBusLive(server, st.rowBus)){
            delete st.rowBus;
            st.rowBus = new ofxSCBus(RATE_AUDIO, 1, server);
            if(!audioBusLive(server, st.rowBus)){ st.rowBus->free(); delete st.rowBus; st.rowBus = nullptr; }
        }
        if(any){ server->requestNRTSync(); st.tableWritten = false; }
    }
    void releaseServer(ofxSCServer* server, ServerState& st, bool resources){
        if(st.group && st.created) st.group->free();
        delete st.clock; st.clock = nullptr;
        delete st.voices; st.voices = nullptr;
        delete st.group; st.group = nullptr;
        st.created = false;
        if(resources){
            auto freeBuf = [&](ofxSCBuffer*& b){ if(b){ if(bufferLive(server, b)) b->free(); delete b; b = nullptr; } };
            freeBuf(st.info);
            freeBuf(st.rows);
            if(st.rowBus){ if(audioBusLive(server, st.rowBus)) st.rowBus->free(); delete st.rowBus; st.rowBus = nullptr; }
        }
    }
    void refreshPorts(){ for(auto& output : scNode::outputs) output = output; }

    void sendVoices(ServerState& st){
        if(!st.voices) return;
        scSynthControlBatch updates(st.voices, st.server);
        if(st.rowBus) updates.set("rowbus", st.rowBus->index);
        if(st.rows) updates.set("rows", st.rows->index);
        updates.set("glide", glideP.get());
        auto b = st.outputBuses.find(PORT_PITCH);
        if(b != st.outputBuses.end()) updates.set("pitchout", b->second);
        updates.send();
    }
    void sendOutputBuses(ServerState& st){
        if(st.clock){
            for(auto [port, name] : {std::pair<int, const char*>{PORT_TRIG, "trigout"}, {PORT_ROOT, "rootout"}}){
                auto b = st.outputBuses.find(port);
                if(b != st.outputBuses.end()) st.clock->set(name, b->second);
            }
        }
        sendVoices(st);
    }
    void sendIndexInput(ServerState& st){
        if(!st.clock) return;
        const int b = st.server ? indexIn.bus(st.server) : -1;
        if(b >= 0){ st.clock->set("index_sel", 1.0f); st.clock->mapan("index_ar", b, 1); }
        else st.clock->set("index_sel", 0.0f);
    }
    void sendStatic(ServerState& st){
        if(!st.clock) return;
        scSynthControlBatch updates(st.clock, st.server);
        if(st.info) updates.set("info", st.info->index);
        if(st.rowBus) updates.set("rowbus", st.rowBus->index);
        updates.set("mode", 0.0f);
        updates.set("steps", (float)std::max(1, engine.size()));
        updates.set("liveRow", (float)LIVE_ROW);
        updates.set("liveCount", (float)st.liveCount);
        updates.send();
        sendOutputBuses(st);
        sendIndexInput(st);
    }

    // /b_setn runs
    static void setRun(ofxSCBuffer* buf, int start, const float* data, int count){
        ofxOscMessage m;
        m.setAddress("/b_setn");
        m.addIntArg(buf->index);
        m.addIntArg(start);
        m.addIntArg(count);
        for(int i = 0; i < count; i++) m.addFloatArg(data[i]);
        buf->server->sendMsg(m);
    }
    // a row: the slot's root and its voicing repeated over 16 channels (with Transpose)
    void writeRow(ServerState& st, int row, int slot){
        const int t = transposeP.get();
        const float info[4] = {(float)harmonybox::pc((int)engine.root(slot) + t), 0.0f, 0.0f, (float)slot};
        setRun(st.info, row * 4, info, 4);
        const auto pitch = engine.pitch(slot);
        float notes[NV];
        for(int v = 0; v < NV; v++) notes[v] = pitch.empty() ? 0.0f : pitch[v % pitch.size()] + (float)t;
        setRun(st.rows, row * NV, notes, NV);
    }
    bool tablesLive(ServerState& st){
        return !st.allocPending && bufferLive(st.server, st.info) && bufferLive(st.server, st.rows);
    }
    void writeLiveRow(ServerState& st){
        if(!tablesLive(st)) return;
        writeRow(st, LIVE_ROW, currentSlot());
        st.liveCount++;
        if(st.clock) st.clock->set("liveCount", (float)st.liveCount);
    }
    // the cable's table, once per bank / transpose change; the live row on a new chord
    void fillBuffers(){
        const uint64_t key = (uint64_t)currentSlot() | ((uint64_t)(transposeP.get() + 64) << 8) | (engine.revision << 16);
        const bool liveChanged = key != liveKey;
        liveKey = key;
        for(auto& [server, st] : servers){
            if(!st.server) st.server = server;
            if(!tablesLive(st)){ if(!st.allocPending) ensureResources(server, st); continue; }
            if(!st.tableWritten){
                // the cable's Index k reads row k % n: chord k (1-based), the last one at 0
                const int n = engine.size();
                for(int i = 0; i < n; i++) writeRow(st, INDEX_ROW + (i + 1) % n, i);
                if(st.clock) st.clock->set("steps", (float)std::max(1, engine.size()));
                st.tableWritten = true;
                writeLiveRow(st);
            }else if(liveChanged) writeLiveRow(st);
        }
    }
    void tablesChanged(){ for(auto& [server, st] : servers) st.tableWritten = false; }

    // ════════════════════════════════════════════════════════════════════════
    // Publishing the window's controls (the parameters exist, hidden)
    // ════════════════════════════════════════════════════════════════════════

    scTimelineMenu timeline;
    std::set<std::string> windowKeys, published;

    // unpublished: no widget (and so no pin unless connected). Not DisableIn/Out
    // Connection: MacroMixer treats those as protected and would not morph them
    static constexpr int HIDDEN = ofxOceanodeParameterFlags_NoGuiWidget;
    void windowOnly(const std::shared_ptr<ofxOceanodeAbstractParameter>& h){ if(h) windowKeys.insert(h->getEscapedName()); }
    void setVisible(const std::string& key, bool visible){
        auto* p = timeline.parameter(key);
        if(!p) return;
        const auto flags = p->getFlags();
        const auto wanted = visible ? (flags & ~HIDDEN) : (flags | HIDDEN);
        if(wanted != flags){ p->setFlags(wanted); parameterGroupChanged.notify(this); }
    }
    void publish(const std::string& key){
        if(!windowKeys.count(key)) return;
        published.insert(key);
        setVisible(key, true);
    }
    void unpublish(const std::string& key){
        auto* p = timeline.parameter(key);
        if(!windowKeys.count(key) || (p && p->hasInConnection())) return;   // a cable keeps it on the node
        published.erase(key);
        setVisible(key, false);
    }

    // A window control's right-click menu: value, publishing, timeline, MIDI
    void paramMenu(const std::string& key){
        timeline.mark(key);
        auto* p = timeline.parameter(key);
        if(!p || !ImGui::BeginPopupContextItem(("##hbmenu" + key).c_str())) return;
        ImGui::TextDisabled("%s", p->getName().c_str());
        ImGui::Separator();
        static std::string editing;
        static char buffer[256];
        if(ImGui::IsWindowAppearing() || editing != key){
            editing = key;
            std::snprintf(buffer, sizeof(buffer), "%s", p->toString().c_str());
        }
        ImGui::TextDisabled("Value");
        ImGui::SetNextItemWidth(160.0f);
        if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##hbvalue", buffer, sizeof(buffer),
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if(enter || ImGui::IsItemDeactivatedAfterEdit()){
            try { p->fromString(std::string(buffer)); } catch(...) {}
            if(enter) ImGui::CloseCurrentPopup();
        }
        if(windowKeys.count(key)){
            ImGui::Separator();
            if(!published.count(key)){ if(ImGui::MenuItem("Publish to Node GUI")) publish(key); }
            else if(ImGui::MenuItem("Unpublish from Node", nullptr, false, !p->hasInConnection())) unpublish(key);
        }
        timeline.menuItems(key);
        ImGui::EndPopup();
    }

    harmonybox::Engine engine;
    scSnapshots snaps;
    std::shared_ptr<ofxOceanodeAbstractParameter> snapshotHandle;
    ofJson appliedSnapshotState;
    ofxOceanodeAIAssistant ai;
    ofEventListeners listeners;
    uint64_t shownRevision = 0;
    bool syncing = false;

    ofParameter<bool> showWindow;
    ofParameter<int> indexP, styleP, keyP, modeP, numChordsP, seedP, transposeP;
    ofParameter<void> generateP;
    ofParameter<float> colorP, extensionsP, tensionP, glideP;
    ofParameter<int> shapeP, voicesP, lowP, highP, spreadP;
    ofParameter<bool> bassP, leadP;
    ofParameter<std::vector<float>> pitchOut, scaleOut, gateOut;
    ofParameter<float> bassOut, rootOut, tensionOut;
    ofParameter<std::vector<float>> chordGatesP;
    std::shared_ptr<ofxOceanodeAbstractParameter> chordGatesHandle;

    // window state
    int editingSlot = -1;
    int padSlot = -1;     // the card held down
    int heldSlot = 0;     // the last chord played: Index 0 keeps it
    int gateSlot = -1;    // the chord Chord Gates holds (-1: none open)
    float gateLevel = 0.0f;
    std::vector<float> lastChordGates;
    char editBuffer[64] = {};
    bool focusEdit = false;
    std::string progressionBuffer, message;
    uint64_t progressionRevision = 0;

    bool quiet() const { return syncing || ofxOceanodeShared::isPresetLoading(); }

    static uint64_t freshSeed(){
        static std::random_device rd;
        return ((uint64_t)rd() << 32) ^ rd() ^ (uint64_t)ofGetElapsedTimeMicros();
    }
    // a seed for a draw the Seed does not name (dice, new slots): fresh, or
    // derived from Seed so a seeded patch stays reproducible
    uint64_t drawSeed(){
        static uint64_t count = 0;
        return seedP.get() > 0 ? (uint64_t)seedP.get() * 7919ull + (++count) : freshSeed();
    }

    // Generate: Seed 0 draws fresh, Seed > 0 moves to the next number
    // (button) or stays on it (a style or mode change)
    void generateNow(bool advance){
        if(seedP.get() <= 0){ engine.generate(freshSeed()); return; }
        if(advance){ seedP = seedP.get() >= seedP.getMax() ? 1 : seedP.get() + 1; return; }   // its listener generates
        engine.generate((uint64_t)seedP.get());
    }

    // parameters -> engine settings (not key / mode / chord count: those move the bank)
    void pushSettings(){
        auto& s = engine.s;
        s.style = styleP.get();
        s.key = keyP.get();
        s.mode = modeP.get();
        s.numChords = numChordsP.get();
        s.color = colorP.get();
        s.extensions = extensionsP.get();
        s.tension = tensionP.get();
        s.shape = shapeP.get();
        s.voices = voicesP.get();
        s.low = lowP.get();
        s.high = highP.get();
        s.spread = spreadP.get();
        s.bass = bassP.get();
        s.voiceLeading = leadP.get();
    }
    // engine -> parameters, without their listeners acting
    void pullSettings(){
        const auto s = engine.s;
        syncing = true;
        auto put = [](auto& p, auto v){ if(p.get() != v) p = v; };
        put(styleP, s.style);
        put(keyP, s.key);
        put(modeP, s.mode);
        put(numChordsP, engine.size());
        put(colorP, s.color);
        put(extensionsP, s.extensions);
        put(tensionP, s.tension);
        put(shapeP, s.shape);
        put(voicesP, s.voices);
        put(lowP, s.low);
        put(highP, s.high);
        put(spreadP, s.spread);
        put(bassP, s.bass);
        put(leadP, s.voiceLeading);
        syncing = false;
        updateOutputs();
    }

    bool gateOn() const { return padSlot >= 0 || gateSlot >= 0 || indexP.get() != 0; }
    int currentSlot(){
        if(padSlot >= 0 && padSlot < engine.size()) heldSlot = padSlot;
        else if(gateSlot >= 0 && gateSlot < engine.size()) heldSlot = gateSlot;
        else if(indexP.get() != 0) heldSlot = engine.wrap(indexP.get() - 1);
        return engine.wrap(heldSlot);
    }

    void updateOutputs(){
        shownRevision = engine.revision;
        if(engine.size() == 0) return;
        const int i = currentSlot();
        const int t = transposeP.get();
        auto pitch = engine.pitch(i);
        for(auto& v : pitch) v += (float)t;
        if(pitch != pitchOut.get()) pitchOut = pitch;
        const float bass = engine.bass(i) + (float)t;
        if(bass != bassOut.get()) bassOut = bass;
        const float root = (float)harmonybox::pc((int)engine.root(i) + t);
        if(root != rootOut.get()) rootOut = root;
        const auto scale = engine.scale(i);
        if(scale != scaleOut.get()) scaleOut = scale;
        const float tension = engine.tension(i);
        if(tension != tensionOut.get()) tensionOut = tension;
        const float level = padSlot < 0 && gateSlot >= 0 ? gateLevel : 1.0f;   // Chord Gates: its gate's level
        const std::vector<float> gate(std::max<size_t>(1, pitch.size()), gateOn() ? level : 0.0f);
        if(gate != gateOut.get()) gateOut = gate;
        // the players' tables follow the bank and Transpose; the live row the chord
        const uint64_t tk = engine.revision * 128 + (uint64_t)(t + 64);
        if(tk != tableKey){ tableKey = tk; tablesChanged(); }
        fillBuffers();
    }
    uint64_t tableKey = 0;

    // Chord Gates: a gate opening takes its chord; when the one playing
    // closes, another still open (the lowest) takes over, else none.
    // Components past the bank's chords play nothing.
    void chordGatesChanged(const std::vector<float>& v){
        const int n = std::min<int>((int)v.size(), engine.size());
        int opened = -1;
        for(int k = 0; k < n; k++)
            if(v[k] > 0.0f && (k >= (int)lastChordGates.size() || lastChordGates[k] <= 0.0f)) opened = k;
        lastChordGates = v;
        if(opened >= 0) gateSlot = opened;
        else if(gateSlot >= n || gateSlot < 0 || v[gateSlot] <= 0.0f){
            gateSlot = -1;
            for(int k = 0; k < n && gateSlot < 0; k++) if(v[k] > 0.0f) gateSlot = k;
        }
        const float level = gateSlot >= 0 ? std::min(1.0f, v[gateSlot]) : 0.0f;
        gateLevel = level;
        updateOutputs();
    }

public:
#ifdef OFXOCEANODE_HAS_TIMELINE_CHORDS
    // A chord bank for the timeline's Chords lane: Chord Gates, a row per chord
    bool getTimelineInstrument(ofxOceanodeTimelineInstrument& out) override {
        if(!chordGatesHandle) return false;
        out.gate = chordGatesHandle.get();
        out.voices = engine.size();
        out.chords = true;
        for(int i = 0; i < engine.size(); i++){
            out.rowNames.push_back(engine.name(i));
            const float t = std::clamp(engine.tension(i), 0.0f, 1.0f);   // the cards' tension colours
            out.rowColors.push_back(ofFloatColor(0.35f + 0.6f * t, 0.75f - 0.4f * t, 0.7f - 0.5f * t));
        }
        return true;
    }
#endif
private:

    // ════════════════════════════════════════════════════════════════════════
    // AI
    // ════════════════════════════════════════════════════════════════════════

    static std::string aiSystemPrompt(){
        return
R"SYS(You are a composer and harmony teacher writing chord progressions for HarmonyBox, a chord bank in a live music patcher. The chords are played by index, usually in a loop, so the last chord should lead back to the first.

You know functional harmony (cadences, secondary dominants, modal mixture, voice leading), jazz harmony (ii-V-I chains, tritone substitutions, backdoor progressions, Coltrane changes, extensions and alterations, modal and quartal harmony), minimalism (Philip Glass: short repeated cells such as i-VI or i-bVII-bVI-V, pedal tones, one note changing at a time, additive variation; Steve Reich: static modal fields, sus chords and clusters of seconds and fourths over a slowly moving bass, as in Music for 18 Musicians), neo-Riemannian transformations (P, L, R, slides, hexatonic poles, film-score chromatic mediants), neo-soul (D'Angelo, Erykah Badu, Robert Glasper: ninths and elevenths, IV-iii-ii-I, minor plagal iv, IV/V slash dominants, backdoor bVI-bVII-I, Dorian vamps, chords sliding in from a half step above), modal fields (one mode's quartal, sus and seventh chords without dominant pull, its colour note, over a pedal, moving to a neighbouring mode), pop and ambient.

Answer ONLY with:
Key: <note> <mode>
<chord> <chord> <chord> ...

- 1 to 16 chords on one line, separated by spaces. No prose, no numbering, no markdown, no bar lines.
- <mode> is one of: Major, Dorian, Phrygian, Lydian, Mixolydian, Minor, Locrian, Harmonic Minor, Melodic Minor.
- Chord symbols: root (C, F#, Bb...) then a quality from: (major), m, dim, aug, sus4, sus2, 5, maj7, m7, 7, 6, m6, m7b5, dim7, mMaj7, 7sus4, maj7sus2, sus24, 7#5, maj7#5, 7b5, add9, madd9, add11, madd11, maj9, m9, 9, 6/9, m6/9, 7b9, 7#9, 9sus4, m9b5, m7b9, mMaj9, 7#11, maj7#11, 7b13, m11, 11, maj9#11, 9#11, 13, maj13, m13, 13b9, 7b9b13, 7alt, q (stacked fourths, 3 notes), q4 (4 fourths).
- A slash bass is allowed: F/C, Am7/G. Any other set of notes can be written as intervals over the root in parentheses: D(2,4,5), E(b2,5,b7).
- If the request names a number of chords, give exactly that many.)SYS";
    }

    void setupAI(){
        ofxOceanodeAIAssistant::Spec spec;
        spec.title = "Chord progression with Requesty";
        spec.subtitle = [this](){
            return "Now: " + harmonybox::keyNames()[engine.s.key] + " " + harmonybox::modeNames()[engine.s.mode] + " - "
                 + engine.progressionText();
        };
        spec.generateHint = "Describe a progression: a style, a mood, a reference, a key, how many chords "
                            "(\"8 chords like Glass's Glassworks in F minor, with a pedal\").";
        spec.fixHint = "How to change the current progression (\"more jazz substitutions\", \"darker\", "
                       "\"keep chords 1 and 5\"), or leave empty for a variation.";
        spec.generateButton = "Write";
        spec.fixButton = "Vary";
        spec.generateModeLabel = "New";
        spec.fixModeLabel = "Vary current";
        spec.pendingHint = "The answer replaces the chord bank when ready.";
        spec.readyMessage = "AI progression ready";
        spec.systemPrompt = &scHarmonyBox::aiSystemPrompt;
        spec.buildPrompt = [this](const std::string& request, bool vary){
            using namespace harmonybox;
            std::string p = "Current setting: key " + keyNames()[engine.s.key] + " " + modeNames()[engine.s.mode] +
                            ", style " + styleNames()[engine.s.style] + ", " + std::to_string(engine.size()) + " chords.\n";
            p += "Current progression: " + engine.progressionText() + "\n";
            std::string locked;
            for(int i = 0; i < engine.size(); i++) if(engine.slots[i].locked) locked += " " + std::to_string(i + 1) + " (" + engine.name(i) + ")";
            if(!locked.empty()) p += "Locked chords, keep them at their positions:" + locked + "\n";
            if(vary) p += "\nRewrite the current progression. " + (request.empty() ? std::string("Make a fresh variation in the same spirit.") : request);
            else p += "\nWrite a new progression (use the current key and length unless asked otherwise). " + request;
            return p;
        };
        spec.apply = [this](const std::string& text) -> std::string {
            int key = -1, mode = -1;
            const auto chords = harmonybox::parseProgression(text, &key, &mode);
            if(chords.empty()) return "No chord symbols found in the answer";
            if(key >= 0) engine.s.key = key;      // the chords are absolute: no transposition
            if(mode >= 0) engine.s.mode = mode;
            engine.setChords(chords);
            pullSettings();
            return "";
        };
        ai.setup(spec);
    }

    // ════════════════════════════════════════════════════════════════════════
    // Window
    // ════════════════════════════════════════════════════════════════════════

    static std::string noteName(int midi){ return harmonybox::pcName(midi, false) + std::to_string(midi / 12 - 1); }

    template<typename T>
    bool combo(const char* label, ofParameter<T>& p, const std::vector<std::string>& names, float w){
        int v = (int)p.get();
        ImGui::SetNextItemWidth(w);
        bool changed = false;
        if(ImGui::BeginCombo(label, v >= 0 && v < (int)names.size() ? names[v].c_str() : "?")){
            for(int i = 0; i < (int)names.size(); i++){
                if(ImGui::Selectable(names[i].c_str(), i == v)){ p = (T)i; changed = true; }
                if(i == v) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        paramMenu(p.getEscapedName());
        return changed;
    }
    void slider(const char* label, ofParameter<float>& p, float w, const char* tip, const char* fmt = "%.2f"){
        float v = p.get();
        ImGui::SetNextItemWidth(w);
        if(ImGui::SliderFloat(label, &v, p.getMin(), p.getMax(), fmt)) p = v;
        if(ImGui::IsItemHovered() && tip) ImGui::SetTooltip("%s", tip);
        paramMenu(p.getEscapedName());
    }
    void sliderInt(const char* label, ofParameter<int>& p, float w, const char* fmt = "%d"){
        int v = p.get();
        ImGui::SetNextItemWidth(w);
        if(ImGui::SliderInt(label, &v, p.getMin(), p.getMax(), fmt)) p = v;
        paramMenu(p.getEscapedName());
    }
    void checkbox(const char* label, ofParameter<bool>& p, const char* tip){
        bool v = p.get();
        if(ImGui::Checkbox(label, &v)) p = v;
        if(ImGui::IsItemHovered() && tip) ImGui::SetTooltip("%s", tip);
        paramMenu(p.getEscapedName());
    }

    void drawWindow(){
        bool open = showWindow.get();
        ImGui::SetNextWindowSize(ImVec2(1000.0f, 620.0f), ImGuiCond_FirstUseEver);
        if(ImGui::Begin(getWindowId().c_str(), &open)){
            ImGui::PushID(this);
            drawHeader();
            drawTensionCurve(ImGui::GetContentRegionAvail().x, 46.0f);
            ImGui::Separator();
            drawCards();
            ImGui::Separator();
            drawVoicing();
            drawProgressionText();
            ImGui::Spacing();
            snaps.drawBar(ImGui::GetContentRegionAvail().x);
            ai.drawPopup();
            ImGui::PopID();
        }
        ImGui::End();
        if(!open) showWindow = false;
    }

    void drawHeader(){
        using namespace harmonybox;
        if(ImGui::Button("Generate")) generateNow(true);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("New chords for every unlocked card.\nSeed 0: fresh each time; Seed > 0: the next seed number.");
        ImGui::SameLine();
        ai.drawButton(ImVec2(34, 0));
        ImGui::SameLine();
        combo("Style", styleP, styleNames(), 120);
        ImGui::SameLine();
        combo("Key", keyP, keyNames(), 56);
        ImGui::SameLine();
        combo("Mode", modeP, modeNames(), 130);
        ImGui::SameLine();
        sliderInt("Chords", numChordsP, 90);
        ImGui::SameLine();
        {
            int v = seedP.get();
            ImGui::SetNextItemWidth(80);
            if(ImGui::DragInt("Seed", &v, 0.2f, 0, seedP.getMax())) seedP = std::clamp(v, 0, seedP.getMax());
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("0: fresh draws. > 0: one progression per number (with these settings).");
            paramMenu(seedP.getEscapedName());
        }
        ImGui::SameLine();
        sliderInt("Transpose", transposeP, 80);

        slider("Color", colorP, 120, "How far from the key: secondary dominants, borrowed chords,\nsubstitutions, chromatic moves (applies on Generate).");
        ImGui::SameLine();
        slider("Extensions", extensionsP, 120, "Triads .. sevenths .. ninths .. thirteenths (applies on Generate).");
        ImGui::SameLine();
        slider("Tension Level", tensionP, 120, "The average tension the progression aims for (applies on Generate).");
        ImGui::SameLine();
        combo("Shape", shapeP, shapeNames(), 80);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("How tension moves across the bank: Free, Flat, Rise, Fall, Arc, Wave.");
        if(timeline.hasTimeline()){
            ImGui::SameLine();
            if(ImGui::Button("Chord Track")) timeline.addChordTrack(*chordGatesHandle, getParameterGroup().getName());
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("A timeline track to write the chords on: one row per chord,\n"
                                                         "drag in a row to hold that chord (Chord Gates).");
        }
        if(!ai.getStatus().empty()){ ImGui::SameLine(); ImGui::TextDisabled("%s", ai.getStatus().c_str()); }
    }

    // Tension across the bank: each chord's (dots, the playing one ringed), the
    // level asked for (dashed) and the Shape it aims at (faint)
    void drawTensionCurve(float w, float h){
        using namespace harmonybox;
        const int n = engine.size();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 p1(p0.x + w, p0.y + h);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
        const float padX = 12.0f, padY = 6.0f;
        auto X = [&](float i){ return n <= 1 ? (p0.x + p1.x) * 0.5f : p0.x + padX + (w - 2 * padX) * i / (float)(n - 1); };
        auto Y = [&](float t){ return p1.y - padY - (h - 2 * padY) * std::clamp(t, 0.0f, 1.0f); };
        const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        // the level, dashed
        const float ly = Y(engine.s.tension);
        for(float x = p0.x + padX; x < p1.x - padX; x += 10.0f) dl->AddLine(ImVec2(x, ly), ImVec2(std::min(x + 5.0f, p1.x - padX), ly), dim);
        // the shape, around the level
        if(engine.s.shape != SHAPE_FREE && engine.s.shape != SHAPE_FLAT && n > 1){
            std::vector<ImVec2> pts;
            for(int k = 0; k <= 40; k++){
                const float x = (float)k / 40.0f;
                pts.push_back(ImVec2(X(x * (n - 1)), Y(engine.s.tension + 0.5f * (shapeAt(engine.s.shape, x) - 0.5f))));
            }
            dl->AddPolyline(pts.data(), (int)pts.size(), ImGui::GetColorU32(ImGuiCol_PlotLines, 0.35f), 0, 1.5f);
        }
        // the chords
        const ImU32 line = ImGui::GetColorU32(ImGuiCol_PlotLines);
        for(int i = 0; i + 1 < n; i++) dl->AddLine(ImVec2(X((float)i), Y(engine.tension(i))), ImVec2(X((float)i + 1), Y(engine.tension(i + 1))), line, 1.5f);
        const int current = currentSlot();
        for(int i = 0; i < n; i++){
            const ImVec2 c(X((float)i), Y(engine.tension(i)));
            dl->AddCircleFilled(c, 3.5f, ImGui::GetColorU32(ImGuiCol_PlotHistogram));
            if(i == current) dl->AddCircle(c, 7.0f, ImGui::GetColorU32(ImGuiCol_Text), 0, 1.5f);
        }
        ImGui::InvisibleButton("##tension", ImVec2(w, h));
        if(ImGui::IsItemHovered() && n > 0){
            const float mx = ImGui::GetIO().MousePos.x;
            int best = 0;
            for(int i = 1; i < n; i++) if(std::abs(X((float)i) - mx) < std::abs(X((float)best) - mx)) best = i;
            ImGui::SetTooltip("%d  %s  tension %.2f\nTension Level %.2f, Shape %s", best + 1, engine.name(best).c_str(),
                              engine.tension(best), engine.s.tension, shapeNames()[engine.s.shape].c_str());
        }
    }

    // The bank: one card per chord, wrapping
    void drawCards(){
        using namespace harmonybox;
        const int n = engine.size();
        const float avail = ImGui::GetContentRegionAvail().x;
        const float gap = 8.0f;
        const int cols = std::clamp((int)((avail + gap) / (150.0f + gap)), 1, std::min(n, 8));
        const float w = std::floor((avail - gap * (cols - 1)) / cols);
        const float h = 150.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const int rows = (n + cols - 1) / cols;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const int current = currentSlot();
        const ImU32 cardBg = ImGui::GetColorU32(ImGuiCol_FrameBg);
        const ImU32 accent = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
        const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
        const ImU32 dimCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        int moveFrom = -1, moveTo = -1;
        for(int i = 0; i < n; i++){
            ImGui::PushID(i);
            const ImVec2 p0(origin.x + (i % cols) * (w + gap), origin.y + (i / cols) * (h + gap));
            const ImVec2 p1(p0.x + w, p0.y + h);
            const bool locked = engine.slots[i].locked;
            dl->AddRectFilled(p0, p1, cardBg, 6.0f);
            if(i == current) dl->AddRect(p0, p1, accent, 6.0f, 0, 2.5f);
            else if(locked) dl->AddRect(p0, p1, dimCol, 6.0f, 0, 1.0f);

            // the card itself: held plays it (Gate on), double-click edits, drag reorders
            ImGui::SetCursorScreenPos(p0);
            ImGui::InvisibleButton("card", ImVec2(w, h - 26.0f));
            if(ImGui::IsItemActivated()){ padSlot = i; updateOutputs(); }
            if(ImGui::IsItemDeactivated() && padSlot == i){ padSlot = -1; updateOutputs(); }
            if(ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) startEdit(i);
            if(ImGui::IsItemHovered() && !ImGui::IsMouseDragging(0))
                ImGui::SetTooltip("%s (Index %d)\nhold: play  double-click: type a chord  drag: move\nright-click: more", engine.name(i).c_str(), i + 1);
            if(ImGui::BeginDragDropSource()){
                ImGui::SetDragDropPayload("HB_SLOT", &i, sizeof(int));
                ImGui::Text("%s", engine.name(i).c_str());
                ImGui::EndDragDropSource();
            }
            if(ImGui::BeginDragDropTarget()){
                if(const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("HB_SLOT")){ moveFrom = *(const int*)pl->Data; moveTo = i; }
                ImGui::EndDragDropTarget();
            }
            if(ImGui::BeginPopupContextItem("cardMenu")){
                if(ImGui::MenuItem("Type a chord...")) startEdit(i);
                if(ImGui::MenuItem(locked ? "Unlock" : "Lock")) engine.slots[i].locked = !locked;
                if(ImGui::MenuItem("New chord here")) engine.regenerateSlot(i, drawSeed());
                if(ImGui::MenuItem("Duplicate", nullptr, false, n < MAX_CHORDS)) engine.duplicate(i);
                if(ImGui::MenuItem("Delete", nullptr, false, n > 1)) engine.erase(i);
                ImGui::EndPopup();
            }

            // index, numeral
            dl->AddText(ImVec2(p0.x + 6, p0.y + 4), dimCol, std::to_string(i + 1).c_str());
            const std::string rn = engine.romanAt(i);
            const ImVec2 rs = ImGui::CalcTextSize(rn.c_str());
            dl->AddText(ImVec2(p1.x - rs.x - 6, p0.y + 4), dimCol, rn.c_str());

            // the name, large (or its editor)
            if(editingSlot == i){
                ImGui::SetCursorScreenPos(ImVec2(p0.x + 6, p0.y + 26));
                ImGui::SetNextItemWidth(w - 12);
                if(focusEdit){ ImGui::SetKeyboardFocusHere(); focusEdit = false; }
                const bool done = ImGui::InputText("##edit", editBuffer, sizeof(editBuffer),
                                                   ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                if(done){
                    if(engine.setChordText(i, editBuffer)){ message.clear(); editingSlot = -1; }
                    else message = std::string("Not a chord symbol: ") + editBuffer;
                }else if(!ImGui::IsItemActive() && !ImGui::IsItemFocused()) editingSlot = -1;
            }else{
                ImFont* font = ImGui::GetFont();
                const float size = ImGui::GetFontSize() * 1.7f;
                const std::string name = engine.name(i);
                const ImVec2 ns = font->CalcTextSizeA(size, FLT_MAX, 0.0f, name.c_str());
                const float x = p0.x + std::max(6.0f, (w - ns.x) * 0.5f);
                dl->PushClipRect(p0, p1, true);
                dl->AddText(font, size, ImVec2(x, p0.y + 24), textCol, name.c_str());
                dl->PopClipRect();
            }

            // tension bar
            {
                const float t = engine.tension(i);
                const float y = p0.y + 62;
                dl->AddRectFilled(ImVec2(p0.x + 8, y), ImVec2(p1.x - 8, y + 4), ImGui::GetColorU32(ImGuiCol_FrameBgHovered), 2.0f);
                const ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(0.35f + 0.6f * t, 0.75f - 0.4f * t, 0.7f - 0.5f * t, 1.0f));
                dl->AddRectFilled(ImVec2(p0.x + 8, y), ImVec2(p0.x + 8 + (w - 16) * t, y + 4), col, 2.0f);
            }

            // the voicing on a small keyboard
            drawMiniKeys(dl, ImVec2(p0.x + 8, p0.y + 72), ImVec2(w - 16, 44), i);

            // lock / new
            ImGui::SetCursorScreenPos(ImVec2(p0.x + 6, p1.y - 24));
            if(locked) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if(ImGui::SmallButton(locked ? "Locked" : "Lock")) engine.slots[i].locked = !locked;
            if(locked) ImGui::PopStyleColor();
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("A locked chord stays when you Generate.");
            ImGui::SameLine();
            if(ImGui::SmallButton("New")) engine.regenerateSlot(i, drawSeed());
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Another chord here, chosen to fit its neighbours.");
            ImGui::PopID();
        }
        if(moveFrom >= 0 && moveTo >= 0) engine.move(moveFrom, moveTo);
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rows * (h + gap)));
        ImGui::Dummy(ImVec2(avail, 0.0f));
        if(!message.empty()) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", message.c_str());
    }

    void startEdit(int i){
        editingSlot = i;
        focusEdit = true;
        std::snprintf(editBuffer, sizeof(editBuffer), "%s", engine.name(i).c_str());
    }

    void drawMiniKeys(ImDrawList* dl, ImVec2 p, ImVec2 size, int slot){
        static const bool black[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
        const auto pitch = engine.pitch(slot);
        const int bassNote = (int)engine.bass(slot);
        int lo = engine.s.low, hi = engine.s.high;
        for(float v : pitch){ lo = std::min(lo, (int)v); hi = std::max(hi, (int)v); }
        lo -= harmonybox::pc(lo);
        hi += 11 - harmonybox::pc(hi);
        int whites = 0;
        for(int m = lo; m <= hi; m++) if(!black[harmonybox::pc(m)]) whites++;
        const float kw = size.x / std::max(1, whites);
        auto on = [&](int m){ for(float v : pitch) if((int)std::lround(v) == m) return true; return false; };
        const ImU32 white = IM_COL32(225, 225, 225, 255), blackCol = IM_COL32(40, 40, 40, 255);
        const ImU32 noteCol = ImGui::GetColorU32(ImGuiCol_PlotHistogram), bassCol = IM_COL32(90, 170, 240, 255);
        int wi = 0;
        for(int m = lo; m <= hi; m++){
            if(black[harmonybox::pc(m)]) continue;
            const ImVec2 a(p.x + wi * kw, p.y), b(p.x + (wi + 1) * kw - 1, p.y + size.y);
            dl->AddRectFilled(a, b, on(m) ? (m == bassNote && engine.s.bass ? bassCol : noteCol) : white);
            wi++;
        }
        wi = 0;
        for(int m = lo; m <= hi; m++){
            if(black[harmonybox::pc(m)]){
                const float x = p.x + wi * kw - kw * 0.32f;
                dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + kw * 0.64f, p.y + size.y * 0.6f),
                                  on(m) ? (m == bassNote && engine.s.bass ? bassCol : noteCol) : blackCol);
            }else wi++;
        }
    }

    void drawVoicing(){
        using namespace harmonybox;
        ImGui::TextDisabled("Voicing");
        ImGui::SameLine();
        sliderInt("Voices", voicesP, 80);
        ImGui::SameLine();
        sliderInt("##low", lowP, 90, noteName(lowP.get()).c_str());
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Lowest note (the bass lives in its first octave)");
        ImGui::SameLine();
        sliderInt("Range##high", highP, 90, noteName(highP.get()).c_str());
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Highest note");
        ImGui::SameLine();
        combo("Spread", spreadP, spreadNames(), 90);
        ImGui::SameLine();
        checkbox("Bass", bassP, "A separate bass voice (the root or slash note) under the chord.");
        ImGui::SameLine();
        checkbox("Voice leading", leadP, "Each chord moves as little as it can from the one before (around the loop).");
        ImGui::SameLine();
        slider("Glide", glideP, 80, "Pitch AR glides to each new chord over this time.", "%.2f s");
    }

    // The bank as text: copy it, paste one, type one
    void drawProgressionText(){
        if(progressionRevision != engine.revision && !ImGui::IsAnyItemActive()){
            progressionBuffer = engine.progressionText();
            progressionRevision = engine.revision;
        }
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s", progressionBuffer.c_str());
        ImGui::SetNextItemWidth(std::max(200.0f, ImGui::GetContentRegionAvail().x - 120.0f));
        if(ImGui::InputText("##progression", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)){
            progressionBuffer = buf;
            applyProgressionText();
        }else progressionBuffer = buf;
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("The whole bank as text: edit and press Enter, or paste chords\n(\"Am7 Fmaj7 | Dm9 E7b9\", an optional \"Key: A minor\" line).");
        ImGui::SameLine();
        if(ImGui::Button("Apply text")) applyProgressionText();
    }
    void applyProgressionText(){
        int key = -1, mode = -1;
        const auto chords = harmonybox::parseProgression(progressionBuffer, &key, &mode);
        if(chords.empty()){ message = "No chord symbols in the text"; return; }
        message.clear();
        if(key >= 0) engine.s.key = key;
        if(mode >= 0) engine.s.mode = mode;
        engine.setChords(chords);
        pullSettings();
    }
};
