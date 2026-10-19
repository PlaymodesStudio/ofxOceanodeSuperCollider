//
//  scArpeggiatorBox.h
//  ofxOceanodeSuperCollider
//
//  ArpeggiatorBox: the Polyphonic Arpeggiator GUI (ofxSantiNodes), every
//  feature, played sample accurately by SuperCollider.
//
//  • The music (scale or chord pool, patterns, polyphony, bass, accents,
//    durations, velocity, strum / swing, chances, deviations, pulses) is
//    decided in C++ by arpbox::Engine (scArpeggiatorBoxEngine.h), a pure
//    function of each pulse: the node writes the notes about to play into a
//    buffer and ArpeggiatorBoxPlayer (synthdefs/Defaults/ArpeggiatorBox)
//    plays them, so strums and swing land on the sample and note lengths
//    follow tempo changes.
//  • Clock: Transport (the global transport, steps of Beat Div), Internal
//    (its own clock at Clock BPM, runs while the transport is stopped) or
//    Trigger (each Trigger bang, rising Trig In value or rising edge of an
//    audio cable into Trig In steps it; lengths and strums scale with the
//    time between triggers).
//  • Outputs: Trig, Gate (x the note's velocity), Pitch (16 channels written:
//    the voices, then the bass, as in the vectors); Bass Trig / Gate / Pitch
//    (4 written: the bass alone). The voices follow the music: Voice
//    Mode Fixed, chord tone n on channel n (Polyphony + Extra on accents
//    channels) and one per stacked bass octave; Round robin, as many more as
//    notes overlap (longer than a step: each chord on the next voices). The GUI node's vectors
//    (PitchOut, GateOut, VelOut, DurOut, GateVelOut: voices then bass;
//    Total Voices: their size; EucGateOut / EucAccOut / EucDurOut: the
//    cycle's lanes) at frame rate.
//  • Seed: every free random draw of the GUI node (chances, deviations, run
//    gate, Geiger...) is seeded by it: the same position always plays the
//    same, a seek or a loop replays it; 0: fresh draws on each pass.
//  • Snapshots (shared by every ArpeggiatorBox, 128) with Morph Time; the
//    GUI node's snapshots can be imported.
//  • Window: a scrolling piano roll, the cycle's ring, tabs in columns.
//    Right-click any control: type a value, or publish it on the node.
//

#pragma once

#include "ofxOceanodeSuperColliderConfig.h"

#if OFXOCEANODESC_HAS_TRANSPORT

#include "scNode.h"
#include "scTimelineMenu.h"
#include "scAudioParam.h"
#include "scTransportSync.h"
#include "scArpeggiatorBoxEngine.h"
#include "scChordLink.h"
#include "ofxSCSynth.h"
#include "ofxSCBuffer.h"
#include "ofxSCBus.h"
#include "ofxSCServer.h"
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

class scArpeggiatorBox : public scNode {
public:
    // its window (SC Mixer Pro's "Go to origin synth window" opens and focuses it)
    std::string getWindowTitle() override { return (canvasID == "Canvas" ? "" : canvasID + "/") + "ArpeggiatorBox " + ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return &showWindow; }
private:
public:
    static constexpr int TPB = 48;                    // ticks per beat (the player's)
    static constexpr int CHUNK = 8 * TPB;             // ticks per written chunk
    static constexpr int SLOTS = 4;                   // chunks held
    static constexpr int CLOCKED = SLOTS * CHUNK;     // buffer frames of the clocked modes
    static constexpr int ROWS = 256;                  // trigger mode frames (a ring)
    static constexpr int AHEAD = 96;                  // trigger rows written ahead
    static constexpr int NV = 16, NB = 4, NSLOT = NV + NB, CH = 4, BUF_CH = NSLOT * CH;
    static constexpr int64_t FREE_WRAP = (int64_t)CLOCKED * 256;
    static constexpr int64_t NONE = INT64_MIN;
    static constexpr int OUTPUTS = 6;
    static constexpr int NUM_SNAPS = 128;
    enum Clock : int { CLOCK_TRIGGER = 0, CLOCK_TRANSPORT, CLOCK_INTERNAL };

    scArpeggiatorBox() : scNode("ArpeggiatorBox") {}

    ~scArpeggiatorBox() override {
        listeners.unsubscribeAll();
        for(auto& [server, st] : servers){
            if(st.synth){ st.synth->free(); delete st.synth; }
            if(st.buffer){ if(bufferLive(server, st)) st.buffer->free(); delete st.buffer; }
            if(st.status){ if(busLive(server, st.status)) st.status->free(); delete st.status; }
        }
        servers.clear();
    }

    // ════════════════════════════════════════════════════════════════════════
    // Setup
    // ════════════════════════════════════════════════════════════════════════

    void setup() override {
        timeline.setup(this, [this](const std::string& k){ publish(k); }, [this](const std::string& k){ unpublish(k); },
                       [this](){ return publishedKeys; });
        description = "Polyphonic arpeggiator played sample accurately: a scale or a chord pool, patterns, polyphony, "
                      "bass, euclidean / random / step accents and long notes, strum, swing, chances and seeded "
                      "randomness. Clock: the transport, its own clock or triggers (a bang or an audio cable). "
                      "Trig, Gate (x velocity) and Pitch outputs per voice, and for the bass. Show opens the piano "
                      "roll and every control; right-click a control there to type a value or publish it here.";
        addParameter(showWindow.set("Show", false));
        addParameter(clockMode.set("Clock", CLOCK_TRANSPORT, 0, 2))->setDropdownOptions({"Trigger", "Transport", "Internal"});
        addParameter(trigger.set("Trigger"));
        trigIn.attach(addParameter(trigValue.set("Trig In", 0.0f, 0.0f, 1.0f)), availableInputs, [this](){ refreshOutputs(); });
        addParameter(resetP.set("Reset"));
        addParameter(resetNextP.set("Reset Next"));
        addParameter(notePool.set("NotePool", {60.0f, 64.0f, 67.0f}, {0.0f}, {127.0f}));
        addParameter(scale.set("Scale", {0, 2, 4, 5, 7, 9, 11}, {-24.0f}, {127.0f}));
        addParameter(root.set("Root", 0.0f, -24.0f, 127.0f));
        addParameter(seed.set("Seed", 1, 0, 99999));
        addParameter(snapshotRecall.set("Snapshot", 0, 0, NUM_SNAPS));
        for(const char* n : {"Trig", "Gate", "Pitch", "Bass Trig", "Bass Gate", "Bass Pitch"}) scNode::addOutput(n);
        addOutputParameter(pitchOut.set("PitchOut", std::vector<float>(4, 60.0f), {0.0f}, {127.0f}));
        addOutputParameter(gateOut.set("GateOut", std::vector<int>(4, 0), {0}, {1}));
        addOutputParameter(velOut.set("VelOut", std::vector<float>(4, 0.0f), {0.0f}, {1.0f}));
        addOutputParameter(durOut.set("DurOut", std::vector<float>(4, 0.0f), {0.0f}, {60000.0f}));
        addOutputParameter(gateVelOut.set("GateVelOut", std::vector<float>(4, 0.0f), {0.0f}, {1.0f}));
        // the vectors' size (voices then bass): a synth's voice count follows it
        addOutputParameter(totalVoicesOut.set("Total Voices", 3, 1, NV + NB));
        addOutputParameter(eucGateOut.set("EucGateOut", std::vector<int>(16, 0), {0}, {1}));
        addOutputParameter(eucAccOut.set("EucAccOut", std::vector<int>(16, 0), {0}, {1}));
        addOutputParameter(eucDurOut.set("EucDurOut", std::vector<int>(16, 0), {0}, {1}));

        defineControls();
        newFresh();
        syncPatternSizes();

        listeners.push(trigger.newListener([this](){
            if(clockMode.get() != CLOCK_TRIGGER) return;
            for(auto& [server, st] : servers) if(st.synth) st.synth->set("t_bang", 1.0f);
            noteBang();
        }));
        listeners.push(trigValue.newListener([this](float& v){
            for(auto& [server, st] : servers) if(st.synth && trigIn.bus(server) < 0) st.synth->set("trig", (double)v);
            if(v > 0.0f && lastTrigValue <= 0.0f && clockMode.get() == CLOCK_TRIGGER && !trigIn.cabled()) noteBang();
            lastTrigValue = v;
        }));
        listeners.push(resetP.newListener([this](){ doReset(true); }));
        listeners.push(resetNextP.newListener([this](){ doReset(false); }));
        listeners.push(notePool.newListener([this](std::vector<float>&){ sourceDirty = true; }));
        listeners.push(scale.newListener([this](std::vector<float>&){ sourceDirty = true; }));
        // Root: unless a ChordBox's Root is linked (read per beat instead)
        listeners.push(root.newListener([this](float& v){
            if(v == lastRootValue) return;
            lastRootValue = v;
            if(!engine.linkedRoot || clockMode.get() == CLOCK_TRIGGER) paramsDirty = true;
        }));
        watch(seed, false);
        listeners.push(clockMode.newListener([this](int& m){
            if(m == lastClockMode) return;
            lastClockMode = m;
            paramsDirty = true;
            if(clockMode.get() == CLOCK_INTERNAL) resetFreeClock();
            if(clockMode.get() == CLOCK_TRIGGER) resetTriggerCount();
            for(auto& [server, st] : servers) sendStaticControls(st);
            follower.requestAnchor();
        }));
        // a recall command (as the GUI node's: back to 0, so a preset does not recall on load)
        listeners.push(snapshotRecall.newListener([this](int& v){
            if(v <= 0) return;
            recallSnapshot(v - 1);
            snapshotRecall.setWithoutEventNotifications(0);
        }));
        listeners.push(resendParams.newListener([this](){
            for(auto& [server, st] : servers) sendStaticControls(st);
            follower.requestAnchor();
        }));

        engine.resetSource(currentSource());
        syncEngine();
        defaultValues = captureValues();
        loadSnapshotFile();
        if(!ofFile(snapshotPath()).exists()) addDemoSnapshots();
    }

    // ════════════════════════════════════════════════════════════════════════
    // Frame
    // ════════════════════════════════════════════════════════════════════════

    void update(ofEventArgs&) override {
        timeline.update();
        const float now = ofGetElapsedTimef();
        for(auto& [server, st] : servers){
            if(st.allocPending && (!server->isNRTSyncPending() || now - st.allocTime > 2.0f)){
                st.allocPending = false;
                invalidateServer(st);
            }
        }
        if(snapshotRevision() != loadedSnapshotRevision) loadSnapshotFile();
        updateMorph();
        watchTransport();
        learnChain();
        updateLinks();
        if(paramsDirty){ paramsDirty = false; syncEngine(); }
        if(sourceDirty){ sourceDirty = false; applySourceChange(); }
        readStatus();
        nowPulse = findPulseNow();
        fillBuffers();
        sendTransportAnchors();
        updateVectors();
    }

    void draw(ofEventArgs&) override { if(showWindow) drawWindow(); }

    // ════════════════════════════════════════════════════════════════════════
    // scNode
    // ════════════════════════════════════════════════════════════════════════

