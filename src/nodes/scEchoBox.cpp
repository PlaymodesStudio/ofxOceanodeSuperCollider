//
//  scEchoBox.cpp
//  ofxOceanodeSuperCollider
//

#include "scEchoBox.h"
#include "scTypedValue.h"
#include "ofxOceanodeShared.h"
#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxSCServer.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <filesystem>

namespace {
bool liveControlBus(ofxSCServer* s, ofxSCBus* b) {
    return s && b && b->index >= 0 && b->index < (int)s->controlBusses.size() && s->controlBusses[b->index] == b;
}
bool liveAudioBus(ofxSCServer* s, ofxSCBus* b) {
    return s && b && b->index >= 0 && b->index < (int)s->audioBusses.size() && s->audioBusses[b->index] == b;
}
void dropBus(ofxSCServer* s, ofxSCBus*& b, bool audio) {
    if(b && (audio ? liveAudioBus(s, b) : liveControlBus(s, b))) b->free();
    delete b;
    b = nullptr;
}

namespace pal {
const ImU32 canvas     = IM_COL32( 11,  12,  14, 255);
const ImU32 border     = IM_COL32( 46,  50,  58, 255);
const ImU32 grid       = IM_COL32(255, 255, 255,  14);
const ImU32 gridStrong = IM_COL32(255, 255, 255,  38);
const ImU32 header     = IM_COL32( 30,  32,  38, 255);
const ImU32 headerText = IM_COL32(205, 205, 215, 255);
const ImU32 textDim    = IM_COL32(150, 150, 160, 255);
const ImU32 accent     = IM_COL32(150, 165, 255, 255);
const unsigned char tapRGB[8][3] = {
    {150, 165, 255}, {236, 178,  96}, { 96, 200, 176}, {228, 122, 146},
    {170, 208, 112}, {186, 146, 232}, {108, 180, 232}, {232, 146, 104},
};
ImU32 tap(int t, int alpha = 255, float k = 1.0f) {
    const auto* c = tapRGB[((t % 8) + 8) % 8];
    auto ch = [k](unsigned char v){ return (int)std::min(255.0f, v * k); };
    return IM_COL32(ch(c[0]), ch(c[1]), ch(c[2]), alpha);
}
}

float zoom() { return ofxOceanodeShared::getZoomLevel(); }
float gapW() { return 12.0f * zoom(); }
float colW(float w, int n) { return (w - gapW() * (n - 1)) / n; }
void nextCol() { ImGui::EndGroup(); ImGui::SameLine(0, gapW()); ImGui::BeginGroup(); }
void canvas(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    dl->AddRectFilled(a, b, pal::canvas, 3.0f * zoom());
    dl->AddRect(a, b, pal::border, 3.0f * zoom());
}

// Note values, in beats
const std::vector<std::pair<const char*, float>> kNotes = {
    {"1/64", 0.0625f}, {"1/32", 0.125f}, {"1/16T", 1.0f / 6.0f}, {"1/16", 0.25f}, {"1/8T", 1.0f / 3.0f},
    {"1/16D", 0.375f}, {"1/8", 0.5f}, {"1/4T", 2.0f / 3.0f}, {"1/8D", 0.75f}, {"1/4", 1.0f}, {"1/2T", 4.0f / 3.0f},
    {"1/4D", 1.5f}, {"1/2", 2.0f}, {"1/2D", 3.0f}, {"1 bar", 4.0f}, {"1.5 bars", 6.0f}, {"2 bars", 8.0f}};
int nearestNote(float beats) {
    int k = 0;
    for(int i = 0; i < (int)kNotes.size(); i++)
        if(std::abs(std::log(kNotes[i].second / beats)) < std::abs(std::log(kNotes[k].second / beats))) k = i;
    return k;
}
std::string beatsLabel(float beats) {
    const int k = nearestNote(beats);
    if(std::abs(kNotes[k].second - beats) < 1e-3f) return kNotes[k].first;
    return ofToString(beats, 3) + " beats";
}
}

// ════════════════════════════════════════════════════════════════════════════
// Setup
// ════════════════════════════════════════════════════════════════════════════

scEchoBox::scEchoBox(std::vector<serverManager*> s) : scNode("EchoBox"), servers(std::move(s)) {
    description = "Multi-tap echo: each tap with its own time (ms or beats), feedback, filter, diffusion that grows "
                  "with every repeat, saturation, shimmer, ping-pong across the channels, wow and freeze; then "
                  "SpaceMaster reverb on the echoes and ducking. Show opens the editor with the echo map; "
                  "right-click a control there to publish it to the node.";
}

scEchoBox::~scEchoBox() {
    listeners.unsubscribeAll();
    for(auto& [server, st] : states){
        for(auto*& t : st.taps) if(t){ t->free(); delete t; t = nullptr; }
        if(st.rvb){ st.rvb->free(); delete st.rvb; }
        if(st.mix){ st.mix->free(); delete st.mix; }
        dropBus(server, st.wetA, true);
        dropBus(server, st.wetB, true);
        dropBus(server, st.vis, false);
        freeMod(server, st, true);
    }
    states.clear();
}

template<typename T>
void scEchoBox::reg(ofParameter<T>& p, std::function<void()> send) {
    Control c;
    c.key = p.getEscapedName();
    c.param = &p;
    c.add = [this, &p]() { return addParameter(p); };
    c.save = [&p]() { return ofJson(p.get()); };
    c.load = [&p](const ofJson& j) { try { p.set(j.get<T>()); } catch(...) {} };
    controls.push_back(std::move(c));
    listeners.push(p.newListener([this, send](T&) { if(!restoring) send(); }));
}

