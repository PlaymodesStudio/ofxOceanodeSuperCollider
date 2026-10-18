//
//  scSampleBox.cpp
//  ofxOceanodeSuperCollider
//

#include "scSampleBox.h"
#include "scTypedValue.h"
#include "scEnvelopeCurve.h"
#include "ofxOceanodeShared.h"
#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxSuperCollider.h"
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
}


std::string formatTime(float seconds) {
    char s[32];
    if(seconds < 60.0f) std::snprintf(s, sizeof(s), "%.3f s", seconds);
    else std::snprintf(s, sizeof(s), "%d:%06.3f", (int)(seconds / 60.0f), std::fmod(seconds, 60.0f));
    return s;
}
}

// ════════════════════════════════════════════════════════════════════════════
// Setup
// ════════════════════════════════════════════════════════════════════════════

scSampleBox::scSampleBox(std::vector<serverManager*> s)
    : scNode("SampleBox"), servers(std::move(s)) {
    color = ofColor(103, 178, 217);
    description = "Sampler for one audio file: trigger or gate it (Oceanode values or an audio "
                  "signal), one-shot / loop / gated, resample or time stretch, reverse, up to 8 "
                  "voices, envelopes and LFOs routed to its parameters, filter, EQ, compressor, saturator "
                  "and reverb. Show opens the editor; right-click a control there to publish it.";
    for(auto* sm : servers) if(sm && sm->getServer()) { browser.setPreviewServer(sm->getServer()); break; }
    std::string dir = ofToDataPath("Supercollider/Samples", true);
    if(!std::filesystem::exists(dir)) dir = ofToDataPath("", true);
    browser.refresh(dir);
    browser.onActivate = [this](const std::string& path) { loadSample(path); browser.stopPreview(); };
}

scSampleBox::~scSampleBox() {
    nodeListeners.unsubscribeAll();
    browser.stopPreview();
    if(decodeTask.valid()) decodeTask.wait();
    for(auto& [s, synth] : synths) if(synth) { synth->free(); delete synth; }
    synths.clear();
    std::vector<ofxSCServer*> grouped;
    for(auto& [srv, g] : groups) grouped.push_back(srv);
    for(auto* srv : grouped) freeGroups(srv);
    while(!buffers.empty()) releaseBuffers(buffers.begin()->first);
    for(auto& [srv, bus] : visBuses) if(bus) { if(liveControlBus(srv, bus)) bus->free(); delete bus; }
    visBuses.clear();
}