    void buildSynth(ofxSCServer* server) override {
        if(!server) return;
        ServerState& st = servers[server];
        st.server = server;
        if(st.synth){ st.synth->free(); delete st.synth; }
        st.synth = new ofxSCSynth("ArpeggiatorBoxPlayer", server);
        if(!bufferLive(server, st)){
            delete st.buffer;
            st.buffer = new ofxSCBuffer(CLOCKED + ROWS, BUF_CH, server);
            st.buffer->alloc();
            st.allocPending = true;
            st.allocTime = ofGetElapsedTimef();
            server->requestNRTSync();
            invalidateServer(st);
        }
        if(!busLive(server, st.status)){
            delete st.status;
            st.status = new ofxSCBus(RATE_CONTROL, 2, server);
            if(!busLive(server, st.status)){ st.status->free(); delete st.status; st.status = nullptr; }
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
        // a new synth: its clocks start again
        if(clockMode.get() == CLOCK_INTERNAL) resetFreeClock();
        if(clockMode.get() == CLOCK_TRIGGER) resetTriggerCount();
    }
    void moveSynthBefore(ofxSCServer* server, int nodeID) override {
        auto it = servers.find(server);
        if(it == servers.end() || !it->second.synth) return;
        sendStaticControls(it->second);
        it->second.synth->moveBefore(nodeID);
    }
    void free(ofxSCServer* server) override {
        auto it = servers.find(server);
        if(it == servers.end()) return;
        if(it->second.synth){ it->second.synth->free(); delete it->second.synth; it->second.synth = nullptr; }
        it->second.outputBuses.clear();
    }
    void setOutputBus(ofxSCServer* server, int index, int bus) override {
        if(!server || index < 0 || index >= OUTPUTS) return;
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
    void setInputBus(ofxSCServer* server, scNode* node, int bus) override {
        if(trigIn.take(server, node, bus)){ auto& st = servers[server]; st.server = server; sendTrigInput(st); }
    }
    void resetInputBusses(ofxSCServer* server, int) override {
        trigIn.reset(server);
        auto& st = servers[server];
        st.server = server;
        sendTrigInput(st);
    }
    void activate() override {
        for(auto& [server, st] : servers) if(st.synth) st.synth->run(true);
        forceHardAnchor = true;
        follower.requestAnchor();
    }
    void deactivate() override { for(auto& [server, st] : servers) if(st.synth) st.synth->run(false); }
    // The recorder writes the patch's state at score time zero: the note
    // buffer too (written while it was arming, it never reached the score)
    void resendParametersForNRT() override {
        resendParams.notify();
        for(auto& [server, st] : servers) invalidateServer(st);
        fillBuffers();
    }

    // ════════════════════════════════════════════════════════════════════════
    // Presets
    // ════════════════════════════════════════════════════════════════════════

    void presetSave(ofJson& json) override {
        auto& c = json["ArpeggiatorBox"] = ofJson::object();
        for(const auto& ctl : controls) if(!ctl.fixed && !published.count(ctl.key)) c[ctl.key] = ctl.save();
        json["ArpeggiatorBoxPublished"] = publishedKeys;
        timeline.save(json, "ArpeggiatorBoxTimeline");
        json["ArpeggiatorBoxTab"] = selectedTab;
    }
    void loadBeforeConnections(ofJson& json) override {
        if(!json.contains("ArpeggiatorBoxPublished") || !json["ArpeggiatorBoxPublished"].is_array()) return;
        std::vector<std::string> wanted;
        for(const auto& k : json["ArpeggiatorBoxPublished"]) if(k.is_string()) wanted.push_back(k.get<std::string>());
        const auto old = publishedKeys;
        for(const auto& k : old)
            if(std::find(wanted.begin(), wanted.end(), k) == wanted.end() && !hasConnection(k)) unpublish(k);
        for(const auto& k : wanted) publish(k);
            timeline.load(json, "ArpeggiatorBoxTimeline");
}
    void presetRecallBeforeSettingParameters(ofJson& json) override { loadBeforeConnections(json); }
    void presetRecallAfterSettingParameters(ofJson& json) override {
        if(json.contains("ArpeggiatorBox") && json["ArpeggiatorBox"].is_object()){
            const auto& c = json["ArpeggiatorBox"];
            for(const auto& ctl : controls) if(!ctl.fixed && c.contains(ctl.key)) ctl.load(c[ctl.key]);
        }
        if(json.contains("ArpeggiatorBoxTab") && json["ArpeggiatorBoxTab"].is_number_integer()) selectedTab = json["ArpeggiatorBoxTab"].get<int>();
        syncPatternSizes();
        engine.origin = 0;
        engine.armed = false;
        engine.resetSource(currentSource());
        newSalt();
        syncEngine();
        for(auto& [server, st] : servers) sendStaticControls(st);
    }

private:
    // ════════════════════════════════════════════════════════════════════════
    // Controls: every setting, by the GUI node's snapshot key
    // ════════════════════════════════════════════════════════════════════════

    struct Control {
        std::string key;
        bool fixed = false;        // always on the node (not published / unpublished)
        bool source = false;       // changes the source material (Src Change applies)
        std::function<std::shared_ptr<ofxOceanodeAbstractParameter>()> add;
        std::function<std::string()> paramName;
        std::function<ofJson()> save;
        std::function<void(const ofJson&)> load;
        std::function<std::string()> text;
        std::function<void(const std::string&)> apply;
        std::function<double()> get;           // numeric (morph); null: switched at the end
        std::function<void(double)> set;
    };
    std::vector<Control> controls;
    std::map<std::string, std::shared_ptr<ofxOceanodeAbstractParameter>> published;
    std::vector<std::string> publishedKeys;
    scTimelineMenu timeline;              // "Add to Timeline" from the window
    bool publishedSeparator = false;

    // a change (connections may send the same value again)
    template<typename T>
    void watch(ofParameter<T>& p, bool source){
        auto last = std::make_shared<T>(p.get());
        listeners.push(p.newListener([this, source, last](T& v){
            if(v == *last) return;
            *last = v;
            if(source) sourceDirty = true; else paramsDirty = true;
        }));
    }
    Control& ctlI(const std::string& key, ofParameter<int>& p, const std::vector<std::string>& dropdown = {}, bool source = false){
        Control c;
        c.key = key;
        c.source = source;
        c.add = [this, &p, dropdown](){ auto h = addParameter(p); if(!dropdown.empty()) h->setDropdownOptions(dropdown); return h; };
        c.paramName = [&p](){ return p.getEscapedName(); };
        c.save = [&p](){ return ofJson(p.get()); };
        c.load = [&p](const ofJson& j){ try { if(j.is_number()) p.set(std::clamp((int)std::lround(j.get<double>()), p.getMin(), p.getMax())); else if(j.is_boolean()) p.set(j.get<bool>() ? 1 : 0); } catch(...) {} };
        c.text = [&p](){ return ofToString(p.get()); };
        c.apply = [&p](const std::string& t){ p.set(std::clamp((int)std::lround(ofToFloat(t)), p.getMin(), p.getMax())); };
        if(dropdown.empty()){
            c.get = [&p](){ return (double)p.get(); };
            c.set = [&p](double v){ const int iv = std::clamp((int)std::lround(v), p.getMin(), p.getMax()); if(iv != p.get()) p.set(iv); };
        }
        watch(p, source);
        controls.push_back(std::move(c));
        return controls.back();
    }
    Control& ctlF(const std::string& key, ofParameter<float>& p, bool source = false){
        Control c;
        c.key = key;
        c.source = source;
        c.add = [this, &p](){ return addParameter(p); };
        c.paramName = [&p](){ return p.getEscapedName(); };
        c.save = [&p](){ return ofJson(p.get()); };
        c.load = [&p](const ofJson& j){ try { if(j.is_number()) p.set(ofClamp(j.get<float>(), p.getMin(), p.getMax())); } catch(...) {} };
        c.text = [&p](){ return ofToString(p.get(), 3); };
        c.apply = [&p](const std::string& t){ p.set(ofClamp(ofToFloat(t), p.getMin(), p.getMax())); };
        c.get = [&p](){ return (double)p.get(); };
        c.set = [&p](double v){ const float fv = ofClamp((float)v, p.getMin(), p.getMax()); if(fv != p.get()) p.set(fv); };
        watch(p, source);
        controls.push_back(std::move(c));
        return controls.back();
    }
    Control& ctlB(const std::string& key, ofParameter<bool>& p, bool source = false){
        Control c;
        c.key = key;
        c.source = source;
        c.add = [this, &p](){ return addParameter(p); };
        c.paramName = [&p](){ return p.getEscapedName(); };
        c.save = [&p](){ return ofJson(p.get()); };
        c.load = [&p](const ofJson& j){ try { p.set(j.is_boolean() ? j.get<bool>() : j.get<double>() > 0.5); } catch(...) {} };
        c.text = [&p](){ return std::string(p.get() ? "1" : "0"); };
        c.apply = [&p](const std::string& t){ const std::string l = ofToLower(ofTrim(t)); p.set(l == "1" || l == "on" || l == "true" || l == "yes"); };
        watch(p, source);
        controls.push_back(std::move(c));
        return controls.back();
    }
    template<typename T>
    Control& ctlV(const std::string& key, ofParameter<std::vector<T>>& p, bool source = false){
        Control c;
        c.key = key;
        c.source = source;
        c.add = [this, &p](){ return addParameter(p); };
        c.paramName = [&p](){ return p.getEscapedName(); };
        c.save = [&p](){ return ofJson(p.get()); };
        c.load = [&p](const ofJson& j){
            if(!j.is_array()) return;
            std::vector<T> v;
            for(const auto& x : j) if(x.is_number()) v.push_back((T)x.get<double>());
            p.set(v);
        };
        c.text = [&p](){ std::string s; for(size_t i = 0; i < p.get().size(); i++) s += (i ? " " : "") + ofToString(p.get()[i]); return s; };
        c.apply = [&p](const std::string& t){
            std::vector<T> v;
            for(const auto& w : ofSplitString(t, " ", true, true)) v.push_back((T)ofToFloat(w));
            if(!v.empty()) p.set(v);
        };
        watch(p, source);
        controls.push_back(std::move(c));
        return controls.back();
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
        if(!c || c->fixed) return;
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
    // Right-click: type a value, publish it
    void controlMenu(const std::string& key){
        const Control* c = findControl(key);
        if(!c) return;
        timeline.mark(key);
        if((published.count(key) && !timeline.isHidden(key)) || c->fixed){
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y - 1), ImVec2(b.x, b.y - 1), ImGui::GetColorU32(ImGuiCol_Text, c->fixed ? 0.35f : 1.0f));
        }
        if(!ImGui::BeginPopupContextItem(("##abPublish" + key).c_str())) return;
        ImGui::TextDisabled("%s", c->paramName().c_str());
        ImGui::Separator();
        static std::map<std::string, std::array<char, 512>> buffers;
        auto& buf = buffers[key];
        const bool appearing = ImGui::IsWindowAppearing();
        if(appearing) std::snprintf(buf.data(), buf.size(), "%s", c->text().c_str());
        ImGui::TextDisabled("Value");
        ImGui::SetNextItemWidth(220.0f);
        if(appearing) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##abTyped", buf.data(), buf.size(),
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if(enter || ImGui::IsItemDeactivatedAfterEdit()){ c->apply(std::string(buf.data())); if(enter) ImGui::CloseCurrentPopup(); }
        ImGui::Separator();
        if(c->fixed) ImGui::TextDisabled("Always on the node");
        else if(timeline.isHidden(key)){
            if(ImGui::MenuItem("Publish to Node GUI")) timeline.reveal(key);
        }else if(published.count(key)){
            if(hasConnection(key)) ImGui::TextDisabled("Disconnect it to unpublish");
            else if(ImGui::MenuItem("Unpublish from Node")) timeline.unpublish(key);
        }else if(ImGui::MenuItem("Publish to Node GUI")) publish(key);
        timeline.menuItems(key);
        ImGui::EndPopup();
    }

    static const std::vector<std::string>& pulseNames(){ static const std::vector<std::string> n = {"Periodic", "Euclidean", "Geiger", "Step seq", "Bouncing ball"}; return n; }
    static const std::vector<std::string>& eventNames(){ static const std::vector<std::string> n = {"Alternate", "Euclidean", "Random step", "Random note", "Step seq"}; return n; }
    static const std::vector<std::string>& bassNames(){ static const std::vector<std::string> n = {"Alternate", "Euclidean", "Random", "On accents", "On long notes", "Cycle start"}; return n; }
    static const std::vector<std::string>& patternNames(){ static const std::vector<std::string> n = {"Ascending", "Descending", "Random", "User", "Asc + Desc", "Desc + Asc"}; return n; }
    static const std::vector<std::string>& lfoNames(){ static const std::vector<std::string> n = {"Sine", "Saw", "Inverse saw", "Pulse", "Slow random"}; return n; }
    static const std::vector<std::string>& strumNames(){ static const std::vector<std::string> n = {"Up", "Down", "Random"}; return n; }
    static const std::vector<std::string>& clockNames(){ static const std::vector<std::string> n = {"Trigger", "Transport", "Internal"}; return n; }
    static const std::vector<std::string>& changeNames(){ static const std::vector<std::string> n = {"Keep phase", "Reset pattern", "Next cycle"}; return n; }
    static const std::vector<std::string>& voiceModeNames(){ static const std::vector<std::string> n = {"Fixed", "Round robin"}; return n; }

    void defineControls(){
        using namespace arpbox;
        // node parameters (in snapshots, always on the node)
        auto fixedI = [&](const std::string& k, ofParameter<int>& p){
            Control c;
            c.key = k; c.fixed = true;
            c.paramName = [&p](){ return p.getEscapedName(); };
            c.save = [&p](){ return ofJson(p.get()); };
            c.load = [&p](const ofJson& j){ try { if(j.is_number()) p.set(std::clamp((int)std::lround(j.get<double>()), p.getMin(), p.getMax())); } catch(...) {} };
            c.text = [&p](){ return ofToString(p.get()); };
            c.apply = [&p](const std::string& t){ p.set(std::clamp((int)std::lround(ofToFloat(t)), p.getMin(), p.getMax())); };
            controls.push_back(std::move(c));
        };
        fixedI("clockMode", clockMode);
        {
            Control c;
            c.key = "root"; c.fixed = true;
            c.paramName = [this](){ return root.getEscapedName(); };
            c.save = [this](){ return ofJson(root.get()); };
            c.load = [this](const ofJson& j){ if(j.is_number()) root.set(ofClamp(j.get<float>(), root.getMin(), root.getMax())); };
            c.text = [this](){ return ofToString(root.get(), 2); };
            c.apply = [this](const std::string& t){ root.set(ofClamp(ofToFloat(t), root.getMin(), root.getMax())); };
            c.get = [this](){ return (double)root.get(); };
            c.set = [this](double v){ if((float)v != root.get()) root.set((float)v); };
            controls.push_back(std::move(c));
        }
        {
            Control c;
            c.key = "scale"; c.fixed = true;
            c.paramName = [this](){ return scale.getEscapedName(); };
            c.save = [this](){ return ofJson(scale.get()); };
            c.load = [this](const ofJson& j){ if(!j.is_array()) return; std::vector<float> v; for(const auto& x : j) if(x.is_number()) v.push_back(x.get<float>()); scale.set(v); };
            c.text = [this](){ std::string s; for(size_t i = 0; i < scale.get().size(); i++) s += (i ? " " : "") + ofToString(scale.get()[i]); return s; };
            c.apply = [this](const std::string& t){ std::vector<float> v; for(const auto& w : ofSplitString(t, " ", true, true)) v.push_back(ofToFloat(w)); if(!v.empty()) scale.set(v); };
            controls.push_back(std::move(c));
        }

        // clock and pulses
        ctlF("internalClockBpm", clockBpm.set("Clock BPM", 120.0f, 20.0f, 300.0f));
        ctlF("beatDiv", beatDiv.set("Beat Div", 1.0f, 0.125f, 32.0f));
        ctlB("oneShotMode", oneShot.set("One Shot", false));
        ctlI("pulseMode", pulseMode.set("Pulse Mode", PULSE_PERIODIC, 0, 4), pulseNames());
        ctlV("pulseStepPattern", pulseSteps.set("Pulse Steps", std::vector<int>(16, 100), {0}, {100}));
        ctlF("geigerSpeed", geigerSpeed.set("Geiger Speed", 1.0f, 0.125f, 16.0f));
        ctlF("geigerDensity", geigerDensity.set("Geiger Density", 0.45f, 0.0f, 1.0f));
        ctlF("geigerPeriodicity", geigerPeriodicity.set("Geiger Periodicity", 0.75f, 0.0f, 1.0f));
        ctlF("bouncingBallSpeed", bounceSpeed.set("Bounce Speed", 1.0f, 0.125f, 8.0f));
        ctlF("bouncingBallCurve", bounceCurve.set("Bounce Curve", 0.0f, -1.0f, 1.0f));
        ctlB("bouncingBallSymmetric", bounceSym.set("Bounce Symmetric", false));
        ctlI("eucLen", eucLen.set("Euclid Len", 8, 1, 64));
        ctlI("eucHits", eucHits.set("Euclid Hits", 8, 0, 64));
        ctlI("eucOff", eucOff.set("Euclid Offset", 0, 0, 63));
        ctlI("skipSteps", skipSteps.set("Skip Steps", 0, 0, 32));
        ctlF("seqProb", seqProb.set("Seq Chance", 1.0f, 0.0f, 1.0f));
        ctlF("seqProbCycles", seqProbCycles.set("Seq Cycles", 1.0f, 0.25f, 64.0f));
        ctlF("runGateBeats", runBeats.set("Run Beats", 16.0f, 1.0f, 512.0f));
        ctlF("runGateChance", runChance.set("Run Chance", 1.0f, 0.0f, 1.0f));
        ctlF("runGatePhase", runPhase.set("Run Phase", 0.0f, 0.0f, 1.0f));
        ctlF("stepChance", stepChance.set("Step Chance", 1.0f, 0.0f, 1.0f));
        ctlF("noteChance", noteChance.set("Note Chance", 1.0f, 0.0f, 1.0f));
        // source
        ctlI("sourceMode", sourceMode.set("Source", SRC_SCALE, 0, 1), {"Scale", "Chord pool"}, true);
        ctlB("sortPool", sortPool.set("Sort Pool", true), true);
        ctlB("removeDuplicates", dedupPool.set("Dedup Pool", false), true);
        ctlI("sourceChangeMode", sourceChange.set("Source Change", 0, 0, 2), changeNames());
        // pattern
        ctlI("seqSize", seqSize.set("Seq Size", 16, 1, MAX_SEQ));
        ctlI("patternMode", patternMode.set("Pattern", PAT_ASC, 0, 5), patternNames());
        ctlI("patternSeed", patternSeed.set("Pattern Seed", 0, 0, 999999));
        ctlB("patternRandomizeSeedEachCycle", patternRndCycle.set("Pattern Seed Each Cycle", false));
        ctlV("idxPattern", idxPattern.set("Index Pattern", {0, 1, 2, 3}, {0}, {127}));
        ctlI("modulo", modulo.set("Modulo", 0, 0, MAX_SEQ));
        ctlI("sourceStart", sourceStart.set("Source Start", 0, 0, 127));
        ctlI("sourceStride", sourceStride.set("Source Stride", 1, 0, 24));
        ctlB("gateAdvance", gateAdvance.set("Gate Advance", true));
        ctlI("stepShift", stepShift.set("Step Shift", 0, -MAX_SEQ, MAX_SEQ));
        ctlF("rndShiftChance", rndShiftChance.set("Random Shift Chance", 0.0f, 0.0f, 1.0f));
        ctlI("rndShiftRange", rndShiftRange.set("Random Shift Range", 0, 0, MAX_SEQ));
        ctlI("rndShiftQuant", rndShiftQuant.set("Random Shift Quant", 1, 1, MAX_SEQ));
        ctlB("accentOnsetMode", byGate.set("By Gate", true));
        // pitch
        ctlI("octave", octave.set("Octave", 0, -2, 8));
        ctlI("octaveFold", octaveFold.set("Octave Fold", 0, 0, 8));
        ctlB("foldPoly", foldPoly.set("Fold Poly", true));
        ctlB("pitchExpand", pitchExpand.set("Pitch Expand", true));
        ctlI("expandStep", expandStep.set("Expand Step", 12, 1, 48));
        ctlI("transpose", transpose.set("Transpose", 0, -96, 96));
        ctlF("octaveDev", octaveDev.set("Octave Dev", 0.0f, 0.0f, 1.0f));
        ctlI("octaveDevRng", octaveDevRng.set("Octave Dev Range", 1, 1, 4));
        ctlF("idxDev", idxDev.set("Index Dev", 0.0f, 0.0f, 1.0f));
        ctlI("idxDevRng", idxDevRng.set("Index Dev Range", 2, 1, 12));
        ctlF("pitchDev", pitchDev.set("Pitch Dev", 0.0f, 0.0f, 1.0f));
        ctlI("pitchDevRng", pitchDevRng.set("Pitch Dev Range", 2, 1, 12));
        ctlI("devSeed", devSeed.set("Dev Seed", 0, 0, 999999));
        ctlB("devRandomizeSeedEachCycle", devCycle.set("Dev Seed Each Cycle", false));
        // polyphony and outputs
        ctlI("polyphony", polyphony.set("Polyphony", 1, 1, MAX_POLY));
        ctlI("polyInterval", polyInterval.set("Poly Interval", 2, 1, 24));
        ctlI("polyAccent", polyAccent.set("Poly Accent", 0, 0, MAX_POLY - 1));
        ctlI("voiceMode", voiceMode.set("Voice Mode", 0, 0, 1), voiceModeNames());
        ctlB("trigAmp", trigAmp.set("Trig Amp", false));
        // bass
        ctlB("addBass", addBass.set("Add Bass", false));
        ctlI("bassPatternMode", bassMode.set("Bass Pattern", BASS_ALTERNATE, 0, 5), bassNames());
        ctlB("bassPatternInvert", bassInvert.set("Bass Invert", false));
        ctlI("bassAlternateSteps", bassEvery.set("Bass Every", 2, 1, 64));
        ctlI("bassAlternateShift", bassShift.set("Bass Shift", 0, -MAX_SEQ, MAX_SEQ));
        ctlI("bassEucLen", bassEucLen.set("Bass Len", 8, 1, 64));
        ctlI("bassEucHits", bassEucHits.set("Bass Hits", 2, 0, 64));
        ctlI("bassEucOff", bassEucOff.set("Bass Offset", 0, 0, 63));
        ctlI("bassOctave", bassOctave.set("Bass Octave", -2, -4, 4));
        ctlF("bassProb", bassProb.set("Bass Chance", 1.0f, 0.0f, 1.0f));
        ctlI("bassOctRnd", bassOctRnd.set("Bass Oct Random", 0, 0, 4));
        ctlF("bassOctRndChance", bassOctRndChance.set("Bass Oct Chance", 0.0f, 0.0f, 1.0f));
        ctlI("bassOctRndSeed", bassOctRndSeed.set("Bass Oct Seed", 0, 0, 999999));
        ctlB("bassOctRndRandomizeSeedEachCycle", bassOctRndCycle.set("Bass Oct Seed Each Cycle", false));
        ctlI("bassPolyphony", bassPoly.set("Bass Poly", 1, 1, NB));
        ctlF("bassVelBase", bassVel.set("Bass Velocity", 0.75f, 0.0f, 1.0f));
        ctlF("bassVelRndm", bassVelRndm.set("Bass Vel Random", 0.0f, 0.0f, 1.0f));
        ctlB("bassPitchModified", bassPitchMod.set("Bass Follows Octave", true));
        // timing
        ctlF("strum", strum.set("Strum", 0.0f, 0.0f, 1.0f));
        ctlF("strumRndm", strumRndm.set("Strum Random", 0.0f, 0.0f, 1.0f));
        ctlI("strumRndSeed", strumRndSeed.set("Strum Seed", 0, 0, 999999));
        ctlB("strumRndRandomizeSeedEachCycle", strumRndCycle.set("Strum Seed Each Cycle", false));
        ctlI("strumDir", strumDir.set("Strum Dir", STRUM_UP, 0, 2), strumNames());
        ctlI("strumDirSeed", strumDirSeed.set("Strum Dir Seed", 0, 0, 999999));
        ctlB("strumDirRandomizeSeedEachCycle", strumDirCycle.set("Strum Dir Seed Each Cycle", false));
        ctlF("swing", swing.set("Swing", 0.0f, 0.0f, 1.0f));
        ctlF("positionRndm", positionRndm.set("Position Random", 0.0f, 0.0f, 1.0f));
        ctlI("positionRndSeed", positionRndSeed.set("Position Seed", 0, 0, 999999));
        ctlB("positionRndRandomizeSeedEachCycle", positionRndCycle.set("Position Seed Each Cycle", false));
        // velocity
        ctlF("velBase", velBase.set("Velocity", 0.8f, 0.0f, 1.0f));
        ctlF("velRndm", velRndm.set("Vel Random", 0.1f, 0.0f, 1.0f));
        ctlI("velRndSeed", velRndSeed.set("Vel Seed", 0, 0, 999999));
        ctlB("velRndRandomizeSeedEachCycle", velRndCycle.set("Vel Seed Each Cycle", false));
        ctlF("velStepRndm", velStepRndm.set("Vel Step Random", 0.0f, 0.0f, 1.0f));
        ctlI("velStepRndSeed", velStepRndSeed.set("Vel Step Seed", 0, 0, 999999));
        ctlB("velStepRndRandomizeSeedEachCycle", velStepRndCycle.set("Vel Step Seed Each Cycle", false));
        ctlV("velocityCurve", velCurve.set("Vel Curve", std::vector<float>(CURVE_RES, 0.0f), {0.0f}, {1.0f}));
        ctlF("velocitySlowRndm", lfoDepth.set("Vel LFO Depth", 0.0f, 0.0f, 1.0f));
        ctlF("velocitySlowSpeed", lfoSpeed.set("Vel LFO Speed", 0.25f, 0.0f, 8.0f));
        ctlI("velocitySlowSeed", lfoSeed.set("Vel LFO Seed", 0, 0, 999999));
        ctlI("velocityLfoShape", lfoShape.set("Vel LFO Shape", LFO_SLOWRANDOM, 0, 4), lfoNames());
        ctlF("velocityLfoPhase", lfoPhase.set("Vel LFO Phase", 0.0f, 0.0f, 1.0f));
        ctlF("velocityLfoPulseWidth", lfoWidth.set("Vel LFO Width", 0.5f, 0.05f, 0.95f));
        // accents
        ctlI("accentPatternMode", accMode.set("Accent Pattern", EV_EUCLID, 0, 4), eventNames());
        ctlI("accentAlternateSteps", accEvery.set("Accent Every", 2, 1, 64));
        ctlI("accentAlternateShift", accShift.set("Accent Shift", 0, -MAX_SEQ, MAX_SEQ));
        ctlF("accentChance", accChance.set("Accent Chance", 0.5f, 0.0f, 1.0f));
        ctlI("accentSeed", accSeed.set("Accent Seed", 0, 0, 999999));
        ctlB("accentRandomizeSeedEachCycle", accRndCycle.set("Accent Seed Each Cycle", false));
        ctlV("accentStepPattern", accSteps.set("Accent Steps", std::vector<int>(16, 0), {0}, {1}));
        ctlI("eucAccLen", accLen.set("Accent Len", 4, 1, 64));
        ctlI("eucAccHits", accHits.set("Accent Hits", 1, 0, 64));
        ctlI("eucAccOff", accOff.set("Accent Offset", 0, 0, 63));
        ctlF("eucAccStrength", accStrength.set("Accent Strength", 0.2f, 0.0f, 1.0f));
        // durations
        ctlF("durBase", durBase.set("Duration", 1.0f, 0.0f, 4.0f));
        ctlF("durRndm", durRndm.set("Dur Random", 0.0f, 0.0f, 4.0f));
        ctlB("durationRndPerStep", durRndPerStep.set("Dur Random By Step", true));
        ctlI("durationPatternMode", durMode.set("Long Pattern", EV_EUCLID, 0, 4), eventNames());
        ctlI("durationAlternateSteps", durEvery.set("Long Every", 2, 1, 64));
        ctlI("durationAlternateShift", durShift.set("Long Shift", 0, -MAX_SEQ, MAX_SEQ));
        ctlF("durationChance", durChance.set("Long Chance", 0.5f, 0.0f, 1.0f));
        ctlI("durationSeed", durSeed.set("Long Seed", 0, 0, 999999));
        ctlB("durationRandomizeSeedEachCycle", durRndCycle.set("Long Seed Each Cycle", false));
        ctlV("durationStepPattern", durSteps.set("Long Steps", std::vector<int>(16, 0), {0}, {1}));
        ctlI("eucDurLen", durLen.set("Long Len", 4, 1, 64));
        ctlI("eucDurHits", durHits.set("Long Hits", 4, 0, 64));
        ctlI("eucDurOff", durOff.set("Long Offset", 0, 0, 63));
        ctlF("durEucStrength", durStrength.set("Long Strength", 0.0f, 0.0f, 4.0f));
        // snapshots
        ctlF("morphTime", morphTime.set("Morph Time", 0.0f, 0.0f, 10.0f));

        listeners.push(seqSize.newListener([this](int&){ syncPatternSizes(); }));
        listeners.push(trigAmp.newListener([this](bool&){ for(auto& [s, st] : servers) if(st.synth) st.synth->set("trigAmp", trigAmp ? 1.0f : 0.0f); }));
        listeners.push(clockBpm.newListener([this](float&){ for(auto& [s, st] : servers) if(st.synth) st.synth->set("freeBpm", (double)clockBpm.get()); }));
        listeners.push(oneShot.newListener([this](bool&){ engine.armed = false; }));
    }

    // Step patterns follow Seq Size (as the GUI node's)
    void syncPatternSizes(){
        const int s = std::clamp(seqSize.get(), 1, arpbox::MAX_SEQ);
        auto fit = [s](ofParameter<std::vector<int>>& p, int fill, bool indices){
            std::vector<int> v = p.get();
            if((int)v.size() == s) return;
            const int old = (int)v.size();
            v.resize(s, fill);
            if(indices) for(int i = old; i < s; i++) v[i] = i;
            p = v;
        };
        fit(pulseSteps, 100, false);
        fit(accSteps, 0, false);
        fit(durSteps, 0, false);
        fit(idxPattern, 0, true);
        if((int)velCurve.get().size() != arpbox::CURVE_RES) velCurve = std::vector<float>(arpbox::CURVE_RES, 0.0f);
    }

    // ════════════════════════════════════════════════════════════════════════
    // Engine
    // ════════════════════════════════════════════════════════════════════════

    std::vector<float> currentSource() const {
        return arpbox::buildSource(sourceMode.get(), scale.get(), notePool.get(), sortPool.get(), dedupPool.get());
    }
    uint64_t effectiveSeed() const { return seed.get() != 0 ? (uint64_t)seed.get() : salt; }
    // Section seeds of 0 draw anew on every jump and loop pass
    void newFresh(){
        static std::mt19937_64 rng((uint64_t)ofGetElapsedTimeMicros() ^ 0x5F3759DFull);
        engine.fresh = rng() | 1;
    }
    void newSalt(){
        static std::mt19937_64 rng((uint64_t)ofGetElapsedTimeMicros() ^ 0xA4B2C3D4ull);
        salt = rng() | (1ull << 40);
    }

    void syncEngine(){
        using namespace arpbox;
        Params& p = engine.p;
        p.triggerMode = clockMode.get() == CLOCK_TRIGGER;
        p.beatDiv = beatDiv; p.pulseMode = pulseMode; p.pulseStepPattern = pulseSteps.get();
        p.geigerSpeed = geigerSpeed; p.geigerDensity = geigerDensity; p.geigerPeriodicity = geigerPeriodicity;
        p.bounceSpeed = bounceSpeed; p.bounceCurve = bounceCurve; p.bounceSymmetric = bounceSym;
        p.eucLen = eucLen; p.eucHits = eucHits; p.eucOff = eucOff; p.oneShot = oneShot; p.skipSteps = skipSteps;
        p.seqSize = seqSize; p.patternMode = patternMode; p.patternSeed = patternSeed; p.patternRndCycle = patternRndCycle;
        p.idxPattern = idxPattern.get(); p.modulo = modulo; p.sourceStart = sourceStart; p.sourceStride = sourceStride;
        p.gateAdvance = gateAdvance; p.stepShift = stepShift; p.rndShiftChance = rndShiftChance; p.rndShiftRange = rndShiftRange;
        p.rndShiftQuant = rndShiftQuant; p.octave = octave; p.octaveFold = octaveFold; p.foldPoly = foldPoly; p.root = root;
        p.pitchExpand = pitchExpand; p.expandStep = expandStep; p.transpose = transpose;
        p.octaveDev = octaveDev; p.octaveDevRng = octaveDevRng; p.idxDev = idxDev; p.idxDevRng = idxDevRng;
        p.devSeed = devSeed; p.devCycle = devCycle;
        p.pitchDev = pitchDev; p.pitchDevRng = pitchDevRng; p.byGate = byGate;
        p.polyphony = polyphony; p.polyInterval = polyInterval; p.polyAccent = polyAccent;
        p.addBass = addBass; p.bassMode = bassMode; p.bassInvert = bassInvert; p.bassEvery = bassEvery; p.bassShift = bassShift;
        p.bassEucLen = bassEucLen; p.bassEucHits = bassEucHits; p.bassEucOff = bassEucOff; p.bassOctave = bassOctave;
        p.bassProb = bassProb; p.bassOctRnd = bassOctRnd; p.bassOctRndChance = bassOctRndChance; p.bassOctRndSeed = bassOctRndSeed;
        p.bassOctRndCycle = bassOctRndCycle; p.bassPoly = bassPoly; p.bassVel = bassVel; p.bassVelRndm = bassVelRndm;
        p.bassPitchMod = bassPitchMod;
        p.strum = strum; p.strumRndm = strumRndm; p.strumRndSeed = strumRndSeed; p.strumRndCycle = strumRndCycle;
        p.strumDir = strumDir; p.strumDirSeed = strumDirSeed; p.strumDirCycle = strumDirCycle; p.swing = swing;
        p.positionRndm = positionRndm; p.positionRndSeed = positionRndSeed; p.positionRndCycle = positionRndCycle;
        p.seqProb = seqProb; p.seqProbCycles = seqProbCycles; p.runBeats = runBeats; p.runChance = runChance;
        p.runPhase = runPhase; p.stepChance = stepChance; p.noteChance = noteChance;
        p.velBase = velBase; p.velRndm = velRndm; p.velRndSeed = velRndSeed; p.velRndCycle = velRndCycle;
        p.velStepRndm = velStepRndm; p.velStepRndSeed = velStepRndSeed; p.velStepRndCycle = velStepRndCycle;
        p.velocityCurve = velCurve.get(); p.lfoDepth = lfoDepth; p.lfoSpeed = lfoSpeed; p.lfoPhase = lfoPhase;
        p.lfoWidth = lfoWidth; p.lfoSeed = lfoSeed; p.lfoShape = lfoShape;
        p.accMode = accMode; p.accEvery = accEvery; p.accShift = accShift; p.accChance = accChance; p.accSeed = accSeed;
        p.accRndCycle = accRndCycle; p.accSteps = accSteps.get(); p.accLen = accLen; p.accHits = accHits; p.accOff = accOff;
        p.accStrength = accStrength;
        p.durBase = durBase; p.durRndm = durRndm; p.durRndPerStep = durRndPerStep; p.durMode = durMode; p.durEvery = durEvery;
        p.durShift = durShift; p.durChance = durChance; p.durSeed = durSeed; p.durRndCycle = durRndCycle;
        p.durSteps = durSteps.get(); p.durLen = durLen; p.durHits = durHits; p.durOff = durOff; p.durStrength = durStrength;
        engine.seed = effectiveSeed();
        engine.invalidate();
        noteCache.clear();
        for(auto& [server, st] : servers) invalidateServer(st);
    }

    // The notes of a pulse (cached until something changes)
    const std::vector<arpbox::Note>& notesOf(const arpbox::Pulse& pl){
        auto it = noteCache.find(pl.n);
        if(it != noteCache.end()) return it->second;
        if(noteCache.size() > 6000) noteCache.clear();
        return noteCache.emplace(pl.n, engine.notesFor(pl)).first->second;
    }

    std::string voicesText() const {
        const int bv = bassVoiceCount();
        return "Voices " + ofToString(chordVoices()) + (bv > 0 ? " + bass " + ofToString(bv) : "");
    }
    static const char* voicesTip(){
        return "Set by the music: one per chord tone (Polyphony + Extra on accents),\n"
               "then one per bass octave stacked (Add bass), last in the vectors.\n"
               "Round robin: more only when notes outlast a step (to overlap, not cut).\n"
               "Audio: Pitch / Gate carry both (as the vectors),\n"
               "Bass Pitch / Gate the bass alone.";
    }
    // The voices follow the music: Fixed, one per chord tone (Polyphony + the
    // accents' extra tones) and one per stacked bass octave; Round robin, as
    // many more as notes can overlap (notes longer than a step: each chord on
    // the next voices, not cut by the next one; up to a step: the same)
    int chordTones() const { return std::clamp(polyphony.get() + std::max(0, polyAccent.get()), 1, NV); }
    int voiceOverlap() const { return voiceMode.get() == 0 ? 1 : engine.overlap(); }
    int chordVoices() const { return std::min(NV, chordTones() * voiceOverlap()); }
    int bassVoiceCount() const {
        if(!addBass) return 0;
        return std::min(NB, std::clamp(bassPoly.get(), 1, NB) * voiceOverlap());
    }
    // A bass slot's channel in Trig / Gate / Pitch too, after the chord voices
    // (as in the vectors), when it fits in their 16; else (and for a chord slot) -1
    int mainSlotOfBass(int slot) const {
        if(slot < NV) return -1;
        const int m = chordVoices() + (slot - NV);
        return m < NV ? m : -1;
    }
    // The output slot of a note: 0..15 voices, 16..19 bass
    int slotOf(const arpbox::Note& n) const {
        if(n.bass){
            const int bv = std::max(1, bassVoiceCount());
            if(voiceMode.get() == 0) return NV + n.tone % bv;
            return NV + (int)arpbox::wrap64(n.n * std::clamp(bassPoly.get(), 1, NB) + n.tone, bv);
        }
        const int v = chordVoices();
        if(voiceMode.get() == 0) return n.tone % v;
        return (int)arpbox::wrap64(n.n * chordTones() + n.tone, v);
    }

    // Clocked: every note starting in [from, to) beats, its start / end beats
    struct Placed { arpbox::Note note; double start, end; int slot; };
    std::vector<Placed> placedIn(double from, double to){
        std::vector<Placed> out;
        const double sb = engine.stepBeats();
        for(const auto& pl : engine.pulsesIn(from - engine.maxOffsetSteps() * sb, to)){
            for(const auto& n : notesOf(pl)){
                const double start = pl.beat + n.offset * sb;
                if(start < from || start >= to) continue;
                out.push_back({n, start, start + n.dur * sb, slotOf(n)});
            }
        }
        return out;
    }

    // Source material changed (Scale, NotePool, Source, Sort, Dedup): Source Change
    // ════════════════════════════════════════════════════════════════════════
    // A ChordBox on NotePool / Scale / Root (scChordLink.h): its chords read
    // at each pulse's beat, exactly on the chord changes (and ahead)
    // ════════════════════════════════════════════════════════════════════════

    const ofAbstractParameter* linkSource = nullptr;
    const ofAbstractParameter* linkRoot = nullptr;
    bool linkTimed = false, linkChained = false, poolWasLinked = false;
    uint64_t linkSignature = 0;
    // A ChordBox through other nodes: what they make of its chords
    arpbox::ChainMap chain;
    const ofAbstractParameter* chainKey = nullptr;
    std::vector<float> chainLastInput;
    double chainInputBeat = -1e300;     // the beat the input last changed at
    float lastRootValue = 0.0f;
    ofParameter<int> devSeed;
    ofParameter<bool> devCycle;

    static const ofAbstractParameter* linkedKey(ofxOceanodeAbstractParameter& in){ return chordlink::directKey(in); }
    const std::vector<float>& sourceInput() const { return sourceMode.get() == arpbox::SRC_POOL ? notePool.get() : scale.get(); }
    // A ChordBox through other nodes: the input's values for a chord (learned), else the input's own
    std::vector<float> chainValues(const std::vector<float>& chord) const {
        std::vector<float> v;
        return chain.predict(chord, v) ? v : sourceInput();
    }
    // Learn what the nodes in between make of the chord playing: the input's
    // values once they changed after the chord began, or a moment after it
    void learnChain(){
        const auto& input = sourceInput();
        const double now = positionBeat();
        if(!arpbox::ChainMap::same(input, chainLastInput)){ chainLastInput = input; chainInputBeat = now; }
        const auto* pr = linkChained ? providerOf(linkSource) : nullptr;
        if(!pr || !getFrameTransportState().current.isPlaying) return;
        const double start = pr->chordStartAt(now);
        const double settle = 0.15 * std::max(1.0f, getFrameTransportState().current.bpm) / 60.0;
        if(chainInputBeat < start - 1e-9 && now - start < settle) return;
        chain.learn(pr->notesAt(now), input);
    }
    static const chordlink::Provider* providerOf(const ofAbstractParameter* k){
        if(!k) return nullptr;
        auto it = chordlink::registry().find(k);
        return it == chordlink::registry().end() ? nullptr : &it->second;
    }
    void updateLinks(){
        const auto* pool = linkedKey(getOceanodeParameter(notePool));
        const auto* scl = linkedKey(getOceanodeParameter(scale));
        const auto* rt = linkedKey(getOceanodeParameter(root));
        // a ChordBox newly connected to NotePool while the source is the scale: play its chords
        if(pool && !poolWasLinked && !scl && sourceMode.get() == arpbox::SRC_SCALE) sourceMode = arpbox::SRC_POOL;
        poolWasLinked = pool != nullptr;
        const int mode = sourceMode.get();
        const auto* direct = mode == arpbox::SRC_POOL ? pool : scl;
        // none directly: one through other nodes (on the transport, the ChordBox's beats)
        const auto* up = direct || clockMode.get() != CLOCK_TRANSPORT ? nullptr
                       : chordlink::upstreamKey(mode == arpbox::SRC_POOL ? getOceanodeParameter(notePool) : getOceanodeParameter(scale));
        if(up != chainKey){ chainKey = up; chain.clear(); }
        const auto* src = direct ? direct : up;
        const auto* sp = providerOf(src);
        const auto* rp = providerOf(rt);
        const bool timedSrc = sp && sp->timed && sp->timed();
        const bool timedRoot = rp && rp->timed && rp->timed();
        const bool chained = timedSrc && !direct;
        uint64_t sig = arpbox::H((uint64_t)(uintptr_t)(timedSrc ? src : nullptr), (uint64_t)(uintptr_t)(timedRoot ? rt : nullptr),
                                 (timedSrc ? sp->revision() : 0) * 3 + (timedRoot ? rp->revision() : 0) + (chained ? chain.revision << 20 : 0),
                                 (uint64_t)(mode * 64 + sourceChange.get() * 8 + sortPool.get() * 2 + dedupPool.get()));
        if(sig == linkSignature) return;
        linkSignature = sig;
        const bool wasLinked = (bool)engine.linkedSource;
        linkSource = timedSrc ? src : nullptr;
        linkRoot = timedRoot ? rt : nullptr;
        linkTimed = timedSrc;
        linkChained = chained;
        if(timedSrc){
            const bool sortP = sortPool.get(), dedupP = dedupPool.get();
            const auto* key = src;
            engine.linkedSource = [this, key, mode, sortP, dedupP, chained](double b){
                const auto* pr = providerOf(key);
                std::vector<float> v = pr ? pr->notesAt(b) : std::vector<float>{60.0f};
                if(chained) v = chainValues(v);
                return mode == arpbox::SRC_POOL ? arpbox::buildSource(arpbox::SRC_POOL, {}, v, sortP, dedupP)
                                                : arpbox::buildSource(arpbox::SRC_SCALE, v, {}, sortP, dedupP);
            };
            engine.linkedChordStart = [key](double b){ const auto* pr = providerOf(key); return pr ? pr->chordStartAt(b) : 0.0; };
        }else{
            engine.linkedSource = nullptr;
            engine.linkedChordStart = nullptr;
            if(wasLinked) engine.resetSource(currentSource());
        }
        if(timedRoot){
            const auto* key = rt;
            engine.linkedRoot = [key](double b){ const auto* pr = providerOf(key); return pr ? pr->rootAt(b) : 0.0f; };
        }else engine.linkedRoot = nullptr;
        engine.linkedChange = sourceChange.get();
        paramsDirty = true;
    }

    void applySourceChange(){
        if(engine.linked()){
            // a ChordBox: read per beat. Through other nodes, a chord not learned yet plays the input as it comes
            if(linkChained){
                engine.invalidate();
                noteCache.clear();
                for(auto& [server, st] : servers) invalidateServer(st);
            }
            return;
        }
        const auto values = currentSource();
        const auto& latest = engine.latestSource();
        bool same = values.size() == latest.size();
        for(size_t i = 0; same && i < values.size(); i++) if(std::abs(values[i] - latest[i]) > 0.001f) same = false;
        if(same) return;
        if(clockMode.get() == CLOCK_TRIGGER){
            const double pos = (double)triggerCount;
            switch(sourceChange.get()){
                case 1: engine.setSource(pos, values); resetTriggerCount(); break;
                case 2: engine.setSource((double)engine.nextCycleOrdinal(triggerCount), values); break;
                default: engine.setSource(pos, values); break;
            }
        }else{
            const double now = positionBeat();
            switch(sourceChange.get()){
                case 1: {
                    engine.setSource(now, values);
                    engine.origin = engine.firstPulseFrom(now).n;
                    break;
                }
                case 2: engine.setSource(engine.nextCycleBeat(now), values); break;
                default: engine.setSource(now, values); break;
            }
            engine.pruneSources(now - 64.0);
        }
        engine.invalidate();
        noteCache.clear();
        for(auto& [server, st] : servers) invalidateServer(st);
    }

    // Reset: step 0 on the next pulse (Reset also restarts the Internal clock)
    void doReset(bool hard){
        engine.armed = true;
        if(seed.get() == 0) newSalt();
        engine.seed = effectiveSeed();
        if(clockMode.get() == CLOCK_TRIGGER){
            engine.origin = 0;
            resetTriggerCount();
        }else if(clockMode.get() == CLOCK_INTERNAL && hard){
            engine.origin = 0;
            resetFreeClock();
        }else{
            engine.origin = engine.firstPulseFrom(positionBeat()).n;
        }
        engine.invalidate();
        noteCache.clear();
        for(auto& [server, st] : servers) invalidateServer(st);
    }

    // Seed 0: fresh draws on every jump and loop pass
    void watchTransport(){
        const auto t = getFrameTransportState();
        const bool jumped = t.current.generation != lastGeneration || (t.current.isPlaying && !lastPlaying)
                         || (t.current.loopCount != lastLoopCount);
        lastGeneration = t.current.generation;
        lastPlaying = t.current.isPlaying;
        lastLoopCount = t.current.loopCount;
        bpb = beatsPerBar();
        if(!jumped || clockMode.get() != CLOCK_TRANSPORT) return;
        // a jump: the latest source plays everywhere (no history behind a seek)
        if(engine.sourceCount() > 1){ const auto latest = engine.latestSource(); engine.resetSource(latest); paramsDirty = true; }
        if(seed.get() == 0) newSalt();
        newFresh();
        paramsDirty = true;
    }

    // ════════════════════════════════════════════════════════════════════════
    // Clocks
    // ════════════════════════════════════════════════════════════════════════

    double positionBeat() const {
        if(clockMode.get() == CLOCK_INTERNAL) return freeTicks() / (double)TPB;
        return getFrameTransportState().current.beatPosition;
    }
    double freeTicks() const { return (double)freeBase + freePos; }
    void resetFreeClock(){
        freeResetCounter = freeResetCounter % (1 << 20) + 1;
        freeBase = 0; freePos = 0.0; lastFreePos = 0.0;
        statusHold = 3;
        for(auto& [server, st] : servers){ if(st.synth) st.synth->set("freeReset", (double)freeResetCounter); invalidateServer(st); }
    }
    void resetTriggerCount(){
        countResetCounter = countResetCounter % (1 << 20) + 1;
        triggerCount = 0;
        statusHold = 3;
        // the rows from 0 first, then the reset (in this order on the wire)
        for(auto& [server, st] : servers){
            invalidateServer(st);
            writeRows(st, 0);
            if(st.synth) st.synth->set("countReset", (double)countResetCounter);
        }
    }
    void noteBang(){
        const float now = ofGetElapsedTimef();
        if(lastBangTime > 0.0f && now - lastBangTime < 2.0f) bangPeriod = bangPeriod * 0.5f + (now - lastBangTime) * 0.5f;
        lastBangTime = now;
    }
    // The player's trigger count and Internal clock
    void readStatus(){
        for(auto& [server, st] : servers){
            if(!st.synth || !busLive(server, st.status)) continue;
            const auto& v = st.status->readValues;
            if(statusHold > 0) statusHold--;
            else if(v.size() >= 2){
                const int64_t count = (int64_t)std::llround(v[0]);
                if(count != triggerCount){
                    if(count > triggerCount){
                        const float now = ofGetElapsedTimef();
                        if(lastCountTime > 0.0f && now - lastCountTime < 2.0f)
                            bangPeriod = bangPeriod * 0.5f + (now - lastCountTime) / (float)(count - triggerCount) * 0.5f;
                        lastCountTime = now;
                    }
                    triggerCount = count;
                }
                const double pos = v[1];
                if(pos + FREE_WRAP / 2 < lastFreePos) freeBase += FREE_WRAP;
                lastFreePos = pos;
                freePos = pos;
            }
            server->requestControlValues(st.status->index, 2);
            break;
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // The buffer
    // ════════════════════════════════════════════════════════════════════════

    struct ServerState {
        ofxSCSynth* synth = nullptr;
        ofxSCBuffer* buffer = nullptr;
        ofxSCBus* status = nullptr;
        ofxSCServer* server = nullptr;
        bool allocPending = false;
        float allocTime = 0.0f;
        std::map<int, int> outputBuses;
        std::array<int64_t, SLOTS> slotChunk;
        std::array<int64_t, ROWS> rowOrdinal;
        ServerState(){ slotChunk.fill(NONE); rowOrdinal.fill(NONE); }
    };
    std::map<ofxSCServer*, ServerState> servers;

    bool bufferLive(ofxSCServer* server, const ServerState& st) const {
        return server && st.buffer && st.buffer->index >= 0 && st.buffer->index < (int)server->buffers.size()
            && server->buffers[st.buffer->index] == st.buffer;
    }
    static bool busLive(ofxSCServer* s, ofxSCBus* b){
        return s && b && b->index >= 0 && b->index < (int)s->controlBusses.size() && s->controlBusses[b->index] == b;
    }
    void invalidateServer(ServerState& st){ st.slotChunk.fill(NONE); st.rowOrdinal.fill(NONE); }

    // /b_setn messages for runs of samples (up to ~2000 values each)
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
        void flush(){ if(values > 0){ buf->server->sendMsg(m); } begin(); }
    };
    void zero(ServerState& st, int start, int count){
        ofxOscMessage m;
        m.setAddress("/b_fill");
        m.addIntArg(st.buffer->index);
        m.addIntArg(start);
        m.addIntArg(count);
        m.addFloatArg(0.0f);
        st.buffer->server->sendMsg(m);
    }

    // The chunks the player may read soon (after the loop end: from the loop start)
    std::vector<int64_t> wantedChunks(){
        std::vector<int64_t> out;
        if(clockMode.get() == CLOCK_INTERNAL){
            const int64_t c = arpbox::floorDiv((int64_t)std::floor(freeTicks()), CHUNK);
            for(int64_t w : {c, c + 1, c - 1, c + 2}) out.push_back(w);
            return out;
        }
        const auto t = getFrameTransportState().current;
        const int64_t c = arpbox::floorDiv((int64_t)std::floor(t.beatPosition * TPB), CHUNK);
        auto atStartOf = [&](int64_t chunk){
            double beat = (double)chunk * CHUNK / TPB;
            if(t.loopActive() && beat >= t.loopEndBeat - 1e-9){
                const double len = t.loopEndBeat - t.loopStartBeat;
                beat = t.loopStartBeat + std::fmod(beat - t.loopEndBeat, len);
            }
            return arpbox::floorDiv((int64_t)std::floor(beat * TPB), CHUNK);
        };
        for(int64_t w : {c, atStartOf(c + 1), c - 1, atStartOf(c + 2)})
            if(std::find(out.begin(), out.end(), w) == out.end()) out.push_back(w);
        return out;
    }
    void writeChunk(ServerState& st, int64_t chunk){
        const int slot = (int)arpbox::wrap64(chunk, SLOTS);
        const int firstFrame = slot * CHUNK;
        zero(st, firstFrame * BUF_CH, CHUNK * BUF_CH);
        const double from = (double)chunk * CHUNK / TPB, to = from + (double)CHUNK / TPB;
        std::map<int, std::array<float, BUF_CH>> frames;
        std::map<int, std::pair<int, int>> spans;   // frame -> first / last slot used
        const double ticksPerStep = TPB * engine.stepBeats();
        for(const auto& pn : placedIn(from, to)){
            const double at = pn.start * TPB;
            int64_t tick = (int64_t)std::floor(at + 1e-7);
            const int local = (int)std::clamp<int64_t>(tick - chunk * CHUNK, 0, CHUNK - 1);
            auto& f = frames[local];
            for(const int slot : {pn.slot, mainSlotOfBass(pn.slot)}){
                if(slot < 0) continue;
                auto sp = spans.find(local);
                if(sp == spans.end()){ f.fill(0.0f); spans[local] = {slot, slot}; }
                else { sp->second.first = std::min(sp->second.first, slot); sp->second.second = std::max(sp->second.second, slot); }
                float* d = f.data() + slot * CH;
                d[0] = std::max(0.001f, pn.note.vel);
                d[1] = (float)std::max(0.05, pn.note.dur * ticksPerStep);
                d[2] = pn.note.pitch;
                d[3] = (float)std::clamp(at - (double)tick, 0.0, 0.9999);
            }
        }
        Writer w(st.buffer);
        for(const auto& [local, f] : frames){
            const auto& sp = spans[local];
            w.run((firstFrame + local) * BUF_CH + sp.first * CH, f.data() + sp.first * CH, (sp.second - sp.first + 1) * CH);
        }
        w.flush();
        st.slotChunk[slot] = chunk;
    }
    // Trigger mode: the rows of the next triggers (ordinals from `from`)
    void writeRows(ServerState& st, int64_t from){
        if(!bufferLive(st.buffer ? st.buffer->server : nullptr, st) || st.allocPending) return;
        Writer w(st.buffer);
        for(int64_t n = std::max<int64_t>(0, from); n < from + AHEAD; n++){
            const int row = (int)arpbox::wrap64(n, ROWS);
            if(st.rowOrdinal[row] == n) continue;
            std::array<float, BUF_CH> f;
            f.fill(0.0f);
            for(const auto& note : notesOf({n, (double)n / engine.bd()})){
                const int slot = slotOf(note);
                for(const int sl : {slot, mainSlotOfBass(slot)}){
                    if(sl < 0) continue;
                    float* d = f.data() + sl * CH;
                    d[0] = std::max(0.001f, note.vel);
                    d[1] = (float)std::max(0.01, note.dur);
                    d[2] = note.pitch;
                    d[3] = (float)note.offset;
                }
            }
            w.run((CLOCKED + row) * BUF_CH, f.data(), BUF_CH);
            st.rowOrdinal[row] = n;
        }
        w.flush();
    }
    void fillBuffers(){
        if(clockMode.get() == CLOCK_TRIGGER){
            for(auto& [server, st] : servers){
                if(st.allocPending || !bufferLive(server, st)) continue;
                writeRows(st, triggerCount);
            }
            return;
        }
        const auto wanted = wantedChunks();
        for(auto& [server, st] : servers){
            if(st.allocPending || !bufferLive(server, st)) continue;
            std::array<bool, SLOTS> claimed{};
            for(int64_t w : wanted){
                const int slot = (int)arpbox::wrap64(w, SLOTS);
                if(claimed[slot]) continue;
                claimed[slot] = true;
                if(st.slotChunk[slot] != w) writeChunk(st, w);
            }
        }
    }

    // the graph rebuilds with the new cable
    void refreshOutputs(){ for(auto& output : scNode::outputs) output = output; }
    static std::string busName(int index){
        static const char* names[] = {"trigout", "gateout", "pitchout", "btrigout", "bgateout", "bpitchout"};
        return names[std::clamp(index, 0, OUTPUTS - 1)];
    }
    void sendTrigInput(ServerState& st){
        if(!st.synth) return;
        const int b = st.server ? trigIn.bus(st.server) : -1;
        if(b >= 0){ st.synth->set("trig_sel", 1.0f); st.synth->mapan("trig_ar", b, 1); }
        else { st.synth->set("trig_sel", 0.0f); st.synth->set("trig", (double)trigValue.get()); }
    }
    void sendStaticControls(ServerState& st){
        if(!st.synth) return;
        if(st.buffer) st.synth->set("buf", st.buffer->index);
        st.synth->set("clock", clockMode.get());
        st.synth->set("trigAmp", trigAmp ? 1.0f : 0.0f);
        st.synth->set("freeBpm", (double)clockBpm.get());
        if(st.status) st.synth->set("status", st.status->index);
        for(auto& [index, bus] : st.outputBuses) if(index >= 0 && index < OUTPUTS) st.synth->set(busName(index), bus);
        sendTrigInput(st);
    }

    // ════════════════════════════════════════════════════════════════════════
    // Transport (as EuclideanBox: \base = the absolute tick at floor(anchorPos)
    // modulo the buffer's clocked frames)
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
            int64_t delta = (int64_t)std::floor(sa.pos) - arpbox::wrap64(k, period);
            if(delta > period / 2) delta -= period;
            else if(delta < -period / 2) delta += period;
            const int64_t absTick = k + delta;
            const float base = (float)arpbox::wrap64(absTick, CLOCKED);
            anchorIdCounter = anchorIdCounter % (1 << 20) + 1;
            if(hard) anchorHardCounter = anchorHardCounter % (1 << 20) + 1;
            bool pending = false;
            scTransportSync::AnchorScope scope(a);
            for(auto& [server, st] : servers){
                auto* s = st.synth;
                if(!s) continue;
                if(!s->isCreated()){ pending = true; continue; }
                s->set("bpm", a.bpm);
                s->set("run", a.playing ? 1.0f : 0.0f);
                s->set("anchorPos", (float)sa.pos);
                s->set("anchorFire", sa.fire ? 1.0f : 0.0f);
                s->set("base", base);
                if(hard) s->set("anchorHard", (float)anchorHardCounter);
                s->set("anchorId", (float)anchorIdCounter);   // last: starts the new anchor
            }
            if(pending){ forceHardAnchor = true; follower.requestAnchor(); }
        }
    }