void scEchoBox::setup() {
    addParameter(showWindow.set("Show", false));
    addParameter(channels.set("Channels", 2, 1, MAX_CHANNELS));
    addParameter(mix.set("Mix", 0.35f, 0.0f, 1.0f));
    addInput("In");
    addOutput("Out");

    auto mixChanged = [this]() { for(auto& [s, st] : states) sendMix(st.mix); };
    gain.set("Output", 1.0f, 0.0f, 2.0f);           reg(gain, mixChanged);
    duck.set("Duck", 0.0f, 0.0f, 1.0f);             reg(duck, mixChanged);
    duckRel.set("Duck Release", 0.3f, 0.01f, 3.0f); reg(duckRel, mixChanged);

    for(int t = 0; t < MAX_TAPS; t++){
        Tap& tp = taps[t];
        const std::string T = "Tap" + ofToString(t + 1) + " ";
        auto send = [this, t]() { if(t < tapCount) sendTap(t); };
        tp.unit.set(T + "Unit", 1, 0, 1);                       reg(tp.unit, send);
        tp.ms.set(T + "Time", 375.0f, 1.0f, 4000.0f);           reg(tp.ms, send);
        tp.beats.set(T + "Beats", 0.75f, 0.0625f, 16.0f);       reg(tp.beats, send);
        tp.level.set(T + "Level", 1.0f, 0.0f, 2.0f);            reg(tp.level, send);
        tp.send.set(T + "Send", 1.0f, 0.0f, 1.0f);              reg(tp.send, send);
        tp.feed.set(T + "Feedback", 0.45f, 0.0f, 1.2f);         reg(tp.feed, send);
        tp.freeze.set(T + "Freeze", false);                     reg(tp.freeze, send);
        tp.pingpong.set(T + "Ping-pong", 0.0f, 0.0f, 1.0f);     reg(tp.pingpong, send);
        tp.spread.set(T + "Spread", 0.5f, 0.0f, 1.0f);          reg(tp.spread, send);
        tp.rotate.set(T + "Rotate", 0, 0, MAX_CHANNELS - 1);    reg(tp.rotate, send);
        tp.bounce.set(T + "Direction", 0, 0, 1);                reg(tp.bounce, send);
        tp.wowRate.set(T + "Wow Rate", 0.6f, 0.01f, 20.0f);     reg(tp.wowRate, send);
        tp.wowDepth.set(T + "Wow Depth", 0.0f, 0.0f, 20.0f);    reg(tp.wowDepth, send);
        tp.ftype.set(T + "Filter", 1, 0, 4);                    reg(tp.ftype, send);
        tp.cutoff.set(T + "Cutoff", 110.0f, 12.0f, 135.0f);     reg(tp.cutoff, send);
        tp.res.set(T + "Resonance", 0.1f, 0.0f, 0.95f);         reg(tp.res, send);
        tp.lowcut.set(T + "Low Cut", 40.0f, 10.0f, 5000.0f);    reg(tp.lowcut, send);
        tp.blur.set(T + "Diffusion", 0.0f, 0.0f, 1.0f);         reg(tp.blur, send);
        tp.blurTime.set(T + "Diffusion Time", 8.0f, 0.1f, 100.0f); reg(tp.blurTime, send);
        tp.drive.set(T + "Drive", 0.0f, 0.0f, 36.0f);           reg(tp.drive, send);
        tp.shift.set(T + "Shimmer Pitch", 12.0f, -24.0f, 24.0f); reg(tp.shift, send);
        tp.shiftMix.set(T + "Shimmer", 0.0f, 0.0f, 1.0f);       reg(tp.shiftMix, send);
    }

    auto rvbChanged = [this]() { for(auto& [s, st] : states) sendRvb(st.rvb); };
    rvbOn.set("Reverb On", false);
    reg(rvbOn, [this]() { for(auto& [s, st] : states) if(st.mix) updateReverb(s, st); });
    rvbMix.set("Reverb Mix", 0.35f, 0.0f, 1.0f);
    rvbDecay.set("Reverb Decay", 3.0f, 0.05f, 30.0f);
    rvbSize.set("Reverb Size", 30.0f, 0.0f, 60.0f);
    rvbPredelay.set("Reverb Predelay", 10.0f, 0.0f, 500.0f);
    rvbLate.set("Reverb Late", 0.8f, 0.0f, 1.0f);
    rvbSpread.set("Reverb Spread", 0.15f, -1.0f, 1.0f);
    rvbLowpass.set("Reverb Lowpass", 9000.0f, 20.0f, 20000.0f);
    rvbHiDamp.set("Reverb Hi Damp", 6.0f, 0.0f, 60.0f);
    rvbLoDamp.set("Reverb Lo Damp", 0.0f, 0.0f, 60.0f);
    rvbModRate.set("Reverb Mod Rate", 0.2f, 0.001f, 20.0f);
    rvbSpin.set("Reverb Spin", 1.0f, 0.0f, 20.0f);
    rvbWander.set("Reverb Wander", 0.25f, 0.0f, 20.0f);
    for(auto* p : {&rvbMix, &rvbDecay, &rvbSize, &rvbPredelay, &rvbLate, &rvbSpread, &rvbLowpass,
                   &rvbHiDamp, &rvbLoDamp, &rvbModRate, &rvbSpin, &rvbWander}) reg(*p, rvbChanged);

    listeners.push(mix.newListener([this](float&) { for(auto& [s, st] : states) sendMix(st.mix); }));
    listeners.push(channels.newListener([this](int&) {
        for(auto& tp : taps) tp.rotate.setMax(std::max(0, n() - 1));
        rebuildChannels();
        modulation.voicesChanged();                 // an LFO oscillator per channel
        for(auto& [server, st] : states) if(st.modCreated) modulation.rebuild(server);
    }));
    for(auto& tp : taps) tp.rotate.setMax(std::max(0, n() - 1));

    // ── Modulation: LFOs (one oscillator per tap), external sources, routing ──
    {
        scModulation::Host h;
        h.regF = [this](ofParameter<float>& p, std::function<void()> f){ reg(p, f); };
        h.control = [this](const std::string& k) -> ofAbstractParameter* {
            for(auto& c : controls) if(c.key == k) return c.param;
            return nullptr;
        };
        h.regI = [this](ofParameter<int>& p, std::function<void()> f){ reg(p, f); };
        h.regB = [this](ofParameter<bool>& p, std::function<void()> f){ reg(p, f); };
        h.regV = [this](ofParameter<std::vector<float>>& p, std::function<void()> f){ reg(p, f); };
        h.publishMenu = [this](const std::string& k){ publishMenu(k); };
        h.publishedUnderline = [this](const std::string& k){ publishedUnderline(k); };
        h.unpublish = [this](const std::string& k){ unpublish(k); };
        h.publishedKeys = [this]() -> const std::vector<std::string>& { return publishedKeys; };
        h.voices = [this](){ return n(); };        // a "voice" = a channel
        modulation.maxEnvs = 0;                     // no notes: no envelopes
        // the taps read these, in this order (echoBox.scd); Mix: the mix synth
        modulation.setup(h, {{"Time", 1, "oct"}, {"Feedback", 0.5f, ""}, {"Cutoff", 48, "st"}, {"Level", 1, "x"},
                             {"Ping-pong", 1, ""}, {"Drive", 24, "dB"}, {"Shimmer", 1, ""}, {"Diffusion", 1, ""},
                             {"Send", 1, "x"}, {"Mix", 1, ""}}, true, false, false);
        modulation.lfoCount = 0;
        modulation.envCount = 0;
        modulation.extCount = 0;
        for(int t = 0; t < MAX_TAPS; t++){
            Tap& tp = taps[t];
            for(auto& [p, target] : std::vector<std::pair<ofAbstractParameter*, int>>{
                    {&tp.ms, 0}, {&tp.beats, 0}, {&tp.feed, 1}, {&tp.cutoff, 2}, {&tp.level, 3}, {&tp.pingpong, 4},
                    {&tp.drive, 5}, {&tp.shiftMix, 6}, {&tp.blur, 7}, {&tp.send, 8}})
                modulation.targetOfKey[p->getEscapedName()] = target;
        }
        modulation.targetOfKey[mix.getEscapedName()] = 9;
    }
    requireSynthdefs();
}

void scEchoBox::requireSynthdefs() {
    std::filesystem::path folder;
    const std::string expected = "EchoBoxTap2.scsyndef";
    try {
        const std::filesystem::path root = ofToDataPath(SYNTHDEF_DIRECTORY, true);
        if(std::filesystem::exists(root))
            for(const auto& e : std::filesystem::recursive_directory_iterator(root,
                    std::filesystem::directory_options::skip_permission_denied))
                if(e.is_regular_file() && e.path().filename() == expected) { folder = e.path().parent_path(); break; }
        if(folder.empty()){
            const auto bundled = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()
                / "synthdefs/Defaults/EchoBox/CompiledSynthdefs/EchoBox";
            if(std::filesystem::exists(bundled / expected)) folder = bundled;
        }
    } catch(const std::exception& e) { ofLogWarning("scEchoBox") << e.what(); }
    if(folder.empty()) { ofLogError("scEchoBox") << "Compile synthdefs/Defaults/EchoBox/echoBox.scd"; return; }
    for(auto* sm : servers) if(sm) sm->requireSynthdefFolder(folder.string());
}

// ════════════════════════════════════════════════════════════════════════════
// Engine
// ════════════════════════════════════════════════════════════════════════════

float scEchoBox::tapSeconds(int t) const {
    const Tap& tp = taps[t];
    const float s = tp.unit.get() == 1 ? tp.beats.get() * 60.0f / std::max(1.0f, currentBpm) : tp.ms.get() * 0.001f;
    return ofClamp(s, 0.002f, 4.0f);
}