void scSampleBox::setup() {
#define F(p, label, def, lo, hi, sc) p.set(label, def, lo, hi); registerControl(p, sc)
#define I(p, label, def, lo, hi, sc) p.set(label, def, lo, hi); registerControl(p, sc)
#define B(p, label, def, sc) p.set(label, def); registerControl(p, sc)
    addSeparator("SampleBox");
    addParameter(showWindow.set("Show", false));
    F(trigger, "Trigger/Gate", 0.0f, 0.0f, 1.0f, "gate"); addParameter(trigger);
    F(pitch, "Pitch", 60.0f, 0.0f, 127.0f, "pitch");
    pitchIn.attach(addParameter(pitch), availableInputs, [this](){ for(auto& o : outputs) o = o; });
    F(velocity, "Velocity", 1.0f, 0.0f, 1.0f, "velocity");
    velIn.attach(addParameter(velocity), availableInputs, [this](){ for(auto& o : outputs) o = o; });
    F(gain, "Gain", 0.7f, 0.0f, 2.0f, "gain"); addParameter(gain);
    I(channels, "N Chan", 2, 1, MaxChannels, "nchan"); addParameter(channels);
    addInput("Trig/Gate Audio");
    addOutput("Out");

    F(inPoint, "In Point", 0.0f, 0.0f, 1.0f, "inpoint");
    F(outPoint, "Out Point", 1.0f, 0.0f, 1.0f, "outpoint");
    F(loopCrossfade, "Loop Crossfade %", 0.0f, 0.0f, 50.0f, "loopcrossfade");
    F(rate, "Rate", 1.0f, 0.125f, 8.0f, "rate");
    F(transpose, "Transpose", 0.0f, -24.0f, 24.0f, "transpose");
    F(stretch, "Stretch", 1.0f, 0.125f, 8.0f, "stretch");
    F(warpWindow, "Warp Window", 0.15f, 0.01f, 1.0f, "warpwindow");
    B(loop, "Loop", false, "loop");
    B(poly, "Polyphonic", true, "poly");
    B(gateMode, "Gate Mode", false, "gatemode");
    B(reverse, "Reverse", false, "reverse");
    I(voices, "Max Voices", 8, 1, MaxVoices, "voices");
    I(stretchMode, "Stretch Mode", 0, 0, 1, "stretchmode");
    F(attack, "Amp Attack", 0.005f, 0.0f, 5.0f, "attack");
    F(release, "Amp Release", 0.05f, 0.001f, 5.0f, "release");
    F(pan, "Pan", 0.0f, -1.0f, 1.0f, "pan");
    B(filterOn, "Filter On", false, "filteron");
    I(filterType, "Filter Type", 0, 0, 2, "filtertype");
    F(cutoff, "Cutoff", 16000.0f, 20.0f, 20000.0f, "cutoff");
    F(resonance, "Resonance", 0.7f, 0.1f, 2.0f, "resonance");
    B(eqOn, "EQ On", false, "eqon");
    const char* eqNames[5] = {"Low", "Lo Mid", "Mid", "Hi Mid", "High"};
    const char* eqSC[5] = {"eq1", "eq2", "eq3", "eq4", "eq5"};
    const float eqDefaults[5] = {80, 250, 1000, 4000, 12000};
    for(int n = 0; n < 5; ++n) {
        const std::string prefix = std::string("EQ ") + eqNames[n];
        eqGain[n].set(prefix, 0.0f, -24.0f, 24.0f); registerControl(eqGain[n], eqSC[n]);
        eqFreq[n].set(prefix + " Freq", eqDefaults[n], 20.0f, 20000.0f);
        registerControl(eqFreq[n], (std::string(eqSC[n]) + "freq").c_str());
        eqShape[n].set(prefix + (scEQEditor::isShelf(n) ? " Slope" : " Q"), 1.0f,
                       0.1f, scEQEditor::isShelf(n) ? 4.0f : 20.0f);
        registerControl(eqShape[n], (std::string(eqSC[n]) + "shape").c_str());
    }
    F(eqMix, "EQ Mix", 1.0f, 0.0f, 1.0f, "eqmix");
    B(compOn, "Compressor On", false, "compon");
    F(compThreshold, "Comp Threshold", -18.0f, -60.0f, 0.0f, "compthreshold");
    F(compRatio, "Comp Ratio", 3.0f, 1.0f, 20.0f, "compratio");
    F(compAttack, "Comp Attack", 10.0f, 0.1f, 200.0f, "compattack");
    F(compRelease, "Comp Release", 150.0f, 1.0f, 2000.0f, "comprelease");
    F(compMakeup, "Comp Makeup", 0.0f, -12.0f, 24.0f, "compmakeup");
    B(satOn, "Saturator On", false, "saton");
    F(satDrive, "Sat Drive", 6.0f, 0.0f, 36.0f, "satdrive");
    F(satMix, "Sat Mix", 1.0f, 0.0f, 1.0f, "satmix");
    B(reverbOn, "Reverb On", false, "reverbon");
    F(revSize, "Rev Size", 30.0f, 0.0f, 60.0f, "revsize");
    F(revDecay, "Rev Decay", 4.0f, 0.05f, 60.0f, "revdecay");
    F(revPredelay, "Rev Predelay", 20.0f, 0.0f, 1000.0f, "revpredelay");
    F(revLowpass, "Rev Lowpass", 10000.0f, 20.0f, 20000.0f, "revlowpass");
    F(revPosition, "Rev Position", 0.8f, 0.0f, 1.0f, "revposition");
    F(revSpread, "Rev Spread", 0.15f, -1.0f, 1.0f, "revspread");
    F(revMix, "Rev Mix", 0.33f, 0.0f, 1.0f, "revmix");
    F(width, "Width", 1.0f, 0.0f, 2.0f, "width");
    F(diffuse, "Diffuse", 0.0f, 0.0f, 1.0f, "diffuse");
    F(bassMono, "Bass Mono", 0.0f, 0.0f, 1.0f, "bassmono");
    F(bassFreq, "Bass Freq", 150.0f, 20.0f, 1000.0f, "bassfreq");

    // ── The shared modulation: envelopes, LFOs, routing to these targets ─────
    {
        scModulation::Host h;
        h.regF = [this](ofParameter<float>& p, std::function<void()> f){ regMod(p, f); };
        h.control = [this](const std::string& k) -> ofAbstractParameter* {
            for(auto& c : controls) if(c.key == k) return c.f ? (ofAbstractParameter*)c.f : (ofAbstractParameter*)c.i;
            return nullptr;
        };
        h.regI = [this](ofParameter<int>& p, std::function<void()> f){ regMod(p, f); };
        h.regB = [this](ofParameter<bool>& p, std::function<void()> f){ regMod(p, f); };
        h.regV = [this](ofParameter<std::vector<float>>& p, std::function<void()> f){ regMod(p, f); };
        h.publishMenu = [this](const std::string& k){ drawPublishMenu(k); };
        h.publishedUnderline = [this](const std::string& k){ drawPublishedUnderline(k); };
        h.unpublish = [this](const std::string& k){ unpublish(k); };
        h.publishedKeys = [this]() -> const std::vector<std::string>& { return publishedKeys; };
        h.voices = [](){ return 8; };               // the synth's 8 voices
        // the synth reads these, in this order (samplebox.scd)
        mod.setup(h, {{"Pitch", 24, "st"}, {"Rate", 4, "x"}, {"In", 1, "", true}, {"Out", 1, "", true}, {"Gain", 1, ""},
                      {"Pan", 1, ""}, {"Stretch", 4, "x"}, {"Grain", 0.5f, "s"}, {"Loop XF", 50, "%"},
                      {"Cutoff", 8, "oct"}, {"Reso", 1, ""}, {"Drive", 24, "dB"}, {"Comp Thr", 24, "dB"},
                      {"Rev Mix", 1, ""}, {"Width", 1, ""}}, true, true, true);
        mod.targetOfKey = {{pitch.getEscapedName(), 0}, {rate.getEscapedName(), 1}, {inPoint.getEscapedName(), 2},
                           {outPoint.getEscapedName(), 3}, {gain.getEscapedName(), 4}, {pan.getEscapedName(), 5},
                           {stretch.getEscapedName(), 6}, {warpWindow.getEscapedName(), 7}, {loopCrossfade.getEscapedName(), 8},
                           {cutoff.getEscapedName(), 9}, {resonance.getEscapedName(), 10}, {satDrive.getEscapedName(), 11},
                           {compThreshold.getEscapedName(), 12}, {revMix.getEscapedName(), 13}, {width.getEscapedName(), 14}};
    }
#undef F
#undef I
#undef B
    // Oceanode trigger pulses can be shorter than one SC control block. Send
    // an incrementing counter on each rising edge so those pulses survive.
    nodeListeners.push(trigger.newListener([this, last = false](float& v) mutable {
        const bool high = v > 0.0f;   // a gate's level (0.2 plays at 0.2)
        if(high && !last) fire();
        last = high;
    }));
    nodeListeners.push(channels.newListener([this](int&) { if(!restoring) replaceAllSynths(); }));
    nodeListeners.push(stretchMode.newListener([this](int&) { if(!restoring) replaceAllSynths(); }));
    // Files dropped from the Finder on the window
    dragListener = ofEvents().fileDragEvent.newListener([this](ofDragInfo& info) {
        if(!showWindow.get() || !windowHovered || info.files.empty()) return;
        pendingFile = info.files.front().string();
    });
    requireSynthdefs();
}

void scSampleBox::requireSynthdefs() {
    std::filesystem::path folder;
    const std::string expected = "SampleBox_1_2_R.scsyndef";
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
    for(auto* sm : servers) if(sm) sm->requireSynthdefFolder(folder.string());
}

std::string scSampleBox::synthName() const {
    return "SampleBox_" + ofToString(sampleChannels) + "_" + ofToString(channels.get())
        + (stretchMode.get() ? "_T" : "_R");
}

// ════════════════════════════════════════════════════════════════════════════
// The sample
// ════════════════════════════════════════════════════════════════════════════

void scSampleBox::loadSample(const std::string& path) {
    if(path.empty()) return;
    pendingFile = path;     // decoded in update()
}

void scSampleBox::applyDecoded(scSlicerBox::Audio&& a) {
    if(!a.error.empty()) {
        loadError = std::filesystem::path(a.path).filename().string() + ": " + a.error;
        ofLogWarning("scSampleBox") << loadError;
        return;
    }
    loadError.clear();
    const int oldChannels = sampleChannels;
    samplePath = a.path;
    playPath = a.playPath;
    sampleChannels = std::max(1, std::min(2, a.channels));
    sampleRate = a.sampleRate;
    sampleSeconds = a.frames / std::max(1.0f, a.sampleRate);
    waveformPeaks.assign(WaveformBins, 0.0f);
    for(int f = 0; f < a.frames; ++f) {
        const int bin = (int)((int64_t)f * WaveformBins / a.frames);
        waveformPeaks[bin] = std::max(waveformPeaks[bin], std::min(1.0f, std::abs(a.mono[f])));
    }
    for(auto& [srv, synth] : synths) {
        if(!synth) continue;
        // the new buffers first, the old ones freed once the synth reads the new
        auto old = std::move(buffers[srv]);
        buffers.erase(srv);
        ensureBuffers(srv);
        if(oldChannels != sampleChannels) replaceSynth(srv);
        else sendAll(srv, synth);
        for(auto* b : old) if(b) { if(liveBuffer(srv, b)) b->free(); delete b; }
    }
}