    double beatsPerBar() const {
#if OFXOCEANODESC_HAS_TIMELINE
        if(auto* container = getHostContainer())
            return std::max(0.25, container->getTimelineManager().getBeatsPerBar());
#endif
        return 4.0;
    }

    // ════════════════════════════════════════════════════════════════════════
    // The GUI node's vectors (frame rate)
    // ════════════════════════════════════════════════════════════════════════

    void updateVectors(){
        const int v = chordVoices();
        const int bv = bassVoiceCount();
        const int n = v + bv;
        std::vector<float> pitch(n, 0.0f), vel(n, 0.0f), dur(n, 0.0f), gv(n, 0.0f);
        std::vector<int> gate(n, 0);
        // keep the last pitches between notes
        const auto& oldPitch = pitchOut.get();
        for(int i = 0; i < n && i < (int)oldPitch.size(); i++) pitch[i] = oldPitch[i];
        auto index = [&](int slot){ return slot < NV ? (slot < v ? slot : -1) : (slot - NV < bv ? v + slot - NV : -1); };
        int64_t lanePulse = 0;
        if(clockMode.get() == CLOCK_TRIGGER){
            const int64_t last = triggerCount - 1;
            lanePulse = std::max<int64_t>(0, last);
            if(last >= 0){
                const float since = ofGetElapsedTimef() - lastCountTime;
                const float period = std::max(0.01f, bangPeriod);
                for(const auto& note : notesOf({last, (double)last / engine.bd()})){
                    const int i = index(slotOf(note));
                    if(i < 0) continue;
                    pitch[i] = note.pitch;
                    vel[i] = note.vel;
                    dur[i] = (float)(note.dur * period * 1000.0);
                    const bool on = since >= note.offset * period && since < (note.offset + note.dur) * period;
                    gate[i] = on ? 1 : 0;
                    gv[i] = on ? note.vel : 0.0f;
                }
            }
        }else{
            const double now = positionBeat();
            const double sb = engine.stepBeats();
            const double msPerBeat = 60000.0 / std::max(1.0f, clockMode.get() == CLOCK_INTERNAL ? clockBpm.get() : getFrameTransportState().current.bpm);
            std::vector<double> latest(n, -1e300);
            for(const auto& pn : placedIn(now - (engine.maxDurSteps() + engine.maxOffsetSteps() + 1.0) * sb, now + 1e-6)){
                const int i = index(pn.slot);
                if(i < 0 || pn.start < latest[i]) continue;
                latest[i] = pn.start;
                pitch[i] = pn.note.pitch;
                vel[i] = pn.note.vel;
                dur[i] = (float)((pn.end - pn.start) * msPerBeat);
                const bool on = now < pn.end;
                gate[i] = on ? 1 : 0;
                gv[i] = on ? pn.note.vel : 0.0f;
            }
            lanePulse = nowPulse;
            // stopped: no gates
            if(clockMode.get() == CLOCK_TRANSPORT && !getFrameTransportState().current.isPlaying){
                std::fill(gate.begin(), gate.end(), 0);
                std::fill(gv.begin(), gv.end(), 0.0f);
            }
        }
        if(totalVoicesOut.get() != n) totalVoicesOut = n;
        if(pitch != pitchOut.get()) pitchOut = pitch;
        if(gate != gateOut.get()) gateOut = gate;
        if(vel != velOut.get()) velOut = vel;
        if(dur != durOut.get()) durOut = dur;
        if(gv != gateVelOut.get()) gateVelOut = gv;
        std::vector<int> lg, la, ld;
        engine.lanes(lanePulse, lg, la, ld);
        if(lg != eucGateOut.get()) eucGateOut = lg;
        if(la != eucAccOut.get()) eucAccOut = la;
        if(ld != eucDurOut.get()) eucDurOut = ld;
    }

