//
//  scSpectralFilterBox.cpp
//  ofxOceanodeSuperCollider
//

#include "scSpectralFilterBox.h"
#include "scBusResend.h"
#include "imgui.h"
#include "ofxOceanodeShared.h"
#include <algorithm>
#include <cmath>

namespace {
const std::vector<std::string> fftNames = {"2048 (45 ms)", "4096 (91 ms)"};
const std::vector<std::string> modeNames = {"FFT", "Bandpass"};
const std::vector<std::string> bandNames = {"64 (2 st)", "128 (1 st)"};
const std::vector<std::string> interpNames = {"Linear", "Smooth", "Steps"};

// v (any size) read at x = 0..1
float sampleAt(const std::vector<float>& v, float x, int mode){
    const int n = int(v.size());
    if(n == 0) return 1.0f;
    if(n == 1) return v[0];
    if(mode == 2) return v[std::min(n - 1, int(std::clamp(x, 0.0f, 1.0f) * n))];   // Steps: each value a band
    const float p = std::clamp(x, 0.0f, 1.0f) * (n - 1);
    const int i = std::min(int(p), n - 2);
    const float t = p - i;
    if(mode == 1){
        // Catmull-Rom through the neighbours
        const float a = v[std::max(i - 1, 0)], b = v[i], c = v[i + 1], d = v[std::min(i + 2, n - 1)];
        return b + 0.5f * t * (c - a + t * (2 * a - 5 * b + 4 * c - d + t * (3 * (b - c) + d - a)));
    }
    return v[i] + (v[i + 1] - v[i]) * t;
}

// Circular crossfade: each end of the mask blends into the other, mirrored
// about the seam, so the last point meets the first and the mask loops
// without a step. Only mixes existing values (never leaves their range).
// `share` 1 crossfades half the mask from each side.
void circularCrossfade(std::vector<float>& v, float share){
    const int size = int(v.size());
    const int n = int(share * size * 0.5f);
    if(size < 2 || n <= 0) return;
    const std::vector<float> src = v;
    for(int i = 0; i < n; i++){
        const int h = i, t = size - 1 - i;
        float x = 1.0f - float(i) / float(n);          // 1 at the seam .. 0 inside
        const float w = 0.5f * x * x * (3.0f - 2.0f * x);   // smoothstep, half at the seam
        v[h] = src[h] + (src[t] - src[h]) * w;
        v[t] = src[t] + (src[h] - src[t]) * w;
    }
}
}

scSpectralFilterBox::scSpectralFilterBox() : scNode("SpectralFilterBox") {
    description = "Spectral filter drawn by a float vector of any size (e.g. a Manual Vector): the vector lies over "
                  "log frequency (Low Hz .. High Hz) as the gain of every FFT bin; Circular XFade blends its two ends "
                  "into each other so it scrolls round without a step. Transpose, Scroll and Spread move "
                  "it in semitones (per channel with Spread). Shaping (interpolation, "
                  "contrast, blur, steps, invert, dB range) and the per-bin attack / release / hold are in the "
                  "inspector. Custom UGen PV_MaskFilter, about 0.5% CPU per channel. Mode Bandpass: the same "
                  "filter from a bank of bandpasses, without the FFT's latency (64 or 128 bands).";
}

scSpectralFilterBox::~scSpectralFilterBox(){
    for(auto& [server, synth] : synths) if(synth){ synth->free(); delete synth; }
    synths.clear();
}

