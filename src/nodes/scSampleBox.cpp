//
//  scSampleBox.cpp
//  ofxOceanodeSuperCollider
//

#include "scSampleBox.h"
#include "scSampleEmbedding.h"
#include "scTypedValue.h"
#include "scEnvelopeCurve.h"
#include "scSynthControlBatch.h"
#include "ofxOceanodeShared.h"
#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxSuperCollider.h"
#include "ofxOceanodeContainer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace {
bool liveBuffer(ofxSCServer* s, ofxSCBuffer* b) {
    return s && b && b->index >= 0 && b->index < (int)s->buffers.size() && s->buffers[b->index] == b;
}
bool liveAudioBus(ofxSCServer* s, ofxSCBus* b) {
    return s && b && b->index >= 0 && b->index < (int)s->audioBusses.size() && s->audioBusses[b->index] == b;
}
bool liveControlBus(ofxSCServer* s, ofxSCBus* b) {
    return s && b && b->index >= 0 && b->index < (int)s->controlBusses.size() && s->controlBusses[b->index] == b;
}
// a bus that is ours on this server (a new one after a reboot), or nullptr
void ensureBus(ofxSCServer* s, ofxSCBus*& b, int rate, int channels) {
    const bool live = rate == RATE_AUDIO ? liveAudioBus(s, b) : liveControlBus(s, b);
    if(live) return;
    delete b;     // after a reboot its index belongs to nobody
    b = new ofxSCBus(rate, channels, s);
    const bool ok = rate == RATE_AUDIO ? liveAudioBus(s, b) : liveControlBus(s, b);
    if(!ok) { b->free(); delete b; b = nullptr; }
}
void dropBus(ofxSCServer* s, ofxSCBus*& b, int rate) {
    if(!b) return;
    const bool live = rate == RATE_AUDIO ? liveAudioBus(s, b) : liveControlBus(s, b);
    if(live) b->free();
    delete b;
    b = nullptr;
}

// GrainBox's palette (scGrainBoxPoly.cpp, gbpPal)
namespace pal {
const ImU32 canvas     = IM_COL32( 11,  12,  14, 255);
const ImU32 border     = IM_COL32( 46,  50,  58, 255);
const ImU32 grid       = IM_COL32(255, 255, 255,  14);
const ImU32 gridStrong = IM_COL32(255, 255, 255,  34);
const ImU32 header     = IM_COL32( 30,  32,  38, 255);
const ImU32 headerText = IM_COL32(205, 205, 215, 255);
const ImU32 textDim    = IM_COL32(150, 150, 160, 255);
const ImU32 outside    = IM_COL32(  0,   0,   0,  90);
const ImU32 handle     = IM_COL32(235, 235, 240, 230);
const ImU32 splitter   = IM_COL32( 90,  90,  90, 150);
const ImU32 splitterHot= IM_COL32(180, 180, 180, 200);
const unsigned char accent[3] = {108, 180, 232};     // SampleBox: GrainBox's sky
const unsigned char voiceRGB[8][3] = {
    {150, 165, 255}, {236, 178,  96}, { 96, 200, 176}, {228, 122, 146},
    {170, 208, 112}, {186, 146, 232}, {108, 180, 232}, {232, 146, 104},
};
ImU32 accentCol(int alpha, float k = 1.0f) {
    auto c = [k](unsigned char v) { return (int)std::min(255.0f, v * k); };
    return IM_COL32(c(accent[0]), c(accent[1]), c(accent[2]), alpha);
}
ImU32 voiceCol(int v, int alpha) {
    const auto* c = voiceRGB[((v % 8) + 8) % 8];
    return IM_COL32(c[0], c[1], c[2], alpha);
}
// a pad's colour (the list, the strips)
ImU32 padCol(int k, int alpha) { return voiceCol(k, alpha); }
}

std::string formatTime(float seconds) {
    char s[32];
    if(seconds < 60.0f) std::snprintf(s, sizeof(s), "%.3f s", seconds);
    else std::snprintf(s, sizeof(s), "%d:%06.3f", (int)(seconds / 60.0f), std::fmod(seconds, 60.0f));
    return s;
}

const char* fxNames[] = {"EQ", "Comp", "Sat", "Echo", "Reverb", "Stereo"};
const char* fxDefs[] = {"SampleBoxEQ_", "SampleBoxComp_", "SampleBoxSat_", "SampleBoxEcho_", "SampleBoxReverb_", "SampleBoxImage_"};
}

// ════════════════════════════════════════════════════════════════════════════
// Setup
// ════════════════════════════════════════════════════════════════════════════

scSampleBox::scSampleBox(std::vector<serverManager*> s)
    : scNode("SampleBox"), servers(std::move(s)) {
    color = ofColor(103, 178, 217);
    description = "Sampler kit: 1 to 16 pads, each with its own sample, sound (one-shot / loop / gated, resample "
                  "or time stretch, reverse, filter, envelopes and LFOs routed to its parameters) and mixer "
                  "channel (level, pan, mute / solo, EQ, compressor, saturator, echo, reverb, stereo image; an "
                  "effect costs nothing while off). Pads play the channels of Trig/Gate Audio (or the Trigger/Gate "
                  "values): wire a MetroBox's or EuclideanBox's Gates in and lane n plays pad n. Out: every pad "
                  "mixed; Pad <n>: each alone. Show opens the editor; right-click a control there to publish it.";
    for(auto* sm : servers) if(sm && sm->getServer()) { browser.setPreviewServer(sm->getServer()); break; }
    std::string dir = ofToDataPath("Supercollider/Samples", true);
    if(!std::filesystem::exists(dir)) dir = ofToDataPath("", true);
    browser.refresh(dir);
    browser.onActivate = [this](const std::string& path) {
        if(!pads.empty()) loadSample(*pads[std::clamp(selected, 0, (int)pads.size() - 1)], path);
        browser.stopPreview();
    };
}

scSampleBox::~scSampleBox() {
    nodeListeners.unsubscribeAll();
    browser.stopPreview();
    for(auto& p : pads) if(p->sample->task.valid()) p->sample->task.wait();
    std::vector<ofxSCServer*> active;
    for(auto& [srv, n] : nodeSC) active.push_back(srv);
    for(auto* srv : active) free(srv);
    for(auto& p : pads) {
        p->listeners.unsubscribeAll();
        while(!p->sample->buffers.empty()) releaseBuffers(*p->sample, p->sample->buffers.begin()->first);
    }
}

void scSampleBox::setup() {
    addInspectorParameter(embedSamples.set("Embed Samples", true));
    addSeparator("SampleBox");
    addParameter(showWindow.set("Show", false));
    triggerHandle = addParameter(trigger.set("Trigger/Gate", {0.0f}, {0.0f}, {1.0f}));
    pitchIn.attach(addParameter(pitch.set("Pitch", {60.0f}, {0.0f}, {127.0f})), availableInputs,
                   [this](){ for(auto& o : outputs) o = o; });
    velIn.attach(addParameter(velocity.set("Velocity", {1.0f}, {0.0f}, {1.0f})), availableInputs,
                 [this](){ for(auto& o : outputs) o = o; });
    noteInputs.add(triggerHandle, trigger);
    noteInputs.add(pitchIn.handle, pitch);
    noteInputs.add(velIn.handle, velocity);
    noteInputs.enable(this, [this](){
        for(auto& p : pads) for(auto& [srv, sc] : p->sc) if(sc.engine) return true;
        return false;
    });
    addParameter(gain.set("Gain", 0.7f, 0.0f, 2.0f));
    addParameter(channels.set("N Chan", 2, 1, MaxChannels));
    addInput("Trig/Gate Audio");
    addOutput("Out");

    // Trigger/Gate: element n is channel n; a rising edge plays the pads on
    // it (pulses shorter than a control block survive: SC sees a counter)
    // (The timeline's frame set of a value it already sent timetagged: nothing
    // to do -- the edges were taken when it was scheduled.)
    nodeListeners.push(trigger.newListener([this](std::vector<float>& v) {
        if(scScheduling::isBackendSendSuppressed()) return;
        for(auto& up : pads) {
            Pad& p = *up;
            const int ch = padChannel(p);
            const float lvl = ch < (int)v.size() ? v[ch] : 0.0f;
            const bool high = lvl > 0.0f;
            if(high && !p.gateHigh) fire(p);
            p.gateHigh = high;
            if(!p.auditionHeld && !p.autoPlaying) for(auto& [srv, s] : p.sc) if(s.engine) s.engine->set("gate", lvl);
        }
    }));
    nodeListeners.push(pitch.newListener([this](std::vector<float>&) {
        if(scScheduling::isBackendSendSuppressed()) return;
        for(auto& p : pads) sendChannelInputs(*p);
    }));
    nodeListeners.push(velocity.newListener([this](std::vector<float>&) {
        if(scScheduling::isBackendSendSuppressed()) return;
        for(auto& p : pads) sendChannelInputs(*p);
    }));
    nodeListeners.push(gain.newListener([this](float&) {
        for(auto& p : pads) for(auto& [srv, s] : p->sc) if(s.strip) s.strip->set("master", gain.get());
    }));
    nodeListeners.push(channels.newListener([this](int&) { if(!restoring) rebuildAll(); }));
    // Files dropped from the Finder on the window: the pad under the mouse, or the selected one
    dragListener = ofEvents().fileDragEvent.newListener([this](ofDragInfo& info) {
        if(!showWindow.get() || !windowHovered || info.files.empty()) return;
        pendingDropFile = info.files.front().string();
    });
    addPad();
    timeline.setup(this, [this](const std::string& k) { publish(k); }, [this](const std::string& k) { unpublish(k); },
                   [this]() { return publishedKeys; });

    // Snapshots (the shared system): the kit (pads, their samples and settings)
    snaps.setupParameters();
    for(auto* p : {&snaps.snapshotP, &snaps.morphAP, &snaps.morphBP, &snaps.morphCurveP, &snaps.morphSwitchP}) registerNodeControl(*p);
    for(auto* p : {&snaps.morphP, &snaps.morphTimeP}) registerNodeControl(*p);
    for(auto* p : {&snaps.morphBeatsP, &snaps.morphGlideP}) registerNodeControl(*p);
    initialPad = padState(0);
    {
        scSnapshots::Host h;
        h.folder = "SampleBox"; h.title = "SampleBox";
        h.capture = [this]() { return captureSnapshot(); };
        h.apply = [this](const ofJson& a, const ofJson& b, float t, bool sw) { applySnapshot(a, b, t, sw); };
        h.factory = [this]() { return factorySnapshots(); };
        h.bpm = [this]() { return currentBpm(); };
        h.paramMenu = [this](const std::string& k) { drawPublishedUnderline(k); drawPublishMenu(k); };
        h.morphBlocked = [this](const std::string& k) {
            auto it=published.find(k); return it!=published.end() && it->second->hasInConnection();
        };
        h.morphChanged = [this]() { for(auto& p : pads) syncPad(*p); };
        snaps.setup(h);
    }
    requireSynthdefs();
}

// A node control (no pad): the snapshot settings
template<typename T>
void scSampleBox::registerNodeControl(ofParameter<T>& param) {
    Control c;
    c.key = c.base = param.getEscapedName();
    c.pad = -1;
    if constexpr(std::is_same<T, float>::value) c.f = &param;
    else if constexpr(std::is_same<T, int>::value) c.i = &param;
    else c.b = &param;
    c.add = [this, &param]() { return addParameter(param); };
    c.save = [&param]() { return param.toString(); };
    c.load = [&param](const std::string& v) { param.fromString(v); };
    controls.push_back(std::move(c));
}

// ════════════════════════════════════════════════════════════════════════════
// Snapshots
// ════════════════════════════════════════════════════════════════════════════

ofJson scSampleBox::captureSnapshot() const {
    ofJson arr = ofJson::array();
    for(int k = 0; k < (int)pads.size(); k++) arr.push_back(padState(k));
    return {{"pads", arr}};
}

// The arrival's kit at the switch point (pads, samples, every setting);
// before and after it the pads' numbers glide
void scSampleBox::applySnapshot(const ofJson& a, const ofJson& b, float t, bool sw) {
    if(!a.is_object() || !b.is_object()) return;
    const ofJson& side = sw ? b : a;
    if(side.contains("pads") && side["pads"].is_array() && side["pads"] != appliedKit) {
        appliedKit = side["pads"];
        const ofJson& arr = side["pads"];
        if(!side.value("allPads", false)) {   // a kit: as many pads as it has
            const int n = std::clamp((int)arr.size(), 1, MaxPads);
            while((int)pads.size() < n) if(!addPad()) break;
            while((int)pads.size() > n) removePad((int)pads.size() - 1);
        }
        for(int k = 0; k < (int)pads.size(); k++) {
            const ofJson& ps = arr[std::min<size_t>(k, arr.size() - 1)];   // allPads: the one for all
            applyPadState(k, ps);
            const std::string path = ps.value("path", std::string());
            if(!path.empty() && path != pads[k]->sample->path) loadSample(*pads[k], path);
        }
        for(auto& p : pads) syncPad(*p);
        sendChokeMasks();
        sendMutes();
        selected = std::clamp(selected, 0, (int)pads.size() - 1);
    }
    if(t <= 0.0f || t >= 1.0f) return;
    auto controlsOf = [](const ofJson& s, int k) -> const ofJson* {
        if(!s.contains("pads") || !s["pads"].is_array() || s["pads"].empty()) return nullptr;
        const ofJson& ps = s["pads"][std::min<size_t>(k, s["pads"].size() - 1)];
        return ps.contains("controls") ? &ps["controls"] : nullptr;
    };
    for(auto& c : controls) {
        if(!c.f || c.pad < 0) continue;
        const ofJson* ca = controlsOf(a, c.pad);
        const ofJson* cb = controlsOf(b, c.pad);
        if(!ca || !cb || !ca->contains(c.base) || !cb->contains(c.base)) continue;
        const float va = ofToFloat((*ca)[c.base].get<std::string>()), vb = ofToFloat((*cb)[c.base].get<std::string>());
        const float v = scSnapshots::blendValue(va, vb, t, sw, scSnapshots::logKey(c.base)).get<float>();
        if(std::abs(v - c.f->get()) > 1e-6f) c.f->set(v);
    }
}

// Five pad settings for the whole kit (the samples, channels and pad count stay)
std::vector<ofJson> scSampleBox::factorySnapshots() const {
    static const char* src = R"JSON([
 {"name": "Init"},
 {"name": "Tight Drums", "set": {"Polyphonic": false, "Amp Attack": 0.001, "Amp Release": 0.04, "Loop": false}},
 {"name": "Synced Loops", "set": {"Loop": true, "Sync To Transport": true, "Play With Transport": true,
                                  "Stretch Mode": 1, "Loop Crossfade %": 5.0}},
 {"name": "Reverse Swells", "set": {"Reverse": true, "Amp Attack": 0.4, "Amp Release": 1.2, "Polyphonic": true}},
 {"name": "Lo-fi Filtered", "set": {"Filter On": true, "Filter Type": 0, "Cutoff": 2500.0, "Resonance": 1.2,
                                    "Rate": 0.94, "Transpose": -1.0}}
])JSON";
    ofJson pad = initialPad;
    pad.erase("name");
    pad.erase("path");
    if(pad.contains("controls")) pad["controls"].erase("Channel");   // the routing stays
    std::vector<ofJson> out;
    for(const auto& p : ofJson::parse(src)) {
        ofJson ps = pad;
        if(p.contains("set"))
            for(auto it = p["set"].begin(); it != p["set"].end(); ++it) {
                const ofJson& v = it.value();
                ps["controls"][scSnapshots::escapeKey(it.key())] = v.is_boolean() ? std::string(v.get<bool>() ? "1" : "0") : ofToString(v.get<float>());
            }
        out.push_back({{"name", p["name"]}, {"allPads", true}, {"pads", ofJson::array({ps})}});
    }
    return out;
}