void scSampleBox::releaseBuffers(ofxSCServer* srv) {
    auto it = buffers.find(srv);
    if(it == buffers.end()) return;
    for(auto* b : it->second) if(b) { if(liveBuffer(srv, b)) b->free(); delete b; }
    buffers.erase(it);
}

void scSampleBox::ensureBuffers(ofxSCServer* srv) {
    if(!srv || playPath.empty()) return;
    auto it = buffers.find(srv);
    if(it != buffers.end() && (int)it->second.size() == sampleChannels &&
       std::all_of(it->second.begin(), it->second.end(), [srv](ofxSCBuffer* b){ return liveBuffer(srv, b); })) return;
    releaseBuffers(srv);
    for(int ch = 0; ch < sampleChannels; ++ch) {
        auto* b = new ofxSCBuffer(0, 0, srv);
        if(b->index < 0) { delete b; releaseBuffers(srv); return; }   // no free buffer
        b->readChannel(playPath, {ch});
        buffers[srv].push_back(b);
    }
}

void scSampleBox::ensureVisBus(ofxSCServer* srv) {
    auto& bus = visBuses[srv];
    if(liveControlBus(srv, bus)) return;
    delete bus;     // after a reboot its index belongs to nobody
    bus = new ofxSCBus(RATE_CONTROL, MaxVoices, srv);
    if(!liveControlBus(srv, bus)) { bus->free(); delete bus; bus = nullptr; }
}

// ════════════════════════════════════════════════════════════════════════════
// Synths
// ════════════════════════════════════════════════════════════════════════════

void scSampleBox::sendAll(ofxSCServer* srv, ofxSCSynth* s) {
    if(!s) return;
    for(const auto& c : controls) {
        if(c.f) s->set(c.scName, c.f->get());
        else if(c.i) s->set(c.scName, c.i->get());
        else if(c.b) s->set(c.scName, int(c.b->get()));
    }
    const auto bs = buffers.find(srv);
    const bool loaded = bs != buffers.end() && (int)bs->second.size() >= sampleChannels;
    s->set("loaded", int(loaded));
    s->set("buf0", loaded ? bs->second[0]->index : 0);
    s->set("buf1", loaded && sampleChannels > 1 ? bs->second[1]->index : 0);
    s->set("out", outputBuses.count(srv) ? outputBuses[srv] : 0);
    s->set("trigbus", inputBuses.count(srv) ? inputBuses[srv] : -1);
    applyCables(srv, s);
    const auto vb = visBuses.find(srv);
    if(vb != visBuses.end() && vb->second) s->set("vis", vb->second->index);
    s->set("fire", fireCount);
    s->set("stop", stopCount);
    // the shared modulation: the targets it writes, the gates / velocities / randoms it reads
    s->set("modbus", mod.targetBus(srv, 0));
    s->set("velbus", mod.sourceBus(srv, scModulation::SRC_VEL));
    s->set("rndbus", mod.sourceBus(srv, scModulation::SRC_RND));
    auto g = groups.find(srv);
    if(g != groups.end() && g->second.gateBus) s->set("gatebus", g->second.gateBus->index);
}
void scSampleBox::sendAll() { for(auto& [srv, s] : synths) sendAll(srv, s); }
void scSampleBox::resendParametersForNRT() { sendAll(); mod.resend(); }

void scSampleBox::fire() {
    ++fireCount;
    for(auto& [srv, s] : synths) if(s) s->set("fire", fireCount);
}
void scSampleBox::stop() {
    ++stopCount;
    for(auto& [srv, s] : synths) if(s) s->set("stop", stopCount);
}

// A new definition (sample channels, N Chan, Resample / Stretch) takes the old
// synth's place in the graph
void scSampleBox::replaceSynth(ofxSCServer* srv) {
    auto it = synths.find(srv);
    if(it == synths.end() || !it->second) return;
    ofxSCSynth* old = it->second;
    auto* fresh = new ofxSCSynth(synthName(), srv);
    sendAll(srv, fresh);
    fresh->createAndRun(4, old->nodeID, getActive());   // replace
    delete old;
    it->second = fresh;
}
void scSampleBox::replaceAllSynths() {
    std::vector<ofxSCServer*> active;
    for(auto& [srv, s] : synths) if(s) active.push_back(srv);
    for(auto* srv : active) replaceSynth(srv);
}

void scSampleBox::freeGroups(ofxSCServer* srv) {
    auto it = groups.find(srv);
    if(it == groups.end()) return;
    auto& g = it->second;
    mod.free(srv, false);                       // the group's /n_free takes its synths
    if(g.root && g.created) g.root->free();
    delete g.voices; delete g.sources; delete g.routes; delete g.root;
    if(g.gateBus && liveAudioBus(srv, g.gateBus)) g.gateBus->free();
    delete g.gateBus;
    groups.erase(it);
}