void scSpectralFilterBox::setup(){
    addParameter(numChannels.set("N Chan", 2, 1, MAX_CHANNELS));
    addParameterDropdown(modeP, "Mode", 0, modeNames);
    addParameterDropdown(fftSize, "FFT", 1, fftNames);
    scNode::addInput("In");
    addParameter(mask.set("Mask", {1.0f}, {0.0f}, {1.0f}));
    addParameter(circularXFade.set("Circular XFade", 0.0f, 0.0f, 1.0f));
    addParameter(transpose.set("Transpose", 0.0f, -48.0f, 48.0f));
    addParameter(scroll.set("Scroll", 0.0f, -48.0f, 48.0f));
    addParameter(spread.set("Spread", 0.0f, -120.0f, 120.0f));
    addParameter(depth.set("Depth", 1.0f, 0.0f, 1.0f));
    addParameter(gainDb.set("Gain dB", 0.0f, -24.0f, 36.0f));
    addParameter(mix.set("Mix", 1.0f, 0.0f, 1.0f));
    addCustomRegion(
        ofParameter<std::function<void()>>().set("Mask Preview", [this](){ drawPreview(); }),
        ofParameter<std::function<void()>>().set("Mask Preview", [this](){ drawPreview(); }));
    scNode::addOutput("Out");

    addInspectorParameter(interp.set("Interp", 0, 0, 2));
    addInspectorParameter(contrast.set("Contrast", 0.0f, -1.0f, 1.0f));
    addInspectorParameter(rangeDb.set("Range dB", 0.0f, 0.0f, 96.0f));
    addInspectorParameter(blur.set("Blur", 0.0f, 0.0f, 1.0f));
    addInspectorParameter(quantize.set("Quantize", 0, 0, 16));
    addInspectorParameter(invert.set("Invert", false));
    addInspectorParameter(lowFreq.set("Low Hz", 20.0f, 10.0f, 2000.0f));
    addInspectorParameter(highFreq.set("High Hz", 20000.0f, 200.0f, 22000.0f));
    addInspectorParameter(wrap.set("Wrap", true));
    addInspectorParameter(attack.set("Attack", 0.0f, 0.0f, 10.0f));
    addInspectorParameter(release.set("Release", 0.0f, 0.0f, 30.0f));
    addInspectorParameter(hold.set("Hold", false));
    bandsP.set("Bands", 0, 0, 1);
    addInspectorParameter(bandsP);

    auto dirtyMask = [this](){ maskDirty = true; };
    listeners.push(mask.newListener([dirtyMask](std::vector<float>&){ dirtyMask(); }));
    for(auto* p : {&circularXFade, &depth, &contrast, &rangeDb, &blur})
        listeners.push(p->newListener([dirtyMask](float&){ dirtyMask(); }));
    listeners.push(interp.newListener([dirtyMask](int&){ dirtyMask(); }));
    listeners.push(quantize.newListener([dirtyMask](int&){ dirtyMask(); }));
    listeners.push(invert.newListener([dirtyMask](bool&){ dirtyMask(); }));
    listeners.push(numChannels.newListener([this](int& n){ if(n != builtChannels) rebuild(); }));
    listeners.push(fftSize.newListener([this](int& f){ if(f != builtFft && !bandpass()) rebuild(); }));
    listeners.push(modeP.newListener([this](int& m){ if(m != builtMode) rebuild(); }));
    listeners.push(bandsP.newListener([this](int& b){ if(b != builtBands && bandpass()) rebuild(); }));
    listeners.push(gainDb.newListener([this](float& g){ sendScalar("gain", std::pow(10.0f, g / 20.0f)); }));
    listeners.push(mix.newListener([this](float& m){ sendScalar("mix", m); }));
    listeners.push(lowFreq.newListener([this](float& f){ sendScalar("lowfreq", f); }));
    listeners.push(highFreq.newListener([this](float& f){ sendScalar("highfreq", f); }));
    listeners.push(wrap.newListener([this](bool& w){ sendScalar("wrap", w ? 1.0f : 0.0f); }));
    listeners.push(attack.newListener([this](float& a){ sendScalar("attack", a); }));
    listeners.push(release.newListener([this](float& r){ sendScalar("release", r); }));
    listeners.push(hold.newListener([this](bool& h){ sendScalar("hold", h ? 1.0f : 0.0f); }));

    builtChannels = channels();
    builtFft = fftSize;
    builtMode = modeP;
    builtBands = bandsP;
    shaped = shapeMask();
}