void scSampleBox::requireSynthdefs() {
    std::filesystem::path folder;
    const std::string expected = "SampleBoxPad_1_2_R.scsyndef";
    try {
        const std::filesystem::path root = ofToDataPath(SYNTHDEF_DIRECTORY, true);
        if(std::filesystem::exists(root))
            for(const auto& e : std::filesystem::recursive_directory_iterator(root,
                    std::filesystem::directory_options::skip_permission_denied))
                if(e.is_regular_file() && e.path().filename() == expected) { folder = e.path().parent_path(); break; }
        if(folder.empty()) {
            const auto bundled = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()
                / "synthdefs/Defaults/SampleBox/CompiledSynthdefs/SampleBox";
            if(std::filesystem::exists(bundled / expected)) folder = bundled;
        }
    } catch(const std::exception& e) { ofLogWarning("scSampleBox") << e.what(); }
    if(folder.empty()) { ofLogError("scSampleBox") << "Compile synthdefs/Defaults/SampleBox/samplebox.scd"; return; }
    synthdefFolder = folder.string();
    for(auto* sm : servers) if(sm) sm->requireSynthdefFolder(synthdefFolder);
}

std::string scSampleBox::engineDef(const Pad& p) const {
    return "SampleBoxPad_" + ofToString(std::clamp(p.sample->channels, 1, 2)) + "_" + ofToString(channels.get())
        + (p.stretchMode.get() ? "_T" : "_R");
}