void scSampleBox::buildSynth(ofxSCServer* srv) {
    if(!srv) return;
    ensureBuffers(srv);
    ensureVisBus(srv);
    if(synths.count(srv) && synths[srv]) { synths[srv]->free(); delete synths[srv]; }
    synths[srv] = new ofxSCSynth(synthName(), srv);
    freeGroups(srv);
    auto& g = groups[srv];
    g.root = new ofxSCGroup(srv);
}
void scSampleBox::createSynth(ofxSCServer* srv) {
    auto it = synths.find(srv);
    if(it == synths.end() || !it->second) return;
    auto& g = groups[srv];
    if(!g.root) g.root = new ofxSCGroup(srv);
    g.root->create(0, 1);
    g.voices = new ofxSCGroup(srv);  g.voices->create(1, g.root->nodeID);
    g.sources = new ofxSCGroup(srv); g.sources->create(1, g.root->nodeID);
    g.routes = new ofxSCGroup(srv);  g.routes->create(1, g.root->nodeID);
    g.created = true;
    if(!g.gateBus || !liveAudioBus(srv, g.gateBus)){
        delete g.gateBus;
        g.gateBus = new ofxSCBus(RATE_AUDIO, 8, srv);
        if(!liveAudioBus(srv, g.gateBus)){ g.gateBus->free(); delete g.gateBus; g.gateBus = nullptr; }
    }
    mod.create(srv, g.sources->nodeID, g.routes->nodeID, g.gateBus ? g.gateBus->index : 0);
    sendAll(srv, it->second);
    it->second->createAndRun(0, g.voices->nodeID, true);
    if(!getActive()) g.root->run(false);
}
void scSampleBox::free(ofxSCServer* srv) {
    auto it = synths.find(srv);
    if(it != synths.end()) { delete it->second; synths.erase(it); }   // freed with the group
    freeGroups(srv);
}
void scSampleBox::setOutputBus(ofxSCServer* srv, int, int bus) {
    outputBuses[srv] = bus;
    if(synths.count(srv) && synths[srv]) synths[srv]->set("out", bus);
}
int scSampleBox::getOutputBusIndex(ofxSCServer* srv, int) {
    return outputBuses.count(srv) ? outputBuses[srv] : -1;
}
void scSampleBox::moveSynthBefore(ofxSCServer* srv, int nodeID) {
    auto it = groups.find(srv);
    if(it != groups.end() && it->second.created) it->second.root->moveBefore(nodeID);
}
int scSampleBox::getLastSynthID(ofxSCServer* srv) {
    auto it = groups.find(srv);
    return (it != groups.end() && it->second.created) ? it->second.root->nodeID : -1;
}
void scSampleBox::setInputBus(ofxSCServer* srv, scNode* node, int bus) {
    // a Pitch / Velocity cable (told apart by its source's bus), else the trigger / gate input
    if(pitchIn.take(srv, node, bus) | velIn.take(srv, node, bus)){
        if(synths.count(srv) && synths[srv]) applyCables(srv, synths[srv]);
        return;
    }
    inputBuses[srv] = bus;
    if(synths.count(srv) && synths[srv]) synths[srv]->set("trigbus", bus);
}
void scSampleBox::resetInputBusses(ofxSCServer* srv, int) {
    inputBuses.erase(srv);
    pitchIn.reset(srv); velIn.reset(srv);
    if(synths.count(srv) && synths[srv]){
        synths[srv]->set("trigbus", -1);
        applyCables(srv, synths[srv]);
    }
}
void scSampleBox::applyCables(ofxSCServer* srv, ofxSCSynth* s) {
    if(!s) return;
    const int pb = pitchIn.bus(srv), vb = velIn.bus(srv);
    if(pb >= 0) s->mapan("pitch", pb, 1); else s->set("pitch", pitch.get());
    if(vb >= 0) s->mapan("velocity", vb, 1); else s->set("velocity", velocity.get());
}