// ════════════════════════════════════════════════════════════════════════════
// Frame: the mask when it changed, the transposes (Scroll moves them)
// ════════════════════════════════════════════════════════════════════════════

void scSpectralFilterBox::update(ofEventArgs&){
    const float now = ofGetElapsedTimef();
    const float dt = lastTime < 0 ? 0.0f : std::min(0.1f, now - lastTime);
    lastTime = now;
    if(scroll.get() != 0.0f && getActive()){
        scrollOffset += scroll.get() * dt;
        // Wrapping round the mask: a whole span is the same position
        const float span = spanSemitones();
        if(wrap && span > 0) scrollOffset = std::fmod(scrollOffset, span);
        else scrollOffset = std::clamp(scrollOffset, -96.0f, 96.0f);
    }
    if(maskDirty){
        maskDirty = false;
        sendMask(false);
    }
    if(bandpass()) updateBandGains(dt, false);
    else sendTransposes(false);
}

// ════════════════════════════════════════════════════════════════════════════
// Bandpass mode: the mask read at each band (as PV_MaskFilter reads it at each
// bin), smoothed by Attack / Release, frozen by Hold
// ════════════════════════════════════════════════════════════════════════════

float scSpectralFilterBox::maskAt(float x) const {
    if(shaped.size() != MASK_SIZE) return 1.0f;
    float p;
    int i0, i1;
    if(wrap){
        x -= std::floor(x);
        p = x * MASK_SIZE;
        i0 = std::min(int(p), MASK_SIZE - 1);
        i1 = (i0 + 1) % MASK_SIZE;
    } else {
        x = std::clamp(x, 0.0f, 1.0f);
        p = x * (MASK_SIZE - 1);
        i0 = std::min(int(p), MASK_SIZE - 2);
        i1 = i0 + 1;
    }
    return std::max(0.0f, shaped[i0] + (shaped[i1] - shaped[i0]) * (p - i0));
}

std::vector<float> scSpectralFilterBox::bandTargets() const {
    const int k = bandCount(), n = channels();
    const float logLo = std::log2(std::max(1.0f, lowFreq.get()));
    const float span = spanSemitones() / 12.0f;
    const auto t = channelTransposes();
    std::vector<float> g(n * k);
    for(int b = 0; b < k; b++){
        // the band's centre, as in the SynthDef: 10 Hz * 2000^((b + 0.5) / k)
        const float octs = std::log2(10.0f) + std::log2(2000.0f) * (b + 0.5f) / k;
        for(int c = 0; c < n; c++) g[c * k + b] = maskAt((octs - logLo - t[c] / 12.0f) / span);
    }
    return g;
}

void scSpectralFilterBox::updateBandGains(float dt, bool force){
    const auto target = bandTargets();
    if(bandGains.size() != target.size() || force) bandGains = target;
    else if(!hold){
        const float ca = attack.get() > 0.0f ? std::exp(-dt / attack.get()) : 0.0f;
        const float cr = release.get() > 0.0f ? std::exp(-dt / release.get()) : 0.0f;
        for(size_t i = 0; i < target.size(); i++)
            bandGains[i] = target[i] + (bandGains[i] - target[i]) * (target[i] > bandGains[i] ? ca : cr);
    }
    bool changed = force || sentGains.size() != bandGains.size();
    for(size_t i = 0; !changed && i < bandGains.size(); i++) changed = std::abs(bandGains[i] - sentGains[i]) > 1e-4f;
    if(!changed) return;
    sentGains = bandGains;
    for(auto& [server, synth] : synths) if(synth) synth->set("gains", sentGains);
}

// ════════════════════════════════════════════════════════════════════════════
// Mask
// ════════════════════════════════════════════════════════════════════════════