void scSampleBox::setBpm(float b) {
    bpm = b;
    for(auto& p : pads) {
        p->mod.setBpm(b);
        for(auto& [srv, s] : p->sc) if(s.fxSynth[FX_ECHO]) s.fxSynth[FX_ECHO]->set("bpm", b);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Pad controls
// ════════════════════════════════════════════════════════════════════════════

template<typename T>
scSampleBox::Control& scSampleBox::registerPadControl(Pad& p, ofParameter<T>& param, const std::string& label,
                                                      T def, T lo, T hi, int dest, const char* scName) {
    if constexpr(std::is_same<T, bool>::value) param.set(label + p.suffix(), def);
    else param.set(label + p.suffix(), def, lo, hi);
    Control c;
    c.key = param.getEscapedName();
    {
        ofParameter<T> plain;
        if constexpr(std::is_same<T, bool>::value) plain.set(label, def); else plain.set(label, def, lo, hi);
        c.base = plain.getEscapedName();
    }
    c.pad = p.index;
    c.dest = dest;
    c.scName = scName ? scName : "";
    if constexpr(std::is_same<T, float>::value) c.f = &param;
    else if constexpr(std::is_same<T, int>::value) c.i = &param;
    else c.b = &param;
    c.add = [this, &param]() { return addParameter(param); };
    c.save = [&param]() { return param.toString(); };
    c.load = [&param](const std::string& s) { param.fromString(s); };
    const std::string baseKey = c.base;
    const int padIndex = p.index;
    auto readSnapshot = [padIndex, baseKey](const ofJson& j) -> ofJson {
        if(!j.contains("pads") || !j["pads"].is_array() || j["pads"].empty()) return {};
        const auto& ps = j["pads"][std::min<size_t>(padIndex, j["pads"].size()-1)];
        if(!ps.contains("controls") || !ps["controls"].contains(baseKey)) return {};
        return ps["controls"][baseKey];
    };
    if constexpr(std::is_same<T, bool>::value) {
        if((dest >= DEST_FX && c.scName == "on") || c.scName == "filteron") snaps.morph.bypass(param, readSnapshot);
    } else if constexpr(std::is_same<T, int>::value) {
        if(c.scName == "filtertype") snaps.morph.mode(param, 3, readSnapshot);
    }
    controls.push_back(std::move(c));
    Pad* pp = &p;
    const std::string name = scName ? scName : "";
    p.listeners.push(param.newListener([this, pp, &param, dest, name](T& value) {
        snaps.morph.edited(param);
        if(restoring) return;
        Pad& pad = *pp;
        if(name == "stretchmode") { for(auto& [srv, s] : pad.sc) replaceEngine(srv, pad); return; }
        if(name == "channel") { sendChannelInputs(pad); return; }
        if(name == "choke") { sendChokeMasks(); return; }
        if(name == "mute" || name == "solo") { sendMutes(); return; }
        if(name == "fitbeats" || name == "sync") { sendTempo(pad); requestAnchor(); return; }
        if(name == "autoplay") {
            // playing with the transport means looping with it
            if constexpr(std::is_same<T, bool>::value) if(value && !pad.loop.get()) pad.loop = true;
            return;
        }
        if(name.empty()) return;
        auto send = [&](ofxSCSynth* s) {
            if(!s) return;
            if constexpr(std::is_same<T, bool>::value) s->set(name, snaps.morph.activation(param)); else s->set(name, value);
        };
        if(dest == DEST_ENGINE) {
            if(name=="attack" || name=="release" || name=="filtertype") { for(auto& [srv, s] : pad.sc) sendEngine(srv,pad); }
            else for(auto& [srv, s] : pad.sc) send(s.engine);
        }
        else if(dest == DEST_STRIP) { for(auto& [srv, s] : pad.sc) send(s.strip); }
        else if(dest >= DEST_FX && dest < DEST_FX + FX_COUNT) {
            if(name == "on") { for(auto& [srv, s] : pad.sc) syncFx(srv, pad); }
            else for(auto& [srv, s] : pad.sc) send(s.fxSynth[dest - DEST_FX]);
        }
    }));
    return controls.back();
}

// a control of the pad's modulation: publishable, saved, moved with the pad
void scSampleBox::registerModControl(Pad& p, ofAbstractParameter& param,
                                     std::function<std::shared_ptr<ofxOceanodeAbstractParameter>()> add,
                                     std::function<std::string()> save, std::function<void(const std::string&)> load) {
    Control c;
    c.key = param.getEscapedName();
    c.base = c.key;
    const std::string sfx = p.index == 0 ? std::string() : "_" + ofToString(p.index + 1);
    if(!sfx.empty() && c.base.size() > sfx.size() && c.base.compare(c.base.size() - sfx.size(), sfx.size(), sfx) == 0)
        c.base = c.base.substr(0, c.base.size() - sfx.size());
    c.pad = p.index;
    c.add = std::move(add);
    c.save = std::move(save);
    c.load = std::move(load);
    controls.push_back(std::move(c));
}

scSampleBox::Control* scSampleBox::findControl(const std::string& key) {
    for(auto& c : controls) if(c.key == key) return &c;
    return nullptr;
}

void scSampleBox::setupPad(Pad& p) {
    Pad* pp = &p;
#define PAD_F(param, label, def, lo, hi, dest, sc) registerPadControl<float>(p, param, label, def, lo, hi, dest, sc)
#define PAD_I(param, label, def, lo, hi, dest, sc) registerPadControl<int>(p, param, label, def, lo, hi, dest, sc)
#define PAD_B(param, label, def, dest, sc) registerPadControl<bool>(p, param, label, def, false, true, dest, sc)
    // Sound
    PAD_I(p.channel, "Channel", 0, 0, MaxChannels, DEST_NONE, "channel");
    PAD_I(p.choke, "Choke", 0, 0, ChokeGroups, DEST_NONE, "choke");
    PAD_F(p.inPoint, "In Point", 0.0f, 0.0f, 1.0f, DEST_ENGINE, "inpoint");
    PAD_F(p.outPoint, "Out Point", 1.0f, 0.0f, 1.0f, DEST_ENGINE, "outpoint");
    PAD_F(p.loopCrossfade, "Loop Crossfade %", 0.0f, 0.0f, 50.0f, DEST_ENGINE, "loopcrossfade");
    PAD_F(p.rate, "Rate", 1.0f, 0.125f, 8.0f, DEST_ENGINE, "rate");
    PAD_F(p.transpose, "Transpose", 0.0f, -24.0f, 24.0f, DEST_ENGINE, "transpose");
    PAD_F(p.stretch, "Stretch", 1.0f, 0.125f, 8.0f, DEST_ENGINE, "stretch");
    PAD_F(p.warpWindow, "Warp Window", 0.15f, 0.01f, 1.0f, DEST_ENGINE, "warpwindow");
    PAD_B(p.loop, "Loop", false, DEST_ENGINE, "loop");
    PAD_B(p.poly, "Polyphonic", true, DEST_ENGINE, "poly");
    PAD_B(p.gateMode, "Gate Mode", false, DEST_ENGINE, "gatemode");
    PAD_B(p.reverse, "Reverse", false, DEST_ENGINE, "reverse");
    PAD_I(p.voices, "Max Voices", 8, 1, MaxVoices, DEST_ENGINE, "voices");
    PAD_I(p.stretchMode, "Stretch Mode", 0, 0, 1, DEST_ENGINE, "stretchmode");
    PAD_F(p.attack, "Amp Attack", 0.005f, 0.0f, 5.0f, DEST_ENGINE, "attack");
    PAD_F(p.release, "Amp Release", 0.05f, 0.001f, 5.0f, DEST_ENGINE, "release");
    PAD_F(p.pan, "Pan", 0.0f, -1.0f, 1.0f, DEST_ENGINE, "pan");
    PAD_B(p.filterOn, "Filter On", false, DEST_ENGINE, "filteron");
    PAD_I(p.filterType, "Filter Type", 0, 0, 2, DEST_ENGINE, "filtertype");
    PAD_F(p.cutoff, "Cutoff", 16000.0f, 20.0f, 20000.0f, DEST_ENGINE, "cutoff");
    PAD_F(p.resonance, "Resonance", 0.7f, 0.1f, 2.0f, DEST_ENGINE, "resonance");
    {   // the combos' names (published: dropdowns); the window refreshes Channel's sources
        std::vector<std::string> chans = {"Auto: row " + ofToString(p.index + 1)};
        for(int c = 1; c <= MaxChannels; c++) chans.push_back(ofToString(c));
        std::vector<std::string> groups = {"None"};
        for(int g = 1; g <= ChokeGroups; g++) groups.push_back("Group " + ofToString(g));
        dropdowns.set(p.channel.getEscapedName(), chans);
        dropdowns.set(p.choke.getEscapedName(), groups);
        dropdowns.set(p.stretchMode.getEscapedName(), std::vector<std::string>{"Resample", "Stretch"});
        dropdowns.set(p.filterType.getEscapedName(), std::vector<std::string>{"Low pass", "High pass", "Band pass"});
    }
    // Tempo
    PAD_F(p.fitBeats, "Fit Beats", 0.0f, 0.0f, 128.0f, DEST_NONE, "fitbeats");
    PAD_B(p.sync, "Sync To Transport", false, DEST_NONE, "sync");
    PAD_B(p.autoplay, "Play With Transport", false, DEST_NONE, "autoplay");
    // Mixer
    PAD_F(p.level, "Level", 1.0f, 0.0f, 2.0f, DEST_STRIP, "level");
    PAD_B(p.mute, "Mute", false, DEST_STRIP, "mute");
    PAD_B(p.solo, "Solo", false, DEST_STRIP, "solo");
    PAD_B(p.fxOn[FX_EQ], "EQ On", false, DEST_FX + FX_EQ, "on");
    const char* eqNames[5] = {"Low", "Lo Mid", "Mid", "Hi Mid", "High"};
    const char* eqSC[5] = {"eq1", "eq2", "eq3", "eq4", "eq5"};
    const float eqDefaults[5] = {80, 250, 1000, 4000, 12000};
    for(int n = 0; n < 5; ++n) {
        const std::string prefix = std::string("EQ ") + eqNames[n];
        PAD_F(p.eqGain[n], prefix, 0.0f, -24.0f, 24.0f, DEST_FX + FX_EQ, eqSC[n]);
        PAD_F(p.eqFreq[n], prefix + " Freq", eqDefaults[n], 20.0f, 20000.0f, DEST_FX + FX_EQ, (std::string(eqSC[n]) + "freq").c_str());
        PAD_F(p.eqShape[n], prefix + (scEQEditor::isShelf(n) ? " Slope" : " Q"), 1.0f, 0.1f,
           scEQEditor::isShelf(n) ? 4.0f : 20.0f, DEST_FX + FX_EQ, (std::string(eqSC[n]) + "shape").c_str());
    }
    PAD_F(p.eqMix, "EQ Mix", 1.0f, 0.0f, 1.0f, DEST_FX + FX_EQ, "eqmix");
    PAD_B(p.fxOn[FX_COMP], "Compressor On", false, DEST_FX + FX_COMP, "on");
    PAD_F(p.compThreshold, "Comp Threshold", -18.0f, -60.0f, 0.0f, DEST_FX + FX_COMP, "compthreshold");
    PAD_F(p.compRatio, "Comp Ratio", 3.0f, 1.0f, 20.0f, DEST_FX + FX_COMP, "compratio");
    PAD_F(p.compAttack, "Comp Attack", 10.0f, 0.1f, 200.0f, DEST_FX + FX_COMP, "compattack");
    PAD_F(p.compRelease, "Comp Release", 150.0f, 1.0f, 2000.0f, DEST_FX + FX_COMP, "comprelease");
    PAD_F(p.compMakeup, "Comp Makeup", 0.0f, -12.0f, 24.0f, DEST_FX + FX_COMP, "compmakeup");
    PAD_B(p.fxOn[FX_SAT], "Saturator On", false, DEST_FX + FX_SAT, "on");
    PAD_F(p.satDrive, "Sat Drive", 6.0f, 0.0f, 36.0f, DEST_FX + FX_SAT, "satdrive");
    PAD_F(p.satMix, "Sat Mix", 1.0f, 0.0f, 1.0f, DEST_FX + FX_SAT, "satmix");
    PAD_B(p.fxOn[FX_ECHO], "Echo On", false, DEST_FX + FX_ECHO, "on");
    PAD_F(p.echoTime, "Echo Time", 0.75f, 0.0625f, 4.0f, DEST_FX + FX_ECHO, "echotime");
    PAD_F(p.echoFeedback, "Echo Feedback", 0.4f, 0.0f, 0.95f, DEST_FX + FX_ECHO, "echofeedback");
    PAD_F(p.echoTone, "Echo Tone", 6000.0f, 200.0f, 20000.0f, DEST_FX + FX_ECHO, "echotone");
    PAD_F(p.echoSpread, "Echo Spread", 0.0f, 0.0f, 1.0f, DEST_FX + FX_ECHO, "echospread");
    PAD_F(p.echoAmount, "Echo Amount", 0.35f, 0.0f, 1.0f, DEST_FX + FX_ECHO, "echoamount");
    PAD_B(p.fxOn[FX_REVERB], "Reverb On", false, DEST_FX + FX_REVERB, "on");
    PAD_F(p.revSize, "Rev Size", 30.0f, 0.0f, 60.0f, DEST_FX + FX_REVERB, "revsize");
    PAD_F(p.revDecay, "Rev Decay", 4.0f, 0.05f, 60.0f, DEST_FX + FX_REVERB, "revdecay");
    PAD_F(p.revPredelay, "Rev Predelay", 20.0f, 0.0f, 1000.0f, DEST_FX + FX_REVERB, "revpredelay");
    PAD_F(p.revLowpass, "Rev Lowpass", 10000.0f, 20.0f, 20000.0f, DEST_FX + FX_REVERB, "revlowpass");
    PAD_F(p.revPosition, "Rev Position", 0.8f, 0.0f, 1.0f, DEST_FX + FX_REVERB, "revposition");
    PAD_F(p.revSpread, "Rev Spread", 0.15f, -1.0f, 1.0f, DEST_FX + FX_REVERB, "revspread");
    PAD_F(p.revMix, "Rev Mix", 0.33f, 0.0f, 1.0f, DEST_FX + FX_REVERB, "revmix");
    PAD_B(p.fxOn[FX_IMAGE], "Stereo On", false, DEST_FX + FX_IMAGE, "on");
    PAD_F(p.width, "Width", 1.0f, 0.0f, 2.0f, DEST_FX + FX_IMAGE, "width");
    PAD_F(p.diffuse, "Diffuse", 0.0f, 0.0f, 1.0f, DEST_FX + FX_IMAGE, "diffuse");
    PAD_F(p.bassMono, "Bass Mono", 0.0f, 0.0f, 1.0f, DEST_FX + FX_IMAGE, "bassmono");
    PAD_F(p.bassFreq, "Bass Freq", 150.0f, 20.0f, 1000.0f, DEST_FX + FX_IMAGE, "bassfreq");
#undef PAD_F
#undef PAD_I
#undef PAD_B

    // ── The pad's modulation: envelopes, LFOs, routing to its targets ───────
    scModulation::Host h;
    h.node = this;
    auto reg = [this, pp](auto& param, std::function<void()> send) {
        using P = std::decay_t<decltype(param)>;
        registerModControl(*pp, param, [this, &param]() { return addParameter(param); },
            [&param]() {
                if constexpr(std::is_same<P, ofParameter<std::vector<float>>>::value) {
                    std::string out;
                    for(size_t i = 0; i < param.get().size(); i++) out += (i ? "," : "") + ofToString(param.get()[i]);
                    return out;
                } else return param.toString();
            },
            [&param](const std::string& s) {
                if constexpr(std::is_same<P, ofParameter<std::vector<float>>>::value) {
                    std::vector<float> v;
                    for(auto& t : ofSplitString(s, ",", true, true)) v.push_back(ofToFloat(t));
                    if(!v.empty()) param.set(v);
                } else param.fromString(s);
            });
        using V = std::decay_t<decltype(param.get())>;
        pp->listeners.push(param.newListener([this, send](V&) { if(!restoring) send(); }));
    };
    h.regF = [reg](ofParameter<float>& q, std::function<void()> f) { reg(q, f); };
    h.regI = [reg](ofParameter<int>& q, std::function<void()> f) { reg(q, f); };
    h.regB = [reg](ofParameter<bool>& q, std::function<void()> f) { reg(q, f); };
    h.regV = [reg](ofParameter<std::vector<float>>& q, std::function<void()> f) { reg(q, f); };
    h.control = [this](const std::string& k) -> ofAbstractParameter* {
        for(auto& c : controls) if(c.key == k) return c.f ? (ofAbstractParameter*)c.f : c.i ? (ofAbstractParameter*)c.i : nullptr;
        if(k == gain.getEscapedName()) return &gain;
        return nullptr;
    };
    h.publishMenu = [this](const std::string& k) { drawPublishMenu(k); };
    h.publishedUnderline = [this](const std::string& k) { drawPublishedUnderline(k); };
    h.unpublish = [this](const std::string& k) { unpublish(k); };
    h.publishedKeys = [this]() -> const std::vector<std::string>& { return publishedKeys; };
    h.voiceParamsChanged = [this, pp](){ if(!restoring) for(auto& [srv, sc] : pp->sc) sendEngine(srv, *pp); };
    h.voices = []() { return MaxVoices; };      // the synth's 8 voices
    p.mod.nameSuffix = p.suffix();
    // the synth reads these, in this order (samplebox.scd)
    p.mod.setup(h, {{"Pitch", 24, "st"}, {"Rate", 4, "x"}, {"In", 1, "", true}, {"Out", 1, "", true}, {"Gain", 1, ""},
                    {"Pan", 1, ""}, {"Stretch", 4, "x"}, {"Grain", 0.5f, "s"}, {"Loop XF", 50, "%"},
                    {"Cutoff", 8, "oct"}, {"Reso", 1, ""}, {"Drive", 24, "dB"}, {"Comp Thr", 24, "dB"},
                    {"Rev Mix", 1, ""}, {"Width", 1, ""}}, true, true, true);
    p.mod.targetOfKey = {{p.transpose.getEscapedName(), 0}, {p.rate.getEscapedName(), 1}, {p.inPoint.getEscapedName(), 2},
                         {p.outPoint.getEscapedName(), 3}, {p.level.getEscapedName(), 4}, {p.pan.getEscapedName(), 5},
                         {p.stretch.getEscapedName(), 6}, {p.warpWindow.getEscapedName(), 7}, {p.loopCrossfade.getEscapedName(), 8},
                         {p.cutoff.getEscapedName(), 9}, {p.resonance.getEscapedName(), 10}, {p.satDrive.getEscapedName(), 11},
                         {p.compThreshold.getEscapedName(), 12}, {p.revMix.getEscapedName(), 13}, {p.width.getEscapedName(), 14}};
    p.mod.addVoiceParam(p.attack); p.mod.addVoiceParam(p.release);
    p.mod.setBpm(bpm);
}

// ════════════════════════════════════════════════════════════════════════════
// Pads: add, remove, move, duplicate
// ════════════════════════════════════════════════════════════════════════════

scSampleBox::Pad* scSampleBox::addPad() {
    if((int)pads.size() >= MaxPads) return nullptr;
    auto up = std::make_unique<Pad>();
    Pad& p = *up;
    p.index = (int)pads.size();
    p.name = "Pad " + ofToString(p.index + 1);
    pads.push_back(std::move(up));
    restoring = true;
    setupPad(p);
    restoring = false;
    addOutput("Pad " + ofToString(p.index + 1));
    for(auto& [srv, n] : nodeSC) if(n.created) { ensureBuffers(p, srv); createPad(srv, p); }
    sendChokeMasks();
    sendMutes();
    return &p;
}

// The last pad goes (pads after k move up one first)
void scSampleBox::removePad(int k) {
    if(pads.size() <= 1 || k < 0 || k >= (int)pads.size()) return;
    movePad(k, (int)pads.size() - 1);
    Pad& p = *pads.back();
    const bool hadLfoOutputs = p.mod.hasLfoOutputs();
    const int idx = p.index;
    std::vector<ofxSCServer*> active;
    for(auto& [srv, s] : p.sc) active.push_back(srv);
    for(auto* srv : active) destroyPad(srv, p);
    // its published controls go, connected or not
    std::vector<std::string> keys;
    for(const auto& c : controls) if(c.pad == idx) keys.push_back(c.key);
    for(const auto& key : keys) if(published.count(key)) unpublish(key, true);
    for(const auto& c : controls) if(c.pad == idx) {
        if(c.b) snaps.morph.erase(*c.b);
        if(c.i) snaps.morph.erase(*c.i);
    }
    controls.erase(std::remove_if(controls.begin(), controls.end(), [idx](const Control& c) { return c.pad == idx; }), controls.end());
    p.listeners.unsubscribeAll();
    if(p.sample->task.valid()) p.sample->task.wait();
    while(!p.sample->buffers.empty()) releaseBuffers(*p.sample, p.sample->buffers.begin()->first);
    for(auto& [srv, m] : outputBuses) m.erase(idx + 1);
    removeOutput(idx + 1);
    pads.pop_back();
    if(hadLfoOutputs) parameterGroupChanged.notify(this);
    selected = std::clamp(selected, 0, (int)pads.size() - 1);
    sendChokeMasks();
    sendMutes();
}

void scSampleBox::duplicatePad(int k) {
    if(k < 0 || k >= (int)pads.size()) return;
    const ofJson state = padState(k);
    const std::string path = pads[k]->sample->path;
    Pad* p = addPad();
    if(!p) return;
    applyPadState(p->index, state);
    p->name = pads[k]->name + " copy";
    if(!path.empty()) loadSample(*p, path);
    syncPad(*p);
    movePad(p->index, k + 1);
    selected = k + 1;
}

// Every pad's settings, sample and name go with it; the pads keep their
// places (their parameters' names, outputs and synths)
void scSampleBox::movePad(int from, int to) {
    const int n = (int)pads.size();
    if(from < 0 || to < 0 || from >= n || to >= n || from == to) return;
    std::vector<ofJson> states(n);
    std::vector<std::unique_ptr<Sample>> samples(n);
    std::vector<std::string> names(n);
    for(int i = 0; i < n; i++) {
        states[i] = padState(i);
        samples[i] = std::move(pads[i]->sample);
        names[i] = pads[i]->name;
    }
    auto move = [&](auto& v) {
        auto x = std::move(v[from]);
        if(from < to) for(int i = from; i < to; i++) v[i] = std::move(v[i + 1]);
        else for(int i = from; i > to; i--) v[i] = std::move(v[i - 1]);
        v[to] = std::move(x);
    };
    move(states);
    move(samples);
    move(names);
    for(int i = 0; i < n; i++) {
        pads[i]->sample = std::move(samples[i]);
        pads[i]->name = names[i];
        applyPadState(i, states[i]);
        pads[i]->playheads.clear();
    }
    for(auto& p : pads) syncPad(*p);
    sendChokeMasks();
    sendMutes();
    if(selected == from) selected = to;
    else if(from < selected && to >= selected) selected--;
    else if(from > selected && to <= selected) selected++;
}

// ── A pad's settings, keyed by base name; its modulation with relative keys ──

std::string scSampleBox::relKey(const std::string& key) const {
    for(const auto& c : controls) if(c.key == key && c.pad >= 0) return "#" + c.base;
    return key;
}
std::string scSampleBox::absKey(const std::string& rel, int pad) const {
    if(rel.empty() || rel[0] != '#') return rel;
    const std::string base = rel.substr(1);
    for(const auto& c : controls) if(c.pad == pad && c.base == base) return c.key;
    return rel;
}
void scSampleBox::rewriteModKeys(ofJson& m, bool toRelative, int pad) const {
    if(!m.contains("Params") || !m["Params"].is_array()) return;
    for(auto& e : m["Params"])
        if(e.is_object() && e.contains("key") && e["key"].is_string())
            e["key"] = toRelative ? relKey(e["key"].get<std::string>()) : absKey(e["key"].get<std::string>(), pad);
}

ofJson scSampleBox::padState(int k) const {
    const Pad& p = *pads[k];
    ofJson j;
    j["name"] = p.name;
    j["path"] = p.sample->path;
    // a modulated control: its own value (the base)
    ofJson values = ofJson::object();
    for(const auto& c : controls) if(c.pad == k) values[c.key] = c.save();
    ofJson bases = ofJson::object();
    for(const auto& c : controls) if(c.pad == k && p.mod.isParamTarget(c.key)) bases[c.key] = 0.0f;
    p.mod.patchBases(bases);
    for(auto it = bases.begin(); it != bases.end(); ++it)
        if(it.value().is_number_integer()) values[it.key()] = ofToString(it.value().get<int>());
        else if(it.value().is_number()) values[it.key()] = ofToString(it.value().get<float>());
    ofJson rel = ofJson::object();
    for(const auto& c : controls) if(c.pad == k && values.contains(c.key)) rel[c.base] = values[c.key];
    j["controls"] = rel;
    ofJson m;
    p.mod.save(m, "");
    rewriteModKeys(m, true, k);
    j["mod"] = m;
    return j;
}

void scSampleBox::applyPadState(int k, const ofJson& j) {
    Pad& p = *pads[k];
    restoring = true;
    if(j.contains("name") && j["name"].is_string()) p.name = j["name"].get<std::string>();
    if(j.contains("controls") && j["controls"].is_object()) {
        const auto& v = j["controls"];
        for(auto& c : controls)
            if(c.pad == k && v.contains(c.base) && v[c.base].is_string())
                try { c.load(v[c.base].get<std::string>()); } catch(...) {}
    }
    restoring = false;
    if(j.contains("mod") && j["mod"].is_object()) {
        ofJson m = j["mod"];
        rewriteModKeys(m, false, k);
        p.mod.load(m, "");
        p.mod.refresh();
        p.mod.afterLoad();
    }
}

int scSampleBox::padChannel(const Pad& p) const {
    const int c = p.channel.get();
    return std::clamp(c <= 0 ? p.index : c - 1, 0, MaxChannels - 1);
}

float scSampleBox::valueFor(const std::vector<float>& v, int channel, float fallback) const {
    if(v.empty()) return fallback;
    if(v.size() == 1) return v[0];
    return channel < (int)v.size() ? v[channel] : fallback;
}

// What plays a channel of Trig/Gate Audio: the source's own words ("Kick")
std::string scSampleBox::channelSource(int channel) {
    if(inputs.empty()) return "";
    const nodePort port = inputs[0].get();
    if(!port.getNodeRef()) return "";
    return port.getNodeRef()->getOutputChannelName(port.getIndex(), channel);
}

std::string scSampleBox::padLabel(const Pad& p) const {
    return ofToString(p.index + 1) + " " + p.name;
}

bool scSampleBox::anySolo() const {
    for(const auto& p : pads) if(p->solo.get()) return true;
    return false;
}

// ════════════════════════════════════════════════════════════════════════════
// Samples
// ════════════════════════════════════════════════════════════════════════════

void scSampleBox::loadSample(Pad& p, const std::string& path) {
    if(path.empty()) return;
    p.sample->pendingFile = path;     // decoded in update()
}

void scSampleBox::applyDecoded(Pad& p, scSlicerBox::Audio&& a) {
    Sample& s = *p.sample;
    if(!a.error.empty()) {
        s.error = std::filesystem::path(a.path).filename().string() + ": " + a.error;
        ofLogWarning("scSampleBox") << s.error;
        return;
    }
    s.error.clear();
    const int oldChannels = s.channels;
    s.path = a.path;
    s.playPath = a.playPath;
    s.channels = std::max(1, std::min(2, a.channels));
    s.rate = a.sampleRate;
    s.seconds = a.frames / std::max(1.0f, a.sampleRate);
    s.peaks.assign(4096, 0.0f);
    for(int f = 0; f < a.frames; ++f) {
        const int bin = (int)((int64_t)f * 4096 / a.frames);
        s.peaks[bin] = std::max(s.peaks[bin], std::min(1.0f, std::abs(a.mono[f])));
    }
    // a pad still named after its position: the file's name
    if(p.name == "Pad " + ofToString(p.index + 1))
        p.name = std::filesystem::path(a.path).stem().string().substr(0, 24);
    for(auto& [srv, sc] : p.sc) {
        if(!sc.engine) continue;
        // the new buffers first, the old ones freed once the synth reads the new
        auto old = std::move(s.buffers[srv]);
        s.buffers.erase(srv);
        ensureBuffers(p, srv);
        if(oldChannels != s.channels) replaceEngine(srv, p);
        else sendEngine(srv, p);
        for(auto* b : old) if(b) { if(liveBuffer(srv, b)) b->free(); delete b; }
    }
}

void scSampleBox::releaseBuffers(Sample& s, ofxSCServer* srv) {
    auto it = s.buffers.find(srv);
    if(it == s.buffers.end()) return;
    for(auto* b : it->second) if(b) { if(liveBuffer(srv, b)) b->free(); delete b; }
    s.buffers.erase(it);
}

void scSampleBox::ensureBuffers(Pad& p, ofxSCServer* srv) {
    Sample& s = *p.sample;
    if(!srv || s.playPath.empty()) return;
    auto it = s.buffers.find(srv);
    if(it != s.buffers.end() && (int)it->second.size() == s.channels &&
       std::all_of(it->second.begin(), it->second.end(), [srv](ofxSCBuffer* b){ return liveBuffer(srv, b); })) return;
    releaseBuffers(s, srv);
    for(int ch = 0; ch < s.channels; ++ch) {
        auto* b = new ofxSCBuffer(0, 0, srv);
        if(b->index < 0) { delete b; releaseBuffers(s, srv); return; }   // no free buffer
        b->readChannel(s.playPath, {ch});
        s.buffers[srv].push_back(b);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Synths: the node's group, each pad's group
// ════════════════════════════════════════════════════════════════════════════

void scSampleBox::sendEngine(ofxSCServer* srv, Pad& p) {
    auto it = p.sc.find(srv);
    if(it == p.sc.end() || !it->second.engine) return;
    PadSC& sc = it->second;
    ofxSCSynth* s = sc.engine;
    for(const auto& c : controls) {
        if(c.pad != p.index || c.dest != DEST_ENGINE || c.scName.empty() || c.scName == "stretchmode") continue;
        if(c.f) {
            if(c.f==&p.attack || c.f==&p.release) s->set(c.scName,p.mod.voiceValues(*c.f));
            else s->set(c.scName, c.f->get());
        }
        else if(c.i) s->set(c.scName, c.i->get());
        else if(c.b) s->set(c.scName, snaps.morph.activation(*c.b));
    }
    s->set("filterweights", snaps.morph.weights(p.filterType));
    const auto bs = p.sample->buffers.find(srv);
    const bool loaded = bs != p.sample->buffers.end() && (int)bs->second.size() >= p.sample->channels;
    s->set("loaded", int(loaded));
    s->set("buf0", loaded ? bs->second[0]->index : 0);
    s->set("buf1", loaded && p.sample->channels > 1 ? bs->second[1]->index : 0);
    s->set("gain", 1.0f);
    p.sentFitRate = fitRateOf(p);
    s->set("fitrate", p.sentFitRate);
    s->set("fitbeats", p.fitBeats.get() > 0.0f ? p.fitBeats.get() : 4.0f);
    s->set("sync", int(p.synced()));
    if(sc.bus) s->set("out", sc.bus->index);
    if(sc.vis) s->set("vis", sc.vis->index);
    if(sc.latest) s->set("latestbus", sc.latest->index);
    if(sc.gateBus) s->set("gatebus", sc.gateBus->index);
    s->set("fire", p.fireCount);
    s->set("stop", p.stopCount);
    // the pad's modulation: the targets it writes, the gates / velocities / randoms it reads
    s->set("modbus", p.mod.targetBus(srv, 0));
    s->set("velbus", p.mod.sourceBus(srv, scModulation::SRC_VEL));
    s->set("rndbus", p.mod.sourceBus(srv, scModulation::SRC_RND));
    auto n = nodeSC.find(srv);
    if(n != nodeSC.end() && n->second.choke) s->set("chokeblock", n->second.choke->index);
    s->set("chokeslot", p.index);
    std::vector<float> mask(16, 0.0f);
    if(p.choke.get() > 0)
        for(const auto& q : pads) if(q.get() != &p && q->choke.get() == p.choke.get() && q->index < 16) mask[q->index] = 1.0f;
    s->set("chokemask", mask);
    // the channel's trigger, pitch, velocity
    const int ch = padChannel(p);
    s->set("trigbus", inputBuses.count(srv) && inputBuses[srv] >= 0 ? inputBuses[srv] + ch : -1);
    const int pb = pitchIn.bus(srv), vb = velIn.bus(srv);
    if(pb >= 0) s->mapan("pitch", pb + ch, 1); else s->set("pitch", valueFor(pitch.get(), ch, 60.0f));
    if(vb >= 0) s->mapan("velocity", vb + ch, 1); else s->set("velocity", valueFor(velocity.get(), ch, 1.0f));
    if(!p.auditionHeld && !p.autoPlaying) s->set("gate", ch < (int)trigger.get().size() ? trigger.get()[ch] : 0.0f);
}

void scSampleBox::sendFx(ofxSCServer* srv, Pad& p, int fx) {
    auto it = p.sc.find(srv);
    if(it == p.sc.end() || !it->second.fxSynth[fx]) return;
    PadSC& sc = it->second;
    ofxSCSynth* s = sc.fxSynth[fx];
    s->set("on", snaps.morph.activation(p.fxOn[fx]));
    if(sc.bus) s->set("bus", sc.bus->index);
    s->set("modbus", p.mod.targetBus(srv, 0));
    if(sc.latest) s->set("latestbus", sc.latest->index);
    if(fx == FX_ECHO) s->set("bpm", bpm);
    for(const auto& c : controls) {
        if(c.pad != p.index || c.dest != DEST_FX + fx || c.scName.empty() || c.scName == "on") continue;
        if(c.f) s->set(c.scName, c.f->get());
        else if(c.i) s->set(c.scName, c.i->get());
        else if(c.b) s->set(c.scName, snaps.morph.activation(*c.b));
    }
}

void scSampleBox::sendStrip(ofxSCServer* srv, Pad& p) {
    auto it = p.sc.find(srv);
    if(it == p.sc.end() || !it->second.strip) return;
    PadSC& sc = it->second;
    ofxSCSynth* s = sc.strip;
    if(sc.bus) s->set("bus", sc.bus->index);
    s->set("level", p.level.get());
    s->set("master", gain.get());
    s->set("mute", int(p.mute.get() || (anySolo() && !p.solo.get())));
    const auto& ob = outputBuses[srv];
    const auto mix = ob.find(0), own = ob.find(p.index + 1);
    s->set("mix", mix != ob.end() ? mix->second : -1);
    s->set("padout", own != ob.end() ? own->second : -1);
    if(sc.meter) s->set("meterbus", sc.meter->index);
}

#ifdef OFXOCEANODE_HAS_TIMELINE_INSTRUMENT
bool scSampleBox::getTimelineInstrument(ofxOceanodeTimelineInstrument& out) {
    if(!triggerHandle || !velIn.handle) return false;
    out.gate = triggerHandle.get();
    out.velocity = velIn.handle.get();
    out.kit = true;
    out.voices = 1;
    for(const auto& p : pads) out.voices = std::max(out.voices, padChannel(*p) + 1);
    return true;
}
#endif

void scSampleBox::sendChannelInputs(Pad& p) {
    auto inEffect = noteInputs.inEffectNow();   // not a scheduled send: the notes as they stand now
    for(auto& [srv, sc] : p.sc) {
        if(!sc.engine) continue;
        const int ch = padChannel(p);
        ofxSCSynth* s = sc.engine;
        s->set("trigbus", inputBuses.count(srv) && inputBuses[srv] >= 0 ? inputBuses[srv] + ch : -1);
        const int pb = pitchIn.bus(srv), vb = velIn.bus(srv);
        if(pb >= 0) s->mapan("pitch", pb + ch, 1); else s->set("pitch", valueFor(pitch.get(), ch, 60.0f));
        if(vb >= 0) s->mapan("velocity", vb + ch, 1); else s->set("velocity", valueFor(velocity.get(), ch, 1.0f));
        if(!p.auditionHeld && !p.autoPlaying) s->set("gate", ch < (int)trigger.get().size() ? trigger.get()[ch] : 0.0f);
    }
}

void scSampleBox::sendChokeMasks() {
    for(auto& up : pads) {
        Pad& p = *up;
        std::vector<float> mask(16, 0.0f);
        if(p.choke.get() > 0)
            for(const auto& q : pads) if(q.get() != &p && q->choke.get() == p.choke.get() && q->index < 16) mask[q->index] = 1.0f;
        for(auto& [srv, sc] : p.sc) if(sc.engine) { sc.engine->set("chokemask", mask); sc.engine->set("chokeslot", p.index); }
    }
}

void scSampleBox::sendMutes() {
    const bool solo = anySolo();
    for(auto& p : pads)
        for(auto& [srv, sc] : p->sc) if(sc.strip) sc.strip->set("mute", int(p->mute.get() || (solo && !p->solo.get())));
}

// The effects that are on, in the chain's order (an effect switched on later
// goes before the next one that exists)
void scSampleBox::syncFx(ofxSCServer* srv, Pad& p) {
    auto it = p.sc.find(srv);
    if(it == p.sc.end() || !it->second.created) return;
    PadSC& sc = it->second;
    for(int fx = 0; fx < FX_COUNT; fx++) {
        const bool want = snaps.morph.activation(p.fxOn[fx]) > 0 && (fx != FX_IMAGE || channels.get() > 1);
        ofxSCSynth*& s = sc.fxSynth[fx];
        if(want && !s) {
            s = new ofxSCSynth(fxDefs[fx] + ofToString(channels.get()), srv);
            sendFx(srv, p, fx);
            ofxSCSynth* next = nullptr;
            for(int j = fx + 1; j < FX_COUNT && !next; j++) next = sc.fxSynth[j];
            if(next) s->createAndRun(2, next->nodeID, true);           // before the next effect
            else s->createAndRun(1, sc.fx->nodeID, true);              // the chain's tail
        } else if(want && s) {
            sendFx(srv, p, fx);
        } else if(!want && s) {
            s->free();
            delete s;
            s = nullptr;
        }
    }
}

void scSampleBox::createPad(ofxSCServer* srv, Pad& p) {
    auto n = nodeSC.find(srv);
    if(n == nodeSC.end() || !n->second.created || !n->second.root) return;
    if(n->second.padsPending) return;            // update() creates every pad once the definitions are in
    if(p.sc.count(srv)) destroyPad(srv, p);   // (it erases the entry)
    PadSC& sc = p.sc[srv];
    ensureBus(srv, sc.bus, RATE_AUDIO, MaxChannels);
    ensureBus(srv, sc.gateBus, RATE_AUDIO, MaxVoices);
    ensureBus(srv, sc.vis, RATE_CONTROL, MaxVoices);
    ensureBus(srv, sc.latest, RATE_CONTROL, 1);
    ensureBus(srv, sc.meter, RATE_CONTROL, 1);
    ensureBuffers(p, srv);
    auto group = [&](ofxSCGroup*& g, int target) { g = new ofxSCGroup(srv); g->create(1, target); };   // tail
    group(sc.group, n->second.root->nodeID);
    group(sc.voices, sc.group->nodeID);
    group(sc.sources, sc.group->nodeID);
    group(sc.routes, sc.group->nodeID);
    group(sc.fx, sc.group->nodeID);
    sc.created = true;
    p.mod.create(srv, sc.sources->nodeID, sc.routes->nodeID, sc.gateBus ? sc.gateBus->index : 0);
    sc.engineDef = engineDef(p);
    sc.engine = new ofxSCSynth(sc.engineDef, srv);
    sendEngine(srv, p);
    sc.engine->createAndRun(0, sc.voices->nodeID, true);
    requestAnchor();
    resumeNotes(srv, p);
    syncFx(srv, p);
    sc.strip = new ofxSCSynth("SampleBoxStrip_" + ofToString(channels.get()), srv);
    sendStrip(srv, p);
    sc.strip->createAndRun(1, sc.group->nodeID, true);                 // after the effects
}

void scSampleBox::destroyPad(ofxSCServer* srv, Pad& p, bool sendFree) {
    auto it = p.sc.find(srv);
    if(it == p.sc.end()) return;
    PadSC& sc = it->second;
    p.mod.free(srv, false);                       // the group's /n_free takes its synths
    if(sendFree && sc.group && sc.created) sc.group->free();
    delete sc.engine; sc.engine = nullptr;
    for(auto*& s : sc.fxSynth) { delete s; s = nullptr; }
    delete sc.strip; sc.strip = nullptr;
    for(auto** g : {&sc.voices, &sc.sources, &sc.routes, &sc.fx, &sc.group}) { delete *g; *g = nullptr; }
    dropBus(srv, sc.bus, RATE_AUDIO);
    dropBus(srv, sc.gateBus, RATE_AUDIO);
    dropBus(srv, sc.vis, RATE_CONTROL);
    dropBus(srv, sc.latest, RATE_CONTROL);
    dropBus(srv, sc.meter, RATE_CONTROL);
    sc.created = false;
    p.sc.erase(it);
}

// A new definition (sample channels, N Chan, Resample / Stretch) takes the old
// engine's place in the pad's group
void scSampleBox::replaceEngine(ofxSCServer* srv, Pad& p) {
    auto it = p.sc.find(srv);
    if(it == p.sc.end() || !it->second.engine) return;
    PadSC& sc = it->second;
    ofxSCSynth* old = sc.engine;
    sc.engineDef = engineDef(p);
    sc.engine = new ofxSCSynth(sc.engineDef, srv);
    sendEngine(srv, p);
    sc.engine->createAndRun(4, old->nodeID, true);   // replace
    delete old;
    requestAnchor();
    resumeNotes(srv, p);
}

void scSampleBox::syncPadServer(ofxSCServer* srv, Pad& p) {
    auto it = p.sc.find(srv);
    if(it == p.sc.end() || !it->second.created) return;
    ensureBuffers(p, srv);
    if(it->second.engineDef != engineDef(p)) replaceEngine(srv, p);
    else sendEngine(srv, p);
    syncFx(srv, p);
    for(int fx = 0; fx < FX_COUNT; fx++) sendFx(srv, p, fx);
    sendStrip(srv, p);
}

void scSampleBox::syncPad(Pad& p) {
    std::vector<ofxSCServer*> active;
    for(auto& [srv, sc] : p.sc) active.push_back(srv);
    for(auto* srv : active) syncPadServer(srv, p);
}

// N Chan: every synth is another definition
void scSampleBox::rebuildAll() {
    for(auto& [srv, n] : nodeSC) {
        if(!n.created) continue;
        for(auto& p : pads) createPad(srv, *p);   // destroys the old first
    }
}

void scSampleBox::buildSynth(ofxSCServer* srv) {
    if(!srv) return;
    if(nodeSC.count(srv)) free(srv);
    for(auto& p : pads) ensureBuffers(*p, srv);
    nodeSC[srv].root = new ofxSCGroup(srv);
}

void scSampleBox::createSynth(ofxSCServer* srv) {
    auto& n = nodeSC[srv];
    if(!n.root) n.root = new ofxSCGroup(srv);
    n.root->create(0, 1);
    n.created = true;
    ensureBus(srv, n.choke, RATE_CONTROL, 16);
    // Our definitions still loading (a preset opened right after the node was
    // made): an /s_new now would fail, silent until the next graph rebuild.
    // The group holds our place in the graph; the pads come once synced.
    if(srv->isNRTSyncPending()) {
        n.padsPending = true;
        n.pendingSince = ofGetElapsedTimef();
    }
    for(auto& p : pads) createPad(srv, *p);
    if(!getActive()) n.root->run(false);
}

void scSampleBox::free(ofxSCServer* srv) {
    for(auto& p : pads) destroyPad(srv, *p, false);
    auto it = nodeSC.find(srv);
    if(it == nodeSC.end()) return;
    if(it->second.root && it->second.created) it->second.root->free();   // and everything in it
    delete it->second.root;
    dropBus(srv, it->second.choke, RATE_CONTROL);
    nodeSC.erase(it);
}

void scSampleBox::setOutputBus(ofxSCServer* srv, int index, int bus) {
    outputBuses[srv][index] = bus;
    for(auto& p : pads) {
        if(index != 0 && index != p->index + 1) continue;
        auto it = p->sc.find(srv);
        if(it != p->sc.end() && it->second.strip) it->second.strip->set(index == 0 ? "mix" : "padout", bus);
    }
}
int scSampleBox::getOutputBusIndex(ofxSCServer* srv, int index) {
    auto it = outputBuses.find(srv);
    if(it == outputBuses.end()) return -1;
    auto b = it->second.find(index);
    return b != it->second.end() ? b->second : -1;
}
void scSampleBox::moveSynthBefore(ofxSCServer* srv, int nodeID) {
    auto it = nodeSC.find(srv);
    if(it != nodeSC.end() && it->second.created) it->second.root->moveBefore(nodeID);
}
int scSampleBox::getLastSynthID(ofxSCServer* srv) {
    auto it = nodeSC.find(srv);
    return (it != nodeSC.end() && it->second.created) ? it->second.root->nodeID : -1;
}
void scSampleBox::setInputBus(ofxSCServer* srv, scNode* node, int bus) {
    // a Pitch / Velocity cable (told apart by its source's bus), else the trigger / gate input
    if(!(pitchIn.take(srv, node, bus) | velIn.take(srv, node, bus))) inputBuses[srv] = bus;
    for(auto& p : pads) sendChannelInputs(*p);
}
void scSampleBox::resetInputBusses(ofxSCServer* srv, int) {
    inputBuses.erase(srv);
    pitchIn.reset(srv);
    velIn.reset(srv);
    for(auto& p : pads) sendChannelInputs(*p);
}

void scSampleBox::fire(Pad& p) {
    ++p.fireCount;
    if(p.loop.get()) p.loopRunning = true;
    for(auto& [srv, sc] : p.sc) if(sc.engine) sc.engine->set("fire", p.fireCount);
}
void scSampleBox::resumeNotes(ofxSCServer* srv, Pad& p) {
    if(!p.loop.get() || !(p.autoPlaying || p.loopRunning)) return;
    auto it = p.sc.find(srv);
    if(it == p.sc.end() || !it->second.engine) return;
    if(p.autoPlaying && p.gateMode.get()) it->second.engine->set("gate", 1.0f);
    // only this server's engine: the others still play theirs
    // (their Changed.kr sees the next fire as one change, whatever the jump)
    it->second.engine->set("fire", ++p.fireCount);
}
void scSampleBox::stop(Pad& p) {
    p.loopRunning = false;
    ++p.stopCount;
    for(auto& [srv, sc] : p.sc) if(sc.engine) sc.engine->set("stop", p.stopCount);
}

void scSampleBox::resendParametersForNRT() {
    for(auto& p : pads) {
        syncPad(*p);
        p->mod.resend();
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Tempo: Fit and Sync To Transport
// ════════════════════════════════════════════════════════════════════════════

float scSampleBox::currentBpm() const {
    const float b = getFrameTransportState().current.bpm;
    if(b > 0.0f) return b;
    return bpm > 0.0f ? bpm : 120.0f;
}

double scSampleBox::beatsPerBar() const {
    if(auto* container = getHostContainer()) {
        if(auto* extension = container->getAutomationExtension())
            return std::max(0.25, extension->getBeatsPerBar());
    }
    return 4.0;
}

// The speed that makes the In..Out region last Fit beats now
float scSampleBox::fitRateOf(const Pad& p) const {
    const float region = (p.outPoint.get() - p.inPoint.get()) * p.sample->seconds;
    if(p.fitBeats.get() <= 0.0f || region <= 0.0f) return 1.0f;
    return region / (p.fitBeats.get() * 60.0f / currentBpm());
}

// Beats a region of `seconds` most likely is: a power of two of bars (or of
// beats) that puts its own tempo nearest 120 BPM, between 70 and 180
float scSampleBox::guessBeats(float seconds, double bpb) {
    if(seconds <= 0.0f) return 0.0f;
    float best = 0.0f, bestScore = 1e9f;
    std::vector<float> candidates;
    for(int k = -2; k <= 6; k++) candidates.push_back((float)(bpb * std::pow(2.0, k)));
    for(int k = 0; k <= 3; k++) candidates.push_back((float)std::pow(2.0, k));   // 1, 2, 4, 8 beats
    for(float beats : candidates) {
        const float tempo = beats * 60.0f / seconds;
        float score = std::abs(std::log2(tempo / 120.0f));
        if(tempo < 70.0f || tempo > 180.0f) score += 10.0f;
        if(score < bestScore) { bestScore = score; best = beats; }
    }
    return best;
}

void scSampleBox::sendTempo(Pad& p) {
    p.sentFitRate = fitRateOf(p);
    for(auto& [srv, sc] : p.sc) {
        if(!sc.engine) continue;
        sc.engine->set("fitrate", p.sentFitRate);
        sc.engine->set("fitbeats", p.fitBeats.get() > 0.0f ? p.fitBeats.get() : 4.0f);
        sc.engine->set("sync", int(p.synced()));
    }
}

void scSampleBox::requestAnchor() {
    follower.requestAnchor();
}

// The transport's position for the synced pads: each gets the beat modulo a
// multiple of its Fit (below 2048 beats, so float32 stays precise; the phase
// over the region is the same), timetagged at the anchor's instant
void scSampleBox::sendTransportAnchors() {
    bool any = false;
    for(auto& p : pads) if(p->synced()) any = true;
    if(!any) return;
    bool pending = false;
    for(const auto& a : follower.poll(getFrameTransportState())) {
        anchorIdCounter = anchorIdCounter % (1 << 20) + 1;
        scTransportSync::AnchorScope scope(a);
        for(auto& up : pads) {
            Pad& p = *up;
            if(!p.synced()) continue;
            const double fit = p.fitBeats.get();
            const double period = fit * std::ceil(2048.0 / fit);
            const float pos = (float)scTransportSync::positiveMod(a.beat, period);
            for(auto& [srv, sc] : p.sc) {
                if(!sc.engine) continue;
                // sets on a synth not confirmed yet would go out without the timetag
                if(!sc.engine->isCreated()) { pending = true; continue; }
                scSynthControlBatch controls(sc.engine, srv);
                controls.set("tbpm", a.bpm);
                controls.set("run", a.playing ? 1.0f : 0.0f);
                controls.set("anchorPos", pos);
                controls.set("anchorId", (float)anchorIdCounter);
                controls.send();
            }
        }
    }
    if(pending) follower.requestAnchor();
}

void scSampleBox::updateTempo() {
    // the fitted speeds follow the tempo, the region and the sample
    for(auto& p : pads) {
        const float r = fitRateOf(*p);
        if(std::abs(r - p->sentFitRate) > 1e-6f * std::max(1.0f, r)) sendTempo(*p);
    }
    sendTransportAnchors();
    // Play With Transport: a note held while the transport plays
    const bool playing = getFrameTransportState().current.isPlaying;
    for(auto& up : pads) {
        Pad& p = *up;
        const bool want = p.autoplay.get() && p.synced() && playing && !p.sample->path.empty();
        if(want == p.autoPlaying) continue;
        p.autoPlaying = want;
        if(want) {
            fire(p);
            if(p.gateMode.get()) for(auto& [srv, sc] : p.sc) if(sc.engine) sc.engine->set("gate", 1.0f);
        } else {
            stop(p);
            sendChannelInputs(p);
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Frame
// ════════════════════════════════════════════════════════════════════════════

void scSampleBox::update(ofEventArgs&) {
    timeline.update();
    snaps.update();
    {   // the snapshot names, to a published Snapshot / Morph A / B dropdown
        const auto names = snaps.slotNames();
        for(const auto& key : snaps.slotKeys()) { auto it = published.find(key); if(it != published.end()) scDropdowns::setNames(it->second, names); }
    }
    // Pads waiting for the definitions (createSynth): created once the server
    // has synced (or after 3 s without a reply)
    for(auto& [srv, n] : nodeSC) {
        if(!n.padsPending) continue;
        if(srv->isNRTSyncPending() && ofGetElapsedTimef() - n.pendingSince < 3.0f) continue;
        n.padsPending = false;
        for(auto& p : pads) createPad(srv, *p);
        sendChokeMasks();
        sendMutes();
    }
    for(auto& up : pads) {
        Pad& p = *up;
        p.mod.showing = showWindow.get() && p.index == selected;
        p.mod.update();
        Sample& s = *p.sample;
        if(!s.pendingFile.empty() && !s.task.valid()) {
            s.decodingPath = s.pendingFile;
            s.pendingFile.clear();
            s.task = std::async(std::launch::async, &scSlicerBox::decodeFile, s.decodingPath,
                                ofToDataPath("Supercollider/tmp/SampleBox", true), false);
        }
        if(s.task.valid() && s.task.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            applyDecoded(p, s.task.get());
            s.decodingPath.clear();
        }
        // After a server reboot: new buffers (the graph rebuilds the synths)
        for(auto& [srv, sc] : p.sc) {
            if(!sc.engine || s.playPath.empty()) continue;
            auto it = s.buffers.find(srv);
            const bool live = it != s.buffers.end() && !it->second.empty() &&
                std::all_of(it->second.begin(), it->second.end(), [srv = srv](ofxSCBuffer* b){ return liveBuffer(srv, b); });
            if(!live) {
                if(it != s.buffers.end()) { for(auto* b : it->second) delete b; s.buffers.erase(it); }
                ensureBuffers(p, srv);
                sendEngine(srv, p);
            }
        }
    }
    updateTempo();
    if(pendingLoadDialog) {
        pendingLoadDialog = false;
        Pad& p = *pads[std::clamp(selected, 0, (int)pads.size() - 1)];
        auto r = ofSystemLoadDialog("Load a sample into " + p.name, false,
            p.sample->path.empty() ? browser.getDir() : std::filesystem::path(p.sample->path).parent_path().string());
        if(r.bSuccess) loadSample(p, r.getPath());
    }
    // Playheads (the selected pad) and meters, while the window is shown
    for(auto& p : pads) { p->playheads.clear(); }
    if(showWindow.get()) {
        const float now = ofGetElapsedTimef();
        const bool read = now >= nextMeterRead;
        if(read) nextMeterRead = now + 1.0f / 30.0f;
        for(auto& up : pads) {
            Pad& p = *up;
            if(p.sc.empty()) continue;
            auto& [srv, sc] = *p.sc.begin();   // the first server is enough for the window
            if(p.index == selected && liveControlBus(srv, sc.vis)) {
                p.playheads = sc.vis->readValues;
                if(read) srv->requestControlValues(sc.vis->index, std::min(p.voices.get(), MaxVoices));
            }
            if(liveControlBus(srv, sc.meter)) {
                if(!sc.meter->readValues.empty()) p.meter = sc.meter->readValues[0];
                if(read) srv->requestControlValues(sc.meter->index, 1);
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Presets
// ════════════════════════════════════════════════════════════════════════════

void scSampleBox::presetSave(ofJson& j) {
    for(auto& p : pads) p->mod.saveLfoOutputs(j);
    ofJson arr = ofJson::array();
    for(int k = 0; k < (int)pads.size(); k++) arr.push_back(padState(k));
    j["embedSamples"] = embedSamples.get();
    if(embedSamples.get()) for(auto& pad : arr)
        pad["path"] = scSampleEmbedding::embed(pad.value("path", std::string()));
    j["sampleBoxPads"] = arr;
    j["sampleBoxSelected"] = selected;
    j["sampleBoxBrowseDir"] = browser.getDir();
    j["sampleBoxPublished"] = publishedKeys;
    timeline.save(j, "sampleBoxTimeline");
    // the snapshot settings (not pad controls: saved here unless published)
    j["sampleBoxSnapshots"] = snaps.settingsJson();
    j["sampleBoxBrowserWidth"] = browserWidth;
    j["sampleBoxWaveformHeight"] = waveformHeight / std::max(0.01f, ofxOceanodeShared::getZoomLevel());
}

// The pads (and so their parameters) must exist before Oceanode sets the
// published ones and restores their connections
void scSampleBox::loadBeforeConnections(ofJson& j) {
    if(j.contains("sampleBoxPads") && j["sampleBoxPads"].is_array()) {
        const int n = std::clamp((int)j["sampleBoxPads"].size(), 1, MaxPads);
        while((int)pads.size() < n) if(!addPad()) break;
        while((int)pads.size() > n) removePad((int)pads.size() - 1);
    }
    for(auto& p : pads) p->mod.loadLfoOutputs(j);
    if(!j.contains("sampleBoxPublished") || !j["sampleBoxPublished"].is_array()) return;
    std::vector<std::string> wanted;
    for(const auto& key : j["sampleBoxPublished"])
        if(key.is_string()) wanted.push_back(key.get<std::string>());
    const auto old = publishedKeys;
    for(const auto& key : old)
        if(std::find(wanted.begin(), wanted.end(), key) == wanted.end()) unpublish(key);
    for(const auto& key : wanted) publish(key);
    timeline.load(j, "sampleBoxTimeline");
}
void scSampleBox::presetRecallBeforeSettingParameters(ofJson& j) { loadBeforeConnections(j); }

void scSampleBox::presetRecallAfterSettingParameters(ofJson& j) {
    embedSamples = j.value("embedSamples", true);
    if(j.contains("sampleBoxPads") && j["sampleBoxPads"].is_array()) {
        const auto& arr = j["sampleBoxPads"];
        for(int k = 0; k < (int)pads.size() && k < (int)arr.size(); k++) {
            applyPadState(k, arr[k]);
            const std::string path = scSampleEmbedding::resolve(arr[k].value("path", std::string()));
            if(!path.empty() && path != pads[k]->sample->path) loadSample(*pads[k], path);
        }
    }
    for(auto& p : pads) syncPad(*p);
    sendChokeMasks();
    sendMutes();
    if(j.contains("sampleBoxSelected") && j["sampleBoxSelected"].is_number_integer())
        selected = std::clamp(j["sampleBoxSelected"].get<int>(), 0, (int)pads.size() - 1);
    if(j.contains("sampleBoxBrowserWidth") && j["sampleBoxBrowserWidth"].is_number())
        browserWidth = j["sampleBoxBrowserWidth"].get<float>();
    if(j.contains("sampleBoxWaveformHeight") && j["sampleBoxWaveformHeight"].is_number())
        waveformHeight = std::max(0.0f, j["sampleBoxWaveformHeight"].get<float>()) * ofxOceanodeShared::getZoomLevel();
    if(j.contains("sampleBoxBrowseDir") && j["sampleBoxBrowseDir"].is_string())
        browser.refresh(j["sampleBoxBrowseDir"].get<std::string>());
    if(j.contains("sampleBoxSnapshots")) snaps.loadSettings(j["sampleBoxSnapshots"]);
}

// ════════════════════════════════════════════════════════════════════════════
// Publishing window controls into the node
// ════════════════════════════════════════════════════════════════════════════

void scSampleBox::publish(const std::string& key) {
    if(published.count(key)) return;
    Control* c = findControl(key);
    if(!c || getParameterGroup().contains(key)) return;
    if(!publishedSeparator) { addSeparator("Published", ofColor(200)); publishedSeparator = true; }
    published[key] = c->add();
    dropdowns.apply(key, published[key]);
    publishedKeys.push_back(key);
    parameterGroupChanged.notify(this);
}
void scSampleBox::unpublish(const std::string& key, bool force) {
    auto it = published.find(key);
    if(it == published.end()) return;
    if(isConnected(key) && !force) return;
    removeParameter(it->second->getName());
    published.erase(it);
    publishedKeys.erase(std::remove(publishedKeys.begin(), publishedKeys.end(), key), publishedKeys.end());
    if(published.empty() && publishedSeparator) { removeSeparator("Published"); publishedSeparator = false; }
    parameterGroupChanged.notify(this);
}
bool scSampleBox::isConnected(const std::string& key) const {
    auto it = published.find(key);
    return it != published.end() && it->second &&
        (it->second->hasInConnection() || it->second->hasOutConnections());
}
void scSampleBox::drawPublishMenu(const std::string& key) {
    if(!ImGui::BeginPopupContextItem(("##sampleBoxPublish" + key).c_str())) return;
    ImGui::TextDisabled("%s", key.c_str());
    ImGui::Separator();
    // the value, typed (vectors: a, b, c)
    if(Control* c = findControl(key); c && c->save && c->load)
        scTyped::field(key, c->save, [&](const std::string& t){ c->load(ofTrim(t)); }, false);
    if(getParameterGroup().contains(key) && !published.count(key)) ImGui::TextDisabled("Always on the node");
    else if(timeline.isHidden(key)) { if(ImGui::MenuItem("Publish to Node GUI")) timeline.reveal(key); }
    else if(published.count(key)) {
        if(isConnected(key)) ImGui::TextDisabled("Disconnect it to unpublish");
        else if(ImGui::MenuItem("Unpublish from Node")) timeline.unpublish(key);
    } else if(ImGui::MenuItem("Publish to Node GUI")) publish(key);
    timeline.menuItems(key);
    // Modulate with: the modulation of the pad the control belongs to (the
    // selected pad's for the node's own)
    if(Control* c = findControl(key); c && c->pad >= 0 && c->pad < (int)pads.size()) pads[c->pad]->mod.modulateMenu(key);
    else if(!pads.empty()) pads[std::clamp(selected, 0, (int)pads.size() - 1)]->mod.modulateMenu(key);
    ImGui::EndPopup();
}
void scSampleBox::drawPublishedUnderline(const std::string& key) const {
    timeline.mark(key);
    if(!published.count(key) || timeline.isHidden(key)) return;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y - 1.0f), ImVec2(b.x, b.y - 1.0f),
                                        ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
}

// ════════════════════════════════════════════════════════════════════════════
// Window widgets (GrainBox's: label column | slider, filled section headers)
// ════════════════════════════════════════════════════════════════════════════

void scSampleBox::slider(ofParameter<float>& p, const char* label, float w, const char* fmt, bool logarithmic) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    drawPublishedUnderline(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(40.0f * z, w - labelW * z));
    float v = p.get();
    if(ImGui::SliderFloat("##v", &v, p.getMin(), p.getMax(), fmt,
                          logarithmic ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None)) p.set(v);
    drawPublishMenu(key);
    ImGui::PopID();
}

void scSampleBox::sliderInt(ofParameter<int>& p, const char* label, float w, const char* fmt) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    drawPublishedUnderline(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(40.0f * z, w - labelW * z));
    int v = p.get();
    if(ImGui::SliderInt("##v", &v, p.getMin(), p.getMax(), fmt)) p.set(v);
    drawPublishMenu(key);
    ImGui::PopID();
}

void scSampleBox::combo(ofParameter<int>& p, const char* label, float w, const std::vector<std::string>& items) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    drawPublishedUnderline(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(40.0f * z, w - labelW * z));
    {   // the names as drawn (Channel: with its sources) to a published dropdown
        const auto it = published.find(key);
        dropdowns.set(key, items, it == published.end() ? nullptr : it->second);
    }
    if(items.empty()) { ImGui::PopID(); return; }
    const int v = std::max(0, std::min((int)items.size() - 1, p.get()));
    if(ImGui::BeginCombo("##v", items[v].c_str())) {
        for(int i = 0; i < (int)items.size(); i++) if(ImGui::Selectable(items[i].c_str(), i == v)) p.set(i);
        ImGui::EndCombo();
    }
    drawPublishMenu(key);
    ImGui::PopID();
}

void scSampleBox::toggle(ofParameter<bool>& p, const char* label) {
    bool v = p.get();
    ImGui::PushID(p.getEscapedName().c_str());
    if(ImGui::Checkbox(label, &v)) p.set(v);
    drawPublishedUnderline(p.getEscapedName());
    drawPublishMenu(p.getEscapedName());
    ImGui::PopID();
}

void scSampleBox::sectionHeader(const char* title, float w, ofParameter<bool>* on) {
    const float z = ofxOceanodeShared::getZoomLevel();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    const float padV = 3.0f * z, padH = 6.0f * z;
    const float bh = ImGui::GetFrameHeight() + 2.0f * padV;
    dl->AddRectFilled(cur, ImVec2(cur.x + w, cur.y + bh), pal::header, 3.0f * z);
    dl->AddLine(ImVec2(cur.x, cur.y + bh), ImVec2(cur.x + w, cur.y + bh), pal::accentCol(120), 1.0f * z);
    ImGui::SetCursorScreenPos(ImVec2(cur.x + padH, cur.y + padV));
    if(on) {
        bool v = on->get();
        ImGui::PushID(title);
        if(ImGui::Checkbox("##on", &v)) on->set(v);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s on / off (off: no CPU)", title);
        drawPublishMenu(on->getEscapedName());
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

void scSampleBox::beginColumn(const char* id) {
    ImGui::PushID(id);
    ImGui::BeginGroup();
}
void scSampleBox::endColumn(bool last) {
    ImGui::EndGroup();
    ImGui::PopID();
    if(!last) ImGui::SameLine(0, 12.0f * ofxOceanodeShared::getZoomLevel());
}

// ════════════════════════════════════════════════════════════════════════════
// Window
// ════════════════════════════════════════════════════════════════════════════

void scSampleBox::draw(ofEventArgs&) {
    if(showWindow.get()) drawWindow();
    else { browser.stopPreview(); windowHovered = false; }
    // structural changes once the frame is drawn
    if(pendingMoveFrom >= 0) { movePad(pendingMoveFrom, pendingMoveTo); pendingMoveFrom = pendingMoveTo = -1; }
    if(pendingDuplicate >= 0) { duplicatePad(pendingDuplicate); pendingDuplicate = -1; }
    if(pendingRemove >= 0) { removePad(pendingRemove); pendingRemove = -1; }
    if(!pendingDropFile.empty() && !pads.empty()) {
        const int k = hoveredPad >= 0 && hoveredPad < (int)pads.size() ? hoveredPad : selected;
        loadSample(*pads[std::clamp(k, 0, (int)pads.size() - 1)], pendingDropFile);
        pendingDropFile.clear();
    }
    keyboardPiano.finishFrame();
}

void scSampleBox::drawWindow() {
    const float z = ofxOceanodeShared::getZoomLevel();
    ImGui::SetNextWindowSize(ImVec2(1120 * z, 760 * z), ImGuiCond_FirstUseEver);
    bool open = showWindow.get();
    const std::string title = getWindowTitle();
    if(!ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        windowHovered = false;
        browser.stopPreview();
        ImGui::End();
        if(!open) showWindow = false;
        return;
    }
    if(!open) { showWindow = false; windowHovered = false; browser.stopPreview(); ImGui::End(); return; }
    if(pads.empty()) { browser.stopPreview(); ImGui::End(); return; }
    keyboardPiano.beginContent(pitch, trigger, 1, padChannel(*pads[std::clamp(selected, 0, (int)pads.size() - 1)]));
    windowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                           ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    hoveredPad = -1;
    const float totalW = ImGui::GetContentRegionAvail().x;
    const float totalH = ImGui::GetContentRegionAvail().y;
    if(totalW < 200.0f * z || totalH < 80.0f * z || pads.empty()) { browser.stopPreview(); scKeyboardPiano::endContent(); ImGui::End(); return; }
    selected = std::clamp(selected, 0, (int)pads.size() - 1);
    Pad& p = *pads[selected];

    // ── Left: the pads, the browser below; the splitter ─────────────────────
    const float dragW = 5.0f * z;
    const float bw = std::min(browserWidth * z, totalW * 0.45f);
    ImGui::BeginChild("##sbLeft", ImVec2(bw, totalH), false);
    const float rowH = ImGui::GetFrameHeight() + 6.0f * z;
    const float listH = std::clamp(pads.size() * rowH + ImGui::GetFrameHeightWithSpacing() * 2.0f + 12.0f * z,
                                   120.0f * z, totalH * 0.6f);
    drawPadList(bw, listH);
    const float browserH = std::max(60.0f * z, ImGui::GetContentRegionAvail().y);
    ImGui::BeginChild("##sbBrowser", ImVec2(bw, browserH), false);
    browser.draw(bw, browserH);
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::SameLine(0, 0);
    scSampleBrowser::drawSplitter("##sbSplit", browserWidth, dragW, totalH,
        140.0f, std::max(140.0f, totalW * 0.45f / z), 1.0f / z);
    ImGui::SameLine(0, 0);

    // ── Right: the selected pad (header, waveform, transport), then the tabs ─
    ImGui::BeginChild("##sbRight", ImVec2(0, totalH), false);
    const float rightW = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize - 4.0f * z;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f * z);
    ImGui::BeginGroup();
    const float w = rightW - 6.0f * z;
    // snapshots, named presets, morph (scSnapshots), then the pad
    snaps.drawBar(w);
    drawHeader(p, w);
    drawWaveform(p, w);
    drawTransport(p, w);
    ImGui::Spacing();
    if(ImGui::BeginTabBar("##sbTabs")) {
        if(ImGui::BeginTabItem("Sound")) { ImGui::Spacing(); drawSoundTab(p, w); ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("Envelopes")) { ImGui::Spacing(); p.mod.drawEnvelopes(w); ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("LFOs")) { ImGui::Spacing(); p.mod.drawLfos(w); ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("Routing")) { ImGui::Spacing(); p.mod.drawRouting(w); ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("Mixer")) { ImGui::Spacing(); drawMixerTab(w); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndGroup();
    ImGui::EndChild();
    scKeyboardPiano::endContent(); ImGui::End();
}

// ── The pad list ────────────────────────────────────────────────────────────

namespace {
struct PadDrag { const void* node; int pad; };
}

void scSampleBox::drawPadList(float w, float h) {
    const float z = ofxOceanodeShared::getZoomLevel();
    ImGui::BeginChild("##sbPads", ImVec2(w, h), true);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rowH = ImGui::GetFrameHeight() + 6.0f * z;
    const float innerW = ImGui::GetContentRegionAvail().x;
    for(int k = 0; k < (int)pads.size(); k++) {
        Pad& p = *pads[k];
        ImGui::PushID(k);
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const bool isSel = k == selected;
        if(ImGui::Selectable("##row", isSel, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick,
                             ImVec2(innerW, rowH))) {
            selected = k;
            if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                renaming = k;
                std::snprintf(renameBuf, sizeof(renameBuf), "%s", p.name.c_str());
            }
        }
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) hoveredPad = k;
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::GetDragDropPayload())
            ImGui::SetTooltip("Click: edit   Double-click: rename   Drag: move it   Right-click: more\n"
                              "Drop a sample here (browser or Finder) to load it into this pad");
        if(ImGui::BeginDragDropSource()) {
            const PadDrag d{this, k};
            ImGui::SetDragDropPayload("SampleBoxPad", &d, sizeof(d));
            ImGui::Text("Move %s", padLabel(p).c_str());
            ImGui::EndDragDropSource();
        }
        if(ImGui::BeginDragDropTarget()) {
            if(const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("SampleBoxPad")) {
                const PadDrag d = *(const PadDrag*)pl->Data;
                if(d.node == this && d.pad != k) { pendingMoveFrom = d.pad; pendingMoveTo = k; }
            }
            ImGui::EndDragDropTarget();
        }
        std::string dropped;
        if(browser.acceptDropOnLastItem(dropped)) { loadSample(p, dropped); selected = k; }
        if(ImGui::BeginPopupContextItem("##padMenu")) {
            ImGui::TextDisabled("%s", padLabel(p).c_str());
            ImGui::Separator();
            if(ImGui::MenuItem("Rename")) { renaming = k; std::snprintf(renameBuf, sizeof(renameBuf), "%s", p.name.c_str()); }
            const std::string source = channelSource(padChannel(p));
            if(!source.empty() && ImGui::MenuItem(("Name it \"" + source + "\"").c_str())) p.name = source;
            if(ImGui::MenuItem("Duplicate", nullptr, false, (int)pads.size() < MaxPads)) pendingDuplicate = k;
            if(ImGui::MenuItem("Delete", nullptr, false, pads.size() > 1)) pendingRemove = k;
            ImGui::EndPopup();
        }

        // the row: colour, number and name, what plays it; Play, M, S on the right
        const float cy = a.y + rowH * 0.5f;
        dl->AddRectFilled(ImVec2(a.x + 2 * z, a.y + 4 * z), ImVec2(a.x + 6 * z, a.y + rowH - 4 * z),
                          pal::padCol(k, p.mute.get() ? 80 : 255));
        const float btn = ImGui::GetFrameHeight();
        const float buttonsW = 3 * btn + 6 * z;
        const float textX = a.x + 12 * z, textR = a.x + innerW - buttonsW - 6 * z;
        const int ch = padChannel(p);
        const std::string source = channelSource(ch);
        std::string chText = "ch " + ofToString(ch + 1) + (source.empty() ? "" : " " + source);
        if(renaming == k) {
            ImGui::SetCursorScreenPos(ImVec2(textX, cy - ImGui::GetFrameHeight() * 0.5f));
            ImGui::SetNextItemWidth(std::max(40.0f * z, textR - textX));
            if(ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
            const bool enter = ImGui::InputText("##rename", renameBuf, sizeof(renameBuf),
                                                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if(enter || ImGui::IsItemDeactivated()) { if(renameBuf[0]) p.name = renameBuf; renaming = -1; }
        } else {
            dl->PushClipRect(ImVec2(textX, a.y), ImVec2(textR, a.y + rowH), true);
            const std::string label = padLabel(p);
            const float ty = cy - ImGui::GetTextLineHeight() * 0.5f;
            dl->AddText(ImVec2(textX, ty), p.mute.get() ? pal::textDim : pal::headerText, label.c_str());
            const float lw = ImGui::CalcTextSize(label.c_str()).x;
            const float cw = ImGui::CalcTextSize(chText.c_str()).x;
            dl->AddText(ImVec2(std::max(textX + lw + 8 * z, textR - cw), ty), pal::textDim, chText.c_str());
            dl->PopClipRect();
        }
        ImGui::SetCursorScreenPos(ImVec2(a.x + innerW - buttonsW, cy - btn * 0.5f));
        ImGui::Button("##play", ImVec2(btn, btn));
        {
            const ImVec2 b0 = ImGui::GetItemRectMin(), b1 = ImGui::GetItemRectMax();
            const float s = (b1.y - b0.y) * 0.28f;
            const ImVec2 c((b0.x + b1.x) * 0.5f, (b0.y + b1.y) * 0.5f);
            dl->AddTriangleFilled(ImVec2(c.x - s * 0.7f, c.y - s), ImVec2(c.x - s * 0.7f, c.y + s), ImVec2(c.x + s, c.y),
                                  pal::headerText);
        }
        if(ImGui::IsItemActivated()) {
            fire(p);
            if(p.gateMode.get()) { p.auditionHeld = true; for(auto& [srv, sc] : p.sc) if(sc.engine) sc.engine->set("gate", 1.0f); }
        }
        if(ImGui::IsItemDeactivated() && p.auditionHeld) { p.auditionHeld = false; sendChannelInputs(p); }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Play this pad");
        ImGui::SameLine(0, 3 * z);
        auto flag = [&](ofParameter<bool>& f, const char* text, ImU32 on) {
            if(f.get()) ImGui::PushStyleColor(ImGuiCol_Button, on);
            if(ImGui::Button(text, ImVec2(btn, btn))) f = !f.get();
            if(f.get()) ImGui::PopStyleColor();
            drawPublishMenu(f.getEscapedName());
        };
        flag(p.mute, "M", IM_COL32(190, 90, 70, 255));
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Mute");
        ImGui::SameLine(0, 3 * z);
        flag(p.solo, "S", IM_COL32(200, 170, 60, 255));
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Solo");
        ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + rowH));
        ImGui::Dummy(ImVec2(innerW, 0));
        ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::BeginDisabled((int)pads.size() >= MaxPads);
    if(ImGui::Button("+ Add pad")) { if(Pad* np = addPad()) selected = np->index; }
    ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("A new pad: its own sample, sound and mixer channel; it plays the next channel");
    // the input knows what plays each channel: name the pads after it
    bool anySource = false;
    for(auto& q : pads) if(!channelSource(padChannel(*q)).empty()) anySource = true;
    if(anySource) {
        ImGui::SameLine();
        if(ImGui::Button("Name from input"))
            for(auto& q : pads) { const std::string s = channelSource(padChannel(*q)); if(!s.empty()) q->name = s; }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Each pad takes the name of what plays its channel (a MetroBox lane...)");
    }
    ImGui::EndChild();
}

// Name, file and format, Load...; the node's Gain on the right
void scSampleBox::drawHeader(Pad& p, float w) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const float x0 = ImGui::GetCursorPosX();
    ImGui::PushStyleColor(ImGuiCol_Text, pal::padCol(p.index, 255));
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%d", p.index + 1);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    char nameBuf[64];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", p.name.c_str());
    ImGui::SetNextItemWidth(130.0f * z);
    if(ImGui::InputText("##padName", nameBuf, sizeof(nameBuf))) p.name = nameBuf;
    ImGui::SameLine();
    if(ImGui::Button("Load...")) pendingLoadDialog = true;
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Open an audio file (WAV, AIFF, FLAC, MP3, M4A, CAF...) for this pad.\n"
                                                 "Or drop one from the browser or the Finder on the waveform or a pad.");
    ImGui::SameLine(0, 10.0f * z);
    ImGui::AlignTextToFramePadding();
    const Sample& s = *p.sample;
    if(!s.decodingPath.empty() || !s.pendingFile.empty()) {
        const std::string& f = s.decodingPath.empty() ? s.pendingFile : s.decodingPath;
        ImGui::TextColored(ImColor(pal::textDim), "Loading %s...", std::filesystem::path(f).filename().string().c_str());
    } else if(s.path.empty()) {
        ImGui::TextColored(ImColor(pal::textDim), "No sample loaded");
    } else {
        ImGui::TextUnformatted(std::filesystem::path(s.path).filename().string().c_str());
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.path.c_str());
        ImGui::SameLine(0, 10.0f * z);
        ImGui::TextColored(ImColor(pal::textDim), "%s  |  %s  |  %.1f kHz", formatTime(s.seconds).c_str(),
                           s.channels == 1 ? "mono" : "stereo", s.rate / 1000.0f);
    }
    const float gainW = 210.0f * z;
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + w - gainW));
    const float keep = labelW;
    labelW = 38.0f;
    slider(gain, "Gain", gainW, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Every pad's level (the Out mix and the Pad outputs)");
    labelW = keep;
    if(!s.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", s.error.c_str());
}

void scSampleBox::drawWaveform(Pad& p, float w) {
    const float z = ofxOceanodeShared::getZoomLevel();
    if(waveformHeight <= 0.0f) waveformHeight = 170.0f * z;
    const float h = waveformHeight;
    const Sample& smp = *p.sample;
    const int bins = (int)smp.peaks.size();
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##sbWave", ImVec2(std::max(1.0f, w), std::max(1.0f, h)));
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    const ImVec2 b(a.x + w, a.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(a, b, true);
    dl->AddRectFilled(a, b, pal::canvas);
    const float midY = a.y + h * 0.5f;
    const float in = p.inPoint.get(), out = p.outPoint.get();
    const float xi = a.x + w * in, xo = a.x + w * out;
    const float seconds = smp.seconds;

    if(smp.peaks.empty()) {
        const char* msg = "Drop a sample here (browser or Finder), double-click one in the browser, or Load...";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(a.x + (w - ts.x) * 0.5f, midY - ts.y * 0.5f), pal::textDim, msg);
    } else {
        // time grid, a line at every step that leaves room for its label
        static const float steps[] = {0.01f, 0.02f, 0.05f, 0.1f, 0.2f, 0.5f, 1, 2, 5, 10, 15, 30, 60, 120, 300};
        float step = steps[0];
        for(float s : steps) { step = s; if(seconds > 0 && w * s / seconds >= 70.0f * z) break; }
        for(float t = step; seconds > 0 && t < seconds; t += step) {
            const float x = a.x + w * t / seconds;
            dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), pal::grid);
            char label[24];
            if(step < 1.0f) std::snprintf(label, sizeof(label), "%.2f", t);
            else if(t < 60.0f) std::snprintf(label, sizeof(label), "%.0f s", t);
            else std::snprintf(label, sizeof(label), "%d:%02d", (int)t / 60, (int)t % 60);
            dl->AddText(ImVec2(x + 3.0f * z, b.y - ImGui::GetTextLineHeight() - 2.0f * z), pal::textDim, label);
        }
        dl->AddLine(ImVec2(a.x, midY), ImVec2(b.x, midY), pal::gridStrong);
        // the peaks, bright inside the region
        const int pixels = std::max(1, (int)w);
        for(int i = 0; i < pixels; ++i) {
            const int b0 = (int)((int64_t)i * bins / pixels);
            const int b1 = std::max(b0 + 1, (int)((int64_t)(i + 1) * bins / pixels));
            float peak = 0;
            for(int k = b0; k < b1 && k < bins; ++k) peak = std::max(peak, smp.peaks[k]);
            if(peak <= 0.0f) continue;
            const float x = a.x + i + 0.5f, dy = std::max(0.5f, peak * h * 0.46f);
            const bool inside = x >= xi && x <= xo;
            dl->AddLine(ImVec2(x, midY - dy), ImVec2(x, midY + dy), pal::padCol(p.index, inside ? 190 : 50));
        }
        dl->AddRectFilled(a, ImVec2(xi, b.y), pal::outside);
        dl->AddRectFilled(ImVec2(xo, a.y), b, pal::outside);
        // Fit: the beats over the region (bars stronger)
        if(p.fitBeats.get() >= 1.0f && p.fitBeats.get() <= 128.0f) {
            const int beats = (int)std::lround(p.fitBeats.get());
            const int bpb = std::max(1, (int)std::lround(beatsPerBar()));
            for(int k = 1; k < beats; k++) {
                const float x = xi + (xo - xi) * k / p.fitBeats.get();
                dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), k % bpb == 0 ? pal::padCol(p.index, 150) : pal::padCol(p.index, 60), 1.0f * z);
            }
        }
        // the loop's crossfade (the seam's end of the region)
        if(p.loop.get() && p.loopCrossfade.get() > 0.0f) {
            const bool rev = p.reverse.get();
            const float xf = (xo - xi) * p.loopCrossfade.get() * 0.01f;
            const float x0 = rev ? xi : xo - xf, x1 = rev ? xi + xf : xo;
            const ImU32 c0 = rev ? pal::accentCol(60) : pal::accentCol(0), c1 = rev ? pal::accentCol(0) : pal::accentCol(60);
            dl->AddRectFilledMultiColor(ImVec2(x0, a.y), ImVec2(x1, b.y), c0, c1, c1, c0);
        }
        // In / Out: lines, flags and times
        const float flag = 7.0f * z;
        dl->AddLine(ImVec2(xi, a.y), ImVec2(xi, b.y), pal::handle, 2.0f * z);
        dl->AddLine(ImVec2(xo, a.y), ImVec2(xo, b.y), pal::handle, 2.0f * z);
        dl->AddTriangleFilled(ImVec2(xi, a.y), ImVec2(xi + flag, a.y), ImVec2(xi, a.y + flag), pal::handle);
        dl->AddTriangleFilled(ImVec2(xo, a.y), ImVec2(xo - flag, a.y), ImVec2(xo, a.y + flag), pal::handle);
        const std::string ti = "In " + formatTime(in * seconds);
        const std::string to = "Out " + formatTime(out * seconds);
        const float tow = ImGui::CalcTextSize(to.c_str()).x;
        dl->AddText(ImVec2(xi + flag + 3.0f * z, a.y + 2.0f * z), pal::headerText, ti.c_str());
        dl->AddText(ImVec2(std::max(xi + flag + 3.0f * z, xo - flag - 3.0f * z - tow),
                           a.y + 2.0f * z + (xo - xi < tow + 120.0f * z ? ImGui::GetTextLineHeight() : 0.0f)),
                    pal::headerText, to.c_str());
        // a playhead per sounding voice
        for(int v = 0; v < (int)p.playheads.size() && v < std::min(p.voices.get(), MaxVoices); ++v) {
            if(p.playheads[v] < 0.0f) continue;
            const float x = a.x + w * ofClamp(p.playheads[v], 0.0f, 1.0f);
            dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), pal::voiceCol(v, 235), 1.5f * z);
            dl->AddCircleFilled(ImVec2(x, b.y - 4.0f * z), 3.0f * z, pal::voiceCol(v, 235));
        }
    }
    const bool dragging = browser.isDraggingFile() && hovered;
    dl->AddRect(a, b, dragging ? pal::accentCol(255) : pal::border, 0, 0, dragging ? 2.0f * z : 1.0f);
    dl->PopClipRect();
    if(hovered) hoveredPad = p.index;

    // ── Mouse: drag In / Out (near a line) or the whole region (inside it);
    //    a click outside moves the nearest edge there; double-click = all ──
    if(!smp.peaks.empty()) {
        const float mx = ofClamp((ImGui::GetMousePos().x - a.x) / w, 0.0f, 1.0f);
        const float grab = 7.0f * z, mouseX = ImGui::GetMousePos().x;
        const bool nearIn = std::abs(mouseX - xi) <= grab, nearOut = std::abs(mouseX - xo) <= grab;
        if(hovered && !active) {
            if(nearIn || nearOut) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            else if(mx > in && mx < out) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("%s\nDrag In / Out, or the region to move it\nDouble-click: the whole file",
                              formatTime(mx * seconds).c_str());
        }
        if(ImGui::IsItemActivated()) {
            if(nearIn || nearOut) regionDrag = (nearIn && nearOut) ? (mx < in ? 0 : 1) : nearIn ? 0 : 1;
            else if(mx > in && mx < out) { regionDrag = 2; regionDragOffset = mx - in; }
            else regionDrag = std::abs(mx - in) < std::abs(mx - out) ? 0 : 1;
        }
        if(active && regionDrag >= 0) {
            const float eps = 0.0005f;
            if(regionDrag == 0) p.inPoint = std::min(mx, p.outPoint.get() - eps);
            else if(regionDrag == 1) p.outPoint = std::max(mx, p.inPoint.get() + eps);
            else {
                const float len = p.outPoint.get() - p.inPoint.get();
                const float start = ofClamp(mx - regionDragOffset, 0.0f, 1.0f - len);
                p.inPoint = start;
                p.outPoint = start + len;
            }
        }
        if(ImGui::IsItemDeactivated()) regionDrag = -1;
        if(hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { p.inPoint = 0.0f; p.outPoint = 1.0f; }
    }
    std::string dropped;
    if(browser.acceptDropOnLastItem(dropped)) loadSample(p, dropped);

    // ── Resize grip (GrainBox's) ─────────────────────────────────────────────
    const float gripH = 8.0f * z;
    const ImVec2 g = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##sbWaveResize", ImVec2(std::max(1.0f, w), gripH));
    const bool gh = ImGui::IsItemHovered(), ga = ImGui::IsItemActive();
    const ImU32 col = gh || ga ? pal::splitterHot : pal::splitter;
    const float cy = g.y + gripH * 0.5f;
    dl->AddLine(ImVec2(g.x, cy), ImVec2(g.x + w, cy), col, (gh || ga ? 2.0f : 1.0f) * z);
    for(int k = -1; k <= 1; ++k) dl->AddCircleFilled(ImVec2(g.x + w * 0.5f + k * 7.0f * z, cy), 2.0f * z, col);
    if(gh || ga) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if(gh && !ga) ImGui::SetTooltip("Drag to resize the waveform\n(double-click: default height)");
    if(ga && ImGui::IsMouseDragging(0, 0.0f))
        waveformHeight = std::max(60.0f * z, std::min(900.0f * z, waveformHeight + ImGui::GetIO().MouseDelta.y));
    if(gh && ImGui::IsMouseDoubleClicked(0)) waveformHeight = 0.0f;
}

// Play / Stop, how notes play, the engine; the voices' lights on the right
void scSampleBox::drawTransport(Pad& p, float w) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const float x0 = ImGui::GetCursorPosX();
    ImGui::Button("Play", ImVec2(64.0f * z, 0));
    if(ImGui::IsItemActivated()) {
        fire(p);
        if(p.gateMode.get()) { p.auditionHeld = true; for(auto& [srv, sc] : p.sc) if(sc.engine) sc.engine->set("gate", 1.0f); }
    }
    if(ImGui::IsItemDeactivated() && p.auditionHeld) { p.auditionHeld = false; sendChannelInputs(p); }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip(p.gateMode.get() ? "Hold to play (Gate Mode)" : "Play a note");
    ImGui::SameLine();
    if(ImGui::Button("Stop", ImVec2(64.0f * z, 0))) stop(p);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Release this pad's notes (and end loops)");

    ImGui::SameLine(0, 18.0f * z);
    toggle(p.loop, "Loop");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Repeat the region until Stop (or the gate falls in Gate Mode)");
    ImGui::SameLine(0, 10.0f * z);
    toggle(p.reverse, "Reverse");
    ImGui::SameLine(0, 10.0f * z);
    toggle(p.gateMode, "Gate Mode");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Notes last while the gate is high\n(off: a trigger plays the whole region)");
    ImGui::SameLine(0, 10.0f * z);
    toggle(p.poly, "Polyphonic");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("On: notes overlap (up to Voices)\nOff: a new note fades the previous one out");
    if(p.poly.get()) {
        ImGui::SameLine(0, 6.0f * z);
        const float keep = labelW;
        labelW = 44.0f;
        sliderInt(p.voices, "Voices", 130.0f * z);
        labelW = keep;
    }
    ImGui::SameLine(0, 18.0f * z);
    {
        const float keep = labelW;
        labelW = 40.0f;
        combo(p.stretchMode, "Mode", 170.0f * z, {"Resample", "Stretch"});
        labelW = keep;
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Resample: speed and pitch together (like tape)\n"
                                                     "Stretch: Warp1 granular, speed and pitch apart");
    }
    // voice lights
    const int n = p.poly.get() ? p.voices.get() : 2;
    const float r = 4.0f * z, gap = 4.0f * z;
    const float lightsW = n * (2 * r + gap);
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + w - lightsW));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float cy = pos.y + ImGui::GetFrameHeight() * 0.5f;
    for(int v = 0; v < n; ++v) {
        const bool on = v < (int)p.playheads.size() && p.playheads[v] >= 0.0f;
        const ImVec2 c(pos.x + r + v * (2 * r + gap), cy);
        if(on) ImGui::GetWindowDrawList()->AddCircleFilled(c, r, pal::voiceCol(v, 255));
        else ImGui::GetWindowDrawList()->AddCircle(c, r, pal::border, 0, 1.5f * z);
    }
    ImGui::Dummy(ImVec2(lightsW, ImGui::GetFrameHeight()));
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Voices sounding");
}