void scEchoBox::sendTap(ofxSCSynth* s, int t) {
    if(!s) return;
    const Tap& tp = taps[t];
    s->set("time", tapSeconds(t));
    s->set("level", tp.level.get());
    s->set("send", tp.send.get());
    s->set("feed", tp.feed.get());
    s->set("freeze", tp.freeze.get() ? 1.0f : 0.0f);
    s->set("pingpong", tp.pingpong.get());
    s->set("bounce", tp.bounce.get() == 0 ? 1.0f : -1.0f);
    s->set("rotate", (float)std::min(tp.rotate.get(), n() - 1));
    s->set("spread", tp.spread.get());
    s->set("wowrate", tp.wowRate.get());
    s->set("wowdepth", tp.wowDepth.get());
    s->set("ftype", (float)tp.ftype.get());
    s->set("cutoff", tp.cutoff.get());
    s->set("res", tp.res.get());
    s->set("lowcut", tp.lowcut.get());
    s->set("blur", tp.blur.get());
    s->set("blurtime", tp.blurTime.get());
    s->set("drive", tp.drive.get());
    s->set("shift", tp.shift.get());
    s->set("shiftmix", tp.shiftMix.get());
}
void scEchoBox::sendTap(int t) { for(auto& [s, st] : states) sendTap(st.taps[t], t); }

void scEchoBox::sendMix(ofxSCSynth* s) {
    if(!s) return;
    s->set("mix", mix.get());
    s->set("gain", gain.get());
    s->set("duck", duck.get());
    s->set("duckrel", duckRel.get());
}

void scEchoBox::sendRvb(ofxSCSynth* x) {
    if(!x) return;
    x->set("mix", ofClamp(rvbMix.get(), 0.0f, 1.0f));
    x->set("decay", std::max(0.05f, rvbDecay.get()));
    x->set("size", ofClamp(rvbSize.get(), 0.0f, 60.0f));
    x->set("predelay", ofClamp(rvbPredelay.get(), 0.0f, 500.0f));
    x->set("position", ofClamp(rvbLate.get(), 0.0f, 1.0f));
    x->set("spread", ofClamp(rvbSpread.get(), -1.0f, 1.0f));
    x->set("lowpass", ofClamp(rvbLowpass.get(), 20.0f, 20000.0f));
    x->set("highdamp", ofClamp(rvbHiDamp.get(), 0.0f, 60.0f));
    x->set("lowdamp", ofClamp(rvbLoDamp.get(), 0.0f, 60.0f));
    x->set("modfreq", ofClamp(rvbModRate.get(), 0.001f, 20.0f));
    x->set("spin", ofClamp(rvbSpin.get(), 0.0f, 20.0f));
    x->set("wander", ofClamp(rvbWander.get(), 0.0f, 20.0f));
    x->set("on", rvbOn.get() ? 1.0f : 0.0f);
}

float scEchoBox::reverbTail() const { return std::min(30.0f, rvbDecay.get() + rvbPredelay.get() * 0.001f + 0.3f); }

void scEchoBox::ensureBuses(ofxSCServer* server, ServerState& st) {
    const bool resize = st.builtChannels != n();
    auto audio = [&](ofxSCBus*& b){
        if(liveAudioBus(server, b) && !resize) return;
        dropBus(server, b, true);
        b = new ofxSCBus(RATE_AUDIO, n(), server);
        if(!liveAudioBus(server, b)){ b->free(); delete b; b = nullptr; }
    };
    audio(st.wetA);
    audio(st.wetB);
    if(!liveControlBus(server, st.vis)){
        dropBus(server, st.vis, false);
        st.vis = new ofxSCBus(RATE_CONTROL, MAX_TAPS + 3, server);
        if(!liveControlBus(server, st.vis)){ st.vis->free(); delete st.vis; st.vis = nullptr; }
    }
}

// A tap: right before the reverb (or the mix)
void scEchoBox::createTap(ofxSCServer* server, ServerState& st, int t) {
    if(!st.mix || !st.wetA || st.taps[t]) return;
    auto* s = new ofxSCSynth("EchoBoxTap" + ofToString(n()), server);
    s->set("in", st.inBus);
    s->set("out", st.wetA->index);
    s->set("vis", st.vis ? st.vis->index + t : 0);
    sendTap(s, t);
    setModArgs(server, st, s, t);
    s->createAndRun(2, st.rvb ? st.rvb->nodeID : st.mix->nodeID, getActive());   // addBefore
    st.taps[t] = s;
}

void scEchoBox::routeWet(ServerState& st) {
    if(!st.mix || !st.wetA) return;
    const bool viaRvb = st.rvb && st.rvbRunning && st.wetB;
    st.mix->set("wet", viaRvb ? st.wetB->index : st.wetA->index);
    if(st.rvb && st.wetB){ st.rvb->set("in", st.wetA->index); st.rvb->set("out", st.wetB->index); }
}

void scEchoBox::updateReverb(ofxSCServer* server, ServerState& st) {
    if(rvbOn.get()){
        if(!st.rvb && st.mix){
            st.rvb = new ofxSCSynth("mixerProRvb" + ofToString(n()), server);
            sendRvb(st.rvb);
            st.rvb->set("in", st.wetA ? st.wetA->index : 0);
            st.rvb->set("out", st.wetB ? st.wetB->index : 0);
            st.rvb->createAndRun(2, st.mix->nodeID, getActive());   // addBefore the mix
        }else if(st.rvb){
            sendRvb(st.rvb);
            if(!st.rvbRunning && getActive()) st.rvb->run(true);
        }
        st.rvbRunning = st.rvb != nullptr;
        st.rvbOffAt = -1.0f;
    }else if(st.rvb && st.rvbRunning && st.rvbOffAt < 0.0f){
        st.rvb->set("on", 0.0f);                 // the tail rings out, then it pauses
        st.rvbOffAt = ofGetElapsedTimef() + reverbTail();
    }
    routeWet(st);
}

void scEchoBox::addTap() {
    if(tapCount >= MAX_TAPS) return;
    const int t = tapCount;
    // a new tap starts as a copy of the last one, a little later and bouncing
    Tap& src = taps[t - 1];
    Tap& dst = taps[t];
    restoring = true;
    dst.unit = src.unit.get(); dst.ms = std::min(4000.0f, src.ms.get() * 1.5f);
    dst.beats = kNotes[std::min((int)kNotes.size() - 1, nearestNote(src.beats.get()) + 2)].second;
    dst.level = src.level.get() * 0.8f; dst.send = src.send.get(); dst.feed = src.feed.get(); dst.freeze = false;
    dst.pingpong = src.pingpong.get(); dst.spread = src.spread.get();
    dst.rotate = n() > 1 ? (src.rotate.get() + 1) % n() : 0; dst.bounce = src.bounce.get();
    dst.wowRate = src.wowRate.get(); dst.wowDepth = src.wowDepth.get();
    dst.ftype = src.ftype.get(); dst.cutoff = src.cutoff.get(); dst.res = src.res.get(); dst.lowcut = src.lowcut.get();
    dst.blur = src.blur.get(); dst.blurTime = src.blurTime.get(); dst.drive = src.drive.get();
    dst.shift = src.shift.get(); dst.shiftMix = src.shiftMix.get();
    restoring = false;
    tapCount++;
    for(auto& [server, st] : states) createTap(server, st, t);
    selectTabNext = t;
}