std::vector<float> scSpectralFilterBox::shapeMask() const {
    const int mode = std::clamp(interp.get(), 0, 2);
    const auto& a = mask.get();
    std::vector<float> v(MASK_SIZE);
    for(int i = 0; i < MASK_SIZE; i++) v[i] = sampleAt(a, float(i) / (MASK_SIZE - 1), mode);
    // The end into the beginning, before anything smooths or curves it
    circularCrossfade(v, std::clamp(circularXFade.get(), 0.0f, 1.0f));
    // Blur: a Gaussian across neighbouring points
    if(blur.get() > 0.001f){
        const float sigma = 0.5f + blur.get() * 12.0f;
        const int radius = int(std::ceil(sigma * 2.5f));
        std::vector<float> out(MASK_SIZE);
        for(int i = 0; i < MASK_SIZE; i++){
            float s = 0, ws = 0;
            for(int j = -radius; j <= radius; j++){
                int n = i + j;
                if(wrap) n = (n % MASK_SIZE + MASK_SIZE) % MASK_SIZE;
                else if(n < 0 || n >= MASK_SIZE) continue;
                const float w = std::exp(-0.5f * (j / sigma) * (j / sigma));
                s += v[n] * w;
                ws += w;
            }
            out[i] = s / ws;
        }
        v = std::move(out);
    }
    const float exponent = std::pow(4.0f, contrast.get());   // -1: x^0.25 (fuller) .. 1: x^4 (sharper)
    const int q = quantize.get();
    const float range = rangeDb.get();
    const float d = std::clamp(depth.get(), 0.0f, 1.0f);
    for(float& x : v){
        float y = std::clamp(x, 0.0f, 1.0f);
        if(exponent != 1.0f) y = std::pow(y, exponent);
        if(q >= 2) y = std::round(y * (q - 1)) / (q - 1);
        if(invert) y = 1.0f - y;
        // Range: 0..1 as -Range .. 0 dB (more even to the ear than a linear gain)
        if(range > 0.0f) y = std::pow(10.0f, range * (y - 1.0f) / 20.0f);
        x = 1.0f - d + d * y;
    }
    return v;
}

float scSpectralFilterBox::spanSemitones() const {
    const float lo = std::max(1.0f, lowFreq.get());
    const float hi = std::max(lo * 1.001f, highFreq.get());
    return 12.0f * std::log2(hi / lo);
}

std::vector<float> scSpectralFilterBox::channelTransposes() const {
    std::vector<float> t(channels());
    for(int c = 0; c < (int)t.size(); c++) t[c] = transpose.get() + scrollOffset + spread.get() * c;
    return t;
}

// ════════════════════════════════════════════════════════════════════════════
// Synths
// ════════════════════════════════════════════════════════════════════════════

int scSpectralFilterBox::channels() const { return std::clamp(numChannels.get(), 1, MAX_CHANNELS); }

std::string scSpectralFilterBox::synthDefName() const {
    if(bandpass()) return "SpectralFilterBoxBP_" + ofToString(bandCount()) + "_" + ofToString(channels());
    return "SpectralFilterBox_" + std::string(fftSize.get() == 0 ? "2048" : "4096") + "_" + ofToString(channels());
}

void scSpectralFilterBox::sendMask(bool force){
    auto v = shapeMask();
    if(!force && v == shaped && !synths.empty()) return;
    shaped = v;
    if(bandpass()) return;   // the gains follow at this frame
    for(auto& [server, synth] : synths) if(synth) synth->set("mask", shaped);
}

void scSpectralFilterBox::sendTransposes(bool force){
    auto t = channelTransposes();
    if(!force && t == sentTransposes) return;
    sentTransposes = t;
    for(auto& [server, synth] : synths) if(synth) synth->set("transpose", t);
}

void scSpectralFilterBox::sendScalar(const std::string& name, float value){
    sentScalars[name] = value;
    for(auto& [server, synth] : synths) if(synth) synth->set(name, value);
}