void scSampleBox::update(ofEventArgs&) {
    mod.showing = showWindow.get();
    mod.update();
    if(pendingLoadDialog) {
        pendingLoadDialog = false;
        auto r = ofSystemLoadDialog("Load a sample", false,
            samplePath.empty() ? browser.getDir() : std::filesystem::path(samplePath).parent_path().string());
        if(r.bSuccess) pendingFile = r.getPath();
    }
    if(!pendingFile.empty() && !decodeTask.valid()) {
        decodingPath = pendingFile;
        pendingFile.clear();
        decodeTask = std::async(std::launch::async, &scSlicerBox::decodeFile, decodingPath,
                                ofToDataPath("Supercollider/tmp/SampleBox", true), false);
    }
    if(decodeTask.valid() && decodeTask.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        applyDecoded(decodeTask.get());
        decodingPath.clear();
    }
    // After a server reboot: new buffers and playhead bus (the graph rebuilds the synth)
    for(auto& [srv, synth] : synths) {
        if(!synth || playPath.empty()) continue;
        auto it = buffers.find(srv);
        const bool live = it != buffers.end() && !it->second.empty() &&
            std::all_of(it->second.begin(), it->second.end(), [srv = srv](ofxSCBuffer* b){ return liveBuffer(srv, b); });
        if(!live) {
            for(auto* b : it != buffers.end() ? it->second : std::vector<ofxSCBuffer*>{}) delete b;
            buffers.erase(srv);
            ensureBuffers(srv);
            ensureVisBus(srv);
            sendAll(srv, synth);
        }
    }
    // Playheads, only while the window is shown
    playheads.clear();
    if(showWindow.get()) {
        for(auto& [srv, synth] : synths) {
            auto vb = visBuses.find(srv);
            if(!synth || vb == visBuses.end() || !liveControlBus(srv, vb->second)) continue;
            playheads = vb->second->readValues;
            const float now = ofGetElapsedTimef();
            if(now >= nextPlayheadRead){
                nextPlayheadRead = now + 1.0f / 30.0f;
                srv->requestControlValues(vb->second->index, std::min(voices.get(), MaxVoices));
            }
            break;
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Presets
// ════════════════════════════════════════════════════════════════════════════

void scSampleBox::presetSave(ofJson& j) {
    j["sampleBoxPath"] = samplePath;
    j["sampleBoxBrowseDir"] = browser.getDir();
    mod.save(j, "sampleBoxMod");
    j["sampleBoxPublished"] = publishedKeys;
    j["sampleBoxBrowserWidth"] = browserWidth;
    j["sampleBoxWaveformHeight"] = waveformHeight / std::max(0.01f, ofxOceanodeShared::getZoomLevel());
    auto& state = j["sampleBoxControls"] = ofJson::object();
    for(const auto& c : controls) state[c.key] = c.save();
    // a modulated control: its own value (stored as text here)
    ofJson bases = ofJson::object();
    for(const auto& c : controls) if(mod.isParamTarget(c.key)) bases[c.key] = 0.0f;
    mod.patchBases(bases);
    for(auto it = bases.begin(); it != bases.end(); ++it)
        if(it.value().is_number_integer()) state[it.key()] = ofToString(it.value().get<int>());
        else if(it.value().is_number()) state[it.key()] = ofToString(it.value().get<float>());
    mod.patchBases(j);
}
void scSampleBox::loadBeforeConnections(ofJson& j) {
    if(!j.contains("sampleBoxPublished") || !j["sampleBoxPublished"].is_array()) return;
    std::vector<std::string> wanted;
    for(const auto& key : j["sampleBoxPublished"])
        if(key.is_string()) wanted.push_back(key.get<std::string>());
    const auto old = publishedKeys;
    for(const auto& key : old)
        if(std::find(wanted.begin(), wanted.end(), key) == wanted.end()) unpublish(key);
    for(const auto& key : wanted) publish(key);
}
void scSampleBox::presetRecallAfterSettingParameters(ofJson& j) {
    const std::string previousDef = synthName();
    restoring = true;
    if(j.contains("sampleBoxControls") && j["sampleBoxControls"].is_object())
        for(const auto& c : controls) if(j["sampleBoxControls"].contains(c.key))
            try { c.load(j["sampleBoxControls"][c.key].get<std::string>()); } catch(...) {}
    restoring = false;
    if(previousDef != synthName()) replaceAllSynths();
    mod.load(j, "sampleBoxMod");
    mod.afterLoad();
    if(j.contains("sampleBoxBrowserWidth") && j["sampleBoxBrowserWidth"].is_number())
        browserWidth = j["sampleBoxBrowserWidth"].get<float>();
    if(j.contains("sampleBoxWaveformHeight") && j["sampleBoxWaveformHeight"].is_number())
        waveformHeight = std::max(0.0f, j["sampleBoxWaveformHeight"].get<float>()) * ofxOceanodeShared::getZoomLevel();
    if(j.contains("sampleBoxBrowseDir") && j["sampleBoxBrowseDir"].is_string())
        browser.refresh(j["sampleBoxBrowseDir"].get<std::string>());
    if(j.contains("sampleBoxPath") && j["sampleBoxPath"].is_string()) {
        const std::string path = j["sampleBoxPath"].get<std::string>();
        if(!path.empty() && path != samplePath) loadSample(path);
    }
    sendAll();
}

// ════════════════════════════════════════════════════════════════════════════
// Publishing window controls into the node
// ════════════════════════════════════════════════════════════════════════════

void scSampleBox::publish(const std::string& key) {
    if(published.count(key)) return;
    auto it = std::find_if(controls.begin(), controls.end(), [&](const Control& c){ return c.key == key; });
    if(it == controls.end() || getParameterGroup().contains(key)) return;
    if(!publishedSeparator) { addSeparator("Published", ofColor(200)); publishedSeparator = true; }
    published[key] = it->add();
    publishedKeys.push_back(key);
    parameterGroupChanged.notify(this);
}
void scSampleBox::unpublish(const std::string& key) {
    auto it = published.find(key);
    if(it == published.end()) return;
    if(isConnected(key)) return;
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
    auto it = std::find_if(controls.begin(), controls.end(), [&](const Control& c){ return c.key == key; });
    if(it != controls.end() && it->save && it->load)
        scTyped::field(key, it->save, [&](const std::string& t){ it->load(ofTrim(t)); }, false);
    if(getParameterGroup().contains(key) && !published.count(key)) ImGui::TextDisabled("Always on the node");
    else if(published.count(key)) {
        if(isConnected(key)) ImGui::TextDisabled("Disconnect it to unpublish");
        else if(ImGui::MenuItem("Unpublish from Node")) unpublish(key);
    } else if(ImGui::MenuItem("Publish to Node")) publish(key);
    mod.modulateMenu(key);
    ImGui::EndPopup();
}
void scSampleBox::drawPublishedUnderline(const std::string& key) const {
    if(!published.count(key)) return;
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

void scSampleBox::sliderInt(ofParameter<int>& p, const char* label, float w) {
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
    if(ImGui::SliderInt("##v", &v, p.getMin(), p.getMax())) p.set(v);
    drawPublishMenu(key);
    ImGui::PopID();
}

void scSampleBox::combo(ofParameter<int>& p, const char* label, float w, const std::vector<const char*>& items) {
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
    int v = std::max(0, std::min((int)items.size() - 1, p.get()));
    if(ImGui::Combo("##v", &v, items.data(), (int)items.size())) p.set(v);
    drawPublishMenu(key);
    ImGui::PopID();
}

void scSampleBox::toggle(ofParameter<bool>& p, const char* label) {
    bool v = p.get();
    if(ImGui::Checkbox(label, &v)) p.set(v);
    drawPublishedUnderline(p.getEscapedName());
    drawPublishMenu(p.getEscapedName());
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
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s on / off", title);
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
}

void scSampleBox::drawWindow() {
    const float z = ofxOceanodeShared::getZoomLevel();
    ImGui::SetNextWindowSize(ImVec2(1040 * z, 700 * z), ImGuiCond_FirstUseEver);
    bool open = showWindow.get();
    const std::string title = "SampleBox " + ofToString(getNumIdentifier());
    if(!ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        windowHovered = false;
        browser.stopPreview();
        ImGui::End();
        if(!open) showWindow = false;
        return;
    }
    if(!open) { showWindow = false; windowHovered = false; browser.stopPreview(); ImGui::End(); return; }
    windowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                           ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const float totalW = ImGui::GetContentRegionAvail().x;
    const float totalH = ImGui::GetContentRegionAvail().y;
    if(totalW < 200.0f * z || totalH < 80.0f * z) { browser.stopPreview(); ImGui::End(); return; }

    // ── Left: the browser, and its splitter ─────────────────────────────────
    const float dragW = 5.0f * z;
    float bw = std::min(browserWidth * z, totalW * 0.45f);
    ImGui::BeginChild("##sbBrowser", ImVec2(bw, totalH), false);
    browser.draw(bw, totalH);
    ImGui::EndChild();
    ImGui::SameLine(0, 0);
    scSampleBrowser::drawSplitter("##sbSplit", browserWidth, dragW, totalH,
        100.0f, std::max(100.0f, totalW * 0.45f / z), 1.0f / z);
    ImGui::SameLine(0, 0);

    // ── Right: header, waveform, transport, then the tabs ───────────────────
    ImGui::BeginChild("##sbRight", ImVec2(0, totalH), false);
    const float rightW = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize - 4.0f * z;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f * z);
    ImGui::BeginGroup();
    const float w = rightW - 6.0f * z;
    drawHeader(w);
    drawWaveform(w);
    drawTransport(w);
    ImGui::Spacing();
    if(ImGui::BeginTabBar("##sbTabs")) {
        if(ImGui::BeginTabItem("Playback")) { ImGui::Spacing(); drawPlaybackTab(w); ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("Envelopes")) { ImGui::Spacing(); mod.drawEnvelopes(w); ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("LFOs")) { ImGui::Spacing(); mod.drawLfos(w); ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("Routing")) { ImGui::Spacing(); mod.drawRouting(w); ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("Filter & EQ")) { ImGui::Spacing(); drawFilterEqTab(w); ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("Dynamics")) { ImGui::Spacing(); drawDynamicsTab(w); ImGui::EndTabItem(); }
        if(ImGui::BeginTabItem("Space")) { ImGui::Spacing(); drawSpaceTab(w); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndGroup();
    ImGui::EndChild();
    ImGui::End();
}

// File name and format, Load..., the node's Gain on the right
void scSampleBox::drawHeader(float w) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const float x0 = ImGui::GetCursorPosX();
    if(ImGui::Button("Load...")) pendingLoadDialog = true;
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Open an audio file (WAV, AIFF, FLAC, MP3, M4A, CAF...).\n"
                                                 "Or drop one from the browser or the Finder on the waveform.");
    ImGui::SameLine(0, 10.0f * z);
    ImGui::AlignTextToFramePadding();
    if(!decodingPath.empty() || !pendingFile.empty()) {
        const std::string& p = decodingPath.empty() ? pendingFile : decodingPath;
        ImGui::TextColored(ImColor(pal::textDim), "Loading %s...", std::filesystem::path(p).filename().string().c_str());
    } else if(samplePath.empty()) {
        ImGui::TextColored(ImColor(pal::textDim), "No sample loaded");
    } else {
        ImGui::TextUnformatted(std::filesystem::path(samplePath).filename().string().c_str());
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", samplePath.c_str());
        ImGui::SameLine(0, 10.0f * z);
        ImGui::TextColored(ImColor(pal::textDim), "%s  |  %s  |  %.1f kHz", formatTime(sampleSeconds).c_str(),
                           sampleChannels == 1 ? "mono" : "stereo", sampleRate / 1000.0f);
    }
    const float gainW = 210.0f * z;
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x0 + w - gainW));
    const float keep = labelW;
    labelW = 38.0f;
    slider(gain, "Gain", gainW, "%.2f");
    labelW = keep;
    if(!loadError.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", loadError.c_str());
}

void scSampleBox::drawWaveform(float w) {
    const float z = ofxOceanodeShared::getZoomLevel();
    if(waveformHeight <= 0.0f) waveformHeight = 190.0f * z;
    const float h = waveformHeight;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##sbWave", ImVec2(std::max(1.0f, w), std::max(1.0f, h)));
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    const ImVec2 b(a.x + w, a.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(a, b, true);
    dl->AddRectFilled(a, b, pal::canvas);
    const float midY = a.y + h * 0.5f;
    const float in = inPoint.get(), out = outPoint.get();
    const float xi = a.x + w * in, xo = a.x + w * out;

    if(waveformPeaks.empty()) {
        const char* msg = "Drop a sample here (browser or Finder), double-click one in the browser, or Load...";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(a.x + (w - ts.x) * 0.5f, midY - ts.y * 0.5f), pal::textDim, msg);
    } else {
        // time grid, a line at every step that leaves room for its label
        static const float steps[] = {0.01f, 0.02f, 0.05f, 0.1f, 0.2f, 0.5f, 1, 2, 5, 10, 15, 30, 60, 120, 300};
        float step = steps[0];
        for(float s : steps) { step = s; if(sampleSeconds > 0 && w * s / sampleSeconds >= 70.0f * z) break; }
        for(float t = step; sampleSeconds > 0 && t < sampleSeconds; t += step) {
            const float x = a.x + w * t / sampleSeconds;
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
            const int b0 = (int)((int64_t)i * WaveformBins / pixels);
            const int b1 = std::max(b0 + 1, (int)((int64_t)(i + 1) * WaveformBins / pixels));
            float peak = 0;
            for(int k = b0; k < b1 && k < WaveformBins; ++k) peak = std::max(peak, waveformPeaks[k]);
            if(peak <= 0.0f) continue;
            const float x = a.x + i + 0.5f, dy = std::max(0.5f, peak * h * 0.46f);
            const bool inside = x >= xi && x <= xo;
            dl->AddLine(ImVec2(x, midY - dy), ImVec2(x, midY + dy), pal::accentCol(inside ? 170 : 45));
        }
        // outside the region: dimmed
        dl->AddRectFilled(a, ImVec2(xi, b.y), pal::outside);
        dl->AddRectFilled(ImVec2(xo, a.y), b, pal::outside);
        // the loop's crossfade (the seam's end of the region)
        if(loop.get() && loopCrossfade.get() > 0.0f) {
            const float xf = (xo - xi) * loopCrossfade.get() * 0.01f;
            const float x0 = reverse.get() ? xi : xo - xf, x1 = reverse.get() ? xi + xf : xo;
            dl->AddRectFilledMultiColor(ImVec2(x0, a.y), ImVec2(x1, b.y),
                reverse.get() ? pal::accentCol(60) : pal::accentCol(0), reverse.get() ? pal::accentCol(0) : pal::accentCol(60),
                reverse.get() ? pal::accentCol(0) : pal::accentCol(60), reverse.get() ? pal::accentCol(60) : pal::accentCol(0));
        }
        // In / Out: lines, flags and times
        const float flag = 7.0f * z;
        dl->AddLine(ImVec2(xi, a.y), ImVec2(xi, b.y), pal::handle, 2.0f * z);
        dl->AddLine(ImVec2(xo, a.y), ImVec2(xo, b.y), pal::handle, 2.0f * z);
        dl->AddTriangleFilled(ImVec2(xi, a.y), ImVec2(xi + flag, a.y), ImVec2(xi, a.y + flag), pal::handle);
        dl->AddTriangleFilled(ImVec2(xo, a.y), ImVec2(xo - flag, a.y), ImVec2(xo, a.y + flag), pal::handle);
        const std::string ti = "In " + formatTime(in * sampleSeconds);
        const std::string to = "Out " + formatTime(out * sampleSeconds);
        const float tow = ImGui::CalcTextSize(to.c_str()).x;
        dl->AddText(ImVec2(xi + flag + 3.0f * z, a.y + 2.0f * z), pal::headerText, ti.c_str());
        dl->AddText(ImVec2(std::max(xi + flag + 3.0f * z, xo - flag - 3.0f * z - tow),
                           a.y + 2.0f * z + (xo - xi < tow + 120.0f * z ? ImGui::GetTextLineHeight() : 0.0f)),
                    pal::headerText, to.c_str());
        // a playhead per sounding voice
        for(int v = 0; v < (int)playheads.size() && v < std::min(voices.get(), MaxVoices); ++v) {
            if(playheads[v] < 0.0f) continue;
            const float x = a.x + w * ofClamp(playheads[v], 0.0f, 1.0f);
            dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), pal::voiceCol(v, 235), 1.5f * z);
            dl->AddCircleFilled(ImVec2(x, b.y - 4.0f * z), 3.0f * z, pal::voiceCol(v, 235));
        }
    }
    const bool dragging = browser.isDraggingFile() && hovered;
    dl->AddRect(a, b, dragging ? pal::accentCol(255) : pal::border, 0, 0, dragging ? 2.0f * z : 1.0f);
    dl->PopClipRect();

    // ── Mouse: drag In / Out (near a line) or the whole region (inside it);
    //    a click outside moves the nearest edge there; double-click = all ──
    if(!waveformPeaks.empty()) {
        const float mx = ofClamp((ImGui::GetMousePos().x - a.x) / w, 0.0f, 1.0f);
        const float grab = 7.0f * z, mouseX = ImGui::GetMousePos().x;
        const bool nearIn = std::abs(mouseX - xi) <= grab, nearOut = std::abs(mouseX - xo) <= grab;
        if(hovered && !active) {
            if(nearIn || nearOut) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            else if(mx > in && mx < out) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("%s\nDrag In / Out, or the region to move it\nDouble-click: the whole file",
                              formatTime(mx * sampleSeconds).c_str());
        }
        if(ImGui::IsItemActivated()) {
            if(nearIn || nearOut) regionDrag = (nearIn && nearOut) ? (mx < in ? 0 : 1)
                                             : nearIn ? 0 : 1;
            else if(mx > in && mx < out) { regionDrag = 2; regionDragOffset = mx - in; }
            else regionDrag = std::abs(mx - in) < std::abs(mx - out) ? 0 : 1;
        }
        if(active && regionDrag >= 0) {
            const float eps = 0.0005f;
            if(regionDrag == 0) inPoint = std::min(mx, outPoint.get() - eps);
            else if(regionDrag == 1) outPoint = std::max(mx, inPoint.get() + eps);
            else {
                const float len = outPoint.get() - inPoint.get();
                const float start = ofClamp(mx - regionDragOffset, 0.0f, 1.0f - len);
                inPoint = start;
                outPoint = start + len;
            }
        }
        if(ImGui::IsItemDeactivated()) regionDrag = -1;
        if(hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { inPoint = 0.0f; outPoint = 1.0f; }
    }
    std::string dropped;
    if(browser.acceptDropOnLastItem(dropped)) loadSample(dropped);

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
void scSampleBox::drawTransport(float w) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const float x0 = ImGui::GetCursorPosX();
    ImGui::Button("Play", ImVec2(64.0f * z, 0));
    if(ImGui::IsItemActivated()) {
        fire();
        if(gateMode.get()) { auditionHeld = true; for(auto& [srv, s] : synths) if(s) s->set("gate", 1.0f); }
    }
    if(ImGui::IsItemDeactivated() && auditionHeld) {
        auditionHeld = false;
        for(auto& [srv, s] : synths) if(s) s->set("gate", trigger.get());
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip(gateMode.get() ? "Hold to play (Gate Mode)" : "Play a note");
    ImGui::SameLine();
    if(ImGui::Button("Stop", ImVec2(64.0f * z, 0))) stop();
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Release every note (and end loops)");

    ImGui::SameLine(0, 18.0f * z);
    toggle(loop, "Loop");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Repeat the region until Stop (or the gate falls in Gate Mode)");
    ImGui::SameLine(0, 10.0f * z);
    toggle(reverse, "Reverse");
    ImGui::SameLine(0, 10.0f * z);
    toggle(gateMode, "Gate Mode");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Notes last while Trigger/Gate is high\n(off: a trigger plays the whole region)");
    ImGui::SameLine(0, 10.0f * z);
    toggle(poly, "Polyphonic");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("On: notes overlap (up to Voices)\nOff: a new note fades the previous one out");
    if(poly.get()) {
        ImGui::SameLine(0, 6.0f * z);
        const float keep = labelW;
        labelW = 44.0f;
        sliderInt(voices, "Voices", 130.0f * z);
        labelW = keep;
    }
    ImGui::SameLine(0, 18.0f * z);
    {
        const float keep = labelW;
        labelW = 40.0f;
        combo(stretchMode, "Mode", 170.0f * z, {"Resample", "Stretch"});
        labelW = keep;
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Resample: speed and pitch together (like tape)\n"
                                                     "Stretch: Warp1 granular, speed and pitch apart");
    }
    // voice lights
    const int n = poly.get() ? voices.get() : 2;
    const float r = 4.0f * z, gap = 4.0f * z;
    const float lightsW = n * (2 * r + gap);
    ImGui::SameLine();
    const float x = std::max(ImGui::GetCursorPosX(), x0 + w - lightsW);
    ImGui::SetCursorPosX(x);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float cy = p.y + ImGui::GetFrameHeight() * 0.5f;
    for(int v = 0; v < n; ++v) {
        const bool on = v < (int)playheads.size() && playheads[v] >= 0.0f;
        const ImVec2 c(p.x + r + v * (2 * r + gap), cy);
        if(on) ImGui::GetWindowDrawList()->AddCircleFilled(c, r, pal::voiceCol(v, 255));
        else ImGui::GetWindowDrawList()->AddCircle(c, r, pal::border, 0, 1.5f * z);
    }
    ImGui::Dummy(ImVec2(lightsW, ImGui::GetFrameHeight()));
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Voices sounding");
}

void scSampleBox::drawPlaybackTab(float w) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const float colW = (w - 2 * 12.0f * z) / 3.0f;
    labelW = 74.0f;

    beginColumn("region");
    sectionHeader("Region", colW);
    {
        std::string fi = sampleSeconds > 0 ? formatTime(inPoint.get() * sampleSeconds) : "%.3f";
        std::string fo = sampleSeconds > 0 ? formatTime(outPoint.get() * sampleSeconds) : "%.3f";
        slider(inPoint, "In", colW, fi.c_str());
        slider(outPoint, "Out", colW, fo.c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImColor(pal::textDim), "Length");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelW * z - ImGui::CalcTextSize("Length").x - ImGui::GetStyle().ItemSpacing.x);
        const float len = (outPoint.get() - inPoint.get()) * sampleSeconds;
        ImGui::TextColored(ImColor(pal::textDim), "%s", sampleSeconds > 0 ? formatTime(len).c_str() : "-");
        if(!loop.get()) ImGui::BeginDisabled();
        slider(loopCrossfade, "Loop XFade", colW, "%.0f %%");
        if(!loop.get()) ImGui::EndDisabled();
        if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Loop: fade the end of the region into its start\n(0 %% keeps rhythmic loops tight)");
    }
    endColumn();

    beginColumn("speed");
    sectionHeader("Speed & Pitch", colW);
    slider(pitch, "Pitch", colW, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("MIDI note, taken when a note starts: 60 plays the file at its own pitch");
    slider(transpose, "Transpose", colW, "%+.2f st");
    if(ImGui::IsItemHovered())
        ImGui::SetTooltip(stretchMode.get() == 1 ? "Live, on sounding notes too: the grains' pitch only (Stretch keeps the timing)"
                                                 : "Live, on sounding notes too: speed and pitch together (Resample)");
    slider(rate, "Rate", colW, "%.3fx", true);
    const bool stretchOn = stretchMode.get() == 1;
    if(!stretchOn) ImGui::BeginDisabled();
    slider(stretch, "Stretch", colW, "%.2fx", true);
    slider(warpWindow, "Grain", colW, "%.3f s", true);
    if(!stretchOn) ImGui::EndDisabled();
    if(!stretchOn && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Stretch and Grain act in Stretch mode");
    endColumn();

    beginColumn("amp");
    sectionHeader("Amplitude", colW);
    slider(velocity, "Velocity", colW, "%.2f");
    slider(attack, "Attack", colW, "%.3f s", true);
    slider(release, "Release", colW, "%.3f s", true);
    slider(pan, "Pan", colW, "%+.2f");
    endColumn(true);
}

void scSampleBox::drawEqEditor(float w, float h) {
    for(int b = 0; b < 5; ++b)
        eqEditor.bands[b] = {eqFreq[b].get(), eqGain[b].get(), eqShape[b].get()};
    eqEditor.dbRange = 24.0f;
    eqEditor.gainLimit = 24.0f;
    eqEditor.qMin = 0.1f; eqEditor.qMax = 20.0f;
    eqEditor.slopeMin = 0.1f; eqEditor.slopeMax = 4.0f;
    float sr = 48000.0f;
    for(auto* sm : servers) if(sm && sm->getServer()) { sr = (float)serverManager::getSampleRateForServer(sm->getServer()); break; }
    eqEditor.recompute(sr > 0 ? sr : 48000.0f);
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
    eqEditor.bandMenu = [this](int b) {
        ImGui::TextUnformatted(scEQEditor::bandName(b));
        ImGui::Separator();
        auto value = [this](ofParameter<float>& p, const char* label) {
            float v = p.get();
            ImGui::SetNextItemWidth(180.0f * ofxOceanodeShared::getZoomLevel());
            if(ImGui::InputFloat(label, &v, 0, 0, "%.3f")) p.set(ofClamp(v, p.getMin(), p.getMax()));
            const auto key = p.getEscapedName();
            const bool isPublished = published.count(key) != 0;
            const std::string action = std::string(isPublished ? "Unpublish " : "Publish ") + label;
            if(ImGui::MenuItem(action.c_str(), nullptr, false, !isConnected(key))) {
                if(isPublished) unpublish(key); else publish(key);
            }
        };
        value(eqFreq[b], "Freq (Hz)");
        value(eqGain[b], "Gain (dB)");
        value(eqShape[b], scEQEditor::isShelf(b) ? "Slope" : "Q");
    };
    const bool off = !eqOn.get();
    if(off) ImGui::BeginDisabled();
    if(eqEditor.draw(std::max(120.0f, w), h, col, !off, nullptr, 0, "##sampleBoxEqCurve")) {
        for(int b = 0; b < 5; ++b) {
            eqFreq[b] = eqEditor.bands[b].freqHz;
            eqGain[b] = eqEditor.bands[b].gainDb;
            eqShape[b] = eqEditor.bands[b].shape;
        }
    }
    if(off) ImGui::EndDisabled();
}

void scSampleBox::drawFilterEqTab(float w) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const float gap = 12.0f * z;
    const float filterW = std::max(230.0f * z, (w - 2 * gap) / 3.0f);
    const float eqW = w - filterW - gap;
    labelW = 74.0f;

    beginColumn("filter");
    sectionHeader("Filter", filterW, &filterOn);
    if(!filterOn.get()) ImGui::BeginDisabled();
    combo(filterType, "Type", filterW, {"Low pass", "High pass", "Band pass"});
    slider(cutoff, "Cutoff", filterW, "%.0f Hz", true);
    slider(resonance, "Resonance", filterW, "%.2f");
    if(!filterOn.get()) ImGui::EndDisabled();
    endColumn();

    beginColumn("eq");
    sectionHeader("EQ", eqW, &eqOn);
    if(!eqOn.get()) ImGui::BeginDisabled();
    slider(eqMix, "Mix", std::min(eqW, 260.0f * z), "%.2f");
    if(!eqOn.get()) ImGui::EndDisabled();
    ImGui::TextColored(ImColor(pal::textDim), "Drag the bands; wheel = Q / slope; right-click a band for values");
    drawEqEditor(eqW, 220.0f * z);
    endColumn(true);
}