    // ════════════════════════════════════════════════════════════════════════
    // Snapshots (shared by every ArpeggiatorBox)
    // ════════════════════════════════════════════════════════════════════════

    struct Snapshot { std::string name; ofJson values; };
    std::vector<Snapshot> snapshots;
    int loadedSnapshotRevision = -1;
    static int& snapshotRevisionRef(){ static int r = 0; return r; }
    static int snapshotRevision(){ return snapshotRevisionRef(); }
    static std::string snapshotPath(){ return ofToDataPath("nodeSnapshots/ArpeggiatorBox/Snapshots.json", true); }

    void loadSnapshotFile(){
        loadedSnapshotRevision = snapshotRevision();
        snapshots.clear();
        ofFile f(snapshotPath());
        if(!f.exists()) return;
        try {
            const ofJson j = ofLoadJson(snapshotPath());
            if(j.contains("snapshots") && j["snapshots"].is_array())
                for(const auto& s : j["snapshots"])
                    if(s.is_object()) snapshots.push_back({s.value("name", std::string("Snapshot")), s.value("values", ofJson::object())});
        } catch(...) {}
    }
    void saveSnapshotFile(){
        ofDirectory dir(ofFilePath::getEnclosingDirectory(snapshotPath()));
        if(!dir.exists()) dir.create(true);
        ofJson j;
        j["snapshots"] = ofJson::array();
        for(const auto& s : snapshots) j["snapshots"].push_back({{"name", s.name}, {"values", s.values}});
        ofSavePrettyJson(snapshotPath(), j);
        snapshotRevisionRef()++;
        loadedSnapshotRevision = snapshotRevision();
    }
    // ── Demo snapshots: the defaults with a few settings changed ────────────
    ofJson defaultValues;
    static std::vector<std::pair<std::string, ofJson>> demoOverrides(){
        using namespace arpbox;
        auto arch = [](float depth){ std::vector<float> c(CURVE_RES); for(int i = 0; i < CURVE_RES; i++) c[i] = depth * std::sin((float)PI * i / (CURVE_RES - 1)); return c; };
        auto rise = [](float depth){ std::vector<float> c(CURVE_RES); for(int i = 0; i < CURVE_RES; i++) c[i] = depth * i / (CURVE_RES - 1); return c; };
        const std::vector<float> major = {0, 2, 4, 5, 7, 9, 11}, minor = {0, 2, 3, 5, 7, 8, 10}, dorian = {0, 2, 3, 5, 7, 9, 10},
                                 lydian = {0, 2, 4, 6, 7, 9, 11}, penta = {0, 2, 4, 7, 9}, minPenta = {0, 3, 5, 7, 10},
                                 harmMinor = {0, 2, 3, 5, 7, 8, 11}, whole = {0, 2, 4, 6, 8, 10};
        return {
            {"Demo 01 Classic 16ths", {{"scale", major}, {"beatDiv", 4.0f}, {"seqSize", 8}, {"octave", 4}, {"durBase", 0.5f},
                                       {"eucAccLen", 4}, {"eucAccHits", 1}, {"eucAccStrength", 0.25f}, {"velBase", 0.7f}, {"velRndm", 0.08f}}},
            {"Demo 02 Minor Triads", {{"scale", minor}, {"beatDiv", 2.0f}, {"seqSize", 7}, {"octave", 4}, {"polyphony", 3}, {"polyInterval", 2},
                                      {"strum", 0.06f}, {"durBase", 1.5f}, {"polyAccent", 1}, {"eucAccLen", 4}, {"eucAccHits", 1}, {"velBase", 0.65f}}},
            {"Demo 03 Tresillo + Bass", {{"scale", minPenta}, {"beatDiv", 4.0f}, {"seqSize", 8}, {"octave", 4}, {"pulseMode", PULSE_EUCLID},
                                         {"eucLen", 8}, {"eucHits", 3}, {"durBase", 0.8f}, {"addBass", true}, {"bassPatternMode", BASS_START},
                                         {"bassOctave", -1}, {"bassVelBase", 0.9f}, {"root", 0.0f}}},
            {"Demo 04 Dorian Up-Down Swing", {{"scale", dorian}, {"beatDiv", 4.0f}, {"seqSize", 14}, {"octave", 4}, {"patternMode", PAT_ASCDESC},
                                              {"swing", 0.35f}, {"durBase", 0.6f}, {"velocityLfoShape", LFO_SIN}, {"velocitySlowRndm", 0.3f},
                                              {"velocitySlowSpeed", 0.125f}, {"velBase", 0.55f}}},
            {"Demo 05 Random Walk", {{"scale", minPenta}, {"beatDiv", 4.0f}, {"seqSize", 16}, {"octave", 4}, {"patternMode", PAT_RANDOM},
                                     {"patternRandomizeSeedEachCycle", true}, {"idxDev", 0.2f}, {"octaveDev", 0.12f}, {"stepChance", 0.8f},
                                     {"durBase", 0.7f}, {"durRndm", 0.6f}, {"durationRndPerStep", false}}},
            {"Demo 06 Strummed Lydian", {{"scale", lydian}, {"beatDiv", 0.5f}, {"seqSize", 4}, {"octave", 3}, {"polyphony", 5}, {"polyInterval", 2},
                                         {"strum", 0.08f}, {"strumRndm", 0.03f}, {"durBase", 1.8f}, {"velBase", 0.6f}, {"velRndm", 0.15f},
                                         {"sourceStride", 2}}},
            {"Demo 07 Geiger Sparkle", {{"scale", whole}, {"beatDiv", 4.0f}, {"seqSize", 12}, {"octave", 5}, {"pulseMode", PULSE_GEIGER},
                                        {"geigerDensity", 0.5f}, {"geigerPeriodicity", 0.3f}, {"geigerSpeed", 1.0f}, {"durBase", 0.3f},
                                        {"octaveDev", 0.2f}, {"velRndm", 0.4f}, {"velBase", 0.4f}}},
            {"Demo 08 Bouncing Ball", {{"scale", penta}, {"beatDiv", 1.0f}, {"seqSize", 16}, {"octave", 5}, {"pulseMode", PULSE_BOUNCE},
                                       {"bouncingBallSpeed", 0.75f}, {"bouncingBallCurve", 0.8f}, {"durBase", 0.25f}, {"patternMode", PAT_DESC},
                                       {"velocityCurve", rise(0.3f)}, {"velBase", 0.5f}}},
            {"Demo 09 Step Groove", {{"scale", minor}, {"beatDiv", 4.0f}, {"seqSize", 16}, {"octave", 3}, {"pulseMode", PULSE_STEPSEQ},
                                     {"pulseStepPattern", std::vector<int>{100, 0, 60, 100, 0, 100, 30, 0, 100, 0, 70, 100, 0, 40, 100, 20}},
                                     {"accentPatternMode", EV_STEPSEQ}, {"accentOnsetMode", false},
                                     {"accentStepPattern", std::vector<int>{1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0}},
                                     {"durationPatternMode", EV_STEPSEQ}, {"durEucStrength", 1.5f},
                                     {"durationStepPattern", std::vector<int>{0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0}},
                                     {"swing", 0.2f}, {"durBase", 0.45f}, {"eucAccStrength", 0.3f}}},
            {"Demo 10 Five Against Four", {{"scale", dorian}, {"beatDiv", 4.0f}, {"seqSize", 5}, {"octave", 4}, {"polyphony", 2}, {"polyInterval", 4},
                                           {"accentOnsetMode", false}, {"eucAccLen", 4}, {"eucAccHits", 1}, {"eucAccStrength", 0.35f},
                                           {"eucDurLen", 3}, {"eucDurHits", 1}, {"durEucStrength", 1.5f}, {"durBase", 0.4f}}},
            {"Demo 11 Phrases", {{"scale", harmMinor}, {"beatDiv", 4.0f}, {"seqSize", 8}, {"octave", 4}, {"runGateBeats", 8.0f},
                                 {"runGateChance", 0.6f}, {"seqProb", 0.8f}, {"seqProbCycles", 2.0f}, {"addBass", true},
                                 {"bassPatternMode", BASS_VEL_ACCENT}, {"eucAccLen", 3}, {"eucAccHits", 1}, {"durBase", 0.6f}}},
            {"Demo 12 Folded Fifths", {{"scale", std::vector<float>{0, 7}}, {"beatDiv", 4.0f}, {"seqSize", 24}, {"octave", 4},
                                       {"pitchExpand", true}, {"expandStep", 7}, {"octaveFold", 2}, {"polyphony", 2}, {"polyInterval", 3},
                                       {"durBase", 0.5f}, {"rndShiftChance", 0.5f}, {"rndShiftRange", 4}, {"rndShiftQuant", 2}}},
            {"Demo 13 Velocity Arch", {{"scale", major}, {"beatDiv", 3.0f}, {"seqSize", 12}, {"octave", 4}, {"patternMode", PAT_DESCASC},
                                       {"velocityCurve", arch(0.45f)}, {"velBase", 0.35f}, {"velRndm", 0.0f},
                                       {"accentPatternMode", EV_RANDOM_NOTE}, {"accentChance", 0.25f}, {"durBase", 0.9f}}},
            {"Demo 14 Skipping Octaves", {{"scale", minPenta}, {"beatDiv", 4.0f}, {"seqSize", 10}, {"octave", 3}, {"skipSteps", 2},
                                          {"patternMode", PAT_ASC}, {"octaveDev", 0.3f}, {"octaveDevRng", 2}, {"durBase", 0.35f},
                                          {"positionRndm", 0.1f}, {"addBass", true}, {"bassPatternMode", BASS_EUCLID}, {"bassEucLen", 10},
                                          {"bassEucHits", 3}, {"bassOctave", -1}}},
        };
    }
    // Adds the demos not in the list yet; the number added
    int addDemoSnapshots(){
        int added = 0;
        for(const auto& [name, over] : demoOverrides()){
            if((int)snapshots.size() >= NUM_SNAPS) break;
            bool present = false;
            for(const auto& sn : snapshots) if(sn.name == name) present = true;
            if(present) continue;
            ofJson v = defaultValues;
            for(auto it = over.begin(); it != over.end(); ++it) v[it.key()] = it.value();
            snapshots.push_back({name, v});
            added++;
        }
        saveSnapshotFile();
        return added;
    }
    ofJson captureValues() const {
        ofJson v = ofJson::object();
        for(const auto& c : controls) if(c.key != "morphTime" && c.key != "voices" && c.key != "clockMode") v[c.key] = c.save();
        return v;
    }
    void storeSnapshot(int slot, const std::string& name){
        Snapshot s{name.empty() ? "Snapshot " + ofToString(slot + 1) : name, captureValues()};
        if(slot >= 0 && slot < (int)snapshots.size()) snapshots[slot] = s;
        else if((int)snapshots.size() < NUM_SNAPS) snapshots.push_back(s);
        saveSnapshotFile();
    }
    // the clock, the outputs and Morph Time stay as set
    static bool recalled(const std::string& key){ return key != "morphTime" && key != "voices" && key != "clockMode"; }
    void recallSnapshot(int slot){
        if(slot < 0 || slot >= (int)snapshots.size()) return;
        activeSnapshot = slot;
        const ofJson& v = snapshots[slot].values;
        if(morphTime.get() <= 0.0f){
            for(const auto& c : controls) if(v.contains(c.key) && recalled(c.key)) c.load(v[c.key]);
            return;
        }
        // numbers glide over Morph Time; the rest switch at its end
        morphFrom.clear();
        morphTarget = v;
        for(const auto& c : controls) if(c.get && recalled(c.key) && v.contains(c.key) && v[c.key].is_number()) morphFrom[c.key] = c.get();
        morphStart = ofGetElapsedTimef();
        morphing = true;
    }
    void updateMorph(){
        if(!morphing) return;
        const float t = std::clamp((ofGetElapsedTimef() - morphStart) / std::max(0.001f, morphTime.get()), 0.0f, 1.0f);
        for(const auto& c : controls){
            auto it = morphFrom.find(c.key);
            if(it == morphFrom.end() || !c.set) continue;
            c.set(it->second + (morphTarget[c.key].get<double>() - it->second) * t);
        }
        if(t >= 1.0f){
            morphing = false;
            for(const auto& c : controls) if(morphTarget.contains(c.key) && recalled(c.key)) c.load(morphTarget[c.key]);
        }
    }
    // The Polyphonic Arpeggiator GUI's snapshots (its units: older ones in ms)
    int importGuiSnapshots(){
        const std::string dirPath = ofToDataPath("nodeSnapshots/PolyphonicArpeggiatorGUI/", true);
        ofDirectory dir(dirPath);
        if(!dir.exists()) return 0;
        dir.allowExt("json");
        dir.listDir();
        std::vector<std::pair<int, std::string>> files;
        for(size_t i = 0; i < dir.size(); i++){
            const std::string name = dir.getName(i);
            if(name.rfind("snapshot_", 0) != 0) continue;
            files.push_back({ofToInt(name.substr(9)), dir.getPath(i)});
        }
        std::sort(files.begin(), files.end());
        int count = 0;
        for(const auto& [slot, path] : files){
            if((int)snapshots.size() >= NUM_SNAPS) break;
            ofJson j;
            try { j = ofLoadJson(path); } catch(...) { continue; }
            if(!j.is_object()) continue;
            ofJson v = ofJson::object();
            for(const auto& c : controls) if(j.contains(c.key)) v[c.key] = j[c.key];
            const float bd = std::max(0.001f, j.value("beatDiv", 1.0f));
            const float stepMs = 60000.0f / (120.0f * bd);
            auto num = [&](const char* k, float d){ return j.contains(k) && j[k].is_number() ? j[k].get<float>() : d; };
            if(num("durBase", 1) > 4 || num("durRndm", 0) > 4 || num("durEucStrength", 0) > 4){
                v["durBase"] = num("durBase", 1) / stepMs;
                v["durRndm"] = num("durRndm", 0) / stepMs;
                v["durEucStrength"] = num("durEucStrength", 0) / stepMs;
            }
            for(const char* k : {"strum", "strumRndm", "positionRndm"})
                if(num(k, 0) > 1) v[k] = std::clamp(num(k, 0) / stepMs, 0.0f, 1.0f);
            if(!j.contains("pulseMode")) v["pulseMode"] = arpbox::PULSE_EUCLID;
            if(!j.contains("bassPatternMode") && j.contains("bassOnAccent"))
                v["bassPatternMode"] = j.value("bassOnAccent", false) ? arpbox::BASS_VEL_ACCENT : arpbox::BASS_ALTERNATE;
            snapshots.push_back({j.value("name", "Imported " + ofToString(slot)), v});
            count++;
        }
        if(count) saveSnapshotFile();
        return count;
    }