void scSpectralFilterBox::sendAll(ofxSCSynth* synth){
    if(!synth) return;
    shaped = shapeMask();
    if(bandpass()){
        updateBandGains(0.0f, sentGains.size() != size_t(bandCount() * channels()));
        synth->set("gains", sentGains);
        synth->set("gain", std::pow(10.0f, gainDb.get() / 20.0f));
        synth->set("mix", mix.get());
        return;
    }
    synth->set("mask", shaped);
    sentTransposes = channelTransposes();
    synth->set("transpose", sentTransposes);
    synth->set("gain", std::pow(10.0f, gainDb.get() / 20.0f));
    synth->set("mix", mix.get());
    synth->set("lowfreq", lowFreq.get());
    synth->set("highfreq", highFreq.get());
    synth->set("wrap", wrap ? 1.0f : 0.0f);
    synth->set("attack", attack.get());
    synth->set("release", release.get());
    synth->set("hold", hold ? 1.0f : 0.0f);
}

// N Chan or FFT changed: every synth replaced in place (/s_new action 4),
// its buses and values baked into the new node
void scSpectralFilterBox::rebuild(){
    builtChannels = channels();
    builtFft = fftSize;
    builtMode = modeP;
    builtBands = bandsP;
    bandGains.clear();
    sentGains.clear();
    for(auto& [server, synth] : synths){
        if(!synth) continue;
        auto* fresh = new ofxSCSynth(synthDefName(), server);
        sendAll(fresh);
        scResendInOutBuses(fresh, server, inputBuses, outputBuses);
        if(synth->isCreated()) fresh->createAndRun(4, synth->nodeID, getActive());
        delete synth;   // no /n_free: action 4 replaces the node atomically
        synth = fresh;
    }
}

void scSpectralFilterBox::buildSynth(ofxSCServer* server){
    if(!server) return;
    if(synths.count(server) && synths[server]){ synths[server]->free(); delete synths[server]; }
    builtChannels = channels();
    builtFft = fftSize;
    builtMode = modeP;
    builtBands = bandsP;
    synths[server] = new ofxSCSynth(synthDefName(), server);
    sendAll(synths[server]);
}

void scSpectralFilterBox::createSynth(ofxSCServer* server){
    auto it = synths.find(server);
    if(it == synths.end() || !it->second) return;
    scResendInOutBuses(it->second, server, inputBuses, outputBuses);
    if(!it->second->isCreated()) it->second->createAndRun(0, 1, getActive());
}

void scSpectralFilterBox::free(ofxSCServer* server){
    auto it = synths.find(server);
    if(it != synths.end()){
        if(it->second){ it->second->free(); delete it->second; }
        synths.erase(it);
    }
    inputBuses.erase(server);
    outputBuses.erase(server);
}

void scSpectralFilterBox::setInputBus(ofxSCServer* server, scNode* node, int bus){
    if(!server || !node) return;
    inputBuses[server][node] = bus;
    if(synths.count(server) && synths[server]) synths[server]->set("in", bus);
}

void scSpectralFilterBox::resetInputBusses(ofxSCServer* server, int targetBus){
    if(!server) return;
    inputBuses[server].clear();
    if(synths.count(server) && synths[server]) synths[server]->set("in", targetBus);
}

void scSpectralFilterBox::setOutputBus(ofxSCServer* server, int index, int bus){
    if(!server) return;
    outputBuses[server][index] = bus;
    if(index == 0 && synths.count(server) && synths[server]) synths[server]->set("out", bus);
}

int scSpectralFilterBox::getOutputBusIndex(ofxSCServer* server, int index){
    auto it = outputBuses.find(server);
    if(it == outputBuses.end()) return -1;
    auto b = it->second.find(index);
    return b == it->second.end() ? -1 : b->second;
}

void scSpectralFilterBox::moveSynthBefore(ofxSCServer* server, int nodeID){
    auto it = synths.find(server);
    if(it == synths.end() || !it->second) return;
    scResendInOutBuses(it->second, server, inputBuses, outputBuses);
    it->second->moveBefore(nodeID);
}

int scSpectralFilterBox::getLastSynthID(ofxSCServer* server){
    auto it = synths.find(server);
    return it != synths.end() && it->second ? it->second->nodeID : -1;
}