void scSampleBox::drawDynamicsTab(float w) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const float colW = (w - 2 * 12.0f * z) / 3.0f;
    labelW = 74.0f;

    beginColumn("comp");
    sectionHeader("Compressor", colW, &compOn);
    if(!compOn.get()) ImGui::BeginDisabled();
    slider(compThreshold, "Threshold", colW, "%.1f dB");
    slider(compRatio, "Ratio", colW, "%.1f : 1", true);
    slider(compAttack, "Attack", colW, "%.1f ms", true);
    slider(compRelease, "Release", colW, "%.0f ms", true);
    slider(compMakeup, "Makeup", colW, "%+.1f dB");
    if(!compOn.get()) ImGui::EndDisabled();
    endColumn();

    beginColumn("sat");
    sectionHeader("Saturator", colW, &satOn);
    if(!satOn.get()) ImGui::BeginDisabled();
    slider(satDrive, "Drive", colW, "%.1f dB");
    slider(satMix, "Mix", colW, "%.2f");
    if(!satOn.get()) ImGui::EndDisabled();
    endColumn();

    beginColumn("flow");
    sectionHeader("Signal flow", colW);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + colW);
    ImGui::TextColored(ImColor(pal::textDim),
        "Voices -> Filter -> EQ -> Compressor -> Saturator -> Reverb -> Stereo image -> Out.\n\n"
        "Each stage can be switched on in its own header; envelopes can move any of their controls.");
    ImGui::PopTextWrapPos();
    endColumn(true);
}