void scSampleBox::drawSoundTab(Pad& p, float w) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const float colW = (w - 3 * 12.0f * z) / 4.0f;
    labelW = 74.0f;
    const float seconds = p.sample->seconds;

    beginColumn("region");
    sectionHeader("Region", colW);
    {
        std::string fi = seconds > 0 ? formatTime(p.inPoint.get() * seconds) : "%.3f";
        std::string fo = seconds > 0 ? formatTime(p.outPoint.get() * seconds) : "%.3f";
        slider(p.inPoint, "In", colW, fi.c_str());
        slider(p.outPoint, "Out", colW, fo.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImColor(pal::textDim), "Length");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelW * z - ImGui::CalcTextSize("Length").x - ImGui::GetStyle().ItemSpacing.x);
        const float len = (p.outPoint.get() - p.inPoint.get()) * seconds;
        ImGui::TextColored(ImColor(pal::textDim), "%s", seconds > 0 ? formatTime(len).c_str() : "-");
        if(!p.loop.get()) ImGui::BeginDisabled();
        slider(p.loopCrossfade, "Loop XFade", colW, "%.0f %%");
        if(!p.loop.get()) ImGui::EndDisabled();
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Loop: fade the end of the region into its start\n(0 %% keeps rhythmic loops tight)");
    }
    // ── Tempo: fit the region to beats, follow the transport ──
    sectionHeader("Tempo", colW);
    {
        const double bpb = beatsPerBar();
        const float region = (p.outPoint.get() - p.inPoint.get()) * seconds;
        std::vector<std::pair<float, std::string>> fits = {{0.0f, "Off"}, {0.5f, "1/2 beat"}, {1.0f, "1 beat"}, {2.0f, "2 beats"}};
        for(int bars : {1, 2, 4, 8, 16})
            fits.push_back({(float)(bars * bpb), ofToString(bars) + (bars == 1 ? " bar" : " bars") + " (" + ofToString(bars * bpb) + ")"});
        const float fit = p.fitBeats.get();
        std::string preview = ofToString(fit, 2) + " beats";
        for(const auto& f : fits) if(std::abs(f.first - fit) < 1e-4f) preview = f.second;
        const std::string key = p.fitBeats.getEscapedName();
        ImGui::PushID(key.c_str());
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Fit");
        drawPublishedUnderline(key);
        ImGui::SameLine();
        ImGui::SetCursorPosX(x0 + labelW * z);
        const float guessW = 52.0f * z;
        ImGui::SetNextItemWidth(std::max(40.0f * z, colW - labelW * z - guessW - 4.0f * z));
        if(ImGui::BeginCombo("##fit", preview.c_str())) {
            for(const auto& f : fits) if(ImGui::Selectable(f.second.c_str(), std::abs(f.first - fit) < 1e-4f)) p.fitBeats = f.first;
            ImGui::EndCombo();
        }
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("The region lasts exactly this many beats at the current tempo, and keeps doing so when the tempo\n"
                              "changes. Resample: the pitch follows the tempo (tape). Stretch: the pitch stays.\n"
                              "Right-click: type any number of beats, publish.");
        drawPublishMenu(key);
        ImGui::SameLine(0, 4.0f * z);
        ImGui::BeginDisabled(region <= 0.0f);
        if(ImGui::Button("Guess", ImVec2(guessW, 0))) p.fitBeats = guessBeats(region, bpb);
        ImGui::EndDisabled();
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("How many beats the region is (a power of two of bars or beats, its own tempo nearest 120 BPM)");
        ImGui::PopID();
        if(fit > 0.0f && region > 0.0f)
            ImGui::TextColored(ImColor(pal::textDim), "Its tempo %.1f BPM  ->  x%.3f", fit * 60.0f / region, fitRateOf(p));
        ImGui::BeginDisabled(fit <= 0.0f);
        toggle(p.sync, "Sync to transport");
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(fit <= 0.0f ? "Needs Fit" :
                              "While the transport plays, the region is read at the transport's position: it stays in\n"
                              "phase with the bars through seeks, loops and tempo changes. Triggers still start and\n"
                              "stop notes (gate a break: it always comes back in phase). Stopped: it plays freely.");
        ImGui::BeginDisabled(!p.sync.get());
        toggle(p.autoplay, "Play with transport");
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Plays (looping) whenever the transport plays: no trigger needed");
        ImGui::EndDisabled();
        ImGui::EndDisabled();
    }
    endColumn();

    beginColumn("speed");
    sectionHeader("Speed & Pitch", colW);
    slider(p.transpose, "Transpose", colW, "%+.2f st");
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip(p.stretchMode.get() == 1 ? "Live, on sounding notes too: the grains' pitch only (Stretch keeps the timing)"
                                                   : "Live, on sounding notes too: speed and pitch together (Resample)");
    slider(p.rate, "Rate", colW, "%.3fx", true);
    const bool stretchOn = p.stretchMode.get() == 1;
    if(!stretchOn) ImGui::BeginDisabled();
    slider(p.stretch, "Stretch", colW, "%.2fx", true);
    slider(p.warpWindow, "Grain", colW, "%.3f s", true);
    if(!stretchOn) ImGui::EndDisabled();
    if(!stretchOn && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Stretch and Grain act in Stretch mode");
    ImGui::TextColored(ImColor(pal::textDim), "Pitch: the node's (60 = as recorded)");
    endColumn();

    beginColumn("amp");
    sectionHeader("Amplitude", colW);
    slider(p.attack, "Attack", colW, "%.3f s", true);
    slider(p.release, "Release", colW, "%.3f s", true);
    sectionHeader("Filter", colW, &p.filterOn);
    if(!p.filterOn.get()) ImGui::BeginDisabled();
    combo(p.filterType, "Type", colW, {"Low pass", "High pass", "Band pass"});
    slider(p.cutoff, "Cutoff", colW, "%.0f Hz", true);
    slider(p.resonance, "Resonance", colW, "%.2f");
    if(!p.filterOn.get()) ImGui::EndDisabled();
    endColumn();

    beginColumn("play");
    sectionHeader("Played by", colW);
    {
        std::vector<std::string> chans = {"Auto: row " + ofToString(p.index + 1)};
        for(int c = 1; c <= MaxChannels; c++) {
            const std::string s = channelSource(c - 1);
            chans.push_back(ofToString(c) + (s.empty() ? "" : "  " + s));
        }
        combo(p.channel, "Channel", colW, chans);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("The channel of Trig/Gate Audio (and Trigger/Gate, Pitch, Velocity) this pad plays.\n"
                              "Auto: its row, so a sequencer's lane n plays pad n.\n"
                              "Two pads on one channel play together (layers).");
        const std::string source = channelSource(padChannel(p));
        ImGui::TextColored(ImColor(pal::textDim), "%s", source.empty() ? "Nothing named on this channel" : ("Played by: " + source).c_str());
        std::vector<std::string> groups = {"None"};
        for(int g = 1; g <= ChokeGroups; g++) groups.push_back("Group " + ofToString(g));
        combo(p.choke, "Choke", colW, groups);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Pads in the same group cut each other (a closed hat stops the open hat).");
    }
    endColumn(true);
}