void scEchoBox::removeLastTap() {
    if(tapCount <= 1) return;
    const int t = tapCount - 1;
    for(auto& [server, st] : states) if(st.taps[t]){ st.taps[t]->free(); delete st.taps[t]; st.taps[t] = nullptr; }
    // its published controls go too
    const std::string prefix = "Tap" + ofToString(t + 1) + "_";
    const auto keys = publishedKeys;
    for(const auto& k : keys) if(k.rfind(prefix, 0) == 0) unpublish(k);
    tapCount--;
    selectedTap = std::min(selectedTap, tapCount - 1);
}

// A new channel count: every synth replaced in place by its N-channel version
void scEchoBox::rebuildChannels() {
    for(auto& [server, st] : states){
        if(!st.mix) continue;
        const bool wasLatency = server->getBLatency();
        server->setBLatency(true);
        ensureBuses(server, st);
        st.builtChannels = n();
        auto replace = [&](ofxSCSynth*& old, const std::string& def, std::function<void(ofxSCSynth*)> init, bool run){
            if(!old) return;
            auto* fresh = new ofxSCSynth(def, server);
            init(fresh);
            fresh->createAndRun(4, old->nodeID, run);
            delete old;
            old = fresh;
        };
        for(int t = 0; t < MAX_TAPS; t++)
            replace(st.taps[t], "EchoBoxTap" + ofToString(n()), [&](ofxSCSynth* s){
                s->set("in", st.inBus); s->set("out", st.wetA ? st.wetA->index : 0);
                s->set("vis", st.vis ? st.vis->index + t : 0); sendTap(s, t); setModArgs(server, st, s, t);
            }, getActive());
        replace(st.rvb, "mixerProRvb" + ofToString(n()), [&](ofxSCSynth* s){ sendRvb(s); }, getActive() && st.rvbRunning);
        replace(st.mix, "EchoBoxMix" + ofToString(n()), [&](ofxSCSynth* s){
            s->set("in", st.inBus); s->set("out", st.outBus); s->set("vis", st.vis ? st.vis->index + MAX_TAPS : 0); sendMix(s);
            setModArgs(server, st, s, 0);
        }, getActive());
        routeWet(st);
        server->setBLatency(wasLatency);
    }
}

// ── Graph ──────────────────────────────────────────────────────────────────

void scEchoBox::buildSynth(ofxSCServer* server) {
    if(!server) return;
    auto& st = states[server];
    for(auto*& t : st.taps) if(t){ t->free(); delete t; t = nullptr; }
    if(st.rvb){ st.rvb->free(); delete st.rvb; st.rvb = nullptr; }
    if(st.mix){ st.mix->free(); delete st.mix; st.mix = nullptr; }
    st.rvbRunning = false;
    st.rvbOffAt = -1.0f;
    freeMod(server, st, true);
}

// The modulation: a group at the head of the default group (before the taps:
// they read its buses in the same block), its LFOs and routes inside
void scEchoBox::createMod(ofxSCServer* server, ServerState& st) {
    if(st.modCreated) return;
    st.modGroup = new ofxSCGroup(server);
    st.modGroup->create(0, 1);
    st.modSources = new ofxSCGroup(server);
    st.modSources->create(1, st.modGroup->nodeID);
    st.modRoutes = new ofxSCGroup(server);
    st.modRoutes->create(1, st.modGroup->nodeID);
    st.modCreated = true;
    modulation.create(server, st.modSources->nodeID, st.modRoutes->nodeID, 0);
    if(!getActive()) st.modGroup->run(false);
}

void scEchoBox::freeMod(ofxSCServer* server, ServerState& st, bool sendFree) {
    if(st.modCreated) modulation.free(server, false);   // the group's /n_free takes its synths
    if(sendFree && st.modCreated && st.modGroup) st.modGroup->free();
    delete st.modSources; delete st.modRoutes; delete st.modGroup;
    st.modSources = st.modRoutes = st.modGroup = nullptr;
    st.modCreated = false;
}

// A tap (or the mix: tap 0) reads channel `tap` of each target bus
void scEchoBox::setModArgs(ofxSCServer* server, ServerState& st, ofxSCSynth* s, int tap) {
    if(!s) return;
    s->set("modbus", st.modCreated ? (float)modulation.targetBus(server, 0) : -1.0f);
    s->set("tap", (float)tap);
}

void scEchoBox::createSynth(ofxSCServer* server) {
    if(!server) return;
    auto& st = states[server];
    ensureBuses(server, st);
    st.builtChannels = n();
    st.mix = new ofxSCSynth("EchoBoxMix" + ofToString(n()), server);
    st.mix->set("in", st.inBus);
    st.mix->set("out", st.outBus);
    st.mix->set("wet", st.wetA ? st.wetA->index : 0);
    st.mix->set("vis", st.vis ? st.vis->index + MAX_TAPS : 0);
    sendMix(st.mix);
    st.mix->createAndRun(0, 1, getActive());
    createMod(server, st);           // the head: before the mix and the taps
    setModArgs(server, st, st.mix, 0);
    updateReverb(server, st);
    for(int t = 0; t < tapCount; t++) createTap(server, st, t);
}

void scEchoBox::free(ofxSCServer* server) {
    auto it = states.find(server);
    if(it == states.end()) return;
    buildSynth(server);
    it->second.outBus = -1;
}

void scEchoBox::moveSynthBefore(ofxSCServer* server, int nodeID) {
    auto it = states.find(server);
    if(it == states.end()) return;
    auto& st = it->second;
    if(st.mix) st.mix->set("out", st.outBus);
    std::vector<ofxSCSynth*> chain;
    for(auto* t : st.taps) if(t) chain.push_back(t);
    if(st.rvb) chain.push_back(st.rvb);
    if(st.mix) chain.push_back(st.mix);
    int before = nodeID;
    for(int k = (int)chain.size() - 1; k >= 0; k--){ chain[k]->moveBefore(before); before = chain[k]->nodeID; }
    if(st.modCreated) st.modGroup->moveBefore(before);   // the modulation first
}

int scEchoBox::getLastSynthID(ofxSCServer* server) {
    auto it = states.find(server);
    if(it == states.end()) return -1;
    if(it->second.modCreated) return it->second.modGroup->nodeID;
    for(auto* t : it->second.taps) if(t) return t->nodeID;
    if(it->second.rvb) return it->second.rvb->nodeID;
    return it->second.mix ? it->second.mix->nodeID : -1;
}

void scEchoBox::setOutputBus(ofxSCServer* server, int, int bus) {
    auto& st = states[server];
    st.outBus = bus;
    if(st.mix) st.mix->set("out", bus);
}
int scEchoBox::getOutputBusIndex(ofxSCServer* server, int) {
    auto it = states.find(server);
    return it != states.end() ? it->second.outBus : -1;
}

void scEchoBox::setInputBus(ofxSCServer* server, scNode*, int bus) {
    auto& st = states[server];
    st.inBus = bus;
    for(auto* t : st.taps) if(t) t->set("in", bus);
    if(st.mix) st.mix->set("in", bus);
}
void scEchoBox::resetInputBusses(ofxSCServer* server, int targetBus) { setInputBus(server, nullptr, targetBus); }

void scEchoBox::resendParametersForNRT() {
    for(auto& [server, st] : states){
        for(int t = 0; t < MAX_TAPS; t++) sendTap(st.taps[t], t);
        sendMix(st.mix);
        sendRvb(st.rvb);
        routeWet(st);
    }
    modulation.resend();
}

void scEchoBox::activate() {
    for(auto& [server, st] : states){
        for(auto* t : st.taps) if(t) t->run(true);
        if(st.rvb && st.rvbRunning) st.rvb->run(true);
        if(st.mix) st.mix->run(true);
        if(st.modCreated) st.modGroup->run(true);
    }
}
void scEchoBox::deactivate() {
    for(auto& [server, st] : states){
        for(auto* t : st.taps) if(t) t->run(false);
        if(st.rvb) st.rvb->run(false);
        if(st.mix) st.mix->run(false);
        if(st.modCreated) st.modGroup->run(false);
    }
}