    // ════════════════════════════════════════════════════════════════════════
    // Window
    // ════════════════════════════════════════════════════════════════════════

    static ImU32 voiceColor(int slot, float alpha = 1.0f){
        float r, g, b;
        if(slot >= NV) ImGui::ColorConvertHSVtoRGB(0.07f, 0.75f, 0.95f, r, g, b);
        else ImGui::ColorConvertHSVtoRGB(std::fmod(0.55f + slot * 0.083f, 1.0f), 0.5f, 0.95f, r, g, b);
        return ImGui::GetColorU32(ImVec4(r, g, b, alpha));
    }

    void drawWindow(){
        const std::string title = getWindowTitle();
        bool open = showWindow.get();
        ImGui::SetNextWindowSize(ImVec2(1180.0f, 860.0f), ImGuiCond_FirstUseEver);
        if(ImGui::Begin(title.c_str(), &open)){
            ImGui::PushID(this);
            drawHeader();
            const float w = std::max(400.0f, ImGui::GetContentRegionAvail().x);
            const float h = 270.0f;
            drawPianoRoll(w - h - 12.0f, h);
            ImGui::SameLine(0, 12);
            drawRing(h);
            ImGui::Spacing();
            if(ImGui::BeginTabBar("##abTabs")){
                const char* tabs[] = {"Rhythm", "Notes", "Dynamics & Bass", "Snapshots"};
                for(int i = 0; i < 4; i++){
                    if(ImGui::BeginTabItem(tabs[i])){
                        selectedTab = i;
                        switch(i){
                            case 0: drawRhythmTab(); break;
                            case 1: drawNotesTab(); break;
                            case 2: drawDynamicsTab(); break;
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

    // ── Widgets (right-click: type / publish) ───────────────────────────────
    bool sliderI(const std::string& key, ofParameter<int>& p, const char* label, int lo = INT_MIN, int hi = INT_MIN){
        int v = p.get();
        const int mn = lo == INT_MIN ? p.getMin() : lo, mx = hi == INT_MIN ? p.getMax() : hi;
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool ch = ImGui::SliderInt(("##" + key).c_str(), &v, mn, std::max(mn, mx), (std::string(label) + "  %d").c_str());
        if(ch) p = std::clamp(v, p.getMin(), p.getMax());
        controlMenu(key);
        return ch;
    }
    bool sliderF(const std::string& key, ofParameter<float>& p, const char* label, const char* fmt = "%.2f", bool logarithmic = false){
        float v = p.get();
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool ch = ImGui::SliderFloat(("##" + key).c_str(), &v, p.getMin(), p.getMax(),
                                           (std::string(label) + "  " + fmt).c_str(), logarithmic ? ImGuiSliderFlags_Logarithmic : 0);
        if(ch) p = v;
        controlMenu(key);
        return ch;
    }
    bool check(const std::string& key, ofParameter<bool>& p, const char* label){
        bool v = p.get();
        const bool ch = ImGui::Checkbox((std::string(label) + "##" + key).c_str(), &v);
        if(ch) p = v;
        controlMenu(key);
        return ch;
    }
    bool combo(const std::string& key, ofParameter<int>& p, const char* label, const std::vector<std::string>& options){
        const int v = std::clamp(p.get(), 0, (int)options.size() - 1);
        bool ch = false;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if(ImGui::BeginCombo(("##" + key).c_str(), (std::string(label) + ": " + options[v]).c_str())){
            for(int i = 0; i < (int)options.size(); i++) if(ImGui::Selectable(options[i].c_str(), i == v)){ p = i; ch = true; }
            ImGui::EndCombo();
        }
        controlMenu(key);
        return ch;
    }
    // A seed: typed, a dice, and "each cycle"
    void seedRow(const std::string& key, ofParameter<int>& s, const std::string& cycleKey, ofParameter<bool>* eachCycle){
        int v = s.get();
        ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - 120.0f));
        if(ImGui::InputInt(("##" + key).c_str(), &v, 1, 100)) s = std::clamp(v, s.getMin(), s.getMax());
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("0: no seed (new draws every cycle and every pass)\nElse: the same draws per step, every cycle (or a new set each cycle: Cycle)");
        controlMenu(key);
        ImGui::SameLine(0, 4);
        if(ImGui::SmallButton(("Dice##" + key).c_str())){
            static std::mt19937 rng((unsigned)ofGetElapsedTimeMicros());
            s = (int)(rng() % 1000000);
        }
        if(eachCycle){
            ImGui::SameLine(0, 4);
            bool e = eachCycle->get();
            if(ImGui::Checkbox(("Cycle##" + cycleKey).c_str(), &e)) *eachCycle = e;
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("A new seed every cycle (drawn from the node Seed)");
            controlMenu(cycleKey);
        }
    }
    static void heading(const char* t){ ImGui::TextDisabled("%s", t); ImGui::Separator(); }
    void columns(int count, const std::function<void(int)>& body){
        const float avail = ImGui::GetContentRegionAvail().x;
        const float gap = 14.0f, cw = (avail - gap * (count - 1)) / count;
        for(int k = 0; k < count; k++){
            if(k) ImGui::SameLine(0, gap);
            ImGui::BeginChild(("##col" + ofToString(k)).c_str(), ImVec2(cw, 0), false);
            body(k);
            ImGui::EndChild();
        }
    }

    // ── Header ──────────────────────────────────────────────────────────────
    struct DivPreset { const char* label; float value; };
    static const std::vector<DivPreset>& divPresets(){
        static const std::vector<DivPreset> d = {
            {"2 bars", 0.125f}, {"1 bar", 0.25f}, {"1/2 dotted", 1.0f / 3.0f}, {"1/2", 0.5f}, {"1/2 triplet", 0.75f},
            {"1/4 dotted", 2.0f / 3.0f}, {"1/4", 1.0f}, {"1/4 triplet", 1.5f}, {"1/8 dotted", 4.0f / 3.0f}, {"1/8", 2.0f},
            {"1/8 triplet", 3.0f}, {"1/16 dotted", 8.0f / 3.0f}, {"1/16", 4.0f}, {"1/16 triplet", 6.0f}, {"1/32 dotted", 16.0f / 3.0f},
            {"1/32", 8.0f}, {"1/32 triplet", 12.0f}, {"1/64", 16.0f}, {"1/64 triplet", 24.0f}, {"1/128", 32.0f}};
        return d;
    }
    std::string divLabel() const {
        for(const auto& d : divPresets()) if(std::abs(d.value - beatDiv.get()) < 0.001f) return d.label;
        return ofToString(beatDiv.get(), 3) + " /beat";
    }
    void drawHeader(){
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("ARPEGGIATORBOX");
        ImGui::SameLine(0, 16);
        int cm = clockMode.get();
        ImGui::SetNextItemWidth(150);
        if(ImGui::Combo("##abClock", &cm, "Clock: Trigger\0Clock: Transport\0Clock: Internal\0")) clockMode = cm;
        controlMenu("clockMode");
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Transport: the global transport\nInternal: its own clock (Clock BPM), runs when the transport is stopped\nTrigger: each Trigger / Trig In step");
        if(clockMode.get() == CLOCK_INTERNAL){
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110);
            float b = clockBpm.get();
            if(ImGui::DragFloat("##abBpm", &b, 0.25f, 20.0f, 300.0f, "%.1f BPM")) clockBpm = b;
            controlMenu("internalClockBpm");
        }
        if(clockMode.get() != CLOCK_TRIGGER){
            ImGui::SameLine();
            ImGui::SetNextItemWidth(130);
            if(ImGui::BeginCombo("##abDiv", ("Step " + divLabel()).c_str())){
                for(const auto& d : divPresets()) if(ImGui::Selectable(d.label, std::abs(d.value - beatDiv.get()) < 0.001f)) beatDiv = d.value;
                ImGui::EndCombo();
            }
            controlMenu("beatDiv");
        }else{
            ImGui::SameLine();
            if(ImGui::Button("Step")){ trigger.trigger(); }
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("One trigger (as the Trigger input)");
        }
        ImGui::SameLine(0, 16);
        int sd = seed.get();
        ImGui::SetNextItemWidth(110);
        if(ImGui::InputInt("##abSeed", &sd)) seed = std::clamp(sd, 0, 99999);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Seed: every free random draw (chances, deviations, run gate...)\n0: fresh draws on every pass");
        ImGui::SameLine(0, 4);
        if(ImGui::SmallButton("Dice")){ static std::mt19937 rng((unsigned)ofGetElapsedTimeMicros()); seed = 1 + (int)(rng() % 99999); }
        ImGui::SameLine(0, 16);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", voicesText().c_str());
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", voicesTip());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        int vm = voiceMode.get();
        if(ImGui::Combo("##abVM", &vm, "Voices: fixed\0Voices: round robin\0")) voiceMode = vm;
        controlMenu("voiceMode");
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Fixed: chord tone n on channel n (a long note is cut by the next)\nRound robin: notes longer than a step take the next voices (no cut)");
        ImGui::SameLine(0, 16);
        if(ImGui::Button("Reset")) doReset(true);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Step 0 on the next pulse (re-arms One Shot)");
        ImGui::SameLine();
        check("oneShotMode", oneShot, "One shot");
        ImGui::SameLine();
        check("trigAmp", trigAmp, "Trig amp");
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Trig outputs carry the note's velocity instead of 1");
        ImGui::SameLine(0, 16);
        ImGui::SetNextItemWidth(110);
        ImGui::SliderInt("##abView", &viewBeats, 2, 32, "View %d beats");
    }

    // ── Piano roll: what plays, scrolling past a fixed playhead ─────────────
    void drawPianoRoll(float width, float height){
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##abRoll", ImVec2(std::max(50.0f, width), height));
        dl->AddRectFilled(o, ImVec2(o.x + width, o.y + height), ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
        const bool trig = clockMode.get() == CLOCK_TRIGGER;
        // in steps (trigger mode) or beats
        const double span = trig ? viewBeats * 2.0 : (double)viewBeats;
        const double now = trig ? (double)triggerCount : positionBeat();
        const double start = now - span * 0.25, end = start + span;
        const double sb = trig ? 1.0 : engine.stepBeats();
        struct Bar { double s, e; float pitch, vel; int slot; bool accent, lng; };
        std::vector<Bar> bars;
        if(trig){
            for(int64_t n = std::max<int64_t>(0, (int64_t)std::floor(start) - 17); n < (int64_t)std::ceil(end); n++)
                for(const auto& note : notesOf({n, (double)n / engine.bd()}))
                    bars.push_back({(double)n + note.offset, (double)n + note.offset + note.dur, note.pitch, note.vel, slotOf(note), note.accent, note.longNote});
        }else{
            for(const auto& pn : placedIn(start - engine.maxDurSteps() * sb, end))
                bars.push_back({pn.start, pn.end, pn.note.pitch, pn.note.vel, pn.slot, pn.note.accent, pn.note.longNote});
        }
        float lo = 127.0f, hi = 0.0f;
        for(const auto& b : bars){ lo = std::min(lo, b.pitch); hi = std::max(hi, b.pitch); }
        if(lo > hi){ lo = 48.0f; hi = 72.0f; }
        lo = std::floor(lo) - 2.0f; hi = std::ceil(hi) + 2.0f;
        if(hi - lo < 24.0f){ const float c = (lo + hi) * 0.5f; lo = c - 12.0f; hi = c + 12.0f; }
        const float x0 = o.x + 30.0f, w = width - 34.0f, y0 = o.y + 16.0f, h = height - 20.0f;
        auto X = [&](double t){ return x0 + (float)((t - start) / span) * w; };
        auto Y = [&](float p){ return y0 + h - (p - lo) / (hi - lo) * h; };
        const float rowH = std::max(2.0f, h / (hi - lo));
        dl->PushClipRect(ImVec2(o.x, o.y), ImVec2(o.x + width, o.y + height), true);
        // octaves (C rows) and black keys
        for(int p = (int)lo; p <= (int)hi; p++){
            const int pc = arpbox::wrapI(p, 12);
            if(pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10)
                dl->AddRectFilled(ImVec2(x0, Y((float)p + 0.5f)), ImVec2(x0 + w, Y((float)p - 0.5f)), ImGui::GetColorU32(ImGuiCol_WindowBg, 0.35f));
            if(pc == 0){
                dl->AddLine(ImVec2(x0, Y((float)p - 0.5f)), ImVec2(x0 + w, Y((float)p - 0.5f)), ImGui::GetColorU32(ImGuiCol_Border, 0.6f));
                dl->AddText(ImVec2(o.x + 3, Y((float)p) - 7), ImGui::GetColorU32(ImGuiCol_TextDisabled), ("C" + ofToString(p / 12 - 1)).c_str());
            }
        }
        // the grid: beats and bars (steps and cycles in trigger mode)
        if(!trig){
            // run gate: closed windows shaded
            const double B = std::max(1.0f, runBeats.get());
            if(runChance.get() < 1.0f)
                for(int64_t win = engine.runWindow(start); win <= engine.runWindow(end) && win - engine.runWindow(start) < 256; win++){
                    const double ws = (double)win * B - runPhase.get() * B;
                    if(!engine.runGateOpen(ws + B * 0.5))
                        dl->AddRectFilled(ImVec2(std::max(x0, X(ws)), y0), ImVec2(std::min(x0 + w, X(ws + B)), y0 + h), IM_COL32(0, 0, 0, 70));
                }
            for(double beat = std::ceil(start); beat <= end; beat += 1.0){
                const double inBar = std::fmod(std::fmod(beat, bpb) + bpb, bpb);
                const bool bar = inBar < 1e-6 || bpb - inBar < 1e-6;
                dl->AddLine(ImVec2(X(beat), y0), ImVec2(X(beat), y0 + h), ImGui::GetColorU32(ImGuiCol_Border, bar ? 0.9f : 0.3f));
                if(bar) dl->AddText(ImVec2(X(beat) + 3, o.y + 1), ImGui::GetColorU32(ImGuiCol_TextDisabled), ofToString((int)std::lround(beat / bpb) + 1).c_str());
            }
        }else{
            for(double s = std::ceil(start); s <= end; s += 1.0)
                dl->AddLine(ImVec2(X(s), y0), ImVec2(X(s), y0 + h), ImGui::GetColorU32(ImGuiCol_Border, 0.25f));
        }
        // the notes
        for(const auto& b : bars){
            if(b.e < start || b.s > end) continue;
            const float alpha = b.s < now ? 0.45f : 1.0f;
            const float xa = X(b.s), xb = std::max(xa + 2.0f, X(b.e));
            const float yc = Y(b.pitch);
            const float hh = std::max(2.0f, rowH * (b.lng ? 0.95f : 0.7f) * 0.5f);
            dl->AddRectFilled(ImVec2(xa, yc - hh), ImVec2(xb, yc + hh), voiceColor(b.slot, (0.25f + 0.75f * b.vel) * alpha), 2.0f);
            if(b.accent) dl->AddRect(ImVec2(xa, yc - hh), ImVec2(xb, yc + hh), IM_COL32(255, 255, 255, (int)(200 * alpha)), 2.0f, 0, 1.5f);
            if(b.s <= now && now < b.e) dl->AddCircleFilled(ImVec2(xa, yc), 3.0f, IM_COL32(255, 255, 255, 230));
        }
        const float px = X(now);
        dl->AddLine(ImVec2(px, o.y), ImVec2(px, o.y + height), ImGui::GetColorU32(ImGuiCol_PlotLinesHovered), 1.5f);
        dl->PopClipRect();
        if(ImGui::IsItemHovered()){
            const ImVec2 m = ImGui::GetIO().MousePos;
            for(const auto& b : bars){
                if(m.x < X(b.s) || m.x > std::max(X(b.s) + 2.0f, X(b.e)) || std::abs(m.y - Y(b.pitch)) > std::max(3.0f, rowH * 0.5f)) continue;
                static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
                const int pi = (int)std::lround(b.pitch);
                ImGui::SetTooltip("%s%d (%.2f)  vel %.2f  %s %d%s%s", names[arpbox::wrapI(pi, 12)], pi / 12 - 1, b.pitch, b.vel,
                                  b.slot >= NV ? "bass" : "voice", b.slot >= NV ? b.slot - NV + 1 : b.slot + 1,
                                  b.accent ? "  accent" : "", b.lng ? "  long" : "");
                break;
            }
        }
    }

    // ── The cycle: its steps around a ring ───────────────────────────────────
    void drawRing(float size){
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##abRing", ImVec2(size, size));
        dl->AddRectFilled(o, ImVec2(o.x + size, o.y + size), ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
        const ImVec2 c(o.x + size * 0.5f, o.y + size * 0.5f);
        const int s = std::clamp(seqSize.get(), 1, arpbox::MAX_SEQ);
        // the pulse now and its cycle
        const int64_t n = nowPulse;
        const int64_t r = n - engine.origin;
        const int64_t cyc = engine.cycleOf(r);
        const auto cy = engine.cycle(cyc);   // a copy: notesOf may renew the engine's cache
        const int stepNow = engine.stepOf(r);
        std::vector<int> lg, la, ld;
        engine.lanes(n, lg, la, ld);
        const float R = size * 0.5f - 16.0f;
        auto at = [&](int i, float rad){
            const float a = -(float)PI * 0.5f + 2.0f * (float)PI * i / s;
            return ImVec2(c.x + std::cos(a) * rad, c.y + std::sin(a) * rad);
        };
        dl->AddCircle(c, R, ImGui::GetColorU32(ImGuiCol_Border, 0.4f), 64, 1.0f);
        dl->AddCircle(c, R * 0.62f, ImGui::GetColorU32(ImGuiCol_Border, 0.2f), 64, 1.0f);
        // the pitches each played pulse of this cycle gives (inside the ring)
        float lo = 127, hi = 0;
        std::vector<std::vector<std::pair<float, int>>> chord(s);
        for(size_t k = 0; k < cy.played.size(); k++){
            if(!cy.played[k]) continue;
            const int64_t pn = engine.origin + cy.r0 + (int64_t)k;
            for(const auto& note : notesOf({pn, clockMode.get() == CLOCK_TRIGGER ? (double)pn / engine.bd() : cy.beats[k]})){
                if(note.step < 0 || note.step >= (int)chord.size()) continue;   // the previous Seq Size's, until the engine syncs
                chord[note.step].push_back({note.pitch, slotOf(note)});
                lo = std::min(lo, note.pitch); hi = std::max(hi, note.pitch);
            }
        }
        if(hi < lo){ lo = 48; hi = 72; }
        if(hi - lo < 12){ hi = lo + 12; }
        for(int i = 0; i < s; i++){
            const ImVec2 q = at(i, R);
            const bool now = i == stepNow;
            if(lg[i]) dl->AddCircleFilled(q, la[i] ? 6.0f : 4.0f, ImGui::GetColorU32(ImGuiCol_PlotLines, la[i] ? 1.0f : 0.7f));
            else dl->AddCircleFilled(q, 2.0f, ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.6f));
            if(ld[i]){
                const float a0 = -(float)PI * 0.5f + 2.0f * (float)PI * (i - 0.3f) / s, a1 = -(float)PI * 0.5f + 2.0f * (float)PI * (i + 0.3f) / s;
                dl->PathArcTo(c, R + 7.0f, a0, a1, 8);
                dl->PathStroke(IM_COL32(255, 170, 70, 220), 0, 2.5f);
            }
            for(const auto& [pitch, slot] : chord[i]){
                const float rad = R * 0.2f + (R * 0.72f) * (pitch - lo) / (hi - lo);
                dl->AddCircleFilled(at(i, rad), 2.5f, voiceColor(slot));
            }
            if(now) dl->AddCircle(q, 8.0f, IM_COL32(255, 255, 255, 230), 16, 2.0f);
        }
        const std::string label = "step " + ofToString(stepNow + 1) + "/" + ofToString(s);
        const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
        const std::string sub = "cycle " + ofToString(cyc + 1) + (cy.shift ? "  shift " + ofToString(cy.shift) : "");
        const ImVec2 ss = ImGui::CalcTextSize(sub.c_str());
        dl->AddText(ImVec2(c.x - ss.x * 0.5f, c.y + ts.y * 0.6f), ImGui::GetColorU32(ImGuiCol_TextDisabled), sub.c_str());
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("This cycle: dots = pulses (big: accented), orange arcs = long notes,\ninner dots = the pitches each step plays (outward: higher)");
    }

    // A row of bars: drag to set (toggles: click), the current step marked
    bool barEditor(const char* id, std::vector<int>& vals, int lo, int hi, bool toggles, float height, int current, ImU32 color){
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x;
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id, ImVec2(std::max(20.0f, width), height));
        dl->AddRectFilled(o, ImVec2(o.x + width, o.y + height), ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
        const int n = std::max(1, (int)vals.size());
        const float bw = width / n;
        bool changed = false;
        if(ImGui::IsItemActive()){
            const ImVec2 m = ImGui::GetIO().MousePos;
            const int i = std::clamp((int)((m.x - o.x) / bw), 0, n - 1);
            if(toggles){
                if(ImGui::IsItemClicked()){ toggleValue = vals[i] > 0 ? 0 : 1; }
                if(vals[i] != toggleValue){ vals[i] = toggleValue; changed = true; }
            }else{
                const float t = std::clamp(1.0f - (m.y - o.y) / height, 0.0f, 1.0f);
                const int v = lo + (int)std::lround(t * (hi - lo));
                if(vals[i] != v){ vals[i] = v; changed = true; }
            }
        }
        for(int i = 0; i < n && i < (int)vals.size(); i++){
            const float t = hi > lo ? (float)(vals[i] - lo) / (float)(hi - lo) : 0.0f;
            const float xa = o.x + i * bw + 1, xb = o.x + (i + 1) * bw - 1;
            if(i == current) dl->AddRectFilled(ImVec2(xa - 1, o.y), ImVec2(xb + 1, o.y + height), IM_COL32(255, 255, 255, 30));
            dl->AddRectFilled(ImVec2(xa, o.y + height - std::max(toggles ? 0.0f : 1.0f, t * (height - 2))), ImVec2(xb, o.y + height), color);
        }
        if(ImGui::IsItemHovered() && !ImGui::IsItemActive()){
            const int i = std::clamp((int)((ImGui::GetIO().MousePos.x - o.x) / bw), 0, n - 1);
            ImGui::SetTooltip("step %d: %d", i + 1, i < (int)vals.size() ? vals[i] : 0);
        }
        return changed;
    }
    int toggleValue = 1;
    int currentStep(){ return engine.stepOf(nowPulse - engine.origin); }
    // The pulse playing now (the last one at or before the playhead)
    int64_t findPulseNow(){
        if(clockMode.get() == CLOCK_TRIGGER) return std::max<int64_t>(0, triggerCount - 1);
        const double now = positionBeat();
        auto ps = engine.pulsesIn(now - std::min(32.0, 64.0 * engine.stepBeats()), now + 1e-9);
        return ps.empty() ? engine.firstPulseFrom(now).n : ps.back().n;
    }
    void euclidRing(int len, int hits, int off, float size, ImU32 color){
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(size, size));
        const ImVec2 c(o.x + size * 0.5f, o.y + size * 0.5f);
        const float R = size * 0.5f - 6.0f;
        std::vector<char> pat(std::max(1, len), 0);
        if(len > 0 && hits > 0) for(int j = 0; j < std::min(hits, len); j++) pat[arpbox::wrapI((int64_t)j * len / std::min(hits, len) + off, len)] = 1;
        std::vector<ImVec2> poly;
        for(int i = 0; i < len; i++){
            const float a = -(float)PI * 0.5f + 2.0f * (float)PI * i / len;
            const ImVec2 q(c.x + std::cos(a) * R, c.y + std::sin(a) * R);
            if(pat[i]) poly.push_back(q);
            dl->AddCircleFilled(q, pat[i] ? 4.0f : 2.0f, pat[i] ? color : ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.6f));
        }
        if(poly.size() >= 2) dl->AddPolyline(poly.data(), (int)poly.size(), color, ImDrawFlags_Closed, 1.0f);
    }
    // A curve over 0..1 (the Geiger density, the bounce curve, the LFO)
    void plotCurve(const char* id, float height, const std::function<float(float)>& f, ImU32 color, const std::vector<float>& marks = {}){
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x;
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id, ImVec2(std::max(20.0f, width), height));
        dl->AddRectFilled(o, ImVec2(o.x + width, o.y + height), ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
        std::vector<ImVec2> pts;
        for(int i = 0; i <= 96; i++){
            const float x = i / 96.0f;
            pts.push_back(ImVec2(o.x + x * width, o.y + height - 2 - std::clamp(f(x), 0.0f, 1.0f) * (height - 4)));
        }
        dl->AddPolyline(pts.data(), (int)pts.size(), color, 0, 1.5f);
        for(float m : marks) dl->AddLine(ImVec2(o.x + m * width, o.y + height - 6), ImVec2(o.x + m * width, o.y + height), IM_COL32(255, 255, 255, 200));
    }

    // ── Rhythm ──────────────────────────────────────────────────────────────
    void rhythmCol(int k){
        {
            if(k == 0){
                heading("Clock");
                combo("clockMode", clockMode, "Clock", clockNames());
                if(clockMode.get() == CLOCK_INTERNAL) sliderF("internalClockBpm", clockBpm, "BPM", "%.1f");
                if(clockMode.get() != CLOCK_TRIGGER){
                    float bd = beatDiv.get();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if(ImGui::BeginCombo("##abDiv2", ("Step: " + divLabel()).c_str())){
                        for(const auto& d : divPresets()) if(ImGui::Selectable(d.label, std::abs(d.value - bd) < 0.001f)) beatDiv = d.value;
                        ImGui::EndCombo();
                    }
                    controlMenu("beatDiv");
                }else{
                    ImGui::TextWrapped("Trigger: Trigger bangs, a rising Trig In value or an audio cable into Trig In (sample accurate). "
                                       "Lengths and strums scale with the time between triggers.");
                    sliderF("beatDiv", beatDiv, "Steps per beat", "%.3f", true);
                    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Run gate windows and the velocity LFO count steps / this as beats");
                }
                sliderI("skipSteps", skipSteps, "Skip");
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("Each pulse advances 1 + Skip steps");
                check("oneShotMode", oneShot, "One shot");
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("Plays one cycle after each Reset");
                ImGui::SameLine();
                if(ImGui::SmallButton("Reset")) doReset(true);
                ImGui::SameLine();
                if(ImGui::SmallButton("Reset next")) doReset(false);
                ImGui::Spacing();
                heading("Chances");
                sliderF("stepChance", stepChance, "Step chance");
                sliderF("noteChance", noteChance, "Note chance");
                sliderF("seqProb", seqProb, "Seq chance");
                sliderF("seqProbCycles", seqProbCycles, "Mute cycles", "%.2f", true);
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("Seq chance < 1 mutes blocks this many cycles long");
            }else if(k == 1){
                heading("Pulses");
                combo("pulseMode", pulseMode, "Pulses", pulseNames());
                const int pm = pulseMode.get();
                const ImU32 col = ImGui::GetColorU32(ImGuiCol_PlotLines);
                if(pm == arpbox::PULSE_EUCLID){
                    sliderI("eucHits", eucHits, "Hits", 0, eucLen.get());
                    sliderI("eucLen", eucLen, "Len");
                    sliderI("eucOff", eucOff, "Offset", 0, std::max(0, eucLen.get() - 1));
                    euclidRing(eucLen.get(), eucHits.get(), eucOff.get(), 120.0f, col);
                }else if(pm == arpbox::PULSE_STEPSEQ){
                    ImGui::TextDisabled("Chance per step (drag)");
                    auto v = pulseSteps.get();
                    if(barEditor("##abPulseSteps", v, 0, 100, false, 90.0f, currentStep(), col)) pulseSteps = v;
                    if(ImGui::SmallButton("All")) pulseSteps = std::vector<int>(v.size(), 100);
                    ImGui::SameLine();
                    if(ImGui::SmallButton("Half")) pulseSteps = std::vector<int>(v.size(), 50);
                    ImGui::SameLine();
                    if(ImGui::SmallButton("Random")){ static std::mt19937 rng(7); for(auto& x : v) x = (int)(rng() % 101); pulseSteps = v; }
                }else if(pm == arpbox::PULSE_GEIGER){
                    sliderF("geigerDensity", geigerDensity, "Density");
                    sliderF("geigerSpeed", geigerSpeed, "Speed", "%.2f", true);
                    sliderF("geigerPeriodicity", geigerPeriodicity, "Periodicity");
                    ImGui::TextDisabled("Pulse rate over the next 8 beats");
                    const double now = positionBeat();
                    std::vector<float> marks;
                    if(clockMode.get() != CLOCK_TRIGGER) for(const auto& pl : engine.pulsesIn(now, now + 8.0)) marks.push_back((float)((pl.beat - now) / 8.0));
                    double peak = 1e-9;
                    for(int i = 0; i <= 64; i++) peak = std::max(peak, engine.geigerProbability(now + i / 8.0));
                    plotCurve("##abGeiger", 70.0f, [&](float x){ return (float)(engine.geigerProbability(now + x * 8.0) / peak); }, col, marks);
                    if(geigerPeriodicity.get() >= 0.999f) ImGui::TextDisabled("Periodicity 1: locked to the steps");
                }else if(pm == arpbox::PULSE_BOUNCE){
                    sliderF("bouncingBallSpeed", bounceSpeed, "Pulses / step", "%.2f", true);
                    sliderF("bouncingBallCurve", bounceCurve, "Curve");
                    check("bouncingBallSymmetric", bounceSym, "Symmetric");
                    ImGui::TextDisabled("Density over the cycle (and its pulses)");
                    std::vector<float> marks;
                    if(clockMode.get() != CLOCK_TRIGGER){
                        const double len = engine.cycleBeats();
                        for(const auto& pl : engine.pulsesIn(0.0, len)) marks.push_back((float)(pl.beat / len));
                    }
                    double peak = 1e-9;
                    for(int i = 0; i <= 64; i++) peak = std::max(peak, engine.bounceDensity(i / 64.0));
                    plotCurve("##abBounce", 70.0f, [&](float x){ return (float)(engine.bounceDensity(x) / peak); }, col, marks);
                }else{
                    ImGui::TextDisabled("A pulse on every step");
                }
            }else{
                heading("Run gate");
                sliderF("runGateBeats", runBeats, "Window (beats)", "%.1f", true);
                sliderF("runGateChance", runChance, "Chance");
                sliderF("runGatePhase", runPhase, "Phase");
                ImGui::TextDisabled("The next 16 windows (dark: muted)");
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float width = ImGui::GetContentRegionAvail().x;
                const ImVec2 o = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(width, 22));
                const double now = clockMode.get() == CLOCK_TRIGGER ? (double)triggerCount / engine.bd() : positionBeat();
                const int64_t w0 = engine.runWindow(now);
                const double B = std::max(1.0f, runBeats.get());
                for(int i = 0; i < 16; i++){
                    const double mid = (double)(w0 + i) * B - runPhase.get() * B + B * 0.5;
                    const bool on = engine.runGateOpen(mid);
                    const float xa = o.x + width * i / 16.0f + 1, xb = o.x + width * (i + 1) / 16.0f - 1;
                    dl->AddRectFilled(ImVec2(xa, o.y), ImVec2(xb, o.y + 20), on ? ImGui::GetColorU32(ImGuiCol_PlotLines, 0.8f) : IM_COL32(40, 40, 46, 255), 2.0f);
                }
                const double frac = std::fmod(std::fmod((now + runPhase.get() * B) / B, 1.0) + 1.0, 1.0);
                const float px = o.x + width * (float)frac / 16.0f;
                dl->AddLine(ImVec2(px, o.y - 2), ImVec2(px, o.y + 22), IM_COL32(255, 255, 255, 230), 1.5f);
                ImGui::Spacing();
                ImGui::TextWrapped("Each window plays or not (Chance), drawn from the Seed: the same window always decides the same.");
            }
        }
    }

    // ── Notes: the source and its pitch mapping ─────────────────────────────
    void keyboard(const char* id, const std::vector<float>& lit, float height, bool toggleScale){
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float width = ImGui::GetContentRegionAvail().x;
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id, ImVec2(std::max(20.0f, width), height));
        int lo = 127, hi = 0;
        for(float v : lit){ lo = std::min(lo, (int)std::floor(v)); hi = std::max(hi, (int)std::ceil(v)); }
        if(toggleScale){ lo = 0; hi = 11; }
        if(lo > hi){ lo = 48; hi = 71; }
        lo = (lo / 12) * 12; if(lo < 0) lo -= 12; hi = std::max(hi, lo + 11); hi = ((hi / 12) + 1) * 12 - 1;
        const int n = hi - lo + 1;
        const float kw = width / n;
        auto isLit = [&](int p){ for(float v : lit) if(std::abs(v - p) < 0.01f) return true; return false; };
        for(int p = lo; p <= hi; p++){
            const int pc = arpbox::wrapI(p, 12);
            const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
            const float xa = o.x + (p - lo) * kw, xb = xa + kw - 1;
            const float yb = o.y + (black ? height * 0.65f : height);
            dl->AddRectFilled(ImVec2(xa, o.y), ImVec2(xb, yb), black ? IM_COL32(30, 32, 40, 255) : IM_COL32(225, 222, 215, 255), 2.0f);
            if(isLit(p)) dl->AddRectFilled(ImVec2(xa + 1, o.y + 1), ImVec2(xb - 1, yb - 1), ImGui::GetColorU32(ImGuiCol_PlotLines, 0.85f), 2.0f);
            if(pc == 0) dl->AddText(ImVec2(xa + 2, o.y + height - 14), IM_COL32(60, 60, 70, 255), ("C" + ofToString(p / 12 - 1)).c_str());
        }
        if(toggleScale && ImGui::IsItemClicked()){
            const int p = lo + std::clamp((int)((ImGui::GetIO().MousePos.x - o.x) / kw), 0, n - 1);
            std::vector<float> s = scale.get();
            auto it = std::find_if(s.begin(), s.end(), [&](float v){ return std::abs(v - p) < 0.01f; });
            if(it != s.end()){ if(s.size() > 1) s.erase(it); }
            else { s.push_back((float)p); std::sort(s.begin(), s.end()); }
            scale = s;
        }
    }
    void notesCol(int k){
        static const std::vector<std::pair<const char*, std::vector<float>>> scales = {
            {"Major", {0, 2, 4, 5, 7, 9, 11}}, {"Minor", {0, 2, 3, 5, 7, 8, 10}}, {"Dorian", {0, 2, 3, 5, 7, 9, 10}},
            {"Phrygian", {0, 1, 3, 5, 7, 8, 10}}, {"Lydian", {0, 2, 4, 6, 7, 9, 11}}, {"Mixolydian", {0, 2, 4, 5, 7, 9, 10}},
            {"Harmonic minor", {0, 2, 3, 5, 7, 8, 11}}, {"Melodic minor", {0, 2, 3, 5, 7, 9, 11}}, {"Pentatonic", {0, 2, 4, 7, 9}},
            {"Minor pentatonic", {0, 3, 5, 7, 10}}, {"Blues", {0, 3, 5, 6, 7, 10}}, {"Whole tone", {0, 2, 4, 6, 8, 10}},
            {"Chromatic", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}}, {"Major triad", {0, 4, 7}}, {"Minor triad", {0, 3, 7}},
            {"Maj7", {0, 4, 7, 11}}, {"Min7", {0, 3, 7, 10}}, {"Dom7", {0, 4, 7, 10}}};
        {
            if(k == 0){
                heading("Source");
                combo("sourceMode", sourceMode, "Source", {"Scale", "Chord pool"});
                if(linkTimed && linkChained)
                    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1.0f), "Linked to a ChordBox through other nodes: what they make of each chord, learned, on the beat%s",
                                       linkRoot ? " (and its Root)" : "");
                else if(linkTimed) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1.0f), "Linked to a ChordBox: its chords, exactly on the beat%s",
                                                 linkRoot ? " (and its Root)" : "");
                else if(linkedKey(getOceanodeParameter(notePool)) || linkedKey(getOceanodeParameter(scale)))
                    ImGui::TextDisabled("A ChordBox in Index order: its values as they come");
                if(sourceMode.get() == arpbox::SRC_SCALE){
                    std::string cur = "Custom";
                    for(const auto& sc : scales) if(sc.second == scale.get()) cur = sc.first;
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if(ImGui::BeginCombo("##abScale", ("Scale: " + cur).c_str())){
                        for(const auto& sc : scales) if(ImGui::Selectable(sc.first, cur == sc.first)) scale = sc.second;
                        ImGui::EndCombo();
                    }
                    controlMenu("scale");
                    ImGui::TextDisabled("Click a key: in / out of the scale");
                    keyboard("##abScaleKeys", scale.get(), 54.0f, true);
                    ImGui::TextDisabled("Scale values are semitones (the Scale input takes any)");
                }else{
                    ImGui::TextDisabled("NotePool (the input), as played");
                    keyboard("##abPoolKeys", linkTimed ? engine.linkedSource(positionBeat()) : engine.latestSource(), 54.0f, false);
                    check("sortPool", sortPool, "Sort");
                    ImGui::SameLine();
                    check("removeDuplicates", dedupPool, "Remove duplicates");
                }
                combo("sourceChangeMode", sourceChange, "On change", changeNames());
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("When the scale / pool changes:\nKeep phase: from the next pulse, the pattern runs on\n"
                                                             "Reset pattern: from the next pulse, at step 0\nNext cycle: when the next cycle starts");
            }else if(k == 1){
                heading("Pitch");
                {
                    float r = root.get();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if(ImGui::SliderFloat("##abRoot", &r, root.getMin(), root.getMax(), "Root (bass)  %.0f")) root = r;
                    controlMenu("root");
                }
                sliderI("octave", octave, "Octave");
                sliderI("transpose", transpose, "Transpose");
                sliderI("octaveFold", octaveFold, "Fold (octaves)");
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("Pitches fold back into this many octaves above Octave (0: off)");
                check("foldPoly", foldPoly, "Fold the chord tones too");
                check("pitchExpand", pitchExpand, "Expand");
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("Beyond the source: each pass adds Expand Step semitones");
                if(pitchExpand.get()) sliderI("expandStep", expandStep, "Expand step");
                ImGui::Spacing();
                ImGui::TextDisabled("Pitch = source + Transpose + Octave x 12");
            }else{
                heading("Deviations (per note)");
                sliderF("octaveDev", octaveDev, "Octave up chance");
                sliderI("octaveDevRng", octaveDevRng, "Octaves");
                sliderF("idxDev", idxDev, "Index chance");
                sliderI("idxDevRng", idxDevRng, "Index range");
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("The note moves up to this many source notes");
                sliderF("pitchDev", pitchDev, "Semitone chance");
                sliderI("pitchDevRng", pitchDevRng, "Semitones");
                ImGui::TextDisabled("Deviation seed");
                seedRow("devSeed", devSeed, "devRandomizeSeedEachCycle", &devCycle);
            }
        }
    }

    // ── Pattern ─────────────────────────────────────────────────────────────
    void patternCol(int k){
        {
            if(k == 0){
                heading("Sequence");
                sliderI("seqSize", seqSize, "Seq size");
                combo("patternMode", patternMode, "Pattern", patternNames());
                if(patternMode.get() == arpbox::PAT_RANDOM){
                    ImGui::TextDisabled("Pattern seed");
                    seedRow("patternSeed", patternSeed, "patternRandomizeSeedEachCycle", &patternRndCycle);
                }
                sliderI("modulo", modulo, "Modulo");
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("The pattern travels this many positions (0: Seq size)");
                sliderI("sourceStart", sourceStart, "Source start");
                sliderI("sourceStride", sourceStride, "Stride");
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("Source notes per pattern position");
                check("gateAdvance", gateAdvance, "Advance on played pulses");
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("On: the pattern moves only when a note plays (rests do not skip pitches)\nOff: with every step");
                check("accentOnsetMode", byGate, "Accents / long notes by gate");
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("On: accent and long-note rules count played pulses\nOff: every pulse");
            }else if(k == 1){
                heading("Shift");
                sliderI("stepShift", stepShift, "Step shift");
                sliderF("rndShiftChance", rndShiftChance, "Random shift chance");
                sliderI("rndShiftRange", rndShiftRange, "Range");
                sliderI("rndShiftQuant", rndShiftQuant, "Quantize");
                ImGui::TextWrapped("Each cycle may shift its patterns by a random amount (up to Range, in multiples of Quantize).");
            }else{
                const int s = std::clamp(seqSize.get(), 1, arpbox::MAX_SEQ);
                if(patternMode.get() == arpbox::PAT_USER){
                    heading("User pattern (drag: the position per step)");
                    auto v = idxPattern.get();
                    if(barEditor("##abIdx", v, 0, std::max(1, s - 1), false, 110.0f, currentStep(), ImGui::GetColorU32(ImGuiCol_PlotLines))) idxPattern = v;
                    if(ImGui::SmallButton("Up")){ for(int i = 0; i < (int)v.size(); i++) v[i] = i % s; idxPattern = v; }
                    ImGui::SameLine();
                    if(ImGui::SmallButton("Down")){ for(int i = 0; i < (int)v.size(); i++) v[i] = s - 1 - i % s; idxPattern = v; }
                    ImGui::SameLine();
                    if(ImGui::SmallButton("Shuffle")){ static std::mt19937 rng(3); std::shuffle(v.begin(), v.end(), rng); idxPattern = v; }
                    controlMenu("idxPattern");
                }else{
                    heading("This cycle's pitches per step");
                    std::vector<int> v(s, 0);
                    int lo = 127, hi = 0;
                    std::vector<float> top(s, -1.0f);
                    // from the cycle playing now
                    const int64_t n = nowPulse;
                    const auto cy = engine.cycle(engine.cycleOf(n - engine.origin));
                    for(size_t i = 0; i < cy.played.size(); i++){
                        if(!cy.played[i]) continue;
                        const int64_t pn = engine.origin + cy.r0 + (int64_t)i;
                        for(const auto& note : notesOf({pn, clockMode.get() == CLOCK_TRIGGER ? (double)pn / engine.bd() : cy.beats[i]})){
                            // (a cached note can still have the previous Seq Size's step)
                            if(note.bass || note.tone != 0 || note.step < 0 || note.step >= s) continue;
                            top[note.step] = note.pitch;
                            lo = std::min(lo, (int)note.pitch); hi = std::max(hi, (int)note.pitch);
                        }
                    }
                    for(int i = 0; i < s; i++) v[i] = top[i] < 0 ? lo - 1 : (int)top[i];
                    barEditor("##abPitchView", v, lo - 1, std::max(hi, lo), false, 110.0f, currentStep(), ImGui::GetColorU32(ImGuiCol_PlotLines, 0.7f));
                    ImGui::TextDisabled("(the first chord tone; edit with Pattern: User)");
                }
            }
        }
    }

    // ── Voices & bass ───────────────────────────────────────────────────────
    void voicesCol(int k){
        {
            if(k == 0){
                heading("Chord");
                sliderI("polyphony", polyphony, "Polyphony");
                sliderI("polyInterval", polyInterval, "Interval");
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("Source positions between chord tones (2 in a scale: thirds)");
                sliderI("polyAccent", polyAccent, "Extra on accents");
                ImGui::Spacing();
                heading("Outputs");
                combo("voiceMode", voiceMode, "Voice mode", voiceModeNames());
                ImGui::TextDisabled("%s", voicesText().c_str());
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", voicesTip());
                check("trigAmp", trigAmp, "Trig carries velocity");
            }else if(k == 1){
                heading("Bass");
                check("addBass", addBass, "Add bass");
                combo("bassPatternMode", bassMode, "Pattern", bassNames());
                const int bm = bassMode.get();
                if(bm == arpbox::BASS_ALTERNATE){ sliderI("bassAlternateSteps", bassEvery, "Every"); sliderI("bassAlternateShift", bassShift, "Shift"); }
                if(bm == arpbox::BASS_EUCLID){
                    sliderI("bassEucHits", bassEucHits, "Hits", 0, bassEucLen.get());
                    sliderI("bassEucLen", bassEucLen, "Len");
                    sliderI("bassEucOff", bassEucOff, "Offset", 0, std::max(0, bassEucLen.get() - 1));
                    euclidRing(bassEucLen.get(), bassEucHits.get(), bassEucOff.get(), 90.0f, voiceColor(NV));
                }
                if(bm == arpbox::BASS_VEL_ACCENT || bm == arpbox::BASS_DUR_ACCENT) check("bassPatternInvert", bassInvert, "Invert");
                sliderF("bassProb", bassProb, "Chance");
                sliderI("bassOctave", bassOctave, "Octave");
                check("bassPitchModified", bassPitchMod, "Follows Octave");
                sliderI("bassPolyphony", bassPoly, "Octaves stacked");
                sliderF("bassVelBase", bassVel, "Velocity");
                sliderF("bassVelRndm", bassVelRndm, "Vel random");
                ImGui::TextDisabled("Pitch: Root + Transpose + octaves");
            }else{
                heading("Bass octave jumps");
                sliderI("bassOctRnd", bassOctRnd, "Range (octaves)");
                sliderF("bassOctRndChance", bassOctRndChance, "Chance");
                ImGui::TextDisabled("Seed");
                seedRow("bassOctRndSeed", bassOctRndSeed, "bassOctRndRandomizeSeedEachCycle", &bassOctRndCycle);
            }
        }
    }

    // ── Dynamics: velocity, accents, durations ──────────────────────────────
    void eventControls(bool acc){
        ofParameter<int>& mode = acc ? accMode : durMode;
        const std::string p = acc ? "accent" : "duration";
        combo(acc ? "accentPatternMode" : "durationPatternMode", mode, acc ? "Accents" : "Long notes", eventNames());
        switch(mode.get()){
            case arpbox::EV_ALTERNATE:
                sliderI(acc ? "accentAlternateSteps" : "durationAlternateSteps", acc ? accEvery : durEvery, "Every");
                sliderI(acc ? "accentAlternateShift" : "durationAlternateShift", acc ? accShift : durShift, "Shift");
                break;
            case arpbox::EV_EUCLID:
                sliderI(acc ? "eucAccHits" : "eucDurHits", acc ? accHits : durHits, "Hits", 0, (acc ? accLen : durLen).get());
                sliderI(acc ? "eucAccLen" : "eucDurLen", acc ? accLen : durLen, "Len");
                sliderI(acc ? "eucAccOff" : "eucDurOff", acc ? accOff : durOff, "Offset", 0, std::max(0, (acc ? accLen : durLen).get() - 1));
                break;
            case arpbox::EV_RANDOM_STEP:
            case arpbox::EV_RANDOM_NOTE:
                sliderF(acc ? "accentChance" : "durationChance", acc ? accChance : durChance, "Chance");
                seedRow(acc ? "accentSeed" : "durationSeed", acc ? accSeed : durSeed,
                        acc ? "accentRandomizeSeedEachCycle" : "durationRandomizeSeedEachCycle", acc ? &accRndCycle : &durRndCycle);
                break;
            default: {
                ImGui::TextDisabled("Click steps on / off");
                auto v = (acc ? accSteps : durSteps).get();
                if(barEditor(acc ? "##abAccSteps" : "##abDurSteps", v, 0, 1, true, 36.0f, -1, acc ? IM_COL32(255, 255, 255, 200) : IM_COL32(255, 170, 70, 220)))
                    (acc ? accSteps : durSteps) = v;
                controlMenu(acc ? "accentStepPattern" : "durationStepPattern");
                break;
            }
        }
        // the lane this cycle
        std::vector<int> lg, la, ld;
        engine.lanes(nowPulse, lg, la, ld);
        auto lane = acc ? la : ld;
        barEditor(acc ? "##abAccLane" : "##abDurLane", lane, 0, 1, true, 10.0f, currentStep(), acc ? IM_COL32(255, 255, 255, 140) : IM_COL32(255, 170, 70, 160));
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("This cycle (by gate: counted on played pulses)");
        (void)p;
    }
    void dynamicsCol(int k){
        {
            if(k == 0){
                heading("Velocity");
                sliderF("velBase", velBase, "Base");
                sliderF("velRndm", velRndm, "Random per note");
                seedRow("velRndSeed", velRndSeed, "velRndRandomizeSeedEachCycle", &velRndCycle);
                sliderF("velStepRndm", velStepRndm, "Random per step");
                seedRow("velStepRndSeed", velStepRndSeed, "velStepRndRandomizeSeedEachCycle", &velStepRndCycle);
                ImGui::TextDisabled("Curve over the sequence (draw; added)");
                auto cv = velCurve.get();
                std::vector<int> iv(cv.size());
                for(size_t i = 0; i < cv.size(); i++) iv[i] = (int)std::lround(cv[i] * 100.0f);
                const int s = std::max(1, seqSize.get());
                const int cur = s <= 1 ? 0 : (int)std::lround((double)currentStep() / (s - 1) * (arpbox::CURVE_RES - 1));
                if(barEditor("##abCurve", iv, 0, 100, false, 60.0f, cur, ImGui::GetColorU32(ImGuiCol_PlotLines, 0.8f))){
                    for(size_t i = 0; i < cv.size(); i++) cv[i] = iv[i] / 100.0f;
                    velCurve = cv;
                }
                controlMenu("velocityCurve");
                auto shape = [&](const char* label, const std::function<float(float)>& f){
                    if(ImGui::SmallButton(label)){
                        std::vector<float> c(arpbox::CURVE_RES);
                        for(int i = 0; i < arpbox::CURVE_RES; i++) c[i] = std::clamp(f(i / (float)(arpbox::CURVE_RES - 1)), 0.0f, 1.0f);
                        velCurve = c;
                    }
                    ImGui::SameLine(0, 4);
                };
                shape("Flat", [](float){ return 0.0f; });
                shape("Rise", [](float x){ return 0.3f * x; });
                shape("Fall", [](float x){ return 0.3f * (1 - x); });
                shape("Arch", [](float x){ return 0.3f * std::sin(x * (float)PI); });
                shape("S", [](float x){ return 0.3f / (1.0f + std::exp(-10.0f * (x - 0.5f))); });
                ImGui::NewLine();
            }else if(k == 1){
                heading("Velocity LFO (added)");
                combo("velocityLfoShape", lfoShape, "Shape", lfoNames());
                sliderF("velocitySlowRndm", lfoDepth, "Depth");
                sliderF("velocitySlowSpeed", lfoSpeed, "Cycles / beat", "%.3f", true);
                sliderF("velocityLfoPhase", lfoPhase, "Phase");
                if(lfoShape.get() == arpbox::LFO_PULSE) sliderF("velocityLfoPulseWidth", lfoWidth, "Width");
                if(lfoShape.get() == arpbox::LFO_SLOWRANDOM){ ImGui::TextDisabled("Seed"); seedRow("velocitySlowSeed", lfoSeed, "", nullptr); }
                ImGui::TextDisabled("The next 8 beats");
                const double now = clockMode.get() == CLOCK_TRIGGER ? (double)triggerCount / engine.bd() : positionBeat();
                plotCurve("##abLfo", 60.0f, [&](float x){ return engine.lfoWave(now + x * 8.0); }, ImGui::GetColorU32(ImGuiCol_PlotLines));
            }else if(k == 2){
                heading("Accents");
                eventControls(true);
                sliderF("eucAccStrength", accStrength, "Velocity added");
                sliderI("polyAccent", polyAccent, "Extra chord tones");
            }else{
                heading("Durations (steps)");
                sliderF("durBase", durBase, "Length");
                sliderF("durRndm", durRndm, "Random added");
                check("durationRndPerStep", durRndPerStep, "One random per step");
                eventControls(false);
                sliderF("durEucStrength", durStrength, "Long: steps added");
            }
        }
    }

    // ── Timing: strum, swing, position ──────────────────────────────────────
    void timingCol(int k){
        {
            if(k == 0){
                heading("Strum");
                sliderF("strum", strum, "Strum (steps per note)", "%.3f");
                combo("strumDir", strumDir, "Direction", strumNames());
                if(strumDir.get() == arpbox::STRUM_RANDOM){ ImGui::TextDisabled("Direction seed"); seedRow("strumDirSeed", strumDirSeed, "strumDirRandomizeSeedEachCycle", &strumDirCycle); }
                sliderF("strumRndm", strumRndm, "Random");
                if(strumRndm.get() > 0.0f) seedRow("strumRndSeed", strumRndSeed, "strumRndRandomizeSeedEachCycle", &strumRndCycle);
            }else if(k == 1){
                heading("Swing and position");
                sliderF("swing", swing, "Swing");
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("Odd steps start later (up to half a step)");
                sliderF("positionRndm", positionRndm, "Random delay (steps)");
                if(positionRndm.get() > 0.0f) seedRow("positionRndSeed", positionRndSeed, "positionRndRandomizeSeedEachCycle", &positionRndCycle);
            }else{
                heading("One step, as it plays");
                // the notes of the next played pulse: their delays across the step
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float width = ImGui::GetContentRegionAvail().x, height = 120.0f;
                const ImVec2 o = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(width, height));
                dl->AddRectFilled(o, ImVec2(o.x + width, o.y + height), ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
                const float span = 2.0f;   // steps shown
                for(int i = 0; i <= 2; i++) dl->AddLine(ImVec2(o.x + width * i / span, o.y), ImVec2(o.x + width * i / span, o.y + height), ImGui::GetColorU32(ImGuiCol_Border, 0.5f));
                std::vector<arpbox::Note> notes;
                int64_t n = clockMode.get() == CLOCK_TRIGGER ? triggerCount : engine.firstPulseFrom(positionBeat()).n;
                for(int k2 = 0; k2 < 64 && notes.empty(); k2++, n++)
                    notes = notesOf({n, clockMode.get() == CLOCK_TRIGGER ? (double)n / engine.bd() : engine.pulseBeat(n)});
                const int count = std::max(1, (int)notes.size());
                for(int i = 0; i < (int)notes.size(); i++){
                    const auto& nt = notes[i];
                    const float y = o.y + 8 + (height - 16) * (i + 0.5f) / count;
                    const float xa = o.x + (float)std::min(1.0, nt.offset / span) * width;
                    const float xb = o.x + (float)std::min(1.0, (nt.offset + nt.dur) / span) * width;
                    dl->AddLine(ImVec2(xa, y), ImVec2(std::max(xa + 2, xb), y), voiceColor(slotOf(nt), 0.5f + 0.5f * nt.vel), 4.0f);
                    dl->AddCircleFilled(ImVec2(xa, y), 3.5f, voiceColor(slotOf(nt)));
                }
                ImGui::TextDisabled("Two steps wide: each line a note, from its delay");
            }
        }
    }

    // ── The tabs: five columns each, short sections stacked ─────────────────
    // Rhythm: clock, chances and pulses, run gate, strum and swing, one step
    void drawRhythmTab(){
        columns(5, [&](int k){
            if(k < 3) rhythmCol(k);
            else if(k == 3){ timingCol(0); ImGui::Spacing(); timingCol(1); }
            else timingCol(2);
        });
    }
    // Notes: source, pitch and deviations, chord and outputs, sequence and shift, the pattern
    void drawNotesTab(){
        columns(5, [&](int k){
            switch(k){
                case 0: notesCol(0); break;
                case 1: notesCol(1); ImGui::Spacing(); notesCol(2); break;
                case 2: voicesCol(0); break;
                case 3: patternCol(0); ImGui::Spacing(); patternCol(1); break;
                default: patternCol(2); break;
            }
        });
    }
    // Dynamics & Bass: velocity, its LFO and accents, durations, bass, bass octave jumps
    void drawDynamicsTab(){
        columns(5, [&](int k){
            switch(k){
                case 0: dynamicsCol(0); break;
                case 1: dynamicsCol(1); ImGui::Spacing(); dynamicsCol(2); break;
                case 2: dynamicsCol(3); break;
                case 3: voicesCol(1); break;
                default: voicesCol(2); break;
            }
        });
    }

    // ── Snapshots ───────────────────────────────────────────────────────────
    void drawSnapshotsTab(){
        columns(2, [&](int k){
            if(k == 0){
                heading("Snapshots (shared by every ArpeggiatorBox)");
                ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - 150.0f));
                const bool enter = ImGui::InputTextWithHint("##abSnapName", "name", snapName.data(), snapName.size(), ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if(ImGui::Button("Store") || enter){
                    // a name in the list: replaced; else a new one
                    const std::string name(snapName.data());
                    int slot = -1;
                    for(int i = 0; i < (int)snapshots.size(); i++) if(!name.empty() && snapshots[i].name == name) slot = i;
                    storeSnapshot(slot, name);
                    activeSnapshot = slot >= 0 ? slot : (int)snapshots.size() - 1;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%d/%d", (int)snapshots.size(), NUM_SNAPS);
                ImGui::BeginChild("##abSnapList", ImVec2(0, 0), true);
                for(int i = 0; i < (int)snapshots.size(); i++){
                    ImGui::PushID(i);
                    if(ImGui::Selectable((ofToString(i + 1) + "  " + snapshots[i].name).c_str(), i == activeSnapshot)) recallSnapshot(i);
                    if(ImGui::BeginPopupContextItem("##abSnapCtx")){
                        if(ImGui::MenuItem("Replace with the current settings")){ storeSnapshot(i, snapshots[i].name); }
                        if(ImGui::MenuItem("Rename to the name field") && snapName[0]){ snapshots[i].name = snapName.data(); saveSnapshotFile(); }
                        if(ImGui::MenuItem("Delete")){ snapshots.erase(snapshots.begin() + i); saveSnapshotFile(); if(activeSnapshot >= (int)snapshots.size()) activeSnapshot = -1; ImGui::EndPopup(); ImGui::PopID(); break; }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::EndChild();
            }else{
                heading("Recall");
                sliderF("morphTime", morphTime, "Morph time (s)");
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("Numbers glide to the snapshot; the rest switch at the end");
                ImGui::TextWrapped("The node's Snapshot input recalls by number (1..). Click one in the list to recall it; "
                                   "right-click it to replace, rename or delete it.");
                if(morphing) ImGui::TextDisabled("Morphing...");
                ImGui::Spacing();
                heading("Demos");
                if(ImGui::Button("Add the demo snapshots")) importMessage = "Added " + ofToString(addDemoSnapshots()) + " demo(s)";
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("14 examples of the pulse modes, chords, bass, strum, chances...\n(the ones already in the list are skipped)");
                ImGui::Spacing();
                heading("Import");
                if(ImGui::Button("Import the Polyphonic Arpeggiator GUI's snapshots")){
                    importMessage = "Imported " + ofToString(importGuiSnapshots()) + " snapshot(s)";
                }
                if(!importMessage.empty()) ImGui::TextDisabled("%s", importMessage.c_str());
                ImGui::TextWrapped("Reads nodeSnapshots/PolyphonicArpeggiatorGUI (old millisecond lengths converted).");
            }
        });
    }

    // ════════════════════════════════════════════════════════════════════════
    // Members
    // ════════════════════════════════════════════════════════════════════════

    // node
    ofParameter<bool> showWindow;
    ofParameter<int> clockMode;
    ofParameter<void> trigger, resetP, resetNextP;
    ofParameter<float> trigValue;
    scAudioParam trigIn;
    ofParameter<std::vector<float>> notePool, scale;
    ofParameter<float> root;
    ofParameter<int> seed, snapshotRecall, totalVoicesOut;
    ofParameter<std::vector<float>> pitchOut, velOut, durOut, gateVelOut;
    ofParameter<std::vector<int>> gateOut, eucGateOut, eucAccOut, eucDurOut;
    // controls
    ofParameter<float> clockBpm, beatDiv;
    ofParameter<bool> oneShot;
    ofParameter<int> pulseMode;
    ofParameter<std::vector<int>> pulseSteps;
    ofParameter<float> geigerSpeed, geigerDensity, geigerPeriodicity, bounceSpeed, bounceCurve;
    ofParameter<bool> bounceSym;
    ofParameter<int> eucLen, eucHits, eucOff, skipSteps;
    ofParameter<float> seqProb, seqProbCycles, runBeats, runChance, runPhase, stepChance, noteChance;
    ofParameter<int> sourceMode, sourceChange;
    ofParameter<bool> sortPool, dedupPool;
    ofParameter<int> seqSize, patternMode, patternSeed, modulo, sourceStart, sourceStride, stepShift, rndShiftRange, rndShiftQuant;
    ofParameter<bool> patternRndCycle, gateAdvance, byGate;
    ofParameter<std::vector<int>> idxPattern;
    ofParameter<float> rndShiftChance;
    ofParameter<int> octave, octaveFold, expandStep, transpose, octaveDevRng, idxDevRng, pitchDevRng;
    ofParameter<bool> foldPoly, pitchExpand;
    ofParameter<float> octaveDev, idxDev, pitchDev;
    ofParameter<int> polyphony, polyInterval, polyAccent, voiceMode;
    ofParameter<bool> trigAmp;
    ofParameter<bool> addBass, bassInvert, bassOctRndCycle, bassPitchMod;
    ofParameter<int> bassMode, bassEvery, bassShift, bassEucLen, bassEucHits, bassEucOff, bassOctave, bassOctRnd, bassOctRndSeed, bassPoly;
    ofParameter<float> bassProb, bassOctRndChance, bassVel, bassVelRndm;
    ofParameter<float> strum, strumRndm, swing, positionRndm;
    ofParameter<int> strumRndSeed, strumDir, strumDirSeed, positionRndSeed;
    ofParameter<bool> strumRndCycle, strumDirCycle, positionRndCycle;
    ofParameter<float> velBase, velRndm, velStepRndm, lfoDepth, lfoSpeed, lfoPhase, lfoWidth;
    ofParameter<int> velRndSeed, velStepRndSeed, lfoSeed, lfoShape;
    ofParameter<bool> velRndCycle, velStepRndCycle;
    ofParameter<std::vector<float>> velCurve;
    ofParameter<int> accMode, accEvery, accShift, accSeed, accLen, accHits, accOff;
    ofParameter<float> accChance, accStrength;
    ofParameter<bool> accRndCycle;
    ofParameter<std::vector<int>> accSteps;
    ofParameter<float> durBase, durRndm, durChance, durStrength;
    ofParameter<bool> durRndPerStep, durRndCycle;
    ofParameter<int> durMode, durEvery, durShift, durSeed, durLen, durHits, durOff;
    ofParameter<std::vector<int>> durSteps;
    ofParameter<float> morphTime;

    arpbox::Engine engine;
    std::map<int64_t, std::vector<arpbox::Note>> noteCache;
    bool paramsDirty = false, sourceDirty = false;
    uint64_t salt = 0x5A17;
    ofEvent<void> resendParams;
    ofEventListeners listeners;

    // clocks
    int64_t triggerCount = 0;
    int statusHold = 0;
    int64_t freeBase = 0;
    double freePos = 0.0, lastFreePos = 0.0;
    int freeResetCounter = 0, countResetCounter = 0;
    int lastClockMode = CLOCK_TRANSPORT;
    float lastTrigValue = 0.0f, lastBangTime = 0.0f, lastCountTime = 0.0f, bangPeriod = 0.25f;
    uint64_t lastGeneration = 0;
    bool lastPlaying = false;
    uint64_t lastLoopCount = 0;
    double bpb = 4.0;

    scTransportSync::Follower follower;
    scTransportSync::StepClock stepClock;
    int anchorIdCounter = 0, anchorHardCounter = 0;
    bool forceHardAnchor = true;

    // window
    int selectedTab = 0;
    int64_t nowPulse = 0;
    int viewBeats = 8;
    int activeSnapshot = -1;
    std::array<char, 96> snapName{};
    std::string importMessage;
    // morph
    bool morphing = false;
    float morphStart = 0.0f;
    std::map<std::string, double> morphFrom;
    ofJson morphTarget;
};

#endif // OFXOCEANODESC_HAS_TRANSPORT