// ── The mixer ───────────────────────────────────────────────────────────────

void scSampleBox::drawMixerTab(float w) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const float stripW = 104.0f * z, stripH = 300.0f * z;
    ImGui::BeginChild("##sbStrips", ImVec2(w, stripH + ImGui::GetStyle().ScrollbarSize + 8.0f * z), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for(auto& p : pads) { drawStrip(*p, stripW, stripH); ImGui::SameLine(0, 6.0f * z); }
    ImGui::EndChild();
    ImGui::Spacing();
    drawEffects(*pads[std::clamp(selected, 0, (int)pads.size() - 1)], w);
}

void scSampleBox::drawStrip(Pad& p, float w, float h) {
    const float z = ofxOceanodeShared::getZoomLevel();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(p.index);
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const bool isSel = p.index == selected;
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), isSel ? IM_COL32(40, 44, 54, 255) : pal::header, 4.0f * z);
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + 3.0f * z), pal::padCol(p.index, 255), 4.0f * z);
    ImGui::BeginGroup();
    ImGui::SetCursorScreenPos(ImVec2(a.x + 4 * z, a.y + 6 * z));
    const float iw = w - 8 * z;
    if(ImGui::Selectable(padLabel(p).c_str(), isSel, 0, ImVec2(iw, 0))) selected = p.index;
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s: its effects below", padLabel(p).c_str());
    // inserts: click switches one on / off (an effect that is off costs nothing)
    for(int fx = 0; fx < FX_COUNT; fx++) {
        ImGui::SetCursorScreenPos(ImVec2(a.x + 4 * z, ImGui::GetCursorScreenPos().y));
        const bool on = p.fxOn[fx].get();
        const bool usable = fx != FX_IMAGE || channels.get() > 1;
        if(!usable) ImGui::BeginDisabled();
        if(on) ImGui::PushStyleColor(ImGuiCol_Button, pal::padCol(p.index, 150));
        if(ImGui::Button(fxNames[fx], ImVec2(iw, 0))) { p.fxOn[fx] = !on; selected = p.index; }
        if(on) ImGui::PopStyleColor();
        if(!usable) ImGui::EndDisabled();
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(usable ? "%s: click to switch it %s (its settings below)" : "%s needs N Chan > 1", fxNames[fx], on ? "off" : "on");
        drawPublishMenu(p.fxOn[fx].getEscapedName());
    }
    // pan
    ImGui::SetCursorScreenPos(ImVec2(a.x + 4 * z, ImGui::GetCursorScreenPos().y + 4 * z));
    ImGui::SetNextItemWidth(iw);
    float pan = p.pan.get();
    if(ImGui::SliderFloat("##pan", &pan, -1.0f, 1.0f, pan == 0.0f ? "C" : pan < 0 ? "L %.2f" : "R %.2f")) p.pan = pan;
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Pan (double-click to type; right-click: publish / modulate)");
    drawPublishMenu(p.pan.getEscapedName());
    // fader and meter
    const float btnH = ImGui::GetFrameHeight();
    const float faderTop = ImGui::GetCursorScreenPos().y + 4 * z;
    const float faderH = std::max(40.0f * z, a.y + h - faderTop - 2 * btnH - 14 * z);
    ImGui::SetCursorScreenPos(ImVec2(a.x + 4 * z, faderTop));
    float lv = p.level.get();
    const float db = lv > 0.0001f ? 20.0f * std::log10(lv) : -INFINITY;
    char fmt[16];
    if(std::isinf(db)) std::snprintf(fmt, sizeof(fmt), "-inf");
    else std::snprintf(fmt, sizeof(fmt), "%+.1f", db);
    if(ImGui::VSliderFloat("##level", ImVec2(iw * 0.55f, faderH), &lv, 0.0f, 2.0f, fmt)) p.level = lv;
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Level %.2f (%s dB)", p.level.get(), fmt);
    drawPublishMenu(p.level.getEscapedName());
    {
        const float mx = a.x + 4 * z + iw * 0.62f, mw = iw * 0.3f;
        dl->AddRectFilled(ImVec2(mx, faderTop), ImVec2(mx + mw, faderTop + faderH), pal::canvas, 2.0f * z);
        const float mdb = p.meter > 0.00001f ? 20.0f * std::log10(p.meter) : -80.0f;
        const float k = ofClamp((mdb + 60.0f) / 66.0f, 0.0f, 1.0f);   // -60 .. +6 dB
        const ImU32 mc = mdb > 0.0f ? IM_COL32(230, 80, 60, 255) : mdb > -6.0f ? IM_COL32(230, 200, 80, 255) : pal::padCol(p.index, 230);
        if(k > 0.0f) dl->AddRectFilled(ImVec2(mx, faderTop + faderH * (1 - k)), ImVec2(mx + mw, faderTop + faderH), mc, 2.0f * z);
        const float zero = faderTop + faderH * (1 - 60.0f / 66.0f);
        dl->AddLine(ImVec2(mx - 2 * z, zero), ImVec2(mx + mw + 2 * z, zero), pal::gridStrong);
    }
    // mute, solo
    ImGui::SetCursorScreenPos(ImVec2(a.x + 4 * z, faderTop + faderH + 6 * z));
    auto flag = [&](ofParameter<bool>& f, const char* text, ImU32 col, float bw) {
        if(f.get()) ImGui::PushStyleColor(ImGuiCol_Button, col);
        if(ImGui::Button(text, ImVec2(bw, 0))) f = !f.get();
        if(f.get()) ImGui::PopStyleColor();
        drawPublishMenu(f.getEscapedName());
    };
    const float half = (iw - 4 * z) * 0.5f;
    flag(p.mute, "M", IM_COL32(190, 90, 70, 255), half);
    ImGui::SameLine(0, 4 * z);
    flag(p.solo, "S", IM_COL32(200, 170, 60, 255), half);
    ImGui::SetCursorScreenPos(ImVec2(a.x + 4 * z, ImGui::GetCursorScreenPos().y));
    ImGui::TextColored(ImColor(pal::textDim), "Pad %d out", p.index + 1);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("This pad also goes out alone on the node's \"Pad %d\" output", p.index + 1);
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(a.x + w, a.y));
    ImGui::Dummy(ImVec2(0, h));
    ImGui::PopID();
}