void scEchoBox::setBpm(float bpm) {
    currentBpm = bpm;
    modulation.setBpm(bpm);
    for(int t = 0; t < tapCount; t++)
        if(taps[t].unit.get() == 1)
            for(auto& [s, st] : states) if(st.taps[t]) st.taps[t]->set("time", tapSeconds(t));
}

void scEchoBox::update(ofEventArgs&) {
    const float now = ofGetElapsedTimef();
    modulation.showing = showWindow.get();
    modulation.update();
    for(auto& [server, st] : states){
        if(st.rvbOffAt >= 0.0f && now >= st.rvbOffAt){
            st.rvbOffAt = -1.0f;
            st.rvbRunning = false;
            routeWet(st);
            if(st.rvb) st.rvb->run(false);
        }
    }
    visValues.clear();
    if(showWindow.get()){
        for(auto& [server, st] : states){
            if(!st.mix || !liveControlBus(server, st.vis)) continue;
            visValues = st.vis->readValues;
            if(now >= nextVisRead){
                nextVisRead = now + 1.0f / 30.0f;
                st.vis->requestValues();
            }
            break;
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Presets and publishing
// ════════════════════════════════════════════════════════════════════════════

void scEchoBox::presetSave(ofJson& j) {
    auto& c = j["echoBox"] = ofJson::object();
    for(const auto& control : controls) c[control.key] = control.save();
    modulation.patchBases(c);        // a modulated control: its own value
    modulation.patchBases(j);
    j["echoBoxTaps"] = tapCount;
    modulation.save(j, "echoBoxMod");
    j["echoBoxPublished"] = publishedKeys;
    j["echoBoxMapHeight"] = mapHeight / std::max(0.01f, zoom());
}

void scEchoBox::loadBeforeConnections(ofJson& j) {
    if(!j.contains("echoBoxPublished") || !j["echoBoxPublished"].is_array()) return;
    std::vector<std::string> wanted;
    for(const auto& k : j["echoBoxPublished"]) if(k.is_string()) wanted.push_back(k.get<std::string>());
    const auto old = publishedKeys;
    for(const auto& k : old) if(std::find(wanted.begin(), wanted.end(), k) == wanted.end()) unpublish(k);
    for(const auto& k : wanted) publish(k);
}

void scEchoBox::presetRecallAfterSettingParameters(ofJson& j) {
    if(j.contains("echoBox") && j["echoBox"].is_object()){
        restoring = true;
        for(const auto& control : controls) if(j["echoBox"].contains(control.key)) control.load(j["echoBox"][control.key]);
        restoring = false;
    }
    if(j.contains("echoBoxMapHeight") && j["echoBoxMapHeight"].is_number())
        mapHeight = std::max(0.0f, j["echoBoxMapHeight"].get<float>()) * zoom();
    int wanted = tapCount;
    if(j.contains("echoBoxTaps") && j["echoBoxTaps"].is_number()) wanted = std::clamp(j["echoBoxTaps"].get<int>(), 1, MAX_TAPS);
    for(auto& [server, st] : states){
        for(int t = wanted; t < MAX_TAPS; t++) if(st.taps[t]){ st.taps[t]->free(); delete st.taps[t]; st.taps[t] = nullptr; }
    }
    tapCount = wanted;
    for(auto& [server, st] : states){
        for(int t = 0; t < tapCount; t++){ if(st.taps[t]) sendTap(st.taps[t], t); else createTap(server, st, t); }
        sendMix(st.mix);
        if(st.mix) updateReverb(server, st);
    }
    selectedTap = std::min(selectedTap, tapCount - 1);
    // modulation (presets from before it: nothing routed)
    if(j.contains("echoBoxModMatrix")) modulation.load(j, "echoBoxMod");
    else {
        std::fill(modulation.matrix.begin(), modulation.matrix.end(), 0.0f);
        modulation.lfoCount = modulation.extCount = 0;
        modulation.loadParamsJson(ofJson::array());
    }
    modulation.afterLoad();
}

void scEchoBox::publish(const std::string& key) {
    if(published.count(key)) return;
    auto it = std::find_if(controls.begin(), controls.end(), [&](const Control& c){ return c.key == key; });
    if(it == controls.end() || getParameterGroup().contains(key)) return;
    if(!publishedSeparator){ addSeparator("Published", ofColor(200)); publishedSeparator = true; }
    published[key] = it->add();
    publishedKeys.push_back(key);
    parameterGroupChanged.notify(this);
}
void scEchoBox::unpublish(const std::string& key) {
    auto it = published.find(key);
    if(it == published.end() || isConnected(key)) return;
    removeParameter(it->second->getName());
    published.erase(it);
    publishedKeys.erase(std::remove(publishedKeys.begin(), publishedKeys.end(), key), publishedKeys.end());
    if(published.empty() && publishedSeparator){ removeSeparator("Published"); publishedSeparator = false; }
    parameterGroupChanged.notify(this);
}
bool scEchoBox::isConnected(const std::string& key) const {
    auto it = published.find(key);
    return it != published.end() && it->second && (it->second->hasInConnection() || it->second->hasOutConnections());
}
void scEchoBox::publishMenu(const std::string& key) {
    if(!ImGui::BeginPopupContextItem(("##ebPublish" + key).c_str())) return;
    ImGui::TextDisabled("%s", key.c_str());
    ImGui::Separator();
    // the value, typed
    auto it = std::find_if(controls.begin(), controls.end(), [&](const Control& c){ return c.key == key; });
    if(it != controls.end()) scTyped::jsonField(key, it->save, it->load);
    if(published.count(key)){
        if(isConnected(key)) ImGui::TextDisabled("Disconnect it to unpublish");
        else if(ImGui::MenuItem("Unpublish from Node")) unpublish(key);
    }else if(getParameterGroup().contains(key)) ImGui::TextDisabled("On the node");
    else if(ImGui::MenuItem("Publish to Node")) publish(key);
    modulation.modulateMenu(key);
    ImGui::EndPopup();
}
void scEchoBox::publishedUnderline(const std::string& key) const {
    if(!published.count(key)) return;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y - 1.0f), ImVec2(b.x, b.y - 1.0f), ImGui::GetColorU32(ImGuiCol_Text));
}

// ════════════════════════════════════════════════════════════════════════════
// Window widgets (GrainBox's look)
// ════════════════════════════════════════════════════════════════════════════

void scEchoBox::sectionHeader(const char* title, float w, ofParameter<bool>* on, ImU32 accent) {
    const float z = zoom();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    const float bh = ImGui::GetFrameHeight() + 6.0f * z;
    dl->AddRectFilled(cur, ImVec2(cur.x + w, cur.y + bh), pal::header, 3.0f * z);
    dl->AddLine(ImVec2(cur.x, cur.y + bh), ImVec2(cur.x + w, cur.y + bh), accent ? accent : pal::accent, 1.5f * z);
    ImGui::SetCursorScreenPos(ImVec2(cur.x + 6.0f * z, cur.y + 3.0f * z));
    if(on){
        bool v = on->get();
        ImGui::PushID(title);
        if(ImGui::Checkbox("##on", &v)) *on = v;
        publishMenu(on->getEscapedName());
        ImGui::PopID();
        ImGui::SameLine(0, 4.0f * z);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, pal::headerText);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(cur.x, cur.y + bh + 4.0f * z));
    ImGui::Dummy(ImVec2(w, 0.0f));
}