void scSampleBox::drawSpaceTab(float w) {
    const float z = ofxOceanodeShared::getZoomLevel();
    const float gap = 12.0f * z;
    const float colW = (w - 2 * gap) / 3.0f;
    labelW = 74.0f;

    beginColumn("reverb");
    sectionHeader("Reverb", colW, &reverbOn);
    if(!reverbOn.get()) ImGui::BeginDisabled();
    slider(revSize, "Size", colW, "%.1f");
    slider(revDecay, "Decay", colW, "%.2f s", true);
    slider(revPredelay, "Predelay", colW, "%.0f ms");
    slider(revLowpass, "Lowpass", colW, "%.0f Hz", true);
    if(!reverbOn.get()) ImGui::EndDisabled();
    endColumn();

    beginColumn("reverb2");
    sectionHeader("Reverb Mix", colW);
    if(!reverbOn.get()) ImGui::BeginDisabled();
    slider(revPosition, "Position", colW, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("0 = early reflections, 1 = the late tail");
    slider(revSpread, "Spread", colW, "%+.2f");
    slider(revMix, "Mix", colW, "%.2f");
    if(!reverbOn.get()) ImGui::EndDisabled();
    endColumn();

    beginColumn("stereo");
    sectionHeader("Stereo Image", colW);
    const bool multi = channels.get() > 1;
    if(!multi) ImGui::BeginDisabled();
    slider(width, "Width", colW, "%.2f");
    slider(diffuse, "Diffuse", colW, "%.2f");
    slider(bassMono, "Bass Mono", colW, "%.2f");
    slider(bassFreq, "Bass Freq", colW, "%.0f Hz", true);
    if(!multi) ImGui::EndDisabled();
    if(!multi) ImGui::TextColored(ImColor(pal::textDim), "Needs N Chan > 1");
    endColumn(true);
}
