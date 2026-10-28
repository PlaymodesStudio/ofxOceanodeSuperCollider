//
//  scDrumKitBox.cpp
//  ofxOceanodeSuperCollider
//
//  The node: pads and their controls, the server graph (see drumkitbox.scd
//  for each synth), the effects chain, snapshots and presets. The window is
//  in scDrumKitBoxWindow.cpp, the controls' table and the models in
//  scDrumKitBoxData.cpp.
//

#include "scDrumKitBox.h"
#include "../shared/scGlobalPitch.h"
#include "../../../samples/scSampleEmbedding.h"
#include "../../../audio/scSynthControlBatch.h"
#include "../../../audio/scFxRouting.h"
#include "ofxOceanodeShared.h"
#include "ofxSuperCollider.h"
#include "ofxOceanodeContainer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
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
void ensureBus(ofxSCServer* s, ofxSCBus*& b, int rate, int channels) {
    const bool live = rate == RATE_AUDIO ? liveAudioBus(s, b) : liveControlBus(s, b);
    if(live && b->channels == channels) return;
    if(live) b->free();
    delete b;
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
const char* layerDefs[] = {"DKBody", "DKClick", "DKNoise", "DKMetal", "DKSample"};
// the Box family's effects, and drumkitbox.scd's echo with drift and granular delay
const char* stageDefs[] = {"mixerProEQ", "mixerProComp", "mixerProSat", "boxEchoDrift_", "boxGrainDelay_", "boxMicroSpace", "mixerProWidth"};
// which synths a sound section's controls go to: a layer, or the voice
constexpr int VOICE = 1 << scDrumKitBox::L_COUNT;
int soundTargets(const std::string& section) {
    if(section == "body" || section == "bodyx") return 1 << scDrumKitBox::L_BODY;
    if(section == "click") return 1 << scDrumKitBox::L_CLICK;
    if(section == "noise") return 1 << scDrumKitBox::L_NOISE;
    if(section == "metal") return 1 << scDrumKitBox::L_METAL;
    if(section == "sample") return 1 << scDrumKitBox::L_SAMPLE;
    if(section == "piece") return ~0;
    return VOICE;   // filter, character, transient, output
}
}

// ════════════════════════════════════════════════════════════════════════════
// Setup
// ════════════════════════════════════════════════════════════════════════════

scDrumKitBox::scDrumKitBox(std::vector<serverManager*> s)
    : scNode("DrumKitBox"), servers(std::move(s)) {
    color = ofColor(226, 120, 82);
    description = "Drum synth kit: 1 to 16 pads (Kick, Snare, Clap, Hats, Tom, Rim, Cowbell, Cymbal, Perc, 808 Sub, FX, "
                  "Sample), each a stack of layers (Body, Click, Noise, Metal, Sample) with its own envelopes, voice "
                  "(filter + envelope, drive / crush, transient shaper), modulation and mixer channel (EQ, compressor, "
                  "saturator, echo, reverb, width, ducking). Pad n plays channel n of Gate (level = velocity) and Pitch "
                  "(60 = its own tuning): sequence it from the timeline's Drum Grid, a MetroBox / EuclideanBox Gates "
                  "output or any gate cable. Out: every pad mixed; Pad <n>: each alone. Show opens the editor.";
    for(auto* sm : servers) if(sm && sm->getServer()) { browser.setPreviewServer(sm->getServer()); break; }
    std::string dir = ofToDataPath("Supercollider/Samples", true);
    if(!std::filesystem::exists(dir)) dir = ofToDataPath("", true);
    browser.refresh(dir);
    browser.onActivate = [this](const std::string& path) {
        if(pads.empty()) return;
        Pad& p = *pads[std::clamp(selected, 0, (int)pads.size() - 1)];
        loadSample(p, path);
        if(!p.B("Sample On").get()) p.B("Sample On") = true;
        browser.stopPreview();
    };
}