void scEchoBox::slider(ofParameter<float>& p, const char* label, float w, const char* fmt, bool log) {
    const float z = zoom();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    publishedUnderline(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(40.0f * z, w - labelW * z));
    float v = p.get();
    if(ImGui::SliderFloat("##v", &v, p.getMin(), p.getMax(), fmt, log ? ImGuiSliderFlags_Logarithmic : 0)) p.set(v);
    publishMenu(key);
    ImGui::PopID();
}

void scEchoBox::sliderInt(ofParameter<int>& p, const char* label, float w) {
    const float z = zoom();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    publishedUnderline(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(40.0f * z, w - labelW * z));
    int v = p.get();
    if(ImGui::SliderInt("##v", &v, p.getMin(), p.getMax())) p.set(v);
    publishMenu(key);
    ImGui::PopID();
}

void scEchoBox::combo(ofParameter<int>& p, const char* label, float w, const std::vector<const char*>& items) {
    const float z = zoom();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    publishedUnderline(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(40.0f * z, w - labelW * z));
    int v = std::clamp(p.get(), 0, (int)items.size() - 1);
    if(ImGui::Combo("##v", &v, items.data(), (int)items.size())) p.set(v);
    publishMenu(key);
    ImGui::PopID();
}

void scEchoBox::toggle(ofParameter<bool>& p, const char* label) {
    bool v = p.get();
    if(ImGui::Checkbox(label, &v)) p.set(v);
    publishedUnderline(p.getEscapedName());
    publishMenu(p.getEscapedName());
}

// Time in beats: note values (Shift while dragging: free)
void scEchoBox::beatsSlider(Tap& tp, float w) {
    const float z = zoom();
    const std::string key = tp.beats.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Time");
    publishedUnderline(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(40.0f * z, w - labelW * z));
    if(ImGui::GetIO().KeyShift){
        float v = tp.beats.get();
        if(ImGui::SliderFloat("##free", &v, 0.0625f, 16.0f, "%.3f beats", ImGuiSliderFlags_Logarithmic)) tp.beats = v;
    }else{
        int k = nearestNote(tp.beats.get());
        const std::string label = beatsLabel(tp.beats.get());
        if(ImGui::SliderInt("##note", &k, 0, (int)kNotes.size() - 1, label.c_str())) tp.beats = kNotes[k].second;
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Note values (hold Shift: any number of beats)\n%.1f ms at %.1f BPM",
                                                 tp.beats.get() * 60000.0f / std::max(1.0f, currentBpm), currentBpm);
    publishMenu(key);
    ImGui::PopID();
}

// ════════════════════════════════════════════════════════════════════════════
// Window
// ════════════════════════════════════════════════════════════════════════════

void scEchoBox::draw(ofEventArgs&) { if(showWindow.get()) drawWindow(); }

void scEchoBox::drawWindow() {
    const float z = zoom();
    ImGui::SetNextWindowSize(ImVec2(1080 * z, 760 * z), ImGuiCond_FirstUseEver);
    bool open = showWindow.get();
    const std::string title = getWindowTitle();
    if(!ImGui::Begin(title.c_str(), &open)){ ImGui::End(); if(!open) showWindow = false; return; }
    if(!open){ showWindow = false; ImGui::End(); return; }
    const float w = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;

    // ── Header: channels, mix, output, meters ────────────────────────────────
    {
        labelW = 62.0f;
        ImGui::BeginGroup(); sliderInt(channels, "Channels", 200 * z); ImGui::EndGroup();
        ImGui::SameLine(0, 16 * z);
        ImGui::BeginGroup(); slider(mix, "Mix", 200 * z, "%.2f"); ImGui::EndGroup();
        ImGui::SameLine(0, 16 * z);
        ImGui::BeginGroup(); slider(gain, "Output", 200 * z, "%.2f"); ImGui::EndGroup();
        ImGui::SameLine(0, 16 * z);
        ImGui::TextColored(ImColor(pal::textDim), "%.1f BPM", currentBpm);
        // input / echoes meters, duck
        auto meter = [&](const char* name, float v, ImU32 col){
            ImGui::SameLine(0, 10 * z);
            ImGui::TextColored(ImColor(pal::textDim), "%s", name);
            ImGui::SameLine(0, 4 * z);
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const float mw = 70 * z, mh = ImGui::GetFrameHeight() * 0.5f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float y = a.y + mh * 0.5f;
            dl->AddRectFilled(ImVec2(a.x, y), ImVec2(a.x + mw, y + mh), pal::canvas, 2 * z);
            const float db = 20.0f * std::log10(std::max(1e-5f, v));
            dl->AddRectFilled(ImVec2(a.x, y), ImVec2(a.x + mw * ofClamp((db + 60.0f) / 60.0f, 0.0f, 1.0f), y + mh), col, 2 * z);
            ImGui::Dummy(ImVec2(mw, ImGui::GetFrameHeight()));
        };
        auto vis = [&](int i){ return i < (int)visValues.size() ? visValues[i] : 0.0f; };
        meter("In", vis(MAX_TAPS), pal::headerText);
        meter("Echoes", vis(MAX_TAPS + 1), pal::accent);
        if(duck.get() > 0.0f){
            ImGui::SameLine(0, 8 * z);
            ImGui::TextColored(ImColor(pal::textDim), "duck %d%%", (int)std::round((1.0f - vis(MAX_TAPS + 2)) * 100));
        }
    }
    ImGui::Spacing();
    drawEchoMap(w);
    ImGui::Spacing();

    // ── A tab per tap ────────────────────────────────────────────────────────
    if(ImGui::BeginTabBar("##ebTaps", ImGuiTabBarFlags_FittingPolicyScroll)){
        for(int t = 0; t < tapCount; t++){
            const std::string name = "Tap " + ofToString(t + 1) + "###tap" + ofToString(t);
            bool keep = true;
            const bool closable = t == tapCount - 1 && tapCount > 1;
            ImGuiTabItemFlags flags = selectTabNext == t ? ImGuiTabItemFlags_SetSelected : 0;
            ImGui::PushStyleColor(ImGuiCol_Tab, pal::tap(t, 90, 0.6f));
            ImGui::PushStyleColor(ImGuiCol_TabHovered, pal::tap(t, 170, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_TabActive, pal::tap(t, 200, 0.75f));
            const bool openTab = ImGui::BeginTabItem(name.c_str(), closable ? &keep : nullptr, flags);
            ImGui::PopStyleColor(3);
            if(openTab){
                selectedTap = t;
                ImGui::Spacing();
                drawTapControls(t, w);
                ImGui::EndTabItem();
            }
            if(!keep){ removeLastTap(); break; }
        }
        selectTabNext = -1;
        // the modulation: LFOs (one oscillator per tap) and the routing
        if(ImGui::BeginTabItem("LFOs###ebLfos")){
            ImGui::Spacing();
            modulation.drawLfos(w);
            ImGui::TextDisabled("Each LFO runs one oscillator per channel (Spread / Invert / Rand... give each channel its own "
                                "phase); every tap gets the same per-channel modulation.");
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem("Routing###ebRouting")){
            ImGui::Spacing();
            modulation.drawRouting(w);
            ImGui::EndTabItem();
        }
        if(tapCount < MAX_TAPS && ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing)) addTap();
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Add a tap (a copy of the last one, later, on the next channel)");
        ImGui::EndTabBar();
    }
    ImGui::Spacing();
    drawOutput(w);
    ImGui::End();
}