void scSampleBox::drawEffects(Pad& p, float w) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const float gap = 12.0f * z;
    labelW = 74.0f;
    ImGui::PushStyleColor(ImGuiCol_Text, pal::padCol(p.index, 255));
    ImGui::TextUnformatted(("Effects of " + padLabel(p)).c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextColored(ImColor(pal::textDim), "  sound -> EQ -> Comp -> Sat -> Echo -> Reverb -> Stereo -> Level");
    const float colW = (w - 3 * gap) / 4.0f;
    auto off = [&](int fx) { return !p.fxOn[fx].get(); };

    // ── EQ (two columns wide), Compressor, Saturator ──
    const float eqW = colW * 2 + gap;
    beginColumn("eq");
    sectionHeader("EQ", eqW, &p.fxOn[FX_EQ]);
    if(off(FX_EQ)) ImGui::BeginDisabled();
    slider(p.eqMix, "Mix", std::min(eqW, 260.0f * z), "%.2f");
    if(off(FX_EQ)) ImGui::EndDisabled();
    ImGui::TextColored(ImColor(pal::textDim), "Drag the bands; wheel = Q / slope; right-click a band for values");
    drawEqEditor(p, eqW, 180.0f * z);
    endColumn();

    beginColumn("comp");
    sectionHeader("Compressor", colW, &p.fxOn[FX_COMP]);
    if(off(FX_COMP)) ImGui::BeginDisabled();
    slider(p.compThreshold, "Threshold", colW, "%.1f dB");
    slider(p.compRatio, "Ratio", colW, "%.1f : 1", true);
    slider(p.compAttack, "Attack", colW, "%.1f ms", true);
    slider(p.compRelease, "Release", colW, "%.0f ms", true);
    slider(p.compMakeup, "Makeup", colW, "%+.1f dB");
    if(off(FX_COMP)) ImGui::EndDisabled();
    endColumn();

    beginColumn("sat");
    sectionHeader("Saturator", colW, &p.fxOn[FX_SAT]);
    if(off(FX_SAT)) ImGui::BeginDisabled();
    slider(p.satDrive, "Drive", colW, "%.1f dB");
    slider(p.satMix, "Mix", colW, "%.2f");
    if(off(FX_SAT)) ImGui::EndDisabled();
    endColumn(true);

    ImGui::Spacing();
    // ── Echo, Reverb (two columns), Stereo ──
    beginColumn("echo");
    sectionHeader("Echo", colW, &p.fxOn[FX_ECHO]);
    if(off(FX_ECHO)) ImGui::BeginDisabled();
    slider(p.echoTime, "Time", colW, "%.3f beats", true);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("In beats: 0.75 = a dotted 8th, 0.5 = an 8th, 0.333 = an 8th triplet");
    slider(p.echoFeedback, "Feedback", colW, "%.2f");
    slider(p.echoTone, "Tone", colW, "%.0f Hz", true);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Each repeat darker (a lowpass in the loop)");
    slider(p.echoSpread, "Spread", colW, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Repeats move across the channels (ping-pong)");
    slider(p.echoAmount, "Amount", colW, "%.2f");
    if(off(FX_ECHO)) ImGui::EndDisabled();
    endColumn();

    beginColumn("reverb");
    sectionHeader("Reverb", colW, &p.fxOn[FX_REVERB]);
    if(off(FX_REVERB)) ImGui::BeginDisabled();
    slider(p.revSize, "Size", colW, "%.1f");
    slider(p.revDecay, "Decay", colW, "%.2f s", true);
    slider(p.revPredelay, "Predelay", colW, "%.0f ms");
    slider(p.revLowpass, "Lowpass", colW, "%.0f Hz", true);
    if(off(FX_REVERB)) ImGui::EndDisabled();
    endColumn();

    beginColumn("reverb2");
    sectionHeader("Reverb Mix", colW);
    if(off(FX_REVERB)) ImGui::BeginDisabled();
    slider(p.revPosition, "Position", colW, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("0 = early reflections, 1 = the late tail");
    slider(p.revSpread, "Spread", colW, "%+.2f");
    slider(p.revMix, "Mix", colW, "%.2f");
    if(off(FX_REVERB)) ImGui::EndDisabled();
    endColumn();

    beginColumn("stereo");
    sectionHeader("Stereo Image", colW, &p.fxOn[FX_IMAGE]);
    const bool multi = channels.get() > 1;
    if(off(FX_IMAGE) || !multi) ImGui::BeginDisabled();
    slider(p.width, "Width", colW, "%.2f");
    slider(p.diffuse, "Diffuse", colW, "%.2f");
    slider(p.bassMono, "Bass Mono", colW, "%.2f");
    slider(p.bassFreq, "Bass Freq", colW, "%.0f Hz", true);
    if(off(FX_IMAGE) || !multi) ImGui::EndDisabled();
    if(!multi) ImGui::TextColored(ImColor(pal::textDim), "Needs N Chan > 1");
    endColumn(true);
}

void scSampleBox::drawEqEditor(Pad& p, float w, float h) {
    for(int b = 0; b < 5; ++b)
        p.eqEditor.bands[b] = {p.eqFreq[b].get(), p.eqGain[b].get(), p.eqShape[b].get()};
    p.eqEditor.dbRange = 24.0f;
    p.eqEditor.gainLimit = 24.0f;
    p.eqEditor.qMin = 0.1f; p.eqEditor.qMax = 20.0f;
    p.eqEditor.slopeMin = 0.1f; p.eqEditor.slopeMax = 4.0f;
    float sr = 48000.0f;
    for(auto* sm : servers) if(sm && sm->getServer()) { sr = (float)serverManager::getSampleRateForServer(sm->getServer()); break; }
    p.eqEditor.recompute(sr > 0 ? sr : 48000.0f);
    static scEQEditor::Colors col = [] {
        auto c = scEQEditor::defaultColors();
        c.bg = pal::canvas;
        c.border = pal::border;
        c.gridLine = c.dbGridLine = c.fft = pal::grid;
        c.gridText = c.dbGridText = pal::textDim;
        c.zeroGrid = c.zeroLine = pal::gridStrong;
        c.curve = c.handle = IM_COL32(214, 216, 222, 215);
        c.handleActive = pal::handle;
        for(auto& marker : c.bandMarker) marker = pal::grid;
        return c;
    }();
    Pad* pp = &p;
    p.eqEditor.bandMenu = [this, pp](int b) {
        ImGui::TextUnformatted(scEQEditor::bandName(b));
        ImGui::Separator();
        auto value = [this](ofParameter<float>& q, const char* label) {
            float v = q.get();
            ImGui::SetNextItemWidth(180.0f * ofxOceanodeShared::getZoomLevel());
            if(ImGui::InputFloat(label, &v, 0, 0, "%.3f")) q.set(ofClamp(v, q.getMin(), q.getMax()));
            const auto key = q.getEscapedName();
            const bool isPublished = published.count(key) != 0;
            const std::string action = std::string(isPublished ? "Unpublish " : "Publish ") + label;
            if(ImGui::MenuItem(action.c_str(), nullptr, false, !isConnected(key))) {
                if(isPublished) unpublish(key); else publish(key);
            }
        };
        value(pp->eqFreq[b], "Freq (Hz)");
        value(pp->eqGain[b], "Gain (dB)");
        value(pp->eqShape[b], scEQEditor::isShelf(b) ? "Slope" : "Q");
    };
    const bool off = !p.fxOn[FX_EQ].get();
    if(off) ImGui::BeginDisabled();
    if(p.eqEditor.draw(std::max(120.0f, w), h, col, !off, nullptr, 0, ("##sampleBoxEqCurve" + ofToString(p.index)).c_str())) {
        for(int b = 0; b < 5; ++b) {
            p.eqFreq[b] = p.eqEditor.bands[b].freqHz;
            p.eqGain[b] = p.eqEditor.bands[b].gainDb;
            p.eqShape[b] = p.eqEditor.bands[b].shape;
        }
    }
    if(off) ImGui::EndDisabled();
}