void scSpectralFilterBox::activate(){
    for(auto& [server, synth] : synths) if(synth && synth->isCreated()) synth->run(true);
}

void scSpectralFilterBox::deactivate(){
    for(auto& [server, synth] : synths) if(synth && synth->isCreated()) synth->run(false);
}

void scSpectralFilterBox::resendParametersForNRT(){
    for(auto& [server, synth] : synths) sendAll(synth);
}

// ════════════════════════════════════════════════════════════════════════════
// Node preview: the shaped mask over log frequency where it sits now (bright:
// channel 1, faint: the other channels' Spread), with the octave grid
// ════════════════════════════════════════════════════════════════════════════

void scSpectralFilterBox::drawPreview(){
    const float zoom = ofxOceanodeShared::getZoomLevel();
    const ImVec2 size(240.0f * zoom, 64.0f * zoom);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##sfbpreview", size);
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), IM_COL32(12, 14, 20, 255), 3.0f);
    if(shaped.size() != MASK_SIZE) return;

    // The display spans 20 Hz .. 20 kHz; the mask spans Low .. High Hz
    constexpr float fMin = 20.0f, fMax = 20000.0f;
    const float logDisplay = std::log2(fMax / fMin);
    const float lo = std::max(1.0f, lowFreq.get());
    const float span = spanSemitones() / 12.0f;   // octaves
    for(int oct = 1; oct < 10; oct++){
        const float x = o.x + size.x * oct / logDisplay;
        dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + size.y), IM_COL32(255, 255, 255, 14));
    }
    // Gain the UGen gives at display x (same reading as PV_MaskFilter)
    auto gainAt = [&](float xDisplay, float t){
        const float octFromLo = xDisplay * logDisplay + std::log2(fMin / lo) - t / 12.0f;
        float x = octFromLo / span;
        float p;
        int i0, i1;
        if(wrap){
            x -= std::floor(x);
            p = x * MASK_SIZE;
            i0 = std::min(int(p), MASK_SIZE - 1);
            i1 = (i0 + 1) % MASK_SIZE;
        } else {
            x = std::clamp(x, 0.0f, 1.0f);
            p = x * (MASK_SIZE - 1);
            i0 = std::min(int(p), MASK_SIZE - 2);
            i1 = i0 + 1;
        }
        return shaped[i0] + (shaped[i1] - shaped[i0]) * (p - i0);
    };
    const auto t = channelTransposes();
    const int N = std::max(32, int(size.x));
    std::vector<ImVec2> pts(N);
    // Display level: 0..1 linear, boosts clipped at the top
    auto yOf = [&](float g){ return o.y + size.y - 3.0f - (size.y - 6.0f) * std::clamp(g, 0.0f, 1.0f); };
    for(int c = int(t.size()) - 1; c >= 0; c--){
        if(c > 0 && spread.get() == 0.0f) continue;
        for(int i = 0; i < N; i++){
            const float xd = float(i) / (N - 1);
            pts[i] = ImVec2(o.x + size.x * xd, yOf(gainAt(xd, t[c])));
        }
        if(c == 0){
            for(int i = 0; i + 1 < N; i++)
                dl->AddQuadFilled(pts[i], pts[i + 1], ImVec2(pts[i + 1].x, o.y + size.y), ImVec2(pts[i].x, o.y + size.y),
                                  IM_COL32(80, 170, 255, 60));
            dl->AddPolyline(pts.data(), N, IM_COL32(120, 200, 255, 230), 0, 1.5f);
        } else {
            dl->AddPolyline(pts.data(), N, IM_COL32(120, 200, 255, 60), 0, 1.0f);
        }
    }
    // The mask's own span when it does not wrap round the whole display
    if(!wrap){
        for(float f : {lo, std::max(lo * 1.001f, highFreq.get())}){
            const float x = o.x + size.x * std::log2(std::clamp(f, fMin, fMax) / fMin) / logDisplay;
            dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + size.y), IM_COL32(255, 220, 120, 90));
        }
    }
}