scDrumKitBox::~scDrumKitBox() {
    scGlobalPitch::unfollow(this);
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

void scDrumKitBox::setup() {
    // a kit: drums stay put unless ticked in Global Pitch
    scGlobalPitch::follow(this, false, [this](){
        for(auto& p : pads) for(auto& [srv, sc] : p->sc) if(sc.trig) sc.trig->set("gtrbus", scGlobalPitch::bus(this, srv));
    });
    addInspectorParameter(embedSamples.set("Embed Samples", true));
    addSeparator("DrumKitBox");
    addParameter(showWindow.set("Show", false));
    gateHandle = addParameter(gate.set("Gate", {0.0f}, {0.0f}, {1.0f}));
    pitchIn.attach(addParameter(pitch.set("Pitch", {60.0f}, {0.0f}, {127.0f})), availableInputs,
                   [this](){ for(auto& o : outputs) o = o; });
    durationHandle = addParameter(duration.set("Duration", {1.0f}, {0.05f}, {24.0f}));
    noteInputs.add(gateHandle, gate);
    noteInputs.add(pitchIn.handle, pitch);
    filterHandle = addParameter(filter.set("Filter", {0.0f}, {-1.0f}, {1.0f}));
    noteInputs.add(durationHandle, duration);
    noteInputs.add(filterHandle, filter);
    noteInputs.enable(this, [this](){
        for(auto& p : pads) for(auto& [srv, sc] : p->sc) if(sc.trig) return true;
        return false;
    });
    addParameter(gain.set("Gain", 0.7f, 0.0f, 2.0f));
    addParameter(channels.set("N Chan", 2, 1, MaxChannels));
    addInput("Gate Audio");
    addOutput("Out");

    // Gate: element n is channel n; a rising edge is a hit at that level.
    // The hit and its level go out in one message (the level must be there
    // at the hit's sample). The timeline's frame set of a value it already
    // sent timetagged: nothing to do.
    nodeListeners.push(gate.newListener([this](std::vector<float>& v) {
        if(scScheduling::isBackendSendSuppressed()) return;
        for(auto& up : pads) {
            Pad& p = *up;
            const int ch = padChannel(p);
            const float lvl = ch < (int)v.size() ? v[ch] : 0.0f;
            const bool high = lvl > 0.0f;
            const bool hit = high && !p.gateHigh;
            p.gateHigh = high;
            if(hit) ++p.fireCount;
            if(p.auditionHeld && !hit) continue;
            for(auto& [srv, sc] : p.sc) {
                if(!sc.trig) continue;
                scSynthControlBatch c(sc.trig, srv);
                c.set("gate", lvl);
                if(hit) c.set("fire", p.fireCount);
                c.send();
            }
        }
    }));
    nodeListeners.push(pitch.newListener([this](std::vector<float>&) {
        if(scScheduling::isBackendSendSuppressed()) return;
        for(auto& p : pads) sendChannelInputs(*p, true);
    }));
    nodeListeners.push(duration.newListener([this](std::vector<float>&) {
        if(scScheduling::isBackendSendSuppressed()) return;
        for(auto& p : pads) sendChannelInputs(*p, true);
    }));
    nodeListeners.push(filter.newListener([this](std::vector<float>&) {
        if(scScheduling::isBackendSendSuppressed()) return;
        for(auto& p : pads) sendChannelInputs(*p, true);
    }));
    nodeListeners.push(gain.newListener([this](float&) {
        for(auto& p : pads) for(auto& [srv, s] : p->sc) if(s.strip) s.strip->set("master", gain.get());
    }));
    nodeListeners.push(channels.newListener([this](int&) { if(!restoring) rebuildAll(); }));
    dragListener = ofEvents().fileDragEvent.newListener([this](ofDragInfo& info) {
        if(!showWindow.get() || !windowHovered || info.files.empty()) return;
        pendingDropFile = info.files.front().string();
    });
    // the default kit: kick, snare, closed and open hat (choked together)
    addPad(M_KICK);
    initialPad = padState(0);
    addPad(M_SNARE);
    addPad(M_CLOSED_HAT);
    addPad(M_OPEN_HAT);
    timeline.setup(this, [this](const std::string& k) { publish(k); }, [this](const std::string& k) { unpublish(k); },
                   [this]() { return publishedKeys; });

    // Snapshots (the shared system): the whole kit
    snaps.setupParameters();
    for(auto* p : {&snaps.snapshotP, &snaps.morphAP, &snaps.morphBP, &snaps.morphCurveP, &snaps.morphSwitchP}) registerNodeControl(*p);
    for(auto* p : {&snaps.morphP, &snaps.morphTimeP}) registerNodeControl(*p);
    for(auto* p : {&snaps.morphBeatsP, &snaps.morphGlideP}) registerNodeControl(*p);
    {
        scSnapshots::Host h;
        h.folder = "DrumKitBox"; h.title = "DrumKitBox";
        h.capture = [this]() { return captureSnapshot(); };
        h.apply = [this](const ofJson& a, const ofJson& b, float t, bool sw) { applySnapshot(a, b, t, sw); };
        h.factory = [this]() { return factorySnapshots(); };
        h.bpm = [this]() {
            const float b = getFrameTransportState().current.bpm;
            return b > 0.0f ? b : bpm;
        };
        h.paramMenu = [this](const std::string& k) { drawPublishedUnderline(k); drawPublishMenu(k); };
        h.morphBlocked = [this](const std::string& k) {
            auto it = published.find(k); return it != published.end() && it->second->hasInConnection();
        };
        h.morphChanged = [this]() { for(auto& p : pads) syncPad(*p); };
        snaps.setup(h);
    }
    requireSynthdefs();
}

template<typename T>
void scDrumKitBox::registerNodeControl(ofParameter<T>& param) {
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

void scDrumKitBox::requireSynthdefs() {
    auto find = [](const std::string& expected, const std::string& bundledDir) {
        std::filesystem::path folder;
        try {
            const std::filesystem::path root = ofToDataPath(SYNTHDEF_DIRECTORY, true);
            if(std::filesystem::exists(root))
                for(const auto& e : std::filesystem::recursive_directory_iterator(root,
                        std::filesystem::directory_options::skip_permission_denied))
                    if(e.is_regular_file() && e.path().filename() == expected) { folder = e.path().parent_path(); break; }
            if(folder.empty()) {
                const auto bundled = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()
                    .parent_path().parent_path() / bundledDir;
                if(std::filesystem::exists(bundled / expected)) folder = bundled;
            }
        } catch(const std::exception& e) { ofLogWarning("scDrumKitBox") << e.what(); }
        return folder;
    };
    for(const auto& [expected, dir] : std::vector<std::pair<std::string, std::string>>{
            {"DKVoice_2.scsyndef", "synthdefs/Defaults/DrumKitBox/CompiledSynthdefs/DrumKitBox"},
            {"mixerProComp2.scsyndef", "synthdefs/Defaults/MixerPro/CompiledSynthdefs/MixerPro"},
            {"ModLfo1.scsyndef", "synthdefs/Defaults/Modulation/CompiledSynthdefs/Modulation"}}) {
        const auto folder = find(expected, dir);
        if(folder.empty()) { ofLogError("scDrumKitBox") << "Compile the SynthDefs of " << dir; continue; }
        if(expected.rfind("DKVoice", 0) == 0) synthdefFolder = folder.string();
        for(auto* sm : servers) if(sm) sm->requireSynthdefFolder(folder.string());
    }
}

void scDrumKitBox::setBpm(float b) {
    bpm = b;
    for(auto& p : pads) {
        p->mod.setBpm(b);
        for(auto& [srv, sc] : p->sc) for(int fx : {FX_ECHO, FX_GRAIN}) if(sc.stage[fx].synth) sendStage(srv, *p, fx);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Pad controls
// ════════════════════════════════════════════════════════════════════════════

template<typename T>
scDrumKitBox::Control& scDrumKitBox::registerPadControl(Pad& p, ofParameter<T>& param, const Spec& spec) {
    const std::string label = spec.label;
    if constexpr(std::is_same<T, bool>::value) param.set(label + p.suffix(), spec.def > 0.5f);
    else param.set(label + p.suffix(), (T)spec.def, (T)spec.lo, (T)spec.hi);
    Control c;
    c.key = param.getEscapedName();
    {
        ofParameter<T> plain;
        if constexpr(std::is_same<T, bool>::value) plain.set(label, false); else plain.set(label, (T)spec.def, (T)spec.lo, (T)spec.hi);
        c.base = plain.getEscapedName();
    }
    c.pad = p.index;
    c.dest = spec.dest;
    c.scName = spec.sc;
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
        const auto& ps = j["pads"][std::min<size_t>(padIndex, j["pads"].size() - 1)];
        if(!ps.contains("controls") || !ps["controls"].contains(baseKey)) return {};
        return ps["controls"][baseKey];
    };
    const std::string sc = spec.sc;
    if constexpr(std::is_same<T, bool>::value) {
        // switches the morph can fade (an effect, a layer, a voice stage)
        if(sc == "on" || spec.dest == DEST_LAYER_ON || sc == "filteron" || sc == "charon" || sc == "transon")
            snaps.morph.bypass(param, readSnapshot);
    } else if constexpr(std::is_same<T, int>::value) {
        if(sc == "filtertype") snaps.morph.mode(param, 4, readSnapshot);   // the voice's and the echo's
    }
    controls.push_back(std::move(c));
    Pad* pp = &p;
    const int dest = spec.dest;
    const std::string section = spec.section;
    p.listeners.push(param.newListener([this, pp, &param, dest, sc, section](T& value) {
        snaps.morph.edited(param);
        if(restoring) return;
        Pad& pad = *pp;
        if(sc == "channel") { sendChannelInputs(pad); return; }
        if(sc == "choke") { sendChokeMasks(); return; }
        if(sc == "mute" || sc == "solo") { sendMutes(); return; }
        if(sc == "duckfrom") { sendDucks(); return; }
        if constexpr(std::is_same<T, int>::value) {
            if(sc == "model") { applyModel(pad, value, 0); return; }
            if(sc == "style") { applyModel(pad, pad.I("Model").get(), value); return; }
        }
        if(sc.empty() && dest != DEST_FX + FX_ECHO && dest != DEST_FX + FX_GRAIN) return;
        auto valueOf = [&]() -> float {
            if constexpr(std::is_same<T, bool>::value) return snaps.morph.activation(param); else return (float)value;
        };
        if(dest == DEST_TRIG) { for(auto& [srv, s] : pad.sc) if(s.trig) s.trig->set(sc, valueOf()); }
        else if(dest == DEST_SOUND) {
            const int targets = soundTargets(section);
            for(auto& [srv, s] : pad.sc) {
                for(int l = 0; l < L_COUNT; l++) if((targets & (1 << l)) && s.layer[l]) s.layer[l]->set(sc, valueOf());
                if((targets & VOICE) && s.voice) {
                    s.voice->set(sc, valueOf());
                    if(sc == "filtertype") s.voice->set("filterweights", snaps.morph.weights(pad.I("Filter Type")));
                }
            }
        }
        else if(dest == DEST_LAYER_ON) { for(auto& [srv, s] : pad.sc) syncLayers(srv, pad); }
        else if(dest == DEST_STRIP) { for(auto& [srv, s] : pad.sc) sendStrip(srv, pad); }
        else if(dest >= DEST_FX && dest < DEST_FX + FX_COUNT) {
            if(sc == "on") { for(auto& [srv, s] : pad.sc) syncStages(srv, pad); }
            else for(auto& [srv, s] : pad.sc) sendStage(srv, pad, dest - DEST_FX);
        }
    }));
    return controls.back();
}

void scDrumKitBox::registerModControl(Pad& p, ofAbstractParameter& param,
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

scDrumKitBox::Control* scDrumKitBox::findControl(const std::string& key) {
    for(auto& c : controls) if(c.key == key) return &c;
    return nullptr;
}

void scDrumKitBox::setupPad(Pad& p) {
    for(const auto& s : specs()) {
        if(s.type == 'f') registerPadControl<float>(p, p.f[s.label], s);
        else if(s.type == 'i') registerPadControl<int>(p, p.i[s.label], s);
        else registerPadControl<bool>(p, p.b[s.label], s);
    }
    p.layerOn = {&p.B("Body On"), &p.B("Click On"), &p.B("Noise On"), &p.B("Metal On"), &p.B("Sample On")};
    p.fxOn = {&p.B("EQ On"), &p.B("Compressor On"), &p.B("Saturator On"), &p.B("Echo On"), &p.B("Grains On"), &p.B("Reverb On"), &p.B("Stereo On")};
    {   // the combos' names (published: dropdowns)
        std::vector<std::string> chans = {"Auto: row " + ofToString(p.index + 1)};
        for(int c = 1; c <= MaxChannels; c++) chans.push_back(ofToString(c));
        std::vector<std::string> groups = {"None"};
        for(int g = 1; g <= ChokeGroups; g++) groups.push_back("Group " + ofToString(g));
        std::vector<std::string> duck = {"Off"};
        for(int k = 1; k <= MaxPads; k++) duck.push_back("Pad " + ofToString(k));
        dropdowns.set(p.I("Channel").getEscapedName(), chans);
        dropdowns.set(p.I("Choke").getEscapedName(), groups);
        dropdowns.set(p.I("Duck From").getEscapedName(), duck);
        dropdowns.set(p.I("Model").getEscapedName(), modelNames());
        dropdowns.set(p.I("Style").getEscapedName(), styleNames(p.I("Model").get()));
        dropdowns.set(p.I("Click Type").getEscapedName(), std::vector<std::string>{"Pulse", "Noise", "Blip", "Ping"});
        dropdowns.set(p.I("Noise Filter").getEscapedName(), std::vector<std::string>{"Low pass", "Band pass", "High pass", "Notch"});
        dropdowns.set(p.I("Metal Mode").getEscapedName(), std::vector<std::string>{"Square", "Ring", "FM"});
        dropdowns.set(p.I("Filter Type").getEscapedName(), std::vector<std::string>{"LP 12", "LP 24", "High pass", "Band pass"});
        dropdowns.set(p.I("Drive Type").getEscapedName(), std::vector<std::string>{"Soft", "Hard", "Fold", "Tube"});
        dropdowns.set(p.I("Echo Filter").getEscapedName(), std::vector<std::string>{"Low pass", "High pass", "Band pass", "Peak"});
    }

    // ── The pad's modulation: envelopes, LFOs, routing to its targets ───────
    Pad* pp = &p;
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
    h.voices = []() { return 1; };   // one voice per pad: its hits
    p.mod.nameSuffix = p.suffix();
    // the synths read these, in this order (drumkitbox.scd); amount 1 = the control's span
    p.mod.setup(h, {{"Pitch", 48, "st"}, {"Decay", 4, "oct"}, {"Cutoff", 9.966f, "oct"}, {"Reso", 1, ""},
                    {"Drive", 48, "dB"}, {"Gain", 1, ""}, {"Pan", 2, ""}, {"Sweep", 72, "st"},
                    {"Noise Cut", 9.966f, "oct"}, {"Metal Tone", 6.644f, "oct"}, {"Smp Start", 1, "", true},
                    {"Body Shape", 2, ""}}, false, true, true);
    p.mod.targetOfKey = {{p.F("Tune").getEscapedName(), 0}, {p.F("Decay").getEscapedName(), 1},
                         {p.F("Cutoff").getEscapedName(), 2}, {p.F("Resonance").getEscapedName(), 3},
                         {p.F("Drive").getEscapedName(), 4}, {p.F("Pan").getEscapedName(), 6},
                         {p.F("Sweep").getEscapedName(), 7}, {p.F("Noise Cutoff").getEscapedName(), 8},
                         {p.F("Metal Tone").getEscapedName(), 9}, {p.F("Sample Start").getEscapedName(), 10},
                         {p.F("Body Shape").getEscapedName(), 11}};
    p.mod.setBpm(bpm);
}

// ════════════════════════════════════════════════════════════════════════════
// Pads: add, remove, move, duplicate
// ════════════════════════════════════════════════════════════════════════════

scDrumKitBox::Pad* scDrumKitBox::addPad(int model, bool withModel) {
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
    if(withModel) applyModel(p, model, 0);   // (its synths are created below)
    for(auto& [srv, n] : nodeSC) if(n.created) { ensureBuffers(p, srv); createPad(srv, p); }
    sendChokeMasks();
    sendMutes();
    sendDucks();
    return &p;
}

void scDrumKitBox::removePad(int k) {
    if(pads.size() <= 1 || k < 0 || k >= (int)pads.size()) return;
    movePad(k, (int)pads.size() - 1);
    Pad& p = *pads.back();
    const bool hadLfoOutputs = p.mod.hasLfoOutputs();
    const int idx = p.index;
    std::vector<ofxSCServer*> active;
    for(auto& [srv, s] : p.sc) active.push_back(srv);
    for(auto* srv : active) destroyPad(srv, p);
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
    // a pad ducking from the one that went: from nothing
    for(auto& q : pads) if(q->I("Duck From").get() > (int)pads.size()) q->I("Duck From") = 0;
    selected = std::clamp(selected, 0, (int)pads.size() - 1);
    sendChokeMasks();
    sendMutes();
    sendDucks();
}

void scDrumKitBox::duplicatePad(int k) {
    if(k < 0 || k >= (int)pads.size()) return;
    const ofJson state = padState(k);
    const std::string path = pads[k]->sample->path;
    Pad* p = addPad(M_KICK, false);
    if(!p) return;
    applyPadState(p->index, state);
    p->name = pads[k]->name + " copy";
    if(!path.empty()) loadSample(*p, path);
    syncPad(*p);
    movePad(p->index, k + 1);
    selected = k + 1;
}

void scDrumKitBox::movePad(int from, int to) {
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
    }
    // ducking follows the pads it points at
    std::vector<int> where(n);
    for(int i = 0; i < n; i++) where[i] = i;
    move(where);   // where[new] = old
    std::vector<int> newOf(n);
    for(int i = 0; i < n; i++) newOf[where[i]] = i;
    restoring = true;
    for(auto& q : pads) { const int d = q->I("Duck From").get(); if(d > 0 && d <= n) q->I("Duck From") = newOf[d - 1] + 1; }
    restoring = false;
    for(auto& p : pads) syncPad(*p);
    sendChokeMasks();
    sendMutes();
    sendDucks();
    if(selected == from) selected = to;
    else if(from < selected && to >= selected) selected--;
    else if(from > selected && to <= selected) selected++;
}

std::string scDrumKitBox::relKey(const std::string& key) const {
    for(const auto& c : controls) if(c.key == key && c.pad >= 0) return "#" + c.base;
    return key;
}
std::string scDrumKitBox::absKey(const std::string& rel, int pad) const {
    if(rel.empty() || rel[0] != '#') return rel;
    const std::string base = rel.substr(1);
    for(const auto& c : controls) if(c.pad == pad && c.base == base) return c.key;
    return rel;
}
void scDrumKitBox::rewriteModKeys(ofJson& m, bool toRelative, int pad) const {
    auto rewrite = [&](const std::string& key){ return toRelative ? relKey(key) : absKey(key, pad); };
    if(m.contains("Params") && m["Params"].is_array())
        for(auto& e : m["Params"])
            if(e.is_object() && e.contains("key") && e["key"].is_string())
                e["key"] = rewrite(e["key"].get<std::string>());
    if(m.contains("Generators") && m["Generators"].is_object()) {
        ofJson generators = ofJson::object();
        for(auto it = m["Generators"].begin(); it != m["Generators"].end(); ++it)
            generators[rewrite(it.key())] = it.value();
        m["Generators"] = std::move(generators);
    }
}

ofJson scDrumKitBox::padState(int k) const {
    const Pad& p = *pads[k];
    ofJson j;
    j["name"] = p.name;
    j["path"] = p.sample->path;
    if(!p.sound.empty()) j["sound"] = p.sound;
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

void scDrumKitBox::applyPadState(int k, const ofJson& j) {
    Pad& p = *pads[k];
    restoring = true;
    if(j.contains("name") && j["name"].is_string()) p.name = j["name"].get<std::string>();
    p.sound = j.value("sound", std::string());
    if(j.contains("controls") && j["controls"].is_object()) {
        const auto& v = j["controls"];
        for(auto& c : controls)
            if(c.pad == k && v.contains(c.base) && v[c.base].is_string())
                try { c.load(v[c.base].get<std::string>()); } catch(...) {}
    }
    restoring = false;
    dropdowns.set(p.I("Style").getEscapedName(), styleNames(p.I("Model").get()));
    if(j.contains("mod") && j["mod"].is_object()) {
        ofJson m = j["mod"];
        rewriteModKeys(m, false, k);
        p.mod.load(m, "");
        p.mod.refresh();
        p.mod.afterLoad();
    }
}

int scDrumKitBox::padChannel(const Pad& p) const {
    const int c = p.i.at("Channel").get();
    return std::clamp(c <= 0 ? p.index : c - 1, 0, MaxChannels - 1);
}

float scDrumKitBox::valueFor(const std::vector<float>& v, int channel, float fallback) const {
    if(v.empty()) return fallback;
    if(v.size() == 1) return v[0];
    return channel < (int)v.size() ? v[channel] : fallback;
}

std::string scDrumKitBox::channelSource(int channel) {
    if(inputs.empty()) return "";
    const nodePort port = inputs[0].get();
    if(!port.getNodeRef()) return "";
    return port.getNodeRef()->getOutputChannelName(port.getIndex(), channel);
}

std::string scDrumKitBox::padLabel(const Pad& p) const {
    return ofToString(p.index + 1) + " " + p.name;
}

bool scDrumKitBox::anySolo() const {
    for(const auto& p : pads) if(p->b.at("Solo").get()) return true;
    return false;
}

// ════════════════════════════════════════════════════════════════════════════
// The Sample layer's file
// ════════════════════════════════════════════════════════════════════════════

void scDrumKitBox::loadSample(Pad& p, const std::string& path) {
    if(path.empty()) return;
    p.sample->pendingFile = path;     // decoded in update()
}

void scDrumKitBox::applyDecoded(Pad& p, scSlicerBox::Audio&& a) {
    Sample& s = *p.sample;
    if(!a.error.empty()) {
        s.error = std::filesystem::path(a.path).filename().string() + ": " + a.error;
        ofLogWarning("scDrumKitBox") << s.error;
        return;
    }
    s.error.clear();
    s.path = a.path;
    s.playPath = a.playPath;
    s.channels = std::max(1, std::min(2, a.channels));
    s.rate = a.sampleRate;
    s.seconds = a.frames / std::max(1.0f, a.sampleRate);
    s.peaks.assign(2048, 0.0f);
    for(int f = 0; f < a.frames; ++f) {
        const int bin = (int)((int64_t)f * 2048 / std::max(1, a.frames));
        s.peaks[bin] = std::max(s.peaks[bin], std::min(1.0f, std::abs(a.mono[f])));
    }
    for(auto& [srv, sc] : p.sc) {
        auto old = std::move(s.buffers[srv]);
        s.buffers.erase(srv);
        ensureBuffers(p, srv);
        syncLayers(srv, p);   // the definition follows the file's channels
        if(sc.layer[L_SAMPLE]) sendSound(srv, p, sc.layer[L_SAMPLE], L_SAMPLE);
        for(auto* b : old) if(b) { if(liveBuffer(srv, b)) b->free(); delete b; }
    }
}

void scDrumKitBox::releaseBuffers(Sample& s, ofxSCServer* srv) {
    auto it = s.buffers.find(srv);
    if(it == s.buffers.end()) return;
    for(auto* b : it->second) if(b) { if(liveBuffer(srv, b)) b->free(); delete b; }
    s.buffers.erase(it);
}

void scDrumKitBox::ensureBuffers(Pad& p, ofxSCServer* srv) {
    Sample& s = *p.sample;
    if(!srv || s.playPath.empty()) return;
    auto it = s.buffers.find(srv);
    if(it != s.buffers.end() && (int)it->second.size() == s.channels &&
       std::all_of(it->second.begin(), it->second.end(), [srv](ofxSCBuffer* b){ return liveBuffer(srv, b); })) return;
    releaseBuffers(s, srv);
    for(int ch = 0; ch < s.channels; ++ch) {
        auto* b = new ofxSCBuffer(0, 0, srv);
        if(b->index < 0) { delete b; releaseBuffers(s, srv); return; }
        b->readChannel(s.playPath, {ch});
        s.buffers[srv].push_back(b);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Synths
// ════════════════════════════════════════════════════════════════════════════

std::string scDrumKitBox::layerDef(const Pad& p, int layer) const {
    // the poly build: four voices (the "P" variant)
    const std::string poly = p.b.at("Poly").get() ? "P" : "";
    if(layer == L_SAMPLE) return std::string(layerDefs[layer]) + ofToString(std::clamp(p.sample->channels, 1, 2)) + poly;
    return layerDefs[layer] + poly;
}
std::string scDrumKitBox::stageDef(int fx) const { return stageDefs[fx] + ofToString(channels.get()); }

void scDrumKitBox::sendTrig(ofxSCServer* srv, Pad& p, bool includeNotes) {
    auto it = p.sc.find(srv);
    if(it == p.sc.end() || !it->second.trig) return;
    PadSC& sc = it->second;
    ofxSCSynth* s = sc.trig;
    scSynthControlBatch c(s, srv);
    if(sc.hits) c.set("hitbus", sc.hits->index);
    if(sc.gateBus) c.set("gatebus", sc.gateBus->index);
    c.set("velbus", p.mod.sourceBus(srv, scModulation::SRC_VEL));
    c.set("rndbus", p.mod.sourceBus(srv, scModulation::SRC_RND));
    c.set("vis", sc.vis ? sc.vis->index : -1);
    c.set("tune", p.F("Tune").get());
    c.set("gtrbus", (float)scGlobalPitch::bus(this, srv));
    auto n = nodeSC.find(srv);
    c.set("chokeblock", n != nodeSC.end() && n->second.choke ? n->second.choke->index : -1);
    c.set("chokeslot", p.index);
    std::vector<float> mask(16, 0.0f);
    if(p.I("Choke").get() > 0)
        for(const auto& q : pads) if(q.get() != &p && q->I("Choke").get() == p.I("Choke").get() && q->index < 16) mask[q->index] = 1.0f;
    c.set("chokemask", mask);
    if(includeNotes) {
        const int ch = padChannel(p);
        c.set("trigbus", inputBuses.count(srv) && inputBuses[srv] >= 0 ? inputBuses[srv] + ch : -1);
        const int pb = pitchIn.bus(srv);
        if(pb < 0) c.set("pitch", valueFor(pitch.get(), ch, 60.0f));
        c.set("dur", valueFor(duration.get(), ch, 1.0f));
        c.set("filt", valueFor(filter.get(), ch, 0.0f));
        if(!p.auditionHeld) c.set("gate", ch < (int)gate.get().size() ? gate.get()[ch] : 0.0f);
        c.send();
        if(pb >= 0) s->mapan("pitch", pb + ch, 1);
        return;
    }
    c.send();
}

// A layer's or the voice's controls (layer -1: the voice)
void scDrumKitBox::sendSound(ofxSCServer* srv, Pad& p, ofxSCSynth* s, int layer) {
    auto it = p.sc.find(srv);
    if(it == p.sc.end() || !s) return;
    PadSC& sc = it->second;
    const int mine = layer < 0 ? VOICE : 1 << layer;
    scSynthControlBatch c(s, srv);
    for(const auto& spec : specs()) {
        if(spec.dest != DEST_SOUND || !spec.sc[0] || !(soundTargets(spec.section) & mine)) continue;
        if(spec.type == 'f') c.set(spec.sc, p.F(spec.label).get());
        else if(spec.type == 'i') c.set(spec.sc, p.I(spec.label).get());
        else c.set(spec.sc, snaps.morph.activation(p.B(spec.label)));
    }
    if(sc.hits) c.set("hitbus", sc.hits->index);
    c.set("modbus", p.mod.targetBus(srv, 0));
    if(layer < 0) {
        if(sc.raw) c.set("raw", sc.raw->index);
        c.set("filterweights", snaps.morph.weights(p.I("Filter Type")));
        auto n = nodeSC.find(srv);
        c.set("keybus", n != nodeSC.end() && n->second.keys ? n->second.keys->index + p.index : -1);
    } else if(sc.raw) c.set("out", sc.raw->index);
    if(layer == L_SAMPLE) {
        const auto bs = p.sample->buffers.find(srv);
        const bool loaded = bs != p.sample->buffers.end() && (int)bs->second.size() >= p.sample->channels;
        c.set("loaded", int(loaded));
        c.set("buf0", loaded ? bs->second[0]->index : 0);
        c.set("buf1", loaded && p.sample->channels > 1 ? bs->second[1]->index : 0);
    }
    c.send();
}

float scDrumKitBox::beatsToSeconds(float beats) const {
    const float b = getFrameTransportState().current.bpm > 0.0f ? getFrameTransportState().current.bpm : bpm;
    return beats * 60.0f / std::max(1.0f, b);
}

float scDrumKitBox::echoSeconds(const Pad& p) const {
    const float b = getFrameTransportState().current.bpm > 0.0f ? getFrameTransportState().current.bpm : bpm;
    return ofClamp(p.f.at("Echo Time").get() * 60.0f / std::max(1.0f, b), 0.005f, 4.0f);
}

float scDrumKitBox::stageTail(const Pad& p, int fx) const {
    if(fx == FX_ECHO) {
        const float feed = ofClamp(p.f.at("Echo Feedback").get(), 0.0f, 0.98f);
        const float repeats = feed > 0.001f ? std::log(0.001f) / std::log(feed) : 0.0f;
        return std::min(30.0f, echoSeconds(p) * (repeats + 1.0f) + 0.2f);
    }
    if(fx == FX_GRAIN) {
        const float feed = ofClamp(p.f.at("Grain Feedback").get(), 0.0f, 0.95f);
        const float repeats = feed > 0.001f ? std::log(0.001f) / std::log(feed) : 0.0f;
        const float d = ofClamp(beatsToSeconds(p.f.at("Grain Time").get()), 0.01f, 3.5f) + p.f.at("Grain Jitter").get() + p.f.at("Grain Size").get();
        return std::min(30.0f, d * (repeats + 1.0f) + 0.3f);
    }
    if(fx == FX_REVERB) return std::min(30.0f, p.f.at("Rev Decay").get() + p.f.at("Rev Predelay").get() * 0.001f + 0.3f);
    return 0.05f;
}

void scDrumKitBox::sendStage(ofxSCServer* srv, Pad& p, int fx) {
    auto it = p.sc.find(srv);
    if(it == p.sc.end()) return;
    Stage& st = it->second.stage[fx];
    if(!st.synth) return;
    scSynthControlBatch c(st.synth, srv, &st.sent);
    // the stage's own controls, as named in the synth
    for(const auto& spec : specs()) {
        if(spec.dest != DEST_FX + fx || !spec.sc[0]) continue;
        const std::string sc = spec.sc;
        if(sc == "on") c.set("on", st.offAt >= 0.0f ? 0.0f : snaps.morph.activation(p.B(spec.label)));
        else if(fx == FX_EQ && sc.size() > 6 && sc.compare(2, 5, "shape") == 0) {
            const int band = sc[1] - '1';
            const float shape = p.F(spec.label).get();
            const float gainDb = p.F(std::string(spec.label).substr(0, std::string(spec.label).find(scEQEditor::isShelf(band) ? " Slope" : " Q")).c_str()).get();
            const std::string k = "b" + ofToString(band + 1);
            if(scEQEditor::isShelf(band)) c.set(k + "slope", 1.0f / scEQEditor::safeShelfSlope(shape, gainDb));
            else c.set(k + "rq", 1.0f / std::max(0.01f, shape));
        }
        else if(spec.type == 'f') c.set(sc, p.F(spec.label).get());
        else if(spec.type == 'i') c.set(sc, p.I(spec.label).get());
        else c.set(sc, snaps.morph.activation(p.B(spec.label)));
    }
    switch(fx) {
        case FX_COMP: c.set("grBus", -1); c.set("automakeup", 0.0f); break;
        case FX_SAT: c.set("autogain", 1.0f); break;
        case FX_ECHO:
            c.set("delay", echoSeconds(p));
            c.set("filterweights", snaps.morph.weights(p.I("Echo Filter")));
            break;
        case FX_GRAIN: c.set("delay", ofClamp(beatsToSeconds(p.F("Grain Time").get()), 0.01f, 3.5f)); break;
        case FX_REVERB: c.set("lowdamp", 0.0f); c.set("modfreq", 0.2f); c.set("spin", 1.0f); c.set("wander", 0.25f); break;
        default: break;
    }
    c.send();
}

void scDrumKitBox::sendStrip(ofxSCServer* srv, Pad& p) {
    auto it = p.sc.find(srv);
    if(it == p.sc.end() || !it->second.strip) return;
    PadSC& sc = it->second;
    scSynthControlBatch c(sc.strip, srv);
    if(sc.bus) c.set("bus", sc.bus->index);
    c.set("level", p.F("Level").get());
    c.set("master", gain.get());
    c.set("mute", int(p.B("Mute").get() || (anySolo() && !p.B("Solo").get())));
    const auto& ob = outputBuses[srv];
    const auto mix = ob.find(0), own = ob.find(p.index + 1);
    c.set("mix", mix != ob.end() ? mix->second : -1);
    c.set("padout", own != ob.end() ? own->second : -1);
    if(sc.meter) c.set("meterbus", sc.meter->index);
    const int from = p.I("Duck From").get();
    auto n = nodeSC.find(srv);
    const bool ducks = from > 0 && from <= (int)pads.size() && from - 1 != p.index && n != nodeSC.end() && n->second.keys;
    c.set("duckbus", ducks ? n->second.keys->index + from - 1 : -1);
    c.set("duckamt", p.F("Duck Amount").get());
    c.set("duckatk", p.F("Duck Attack").get());
    c.set("duckrel", p.F("Duck Release").get());
    c.send();
}

#ifdef OFXOCEANODE_HAS_TIMELINE_INSTRUMENT
bool scDrumKitBox::getTimelineInstrument(ofxOceanodeTimelineInstrument& out) {
    if(!gateHandle) return false;
    out.gate = gateHandle.get();
    out.pitch = pitchIn.handle.get();
    out.duration = durationHandle.get();
    out.filter = filterHandle.get();
    out.velocity = nullptr;
    out.kit = true;
    out.voices = 1;
    for(const auto& p : pads) out.voices = std::max(out.voices, padChannel(*p) + 1);
    out.rowNames.assign(out.voices, std::string());
    for(const auto& p : pads) {
        auto& n = out.rowNames[padChannel(*p)];
        n = n.empty() ? p->name : n + " + " + p->name;
    }
    // each row in its pad's colour (the window's pad list, scDrumKitBoxWindow.cpp's padRGB);
    // a channel several pads play takes the first one's
    static const unsigned char padRGB[8][3] = {
        {236, 150,  80}, {108, 180, 232}, {230, 200,  90}, {228, 122, 146},
        {120, 200, 140}, {186, 146, 232}, { 96, 200, 176}, {232, 146, 104}};
    out.rowColors.assign(out.voices, ofColor(130, 130, 140));   // a channel no pad plays: grey
    std::vector<bool> colored(out.voices, false);
    for(const auto& p : pads) {
        const int ch = padChannel(*p);
        if(colored[ch]) continue;
        const auto* c = padRGB[p->index % 8];
        out.rowColors[ch] = ofColor(c[0], c[1], c[2]);
        colored[ch] = true;
    }

    return true;
}
#endif

void scDrumKitBox::sendChannelInputs(Pad& p, bool pitchOnly) {
    auto inEffect = noteInputs.inEffectNow();
    for(auto& [srv, sc] : p.sc) {
        if(!sc.trig) continue;
        const int ch = padChannel(p);
        ofxSCSynth* s = sc.trig;
        if(!pitchOnly) s->set("trigbus", inputBuses.count(srv) && inputBuses[srv] >= 0 ? inputBuses[srv] + ch : -1);
        const int pb = pitchIn.bus(srv);
        if(pb >= 0) s->mapan("pitch", pb + ch, 1); else s->set("pitch", valueFor(pitch.get(), ch, 60.0f));
        s->set("dur", valueFor(duration.get(), ch, 1.0f));
        s->set("filt", valueFor(filter.get(), ch, 0.0f));
        if(!pitchOnly && !p.auditionHeld) s->set("gate", ch < (int)gate.get().size() ? gate.get()[ch] : 0.0f);
    }
}

void scDrumKitBox::sendChokeMasks() {
    for(auto& up : pads) {
        Pad& p = *up;
        std::vector<float> mask(16, 0.0f);
        if(p.I("Choke").get() > 0)
            for(const auto& q : pads) if(q.get() != &p && q->I("Choke").get() == p.I("Choke").get() && q->index < 16) mask[q->index] = 1.0f;
        for(auto& [srv, sc] : p.sc) if(sc.trig) { sc.trig->set("chokemask", mask); sc.trig->set("chokeslot", p.index); }
    }
}

void scDrumKitBox::sendMutes() {
    const bool solo = anySolo();
    for(auto& p : pads)
        for(auto& [srv, sc] : p->sc) if(sc.strip) sc.strip->set("mute", int(p->B("Mute").get() || (solo && !p->B("Solo").get())));
}

void scDrumKitBox::sendDucks() {
    for(auto& p : pads) for(auto& [srv, sc] : p->sc) sendStrip(srv, *p);
}

// The layers that are on exist; the others are freed (no CPU). The Sample
// layer's definition follows its file's channels.
void scDrumKitBox::syncLayers(ofxSCServer* srv, Pad& p) {
    auto it = p.sc.find(srv);
    if(it == p.sc.end() || !it->second.created) return;
    PadSC& sc = it->second;
    for(int l = 0; l < L_COUNT; l++) {
        const bool want = snaps.morph.activation(*p.layerOn[l]) > 0.0f;
        ofxSCSynth*& s = sc.layer[l];
        const bool stale = s && sc.layerName[l] != layerDef(p, l);   // Poly, the sample's channels
        if(want && (!s || stale)) {
            ofxSCSynth* old = s;
            s = new ofxSCSynth(layerDef(p, l), srv);
            sc.layerName[l] = layerDef(p, l);
            sendSound(srv, p, s, l);
            if(old) { s->createAndRun(4, old->nodeID, true); delete old; }   // replace in place
            else s->createAndRun(1, sc.layers->nodeID, true);
        } else if(want) {
            // a morph fades a layer through its level: resend
            if(l == L_SAMPLE) sendSound(srv, p, s, l);
        } else if(!want && s) {
            s->free();
            delete s;
            s = nullptr;
        }
    }
}

// voice -> the running effects, in order -> the strip
void scDrumKitBox::routeChain(ofxSCServer* srv, Pad& p) {
    auto it = p.sc.find(srv);
    if(it == p.sc.end() || !it->second.created) return;
    PadSC& sc = it->second;
    int next = sc.bus ? sc.bus->index : -1;
    for(int s = FX_COUNT - 1; s >= 0; s--) {
        auto& st = sc.stage[s];
        if(!st.synth || !st.running || !st.in) continue;
        st.synth->set("out", next);
        next = st.in->index;
    }
    if(sc.voice) sc.voice->set("out", next);
}

// An effect switched on is created (in the chain's order) and joins the
// chain once the server has it; switched off, it stops taking input and is
// freed once its tail has rung out (update())
void scDrumKitBox::syncStages(ofxSCServer* srv, Pad& p) {
    auto it = p.sc.find(srv);
    if(it == p.sc.end() || !it->second.created) return;
    PadSC& sc = it->second;
    for(int fx = 0; fx < FX_COUNT; fx++) {
        Stage& st = sc.stage[fx];
        const bool on = snaps.morph.activation(*p.fxOn[fx]) > 0.0f && (fx != FX_WIDTH || channels.get() > 1);
        if(on) {
            if(!st.synth && !scFxDefinitionsReady(servers, srv)) continue;
            if(!liveAudioBus(srv, st.in)) {
                ensureBus(srv, st.in, RATE_AUDIO, channels.get());
                if(!st.in) continue;
                if(st.synth) st.synth->set("in", st.in->index);
            }
            st.offAt = -1.0f;
            if(!st.synth) {
                st.synth = new ofxSCSynth(stageDef(fx), srv);
                st.sent.clear();
                st.synth->set("in", st.in->index);
                st.synth->set("out", sc.bus ? sc.bus->index : 0);
                sendStage(srv, p, fx);
                ofxSCSynth* next = nullptr;
                for(int j = fx + 1; j < FX_COUNT && !next; j++) next = sc.stage[j].synth;
                if(next) st.synth->createAndRun(2, next->nodeID, true);
                else st.synth->createAndRun(1, sc.fx->nodeID, true);
            } else sendStage(srv, p, fx);
            st.running = scFxCanRoute(st.synth, srv);
        } else if(st.synth && st.offAt < 0.0f) {
            if(!st.running) {   // never joined the chain: nothing rings
                st.synth->free(); delete st.synth; st.synth = nullptr;
                dropBus(srv, st.in, RATE_AUDIO);
                continue;
            }
            st.synth->set("on", 0.0f);
            st.sent["on"] = {0.0f};
            st.offAt = ofGetElapsedTimef() + stageTail(p, fx);
        }
    }
    routeChain(srv, p);
}

void scDrumKitBox::createPad(ofxSCServer* srv, Pad& p) {
    auto n = nodeSC.find(srv);
    if(n == nodeSC.end() || !n->second.created || !n->second.root) return;
    if(n->second.padsPending) return;
    if(p.sc.count(srv)) destroyPad(srv, p);
    PadSC& sc = p.sc[srv];
    ensureBus(srv, sc.hits, RATE_AUDIO, 10);
    ensureBus(srv, sc.raw, RATE_AUDIO, 2);
    ensureBus(srv, sc.gateBus, RATE_AUDIO, 1);
    ensureBus(srv, sc.bus, RATE_AUDIO, channels.get());
    ensureBus(srv, sc.vis, RATE_CONTROL, 1);
    ensureBus(srv, sc.meter, RATE_CONTROL, 1);
    ensureBuffers(p, srv);
    // the pad's group: trig, sources, routes, layers, voice, effects, strip
    sc.group = new ofxSCGroup(srv);
    sc.group->create(1, n->second.root->nodeID);
    auto group = [&](ofxSCGroup*& g) { g = new ofxSCGroup(srv); g->create(1, sc.group->nodeID); };
    group(sc.sources);
    group(sc.routes);
    group(sc.layers);
    group(sc.fx);
    sc.created = true;
    p.mod.create(srv, sc.sources->nodeID, sc.routes->nodeID, sc.gateBus ? sc.gateBus->index : 0);
    sc.trig = new ofxSCSynth("DKTrig", srv);
    sendTrig(srv, p);
    sc.trig->createAndRun(0, sc.group->nodeID, true);
    sc.voice = new ofxSCSynth("DKVoice_" + ofToString(channels.get()), srv);
    sendSound(srv, p, sc.voice, -1);
    sc.voice->set("out", sc.bus ? sc.bus->index : 0);
    sc.voice->createAndRun(2, sc.fx->nodeID, true);           // before the effects
    syncLayers(srv, p);
    sc.strip = new ofxSCSynth("DKStrip_" + ofToString(channels.get()), srv);
    sendStrip(srv, p);
    sc.strip->createAndRun(1, sc.group->nodeID, true);        // after the effects
    syncStages(srv, p);
}

void scDrumKitBox::destroyPad(ofxSCServer* srv, Pad& p, bool sendFree) {
    auto it = p.sc.find(srv);
    if(it == p.sc.end()) return;
    PadSC& sc = it->second;
    p.mod.free(srv, false);                       // the group's /n_free takes its synths
    if(sendFree && sc.group && sc.created) sc.group->free();
    delete sc.trig; sc.trig = nullptr;
    for(auto*& s : sc.layer) { delete s; s = nullptr; }
    delete sc.voice; sc.voice = nullptr;
    for(auto& st : sc.stage) { delete st.synth; st.synth = nullptr; dropBus(srv, st.in, RATE_AUDIO); }
    delete sc.strip; sc.strip = nullptr;
    for(auto** g : {&sc.sources, &sc.routes, &sc.layers, &sc.fx, &sc.group}) { delete *g; *g = nullptr; }
    dropBus(srv, sc.hits, RATE_AUDIO);
    dropBus(srv, sc.raw, RATE_AUDIO);
    dropBus(srv, sc.gateBus, RATE_AUDIO);
    dropBus(srv, sc.bus, RATE_AUDIO);
    dropBus(srv, sc.vis, RATE_CONTROL);
    dropBus(srv, sc.meter, RATE_CONTROL);
    sc.created = false;
    p.sc.erase(it);
}

void scDrumKitBox::syncPadServer(ofxSCServer* srv, Pad& p) {
    auto it = p.sc.find(srv);
    if(it == p.sc.end() || !it->second.created) return;
    ensureBuffers(p, srv);
    sendTrig(srv, p, false);
    syncLayers(srv, p);
    for(int l = 0; l < L_COUNT; l++) if(it->second.layer[l]) sendSound(srv, p, it->second.layer[l], l);
    sendSound(srv, p, it->second.voice, -1);
    syncStages(srv, p);
    sendStrip(srv, p);
}

void scDrumKitBox::syncPad(Pad& p) {
    std::vector<ofxSCServer*> active;
    for(auto& [srv, sc] : p.sc) active.push_back(srv);
    for(auto* srv : active) syncPadServer(srv, p);
}

void scDrumKitBox::rebuildAll() {
    for(auto& [srv, n] : nodeSC) {
        if(!n.created) continue;
        for(auto& p : pads) createPad(srv, *p);
    }
}

void scDrumKitBox::buildSynth(ofxSCServer* srv) {
    if(!srv) return;
    if(nodeSC.count(srv)) free(srv);
    for(auto& p : pads) ensureBuffers(*p, srv);
    nodeSC[srv].root = new ofxSCGroup(srv);
}

void scDrumKitBox::createSynth(ofxSCServer* srv) {
    auto& n = nodeSC[srv];
    if(!n.root) n.root = new ofxSCGroup(srv);
    n.root->create(0, 1);
    n.created = true;
    ensureBus(srv, n.choke, RATE_CONTROL, 16);
    ensureBus(srv, n.keys, RATE_CONTROL, 16);
    // our definitions still loading: the pads come once the server has synced (update())
    if(srv->isNRTSyncPending()) {
        n.padsPending = true;
        n.pendingSince = ofGetElapsedTimef();
    }
    for(auto& p : pads) createPad(srv, *p);
    if(!getActive()) n.root->run(false);
}

void scDrumKitBox::free(ofxSCServer* srv) {
    for(auto& p : pads) destroyPad(srv, *p, false);
    auto it = nodeSC.find(srv);
    if(it == nodeSC.end()) return;
    if(it->second.root && it->second.created) it->second.root->free();
    delete it->second.root;
    dropBus(srv, it->second.choke, RATE_CONTROL);
    dropBus(srv, it->second.keys, RATE_CONTROL);
    nodeSC.erase(it);
}

void scDrumKitBox::setOutputBus(ofxSCServer* srv, int index, int bus) {
    outputBuses[srv][index] = bus;
    for(auto& p : pads) {
        if(index != 0 && index != p->index + 1) continue;
        auto it = p->sc.find(srv);
        if(it != p->sc.end() && it->second.strip) it->second.strip->set(index == 0 ? "mix" : "padout", bus);
    }
}
int scDrumKitBox::getOutputBusIndex(ofxSCServer* srv, int index) {
    auto it = outputBuses.find(srv);
    if(it == outputBuses.end()) return -1;
    auto b = it->second.find(index);
    return b != it->second.end() ? b->second : -1;
}
void scDrumKitBox::moveSynthBefore(ofxSCServer* srv, int nodeID) {
    auto it = nodeSC.find(srv);
    if(it != nodeSC.end() && it->second.created) it->second.root->moveBefore(nodeID);
}
int scDrumKitBox::getLastSynthID(ofxSCServer* srv) {
    auto it = nodeSC.find(srv);
    return (it != nodeSC.end() && it->second.created) ? it->second.root->nodeID : -1;
}
void scDrumKitBox::setInputBus(ofxSCServer* srv, scNode* node, int bus) {
    if(!pitchIn.take(srv, node, bus)) inputBuses[srv] = bus;
    for(auto& p : pads) sendChannelInputs(*p);
}
void scDrumKitBox::resetInputBusses(ofxSCServer* srv, int) {
    inputBuses.erase(srv);
    pitchIn.reset(srv);
    for(auto& p : pads) sendChannelInputs(*p);
}

void scDrumKitBox::fire(Pad& p) {
    ++p.fireCount;
    for(auto& [srv, sc] : p.sc) if(sc.trig) sc.trig->set("fire", p.fireCount);
}

void scDrumKitBox::resendParametersForNRT() {
    for(auto& p : pads) {
        for(auto& [srv, sc] : p->sc) for(auto& st : sc.stage) st.sent.clear();
        syncPad(*p);
        p->mod.resend();
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Frame
// ════════════════════════════════════════════════════════════════════════════

void scDrumKitBox::update(ofEventArgs&) {
    timeline.update();
    snaps.update();
    {
        const auto& names = snaps.slotNames();
        for(const auto& key : snaps.slotKeys()) { auto it = published.find(key); if(it != published.end()) scDropdowns::setNames(it->second, names); }
    }
    for(auto& [srv, n] : nodeSC) {
        if(!n.padsPending) continue;
        if(srv->isNRTSyncPending() && ofGetElapsedTimef() - n.pendingSince < 3.0f) continue;
        n.padsPending = false;
        for(auto& p : pads) createPad(srv, *p);
        sendChokeMasks();
        sendMutes();
    }
    const float now = ofGetElapsedTimef();
    for(auto& up : pads) {
        Pad& p = *up;
        p.mod.showing = showWindow.get() && p.index == selected;
        p.mod.update();
        Sample& s = *p.sample;
        if(!s.pendingFile.empty() && !s.task.valid()) {
            s.decodingPath = s.pendingFile;
            s.pendingFile.clear();
            s.task = std::async(std::launch::async, &scSlicerBox::decodeFile, s.decodingPath,
                                ofToDataPath("Supercollider/tmp/DrumKitBox", true), false);
        }
        if(s.task.valid() && s.task.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            applyDecoded(p, s.task.get());
            s.decodingPath.clear();
        }
        for(auto& [srv, sc] : p.sc) {
            // after a server reboot: new buffers (the graph rebuilds the synths)
            if(sc.layer[L_SAMPLE] && !s.playPath.empty()) {
                auto it = s.buffers.find(srv);
                const bool live = it != s.buffers.end() && !it->second.empty() &&
                    std::all_of(it->second.begin(), it->second.end(), [srv = srv](ofxSCBuffer* b){ return liveBuffer(srv, b); });
                if(!live) {
                    if(it != s.buffers.end()) { for(auto* b : it->second) delete b; s.buffers.erase(it); }
                    ensureBuffers(p, srv);
                    sendSound(srv, p, sc.layer[L_SAMPLE], L_SAMPLE);
                }
            }
            if(!sc.created) continue;
            // effects: the ones the server confirmed join the chain, the ones rung out go
            if(scFxAwaitingRoute(sc.stage, srv, [&](int fx){
                    return snaps.morph.activation(*p.fxOn[fx]) > 0.0f && (fx != FX_WIDTH || channels.get() > 1); },
                    scFxDefinitionsReady(servers, srv)))
                syncStages(srv, p);
            std::vector<Stage*> rungOut;
            for(auto& st : sc.stage) {
                if(st.offAt < 0.0f || now < st.offAt) continue;
                st.offAt = -1.0f;
                st.running = false;
                rungOut.push_back(&st);
            }
            if(!rungOut.empty()) {
                routeChain(srv, p);   // around them first, then they go
                for(auto* st : rungOut) {
                    if(st->synth) { st->synth->free(); delete st->synth; st->synth = nullptr; }
                    dropBus(srv, st->in, RATE_AUDIO);
                }
            }
        }
        p.flash = std::max(0.0f, p.flash - (float)ofGetLastFrameTime() * 4.0f);
    }
    if(pendingLoadDialog && !pads.empty()) {
        pendingLoadDialog = false;
        Pad& p = *pads[std::clamp(selected, 0, (int)pads.size() - 1)];
        auto r = ofSystemLoadDialog("Load a sample into " + p.name, false,
            p.sample->path.empty() ? browser.getDir() : std::filesystem::path(p.sample->path).parent_path().string());
        if(r.bSuccess) { loadSample(p, r.getPath()); if(!p.B("Sample On").get()) p.B("Sample On") = true; }
    }
    // hits and meters, while the window is shown
    if(showWindow.get()) {
        const bool read = now >= nextMeterRead;
        if(read) nextMeterRead = now + 1.0f / 30.0f;
        for(auto& up : pads) {
            Pad& p = *up;
            if(p.sc.empty()) continue;
            auto& [srv, sc] = *p.sc.begin();
            if(liveControlBus(srv, sc.meter)) {
                if(!sc.meter->readValues.empty()) p.meter = sc.meter->readValues[0];
                if(read) srv->requestControlValues(sc.meter->index, 1);
            }
            if(liveControlBus(srv, sc.vis)) {
                if(!sc.vis->readValues.empty()) {
                    const float h = sc.vis->readValues[0];
                    if(p.hits >= 0.0f && h != p.hits) p.flash = 1.0f;
                    p.hits = h;
                }
                if(read) srv->requestControlValues(sc.vis->index, 1);
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Snapshots
// ════════════════════════════════════════════════════════════════════════════

ofJson scDrumKitBox::captureSnapshot() const {
    ofJson arr = ofJson::array();
    for(int k = 0; k < (int)pads.size(); k++) arr.push_back(padState(k));
    return {{"pads", arr}};
}

// The arrival's kit at the switch point (pads, samples, every setting);
// before and after it the pads' numbers glide
void scDrumKitBox::applySnapshot(const ofJson& a, const ofJson& b, float t, bool sw) {
    if(!a.is_object() || !b.is_object()) return;
    const ofJson& side = sw ? b : a;
    if(side.contains("pads") && side["pads"].is_array() && !side["pads"].empty() && side["pads"] != appliedKit) {
        appliedKit = side["pads"];
        const ofJson& arr = side["pads"];
        if(!side.value("allPads", false)) {
            const int n = std::clamp((int)arr.size(), 1, MaxPads);
            while((int)pads.size() < n) if(!addPad(M_KICK, false)) break;
            while((int)pads.size() > n) removePad((int)pads.size() - 1);
        }
        for(int k = 0; k < (int)pads.size(); k++) {
            const ofJson& ps = arr[std::min<size_t>(k, arr.size() - 1)];
            applyPadState(k, ps);
            const std::string path = ps.value("path", std::string());
            if(!path.empty() && path != pads[k]->sample->path) loadSample(*pads[k], path);
        }
        for(auto& p : pads) syncPad(*p);
        sendChokeMasks();
        sendMutes();
        sendDucks();
        selected = std::clamp(selected, 0, (int)pads.size() - 1);
    }
    if(t <= 0.0f || t >= 1.0f) return;
    auto controlsOf = [](const ofJson& s, int k) -> const ofJson* {
        if(!s.contains("pads") || !s["pads"].is_array() || s["pads"].empty()) return nullptr;
        const ofJson& ps = s["pads"][std::min<size_t>(k, s["pads"].size() - 1)];
        return ps.contains("controls") ? &ps["controls"] : nullptr;
    };
    for(auto& c : controls) {
        if(!c.f || c.pad < 0 || (published.count(c.key) && published.at(c.key)->hasInConnection())) continue;
        const ofJson* ca = controlsOf(a, c.pad);
        const ofJson* cb = controlsOf(b, c.pad);
        if(!ca || !cb || !ca->contains(c.base) || !cb->contains(c.base)) continue;
        const float va = ofToFloat((*ca)[c.base].get<std::string>()), vb = ofToFloat((*cb)[c.base].get<std::string>());
        const float v = scSnapshots::blendValue(va, vb, t, sw, scSnapshots::logKey(c.base)).get<float>();
        if(std::abs(v - c.f->get()) > 1e-6f) c.f->set(v);
    }
    auto padOf = [](const ofJson& state, int k) -> const ofJson* {
        if(!state.contains("pads") || !state["pads"].is_array() || state["pads"].empty()) return nullptr;
        return &state["pads"][std::min<size_t>(k, state["pads"].size() - 1)];
    };
    for(int k = 0; k < int(pads.size()); ++k) {
        const auto* pa = padOf(a, k); const auto* pb = padOf(b, k);
        if(!pa || !pb) continue;
        auto ma = pa->value("mod", ofJson::object()), mb = pb->value("mod", ofJson::object());
        rewriteModKeys(ma, false, k); rewriteModKeys(mb, false, k);
        const auto* ca = controlsOf(a, k); const auto* cb = controlsOf(b, k);
        for(const auto& c : controls) if(c.pad == k) {
            if(ca && ca->contains(c.base)) ma[c.key] = (*ca)[c.base];
            if(cb && cb->contains(c.base)) mb[c.key] = (*cb)[c.base];
        }
        pads[k]->mod.loadMorph(ma, mb, t, sw, "", [this](const std::string& key) {
            auto it = published.find(key); return it != published.end() && it->second->hasInConnection();
        });
        pads[k]->mod.afterLoad(); pads[k]->mod.refresh();
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Presets
// ════════════════════════════════════════════════════════════════════════════

void scDrumKitBox::presetSave(ofJson& j) {
    for(auto& p : pads) p->mod.saveLfoOutputs(j);
    ofJson arr = ofJson::array();
    for(int k = 0; k < (int)pads.size(); k++) arr.push_back(padState(k));
    j["embedSamples"] = embedSamples.get();
    if(embedSamples.get()) for(auto& pad : arr)
        pad["path"] = scSampleEmbedding::embed(pad.value("path", std::string()));
    j["drumKitBoxPads"] = arr;
    j["drumKitBoxSelected"] = selected;
    j["drumKitBoxBrowseDir"] = browser.getDir();
    j["drumKitBoxPublished"] = publishedKeys;
    timeline.save(j, "drumKitBoxTimeline");
    j["drumKitBoxSnapshots"] = snaps.settingsJson();
    j["drumKitBoxBrowserWidth"] = browserWidth;
}

// The pads (and so their parameters) must exist before Oceanode sets the
// published ones and restores their connections
void scDrumKitBox::loadBeforeConnections(ofJson& j) {
    if(j.contains("drumKitBoxPads") && j["drumKitBoxPads"].is_array()) {
        const int n = std::clamp((int)j["drumKitBoxPads"].size(), 1, MaxPads);
        while((int)pads.size() < n) if(!addPad(M_KICK, false)) break;
        while((int)pads.size() > n) removePad((int)pads.size() - 1);
    }
    for(auto& p : pads) p->mod.loadLfoOutputs(j);
    if(!j.contains("drumKitBoxPublished") || !j["drumKitBoxPublished"].is_array()) return;
    std::vector<std::string> wanted;
    for(const auto& key : j["drumKitBoxPublished"])
        if(key.is_string()) wanted.push_back(key.get<std::string>());
    const auto old = publishedKeys;
    for(const auto& key : old)
        if(std::find(wanted.begin(), wanted.end(), key) == wanted.end()) unpublish(key);
    for(const auto& key : wanted) publish(key);
    timeline.load(j, "drumKitBoxTimeline");
}
void scDrumKitBox::presetRecallBeforeSettingParameters(ofJson& j) { loadBeforeConnections(j); }

void scDrumKitBox::presetRecallAfterSettingParameters(ofJson& j) {
    embedSamples = j.value("embedSamples", true);
    if(j.contains("drumKitBoxPads") && j["drumKitBoxPads"].is_array()) {
        const auto& arr = j["drumKitBoxPads"];
        for(int k = 0; k < (int)pads.size() && k < (int)arr.size(); k++) {
            applyPadState(k, arr[k]);
            const std::string path = scSampleEmbedding::resolve(arr[k].value("path", std::string()));
            if(!path.empty() && path != pads[k]->sample->path) loadSample(*pads[k], path);
        }
    }
    for(auto& p : pads) syncPad(*p);
    sendChokeMasks();
    sendMutes();
    sendDucks();
    if(j.contains("drumKitBoxSelected") && j["drumKitBoxSelected"].is_number_integer())
        selected = std::clamp(j["drumKitBoxSelected"].get<int>(), 0, (int)pads.size() - 1);
    if(j.contains("drumKitBoxBrowserWidth") && j["drumKitBoxBrowserWidth"].is_number())
        browserWidth = j["drumKitBoxBrowserWidth"].get<float>();
    if(j.contains("drumKitBoxBrowseDir") && j["drumKitBoxBrowseDir"].is_string())
        browser.refresh(j["drumKitBoxBrowseDir"].get<std::string>());
    if(j.contains("drumKitBoxSnapshots")) snaps.loadSettings(j["drumKitBoxSnapshots"]);
}

// ════════════════════════════════════════════════════════════════════════════
// Publishing window controls into the node
// ════════════════════════════════════════════════════════════════════════════

void scDrumKitBox::publish(const std::string& key) {
    if(published.count(key)) return;
    Control* c = findControl(key);
    if(!c || getParameterGroup().contains(key)) return;
    if(!publishedSeparator) { addSeparator("Published", ofColor(200)); publishedSeparator = true; }
    published[key] = c->add();
    dropdowns.apply(key, published[key]);
    publishedKeys.push_back(key);
    parameterGroupChanged.notify(this);
}
void scDrumKitBox::unpublish(const std::string& key, bool force) {
    auto it = published.find(key);
    if(it == published.end()) return;
    if(isConnected(key) && !force) return;
    removeParameter(it->second->getName());
    published.erase(it);
    publishedKeys.erase(std::remove(publishedKeys.begin(), publishedKeys.end(), key), publishedKeys.end());
    if(published.empty() && publishedSeparator) { removeSeparator("Published"); publishedSeparator = false; }
    parameterGroupChanged.notify(this);
}
bool scDrumKitBox::isConnected(const std::string& key) const {
    auto it = published.find(key);
    return it != published.end() && it->second &&
        (it->second->hasInConnection() || it->second->hasOutConnections());
}