// Every tap's repeats on a timeline, a lane per channel: the bar heights are
// the levels (dB), the bars widen as the diffusion grows. Drag a tap's first
// echo (the handle) to set its time; click a repeat to select its tap.
void scEchoBox::drawEchoMap(float w) {
    const float z = zoom();
    if(mapHeight <= 0.0f) mapHeight = 180.0f * z;
    const float h = mapHeight;
    const int nc = n();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
    ImGui::InvisibleButton("##ebMap", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    canvas(dl, a, b);
    dl->PushClipRect(a, b, true);

    // the view: long enough for a few repeats of the longest tap
    float longest = 0.0f;
    for(int t = 0; t < tapCount; t++) longest = std::max(longest, tapSeconds(t));
    const float view = ofClamp(longest * 6.0f, 1.0f, 16.0f);
    const float padL = 6.0f * z, padB = 16.0f * z;
    const float x0 = a.x + padL, x1 = b.x - 6.0f * z;
    auto X = [&](float sec){ return x0 + (x1 - x0) * sec / view; };
    const float laneTop = a.y + 6.0f * z, laneBottom = b.y - padB;
    const float laneH = (laneBottom - laneTop) / nc;

    // lanes and the beat grid
    for(int c = 0; c < nc; c++){
        const float y = laneTop + c * laneH;
        if(c % 2) dl->AddRectFilled(ImVec2(a.x, y), ImVec2(b.x, y + laneH), IM_COL32(255, 255, 255, 6));
        if(laneH > 12 * z) dl->AddText(ImVec2(a.x + 3 * z, y + 1), pal::textDim, ofToString(c + 1).c_str());
    }
    const float beat = 60.0f / std::max(1.0f, currentBpm);
    for(int k = 1; k * beat < view; k++){
        const float x = X(k * beat);
        dl->AddLine(ImVec2(x, laneTop), ImVec2(x, laneBottom), k % 4 == 0 ? pal::gridStrong : pal::grid);
    }
    // seconds along the bottom
    const float step = view <= 2 ? 0.25f : view <= 6 ? 0.5f : 1.0f;
    for(float s = step; s < view; s += step)
        dl->AddText(ImVec2(X(s) - 8 * z, b.y - padB + 2 * z), pal::textDim, (ofToString(s, step < 1 ? 2 : 0) + "s").c_str());
    dl->AddLine(ImVec2(x0, laneTop), ImVec2(x0, laneBottom), pal::headerText, 1.5f * z);   // the input

    // the repeats of every tap (the selected one on top)
    int hoverTap = -1;
    const ImVec2 m = ImGui::GetMousePos();
    std::vector<int> order;
    for(int t = 0; t < tapCount; t++) if(t != selectedTap) order.push_back(t);
    if(selectedTap < tapCount) order.push_back(selectedTap);
    for(int t : order){
        const Tap& tp = taps[t];
        const float sec = tapSeconds(t);
        const bool sel = t == selectedTap;
        const float feed = tp.freeze.get() ? 1.0f : std::min(1.2f, tp.feed.get());
        const int dir = tp.bounce.get() == 0 ? 1 : -1;
        float amp = tp.level.get() * tp.send.get();
        for(int k = 0; ; k++){
            const float at = sec * (k + 1);
            if(at > view || k > 200) break;
            const float db = 20.0f * std::log10(std::max(1e-5f, amp));
            if(db < -60.0f) break;
            const int lane = ((tp.rotate.get() + (tp.pingpong.get() >= 0.5f ? dir * k : 0)) % nc + nc) % nc;
            const float y0 = laneTop + lane * laneH;
            const float bh = laneH * 0.9f * ofClamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
            const float bw = 2.0f * z + tp.blur.get() * std::min(40.0f, k * 3.0f) * z;   // more diffuse each repeat
            const float x = X(at);
            const ImVec2 r0(x - bw * 0.5f, y0 + laneH - bh), r1(x + bw * 0.5f, y0 + laneH);
            const int alpha = sel ? 230 : 120;
            if(tp.blur.get() > 0.05f && k > 0)
                dl->AddRectFilledMultiColor(r0, r1, pal::tap(t, alpha / 3), pal::tap(t, alpha / 3), pal::tap(t, alpha), pal::tap(t, alpha));
            else
                dl->AddRectFilled(r0, r1, pal::tap(t, alpha));
            if(tp.shiftMix.get() > 0.05f && k > 0){   // shimmer: each repeat climbs (or falls)
                const float d = (tp.shift.get() >= 0 ? -1.0f : 1.0f) * 4.0f * z;
                dl->AddTriangleFilled(ImVec2(x, r0.y + d - 2 * z), ImVec2(x - 3 * z, r0.y - 2 * z), ImVec2(x + 3 * z, r0.y - 2 * z), pal::tap(t, alpha));
            }
            if(hovered && m.x >= r0.x - 3 * z && m.x <= r1.x + 3 * z && m.y >= y0 && m.y <= y0 + laneH) hoverTap = t;
            amp *= feed;
            if(tp.freeze.get() && k > 40) break;
        }
        // the handle: the first echo's time
        const float hx = X(sec);
        dl->AddCircleFilled(ImVec2(hx, laneTop + 1), (sel ? 5.0f : 3.5f) * z, pal::tap(t, sel ? 255 : 160));
        if(hovered && std::abs(m.x - hx) < 7 * z && m.y < laneTop + 12 * z) hoverTap = t;
    }
    // the taps' live levels, top right
    for(int t = 0; t < tapCount; t++){
        const float lv = t < (int)visValues.size() ? visValues[t] : 0.0f;
        const float db = 20.0f * std::log10(std::max(1e-5f, lv));
        const ImVec2 c(b.x - (tapCount - t) * 14.0f * z, a.y + 9 * z);
        dl->AddCircle(c, 4.5f * z, pal::tap(t, 200), 0, 1.2f * z);
        dl->AddCircleFilled(c, 4.5f * z * ofClamp((db + 50.0f) / 50.0f, 0.0f, 1.0f), pal::tap(t, 255));
    }
    dl->PopClipRect();

    // ── Mouse ────────────────────────────────────────────────────────────────
    if(ImGui::IsItemActivated() && hoverTap >= 0){ selectedTap = hoverTap; selectTabNext = hoverTap; mapDragTap = hoverTap; }
    if(ImGui::IsItemActive() && mapDragTap >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)){
        Tap& tp = taps[mapDragTap];
        const float sec = ofClamp((m.x - x0) / (x1 - x0) * view, 0.002f, 4.0f);
        if(tp.unit.get() == 1){
            float beats = sec / beat;
            if(!ImGui::GetIO().KeyShift) beats = kNotes[nearestNote(beats)].second;   // note values
            tp.beats = ofClamp(beats, 0.0625f, 16.0f);
        }else{
            tp.ms = ImGui::GetIO().KeyShift ? sec * 1000.0f : std::round(sec * 1000.0f);
        }
    }
    if(ImGui::IsItemDeactivated()) mapDragTap = -1;
    if(hovered){
        if(hoverTap >= 0){
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            const Tap& tp = taps[hoverTap];
            ImGui::SetTooltip("Tap %d: %s (%.0f ms), feedback %.0f%%\nDrag to set its time (note values; Shift: free)",
                              hoverTap + 1, tp.unit.get() == 1 ? beatsLabel(tp.beats.get()).c_str() : "ms",
                              tapSeconds(hoverTap) * 1000.0f, tp.feed.get() * 100.0f);
        }else{
            ImGui::SetTooltip("Each tap's repeats, one lane per channel (bar height: level; width: diffusion)");
        }
    }
    // resize grip
    const float gripH = 8.0f * z;
    const ImVec2 g = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##ebMapResize", ImVec2(w, gripH));
    const bool gh = ImGui::IsItemHovered(), ga = ImGui::IsItemActive();
    const ImU32 col = gh || ga ? IM_COL32(180, 180, 180, 200) : IM_COL32(90, 90, 90, 150);
    dl->AddLine(ImVec2(g.x, g.y + gripH * 0.5f), ImVec2(g.x + w, g.y + gripH * 0.5f), col, (gh || ga ? 2.0f : 1.0f) * z);
    for(int k = -1; k <= 1; k++) dl->AddCircleFilled(ImVec2(g.x + w * 0.5f + k * 7 * z, g.y + gripH * 0.5f), 2 * z, col);
    if(gh || ga) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if(ga && ImGui::IsMouseDragging(0, 0.0f)) mapHeight = ofClamp(mapHeight + ImGui::GetIO().MouseDelta.y, 80 * z, 600 * z);
    if(gh && ImGui::IsMouseDoubleClicked(0)) mapHeight = 0.0f;
}

void scEchoBox::drawTapControls(int t, float w) {
    const float z = zoom();
    Tap& tp = taps[t];
    const float cw = colW(w, 4);
    const ImU32 ac = pal::tap(t);
    labelW = 70.0f;
    ImGui::PushID(t);
    ImGui::BeginGroup();
    sectionHeader("Time & Feedback", cw, nullptr, ac);
    combo(tp.unit, "Unit", cw, {"Milliseconds", "Beats"});
    if(tp.unit.get() == 1) beatsSlider(tp, cw);
    else slider(tp.ms, "Time", cw, "%.1f ms", true);
    if(tapSeconds(t) >= 3.999f) ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "4 s at most");
    slider(tp.feed, "Feedback", cw, "%.2f");
    if(tp.feed.get() > 1.0f && ImGui::IsItemHovered()) ImGui::SetTooltip("Above 1: it grows until the saturation holds it");
    slider(tp.level, "Level", cw, "%.2f");
    slider(tp.send, "Send", cw, "%.2f");
    toggle(tp.freeze, "Freeze (hold the loop)");
    nextCol();
    sectionHeader("Space", cw, nullptr, ac);
    slider(tp.pingpong, "Ping-pong", cw, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("How much each repeat moves to the next channel");
    combo(tp.bounce, "Direction", cw, {"Forwards", "Backwards"});
    if(n() > 1) sliderInt(tp.rotate, "Start Ch", cw);
    else ImGui::TextColored(ImColor(pal::textDim), "(one channel: no bouncing)");
    slider(tp.spread, "Spread", cw, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Each channel diffuses and wobbles a little differently");
    slider(tp.wowRate, "Wow Rate", cw, "%.2f Hz", true);
    slider(tp.wowDepth, "Wow Depth", cw, "%.2f ms");
    nextCol();
    sectionHeader("Tone", cw, nullptr, ac);
    combo(tp.ftype, "Filter", cw, {"Off", "Low pass", "High pass", "Band pass", "Peak"});
    const float hz = 440.0f * std::pow(2.0f, (tp.cutoff.get() - 69.0f) / 12.0f);
    const std::string cf = ofToString(tp.cutoff.get(), 0) + "  (" + (hz >= 1000 ? ofToString(hz / 1000, 1) + " kHz)" : ofToString(hz, 0) + " Hz)");
    if(tp.ftype.get() == 0) ImGui::BeginDisabled();
    slider(tp.cutoff, "Cutoff", cw, cf.c_str());
    slider(tp.res, "Resonance", cw, "%.2f");
    if(tp.ftype.get() == 0) ImGui::EndDisabled();
    slider(tp.lowcut, "Low Cut", cw, "%.0f Hz", true);
    {   // what one trip around the loop does to the spectrum
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float gh = 46.0f * z;
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + cw, a.y + gh);
        canvas(dl, a, b);
        const float sr = 48000.0f, fc = std::min(hz, sr * 0.45f), q = 1.0f / std::max(0.05f, 1.0f - tp.res.get());
        std::vector<ImVec2> pts;
        for(int i = 0; i <= 80; i++){
            const float f = 20.0f * std::pow(1000.0f, i / 80.0f);
            const std::complex<float> s(0.0f, f / fc);
            std::complex<float> hf(1.0f, 0.0f);
            switch(tp.ftype.get()){
                case 1: hf = 1.0f / (s * s + s / q + 1.0f); break;
                case 2: hf = (s * s) / (s * s + s / q + 1.0f); break;
                case 3: hf = (s / q) / (s * s + s / q + 1.0f); break;
                case 4: hf = 1.0f + (s / q) * (std::pow(10.0f, (1.0f - 1.0f / q) * 12.0f / 20.0f) - 1.0f) / (s * s + s / q + 1.0f); break;
                default: break;
            }
            const float lc = f / std::sqrt(f * f + tp.lowcut.get() * tp.lowcut.get());
            const float db = 20.0f * std::log10(std::max(1e-5f, std::abs(hf) * lc));
            pts.push_back(ImVec2(a.x + cw * i / 80.0f, ofClamp(a.y + gh * 0.3f - db * gh / 40.0f, a.y + 1, b.y - 1)));
        }
        dl->AddPolyline(pts.data(), (int)pts.size(), ac, 0, 1.5f * z);
        ImGui::Dummy(ImVec2(cw, gh));
    }
    nextCol();
    sectionHeader("Color", cw, nullptr, ac);
    slider(tp.blur, "Diffusion", cw, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Each repeat runs through the diffuser again: the first is clear, the later ones melt");
    slider(tp.blurTime, "Diff Time", cw, "%.1f ms", true);
    slider(tp.drive, "Drive", cw, "%.1f dB");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Saturation in the loop: repeats get warmer and denser");
    slider(tp.shiftMix, "Shimmer", cw, "%.2f");
    slider(tp.shift, "Shim Pitch", cw, "%+.1f st");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Each repeat is pitch-shifted by this much");
    ImGui::EndGroup();
    ImGui::PopID();
    (void)z;
}

void scEchoBox::drawOutput(float w) {
    const float cw = colW(w, 3);
    labelW = 70.0f;
    ImGui::BeginGroup();
    sectionHeader("Output", cw);
    slider(mix, "Mix", cw, "%.2f");
    slider(gain, "Output", cw, "%.2f");
    slider(duck, "Duck", cw, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("The echoes step back while the input plays");
    slider(duckRel, "Duck Rel", cw, "%.2f s", true);
    nextCol();
    sectionHeader("Reverb on the echoes", cw, &rvbOn);
    if(!rvbOn.get()) ImGui::BeginDisabled();
    slider(rvbMix, "Mix", cw, "%.2f");
    slider(rvbDecay, "Decay", cw, "%.2f s", true);
    slider(rvbSize, "Size", cw, "%.1f");
    slider(rvbPredelay, "Predelay", cw, "%.0f ms");
    slider(rvbLate, "Late", cw, "%.2f");
    slider(rvbSpread, "Spread", cw, "%+.2f");
    if(!rvbOn.get()) ImGui::EndDisabled();
    nextCol();
    sectionHeader("Reverb Tone", cw);
    if(!rvbOn.get()) ImGui::BeginDisabled();
    slider(rvbLowpass, "Lowpass", cw, "%.0f Hz", true);
    slider(rvbHiDamp, "Hi Damp", cw, "%.1f dB");
    slider(rvbLoDamp, "Lo Damp", cw, "%.1f dB");
    slider(rvbModRate, "Mod Rate", cw, "%.3f Hz", true);
    slider(rvbSpin, "Spin", cw, "%.2f");
    slider(rvbWander, "Wander", cw, "%.2f");
    if(!rvbOn.get()) ImGui::EndDisabled();
    ImGui::EndGroup();
}
