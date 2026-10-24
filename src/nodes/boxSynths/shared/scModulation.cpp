//
//  scModulation.cpp
//  ofxOceanodeSuperCollider
//

#include "scModulation.h"
#include "scMorphPolicies.h"
#include "ofxOceanodeNodeModel.h"
#include "../../../audio/scSynthStartup.h"
#include "../../../ui/scEnvelopeCurve.h"
#include "../../../audio/scMorphWave.h"
#include <algorithm>
#include <cmath>

namespace {
bool liveControlBus(ofxSCServer* s, ofxSCBus* b) {
    return s && b && b->index >= 0 && b->index < (int)s->controlBusses.size() && s->controlBusses[b->index] == b;
}
std::vector<float> expand(const std::vector<float>& v, int n, float fallback = 0.0f) {
    if(v.empty()) return std::vector<float>(n, fallback);
    std::vector<float> out(v);
    out.resize(n, v.back());
    return out;
}
float first(const std::vector<float>& v, float fallback = 0.0f) { return v.empty() ? fallback : v[0]; }
float zoom() { return ofxOceanodeShared::getZoomLevel(); }
float gapW() { return 12.0f * zoom(); }
float colW(float w, int n) { return (w - gapW() * (n - 1)) / n; }

namespace pal {
const ImU32 canvas     = IM_COL32( 11,  12,  14, 255);
const ImU32 border     = IM_COL32( 46,  50,  58, 255);
const ImU32 grid       = IM_COL32(255, 255, 255,  14);
const ImU32 gridStrong = IM_COL32(255, 255, 255,  34);
const ImU32 header     = IM_COL32( 30,  32,  38, 255);
const ImU32 headerText = IM_COL32(205, 205, 215, 255);
const ImU32 textDim    = IM_COL32(150, 150, 160, 255);
const ImU32 accent     = IM_COL32(150, 165, 255, 255);
const ImU32 negative   = IM_COL32(236, 178,  96, 255);
const ImU32 lfo[8]     = {IM_COL32(100, 200, 255, 235), IM_COL32(255, 160, 80, 235),
                          IM_COL32(120, 230, 140, 235), IM_COL32(255, 110, 180, 235),
                          IM_COL32(186, 146, 232, 235), IM_COL32(236, 214, 96, 235),
                          IM_COL32(96, 200, 200, 235), IM_COL32(232, 120, 100, 235)};
const ImU32 env[4]     = {IM_COL32(236, 178, 96, 235), IM_COL32(96, 200, 176, 235),
                          IM_COL32(228, 122, 146, 235), IM_COL32(170, 208, 112, 235)};
}
void canvas(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    dl->AddRectFilled(a, b, pal::canvas, 3.0f * zoom());
    dl->AddRect(a, b, pal::border, 3.0f * zoom());
}

// the LFO shapes (the synth's maths), for the previews
float reflectUnit(float v) { return v + ((v < 0.f) * (0.f - v)) + ((v > 1.f) * (1.f - v)); }
float customPow(float value, float pow) {
    const float k1 = 2.0f * pow * 0.99999f;
    const float k2 = k1 / ((-pow * 0.999999f) + 1.0f);
    const float k3 = k2 * std::abs(value) + 1.0f;
    return value * (k2 + 1.0f) / std::max(k3, 1.0e-6f);
}
float biPow(float value, float b) { return reflectUnit((customPow(value * 2.0f - 1.0f, b) + 1.0f) * 0.5f); }
using scmorph::morph;

float perlin(float x, float seed) {
    auto grad = [seed](float k){ const float v = std::sin((k + seed) * 12.9898f) * 43758.5453f; return (v - std::floor(v)) * 2.0f - 1.0f; };
    const float i = std::floor(x), f = x - i;
    const float u = f * f * f * (f * (f * 6 - 15) + 10);
    const float n0 = grad(i) * f, n1 = grad(i + 1) * (f - 1);
    return ofClamp((n0 + (n1 - n0) * u) * 1.6f + 0.5f, 0.0f, 1.0f);
}
}

// Smooth 2D value noise, shared with the server's valueNoise2 function.
float scModulation::noise2(float x, float y, float salt) {
    // Stagger each voice's interpolation knots as well as its random values.
    // Unlike gradient noise, integer coordinates have no forced middle value.
    x += 0.371f - y * 0.754877666f;
    y += 0.619f;
    auto value = [salt](float cx, float cy){
        const float a = std::fmod(cx, 4096.0f), b = std::fmod(cy, 4096.0f);
        const float s = std::sin((a < 0 ? a + 4096.0f : a) * 12.9898f + (b < 0 ? b + 4096.0f : b) * 78.233f + salt) * 43758.5453f;
        return s - std::floor(s);
    };
    const float ix = std::floor(x), iy = std::floor(y), fx = x - ix, fy = y - iy;
    const float u = fx * fx * fx * (fx * (fx * 6 - 15) + 10), v = fy * fy * fy * (fy * (fy * 6 - 15) + 10);
    const float n00 = value(ix, iy), n10 = value(ix + 1, iy);
    const float n01 = value(ix, iy + 1), n11 = value(ix + 1, iy + 1);
    const float a = n00 + (n10 - n00) * u, b = n01 + (n11 - n01) * u;
    return ofClamp(a + (b - a) * v, 0.0f, 1.0f);
}

// What the shape goes through after the shape (or the noise): Gain, Steps,
// Pow, BiPow, as a transfer curve 0..1 -> 0..1 (the nodes that run the noise
// themselves read it as a table)
std::vector<float> scModulation::lfoTransfer(int li, int size) const {
    const float gain = first(lfoGain[li].get(), 1.0f), pw2 = first(lfoPow[li].get()), bp = first(lfoBiPow[li].get());
    const std::array<float, 9> key{gain, pw2, bp, (float)lfoSteps[li].get()};
    auto& cache = transferCache[li];
    const int count = std::max(2, size);
    if(cache.values.size() == (size_t)count && cache.key == key) return cache.values;
    std::vector<float> t(count);
    for(int i = 0; i < (int)t.size(); i++){
        float v = float(i) / (t.size() - 1);
        v = ofClamp((v - 0.5f) * gain + 0.5f, 0.0f, 1.0f);
        if(std::abs(pw2) > 1e-4f) v = reflectUnit(customPow(v, pw2));
        if(std::abs(bp) > 1e-4f) v = biPow(v, bp);
        if(lfoSteps[li].get() >= 2){
            const float q = (float)lfoSteps[li].get();
            v = std::min(std::floor(v * q), q - 1.0f) / (q - 1.0f);
        }
        t[i] = ofClamp(v, 0.0f, 1.0f);
    }
    cache.key = key;
    cache.values = t;
    return t;
}

// ════════════════════════════════════════════════════════════════════════════
// Setup
// ════════════════════════════════════════════════════════════════════════════

void scModulation::setup(Host h, std::vector<Target> t, bool perVoiceLfo, bool hasVelocity, bool hasRandom) {
    host = h;
    targetList = std::move(t);
    perVoice = perVoiceLfo;
    velocity = hasVelocity;
    random = hasRandom;
    matrix.assign(sourceCount() * T(), 0.0f);
    auto vec = [](ofParameter<std::vector<float>>& p, const std::string& name, float def, float lo, float hi) {
        p.set(name, {def}, {lo}, {hi});
    };
    for(int li = 0; li < MAX_LFOS; li++){
        const std::string L = "LFO" + ofToString(li + 1) + " ";
        const std::string X = nameSuffix;
        auto toLfo = [this, li]() { forStates([this, li](ofxSCServer*, ServerState& st){ sendLfo(st, li); }); notify(); };
        lfoSync[li].set(L + "Sync" + X, true);                    host.regB(lfoSync[li], toLfo);
        vec(lfoBeats[li], L + "Beats" + X, (float)(1 << std::min(li, 4)), 0.125f, 512); host.regV(lfoBeats[li], toLfo);
        vec(lfoHz[li], L + "Hz" + X, 1, 0.0f, 50);                host.regV(lfoHz[li], toLfo);   // 0: still
        vec(lfoSH[li], L + "S&H" + X, 0, 0, 32);                  host.regV(lfoSH[li], toLfo);
        vec(lfoDetune[li], L + "Detune" + X, 0, 0, 8);            host.regV(lfoDetune[li], toLfo);
        lfoInit[li].set(L + "Phase" + X, 0.0f, 0.0f, 1.0f);       host.regF(lfoInit[li], toLfo);
        vec(lfoRound[li], L + "Round" + X, 0.5f, 0, 1);           host.regV(lfoRound[li], toLfo);
        vec(lfoSkew[li], L + "Skew" + X, 0, -1, 1);               host.regV(lfoSkew[li], toLfo);
        vec(lfoPw[li], L + "Width" + X, 0.5f, 0, 1);              host.regV(lfoPw[li], toLfo);
        lfoCentered[li].set(L + "Centered" + X, true);            host.regB(lfoCentered[li], toLfo);
        // Gain: the shape stretched around its middle and clipped at 0 / 1 (noise
        // rarely reaches them), before S&H, Pow and BiPow
        vec(lfoGain[li], L + "Gain" + X, 1, 1, 16);                host.regV(lfoGain[li], toLfo);
        vec(lfoPow[li], L + "Pow" + X, 0, -1, 1);                 host.regV(lfoPow[li], toLfo);
        vec(lfoBiPow[li], L + "BiPow" + X, 0, -1, 1);             host.regV(lfoBiPow[li], toLfo);
        lfoNoise[li].set(L + "Noise" + X, false);                 host.regB(lfoNoise[li], toLfo);
        lfoSHGlide[li].set(L + "S&H Glide" + X, 0.0f, 0.0f, 10.0f); host.regF(lfoSHGlide[li], toLfo);
        lfoNScale[li].set(L + "Noise Scale" + X, 1.0f, 0.0f, 10.0f); host.regF(lfoNScale[li], toLfo);
        // Random: each voice a new value per cycle (at its own phase): another SynthDef
        auto toRnd = [this, toLfo]() { forStates([this](ofxSCServer* sv, ServerState& st){ updateSources(sv, st); }); toLfo(); };
        lfoRndType[li].set(L + "Random Type" + X, 0, 0, 3);      host.regI(lfoRndType[li], toRnd);
        lfoRndSmooth[li].set(L + "Random Smooth" + X, 0.0f, 0.0f, 10.0f); host.regF(lfoRndSmooth[li], toLfo);
        // each voice its own random values unless asked for one value for all
        lfoRndSame[li].set(L + "Random Same" + X, false);        host.regB(lfoRndSame[li], toLfo);
        if(unifiedLfoModels){
            lfoModel[li].set(L + "Model" + X, 1, 0, 3); host.regI(lfoModel[li], toRnd);
            lfoManual[li].set(L + "Value" + X, 0.0f, 0.0f, 1.0f); host.regF(lfoManual[li], toLfo);
        }
        // Steps: the output quantized to that many levels (0: smooth; server LFOs: ModLfoX)
        lfoSteps[li].set(L + "Steps" + X, 0, 0, 32);              host.regI(lfoSteps[li], toRnd);
        auto phases = [this]() { updatePhases(); notify(); };
        if(grainExtras){
            lfoRegion[li].set(L + "Smp" + X, false);          host.regB(lfoRegion[li], toLfo);
        }
        if(partialLfos){
            lfoPSpread[li].set(L + "Partial Spread" + X, 0.0f, -8.0f, 8.0f); host.regF(lfoPSpread[li], [this]() { notify(); });
            lfoPRand[li].set(L + "Partial Random" + X, 0.0f, 0.0f, 1.0f);  host.regF(lfoPRand[li], [this]() { notify(); });
        }
        lfoNWaves[li].set(L + "Spread" + X, 1.0f, 0.0f, (float)maxVoices);   host.regF(lfoNWaves[li], phases);
        lfoNorm[li].set(L + "Normalize" + X, false);              host.regB(lfoNorm[li], phases);
        lfoInvert[li].set(L + "Invert" + X, 0.0f, 0.0f, 1.0f);    host.regF(lfoInvert[li], phases);
        lfoSym[li].set(L + "Symmetry" + X, 0, 0, maxVoices / 2);              host.regI(lfoSym[li], phases);
        lfoShuffle[li].set(L + "Shuffle" + X, 0.0f, 0.0f, 1.0f);  host.regF(lfoShuffle[li], phases);
        lfoRand[li].set(L + "Random" + X, 0.0f, 0.0f, 1.0f);      host.regF(lfoRand[li], phases);
        lfoOffset[li].set(L + "Offset" + X, 0.0f, -maxVoices * 0.5f, maxVoices * 0.5f);   host.regF(lfoOffset[li], phases);
        lfoQuant[li].set(L + "Quantize" + X, maxVoices, 1, maxVoices);  host.regI(lfoQuant[li], phases);   // max: every voice its own step
        lfoComb[li].set(L + "Combination" + X, 0.0f, 0.0f, 1.0f); host.regF(lfoComb[li], phases);
        lfoModulo[li].set(L + "Modulo" + X, maxVoices, 1, maxVoices);   host.regI(lfoModulo[li], phases);  // max: no wrap
        auto& ix = lfoIndexers[li];
        ix.numWaves_Param = 1.0f; ix.normalize_Param = false; ix.indexInvert_Param = 0.0f; ix.symmetry_Param = 0;
        ix.wrapShuffle_Param = false; ix.indexShuffle_Param = 0.0f; ix.indexRand_Param = 0.0f; ix.indexOffset_Param = 0.0f;
        ix.indexQuant_Param = n(); ix.combination_Param = 0.0f; ix.modulo_Param = n(); ix.discrete_Param = false;
        ix.indexCountChanged(n());
    }
    for(int e = 0; e < MAX_ENVS; e++){
        auto& E = envs[e];
        const std::string L = "Env" + ofToString(e + 1) + " ";
        auto toEnv = [this, e]() { forStates([this, e](ofxSCServer*, ServerState& st){ sendEnv(st, e); }); };
        E.a.set(L + "Attack" + nameSuffix, 0.01f, 0.001f, 10.0f);          host.regF(E.a, toEnv);
        E.d.set(L + "Decay" + nameSuffix, 0.5f, 0.001f, 10.0f);            host.regF(E.d, toEnv);
        E.s.set(L + "Sustain" + nameSuffix, ofClamp(defaultEnvSustain, 0.0f, 1.0f), 0.0f, 1.0f); host.regF(E.s, toEnv);
        E.r.set(L + "Release" + nameSuffix, 0.5f, 0.001f, 20.0f);          host.regF(E.r, toEnv);
        E.ca.set(L + "Attack Curve" + nameSuffix, -4.0f, -12.0f, 12.0f);   host.regF(E.ca, toEnv);
        E.cd.set(L + "Decay Curve" + nameSuffix, -4.0f, -12.0f, 12.0f);    host.regF(E.cd, toEnv);
        E.cr.set(L + "Release Curve" + nameSuffix, -4.0f, -12.0f, 12.0f);  host.regF(E.cr, toEnv);
        E.vt.set(L + "Vel" + nameSuffix, 0.0f, 0.0f, 1.0f);                host.regF(E.vt, toEnv);
    }
    for(int k = 0; k < MAX_EXT; k++){
        vec(exts[k], "Ext " + ofToString(k + 1) + nameSuffix, 0, -1, 1);
        host.regV(exts[k], [this]() { forStates([this](ofxSCServer* sv, ServerState& st){ writeSources(sv, st); }); });
    }
    // the module's own controls: never targets of "Modulate"
    auto own = [this](const ofAbstractParameter& q){ if(!q.getName().empty()) ownKeys.insert(q.getEscapedName()); };
    for(int li = 0; li < MAX_LFOS; li++){
        for(auto* q : std::initializer_list<ofAbstractParameter*>{&lfoSync[li], &lfoBeats[li], &lfoHz[li], &lfoSH[li],
                &lfoDetune[li], &lfoInit[li], &lfoRound[li], &lfoSkew[li], &lfoPw[li], &lfoCentered[li], &lfoGain[li],
                &lfoPow[li], &lfoBiPow[li], &lfoNoise[li], &lfoSHGlide[li], &lfoNScale[li], &lfoRegion[li], &lfoSteps[li],
                &lfoPSpread[li], &lfoPRand[li], &lfoNWaves[li], &lfoNorm[li], &lfoInvert[li], &lfoSym[li], &lfoShuffle[li],
                &lfoRand[li], &lfoOffset[li], &lfoQuant[li], &lfoComb[li], &lfoModulo[li], &lfoRndType[li], &lfoRndSmooth[li], &lfoRndSame[li]}) own(*q);
        if(unifiedLfoModels){ own(lfoModel[li]); own(lfoManual[li]); }
    }
    for(auto& E : envs) for(auto* q : std::initializer_list<ofAbstractParameter*>{&E.a, &E.d, &E.s, &E.r, &E.ca, &E.cd, &E.cr, &E.vt}) own(*q);
    for(auto& e : exts) own(e);
    updatePhases();
}

// ════════════════════════════════════════════════════════════════════════════
// Server side
// ════════════════════════════════════════════════════════════════════════════

void scModulation::forStates(const std::function<void(ofxSCServer*, ServerState&)>& f) {
    // A new voice count can precede its rebuild: never resize controls on the
    // old definitions, including LFO phase arrays sent by voicesChanged().
    for(auto& [server, st] : states) if(st.created && st.builtVoices == n()) f(server, st);
}
int scModulation::targetBus(ofxSCServer* server, int t) const {
    auto it = states.find(server);
    return (it != states.end() && it->second.modBlock) ? it->second.modBlock->index + t * busWidth : 0;
}
int scModulation::sourceBus(ofxSCServer* server, int s) const {
    auto it = states.find(server);
    return (it != states.end() && it->second.srcBlock) ? it->second.srcBlock->index + s * busWidth : 0;
}

void scModulation::sendVec(ofxSCSynth* s, const std::string& name, const std::vector<float>& v) {
    if(!s) return;
    if(v.size() <= 1) s->setMultiple(name, first(v), n());
    else s->set(name, expand(v, n()));
}

std::vector<float> scModulation::lfoHzFor(int li) const {
    const float bps = std::max(1.0f, currentBpm) / 60.0f;
    if(grainExtras && lfoRegion[li].get()){
        // a cycle = Beats lengths of the region
        const float region = regionSec ? std::max(0.001f, regionSec()) : 1.0f;
        std::vector<float> hz = expand(lfoBeats[li].get(), n(), 1.0f);
        for(auto& v : hz) v = 1.0f / (std::max(v, 0.001f) * region);
        return hz;
    }
    std::vector<float> hz = expand(lfoSync[li].get() ? lfoBeats[li].get() : lfoHz[li].get(), n(), 1.0f);
    if(lfoSync[li].get()) for(auto& v : hz) v = bps / std::max(v, 0.001f);
    return hz;
}
std::vector<float> scModulation::lfoShFor(int li) const {
    const float bps = std::max(1.0f, currentBpm) / 60.0f;
    std::vector<float> sh = expand(lfoSH[li].get(), n(), 0.0f);
    if(lfoSync[li].get()) for(auto& v : sh) v *= bps;
    return sh;
}

void scModulation::sendLfo(ServerState& st, int li) {
    if(st.builtVoices != n()) return;
    auto* s = st.lfo[li];
    if(!s) return;
    s->set("out", st.srcBlock ? st.srcBlock->index + li * busWidth : 0);
    s->set("freq", lfoHzFor(li));
    s->set("sh", lfoShFor(li));
    sendVec(s, "freqdetune", perVoice ? lfoDetune[li].get() : std::vector<float>{0.0f});
    s->set("initphase", lfoInit[li].get());
    sendVec(s, "round", lfoRound[li].get());
    sendVec(s, "skew", lfoSkew[li].get());
    sendVec(s, "pw", lfoPw[li].get());
    s->set("centered", lfoCentered[li].get() ? 1.0f : 0.0f);
    sendVec(s, "gain", lfoGain[li].get());
    sendVec(s, "pow", lfoPow[li].get());
    sendVec(s, "bipow", lfoBiPow[li].get());
    s->set("noise", effectiveNoise(li) ? 1.0f : 0.0f);
    s->set("shglide", lfoSHGlide[li].get());
    s->set("nscale", perVoice ? lfoNScale[li].get() : 0.0f);
    s->set("seed", li * 17.31f);
    s->set("phoff", perVoice ? expand(lfoPhases[li], n()) : std::vector<float>(n(), 0.0f));
    s->set("reset", (float)resetCounter);
    s->set("rtype", (float)effectiveRandom(li));     // ModLfoX only
    s->set("steps", (float)lfoSteps[li].get());
    s->set("rsmooth", lfoRndSmooth[li].get());
    s->set("rshare", perVoice && !lfoRndSame[li].get() ? 0.0f : 1.0f);   // ModLfoX only
    s->set("hold", manualLfo(li) ? 1.0f : 0.0f);
    if(manualLfo(li)){
        const std::vector<float> zero(n(),0.0f), one(n(),1.0f), half(n(),0.5f);
        s->set("freq",zero); s->set("freqdetune",zero); s->set("phoff",zero);
        s->set("initphase",lfoManual[li].get()*0.5f);
        s->set("round",zero); s->set("skew",zero); s->set("pw",half);
        s->set("centered",1.0f); s->set("noise",0.0f); s->set("rtype",0.0f);
        s->set("gain",one); s->set("pow",zero); s->set("bipow",zero);
        s->set("sh",zero); s->set("steps",0.0f);
    }
}

// One value per cycle, from a hash of the LFO and the cycle (no state): the
// cards' pictures of a random LFO, two cycles wide
float scModulation::rndPreview(int li, float ph, int type, float smooth) {
    auto value = [&](int cycle){ const float x = std::sin(cycle * 12.9898f + li * 78.233f) * 43758.5453f; return x - std::floor(x); };
    const float pos = ph * 2.0f;
    const int c = (int)std::floor(pos);
    const float f = pos - c;
    float stepped = value(c);
    if(smooth > 0.0f) stepped = value(c - 1) + (stepped - value(c - 1)) * (1.0f - std::exp(-f * 0.5f / std::max(1e-3f, smooth)));
    const float v0 = value(c - 2), v1 = value(c - 1), v2 = value(c), v3 = value(c + 1);
    const float cubic = ofClamp(v1 + 0.5f * f * ((v2 - v0) + f * ((2 * v0 - 5 * v1 + 4 * v2 - v3) + f * (3 * (v1 - v2) + v3 - v0))), 0.0f, 1.0f);
    if(type == 1) return stepped;
    if(type == 2) return cubic;
    const float j = stepped + cubic - 1.0f;
    return j - std::floor(j);
}

void scModulation::sendEnv(ServerState& st, int e) {
    if(st.builtVoices != n()) return;
    auto* s = st.env[e];
    if(!s) return;
    auto& E = envs[e];
    s->set("gate", st.gateBus);
    s->set("out", st.srcBlock ? st.srcBlock->index + (SRC_ENV + e) * busWidth : 0);
    s->setMultiple("atk", E.a.get(), n()); s->setMultiple("dec", E.d.get(), n()); s->setMultiple("sus", E.s.get(), n()); s->setMultiple("rel", E.r.get(), n());
    s->setMultiple("cA", E.ca.get(), n()); s->setMultiple("cD", E.cd.get(), n()); s->setMultiple("cR", E.cr.get(), n());
    // velocity tracking: the note's velocity (the velocity source, written by the node's gates)
    s->set("velbus", st.srcBlock ? st.srcBlock->index + SRC_VEL * busWidth : 0);
    s->set("veltrack", velocity ? E.vt.get() : 0.0f);
}

void scModulation::writeSources(ofxSCServer* server, ServerState& st) {
    if(!st.srcBlock) return;
    auto setn = [&](int bus, const std::vector<float>& values){
        ofxOscMessage m;
        m.setAddress("/c_setn");
        m.addIntArg(bus);
        m.addIntArg((int)values.size());
        for(float v : values) m.addFloatArg(v);
        server->sendMsg(m);
    };
    for(int k = 0; k < MAX_EXT; k++){
        std::vector<float> v = expand(exts[k].get(), busWidth, 0.0f);
        for(auto& x : v) x = ofClamp(x, -1.0f, 1.0f);
        setn(st.srcBlock->index + (SRC_EXT + k) * busWidth, v);
    }
    std::vector<float> voice(busWidth, 0.0f);
    for(int i = 0; i < n(); i++) voice[i] = n() > 1 ? (float)i / (n() - 1) * 2.0f - 1.0f : 0.0f;
    setn(st.srcBlock->index + SRC_VOICE * busWidth, voice);
}

void scModulation::updateSources(ofxSCServer* server, ServerState& st) {
    if(st.builtVoices != n()) return;
    if(!st.created) return;
    for(int li = 0; li < MAX_LFOS; li++){
        auto*& s = st.lfo[li];
        const std::string wanted = effectiveRandom(li) > 0 || lfoSteps[li].get() >= 2 ? "ModLfoX" : "ModLfo";
        if(s && st.lfoDef[li] != wanted){ s->free(); delete s; s = nullptr; }   // its Random type changed
        if(li < lfoCount && !s){
            st.lfoDef[li] = wanted;
            s = new ofxSCSynth(def(wanted), server);
            scStartConfiguredSynth(s, server, 1, st.sourceGroup, [&](){ sendLfo(st, li); });
        }else if(li >= lfoCount && s){ s->free(); delete s; s = nullptr; }
    }
    for(int e = 0; e < MAX_ENVS; e++){
        auto*& s = st.env[e];
        if(e < envCount && !s){
            s = new ofxSCSynth(def("ModEnv"), server);
            scStartConfiguredSynth(s, server, 1, st.sourceGroup, [&](){ sendEnv(st, e); });
        }else if(e >= envCount && s){ s->free(); delete s; s = nullptr; }
    }
}

void scModulation::syncRoutes(ofxSCServer* server, ServerState& st) {
    if(st.builtVoices != n()) return;
    if(!st.created || !st.modBlock || !st.srcBlock) return;
    std::vector<bool> routed(T(), false);
    for(int s = 0; s < sourceCount(); s++){
        for(int t = 0; t < T(); t++){
            const int idx = s * T() + t;
            const float amount = matrix[idx];
            const bool used = sourceUsed(s) && std::abs(amount) > 1e-4f && !(skipRoute && skipRoute(s, t));
            auto it = st.routes.find(idx);
            if(used){
                routed[t] = true;
                if(it == st.routes.end()){
                    auto* r = new ofxSCSynth(def("ModRoute"), server);
                    r->set("src", st.srcBlock->index + s * busWidth);
                    r->set("out", st.modBlock->index + t * busWidth);
                    r->set("bipolar", s < MAX_LFOS && !targetList[t].unipolar ? 1.0f : 0.0f);
                    r->set("scale", targetList[t].range);
                    r->set("amount", amount);
                    r->set("decor", s == SRC_RND && !randomShared ? 1.0f : 0.0f);
                    r->set("salt", t * 7.31f + 1.0f);
                    r->createAndRun(1, st.routeGroup, true);
                    st.routes[idx] = r;
                }else{
                    it->second->set("amount", amount);
                    if(s == SRC_RND) it->second->set("decor", randomShared ? 0.0f : 1.0f);
                }
            }else if(it != st.routes.end()){
                it->second->free();
                delete it->second;
                st.routes.erase(it);
            }
        }
    }
    // a target with no route keeps its last value on its bus: zero it (and the spare bus)
    for(int t = 0; t <= T(); t++){
        if(t < T() && routed[t]) continue;
        ofxOscMessage m;
        m.setAddress("/c_fill");
        m.addIntArg(st.modBlock->index + t * busWidth);
        m.addIntArg(busWidth);
        m.addFloatArg(0.0f);
        server->sendMsg(m);
    }
}

void scModulation::create(ofxSCServer* server, int sourceGroup, int routeGroup, int gateBus) {
    auto& st = states[server];
    st.sourceGroup = sourceGroup;
    st.routeGroup = routeGroup;
    st.gateBus = gateBus;
    auto control = [&](ofxSCBus*& b, int channels){
        if(liveControlBus(server, b)) return;
        delete b;
        b = new ofxSCBus(RATE_CONTROL, channels, server);
        if(!liveControlBus(server, b)){ b->free(); delete b; b = nullptr; }
    };
    control(st.srcBlock, sourceCount() * busWidth);
    control(st.modBlock, (T() + 1) * busWidth);
    st.created = true;
    st.builtVoices = n();
    updatePhases();          // as many offsets as voices now
    writeSources(server, st);
    updateSources(server, st);
    syncRoutes(server, st);
}

void scModulation::free(ofxSCServer* server, bool sendFree) {
    auto it = states.find(server);
    if(it == states.end()) return;
    auto& st = it->second;
    auto drop = [&](ofxSCSynth*& s){ if(s){ if(sendFree) s->free(); delete s; s = nullptr; } };
    for(auto*& s : st.lfo) drop(s);
    for(auto*& s : st.env) drop(s);
    for(auto& [idx, r] : st.routes){ if(sendFree) r->free(); delete r; }
    st.routes.clear();
    auto bus = [&](ofxSCBus*& b){ if(b && liveControlBus(server, b)) b->free(); delete b; b = nullptr; };
    bus(st.srcBlock);
    bus(st.modBlock);
    states.erase(it);
}

bool scModulation::hasVoiceBank(ofxSCServer* server,int voices) const {
    auto it=states.find(server);
    return it!=states.end() && it->second.created && it->second.builtVoices==voices;
}

bool scModulation::adoptServerState(scModulation& source, ofxSCServer* server) {
    if(&source==this || !source.hasVoiceBank(server,n())) return false;
    auto it=source.states.find(server);
    free(server);
    states.emplace(server,std::move(it->second));
    source.states.erase(it);
    live=source.live;
    lfoPhases=source.lfoPhases;
    for(auto& pt:params) {
        auto match=std::find_if(source.params.begin(),source.params.end(),[&](const ParamTarget& other){return other.key==pt.key;});
        if(match!=source.params.end()) pt.voiceWritten=match->voiceWritten;
    }
    return true;
}

void scModulation::rebuild(ofxSCServer* server) {
    auto it = states.find(server);
    if(it == states.end() || !it->second.created) return;
    auto& st = it->second;
    for(int li = 0; li < MAX_LFOS; li++) lfoIndexers[li].indexCountChanged(n());
    for(auto*& s : st.lfo) if(s){ s->free(); delete s; s = nullptr; }
    for(auto*& s : st.env) if(s){ s->free(); delete s; s = nullptr; }
    for(auto& [idx, r] : st.routes){ r->free(); delete r; }
    st.routes.clear();
    st.builtVoices = n();
    updatePhases();
    writeSources(server, st);
    updateSources(server, st);
    syncRoutes(server, st);
}

void scModulation::voicesChanged() {
    for(int li = 0; li < MAX_LFOS; li++) lfoIndexers[li].indexCountChanged(n());
    updatePhases();
}

void scModulation::setGateBus(ofxSCServer* server, int gateBus) {
    auto it = states.find(server);
    if(it == states.end()) return;
    it->second.gateBus = gateBus;
    for(int e = 0; e < MAX_ENVS; e++) sendEnv(it->second, e);
}

void scModulation::resend() {
    forStates([this](ofxSCServer* server, ServerState& st){
        for(int li = 0; li < MAX_LFOS; li++) sendLfo(st, li);
        for(int e = 0; e < MAX_ENVS; e++) sendEnv(st, e);
        writeSources(server, st);
        syncRoutes(server, st);
    });
}

void scModulation::setBpm(float bpm) {
    if(bpm != currentBpm){ currentBpm = bpm; notify(); }
    forStates([this](ofxSCServer*, ServerState& st){
        for(int li = 0; li < lfoCount; li++) if(lfoSync[li].get() && st.lfo[li]){
            st.lfo[li]->set("freq", lfoHzFor(li));
            st.lfo[li]->set("sh", lfoShFor(li));
        }
    });
}

void scModulation::resetLfos() {
    resetCounter++;
    notify();
    forStates([this](ofxSCServer*, ServerState& st){ for(auto* l : st.lfo) if(l) l->set("reset", (float)resetCounter); });
}

// A published key of this instance (GrainBox: every engine has one, its names end in " 2", " 3"...)
bool scModulation::ownsKey(const std::string& key) const {
    std::string sfx = nameSuffix;
    std::replace(sfx.begin(), sfx.end(), ' ', '_');
    if(!sfx.empty()) return key.size() >= sfx.size() && key.compare(key.size() - sfx.size(), sfx.size(), sfx) == 0;
    const size_t u = key.find_last_of('_');
    if(u == std::string::npos || u + 1 >= key.size()) return true;
    return !std::all_of(key.begin() + u + 1, key.end(), [](char c){ return std::isdigit((unsigned char)c); });
}

void scModulation::sendMatrix() {
    forStates([this](ofxSCServer* sv, ServerState& st){ syncRoutes(sv, st); });
    notify();
}

// ── The LFOs for a node's own copies ─────────────────────────────────────────
std::vector<float> scModulation::lfoTable(int li, int size) const {
    if(manualLfo(li)) return std::vector<float>(std::max(2,size),lfoManual[li].get());
    const float rnd = first(lfoRound[li].get(), 0.5f), sk = first(lfoSkew[li].get()), pw = first(lfoPw[li].get(), 0.5f);
    const float pw2 = first(lfoPow[li].get()), bp = first(lfoBiPow[li].get()), gain = first(lfoGain[li].get(), 1.0f);
    const std::array<float, 9> key{rnd, sk, pw, pw2, bp, gain,
        (effectiveNoise(li) ? 1.0f : 0.0f) + effectiveRandom(li) * 2.0f + lfoRndSmooth[li].get() * 10.0f, lfoCentered[li].get() ? 1.0f : 0.0f,
        (float)lfoSteps[li].get()};
    auto& cache = cycleCache[li];
    const int count = std::max(2, size);
    if(cache.values.size() == (size_t)count && cache.key == key) return cache.values;
    std::vector<float> t(count);
    const float seed = li * 17.31f;
    for(int i = 0; i < (int)t.size(); i++){
        const float ph = float(i) / (t.size() - 1);
        float v;
        // noise: 4 Perlin steps per cycle, crossfaded into the next cycle's start so it loops
        if(effectiveRandom(li) > 0) v = rndPreview(li, ph, effectiveRandom(li), lfoRndSmooth[li].get());
        else if(effectiveNoise(li)) v = perlin(ph * 4.0f, seed) * (1.0f - ph) + perlin(ph * 4.0f - 4.0f, seed) * ph;
        else v = morph(ph, rnd, sk, pw, lfoCentered[li].get());
        v = ofClamp((v - 0.5f) * gain + 0.5f, 0.0f, 1.0f);
        if(std::abs(pw2) > 1e-4f) v = reflectUnit(customPow(v, pw2));
        if(std::abs(bp) > 1e-4f) v = biPow(v, bp);
        if(lfoSteps[li].get() >= 2){
            const float q = (float)lfoSteps[li].get();
            v = std::min(std::floor(v * q), q - 1.0f) / (q - 1.0f);
        }
        t[i] = ofClamp(v, 0.0f, 1.0f);
    }
    t.back() = t.front();
    cache.key = key;
    cache.values = t;
    return t;
}
float scModulation::lfoVoiceOffset(int li, int voice) const {
    return perVoice && voice >= 0 && voice < (int)lfoPhases[li].size() ? lfoPhases[li][voice] : 0.0f;
}
std::vector<float> scModulation::lfoVoiceOffsets(int li) const {
    return perVoice ? expand(lfoPhases[li], n()) : std::vector<float>(n(), 0.0f);
}
float scModulation::lfoSampleHold(int li) const {
    const auto sh = lfoShFor(li);
    return sh.empty() ? 0.0f : sh[0];
}
scModulation::EnvValues scModulation::envValues(int e) const {
    const auto& E = envs[e];
    return {E.a.get(), E.d.get(), E.s.get(), E.r.get(), E.ca.get(), E.cd.get(), E.cr.get()};
}
std::vector<float> scModulation::extValues(int k) const {
    std::vector<float> v = expand(exts[k].get(), n(), 0.0f);
    for(auto& x : v) x = ofClamp(x, -1.0f, 1.0f);
    return v;
}
float scModulation::lfoRate(int li, int voice) const {
    const auto hz = lfoHzFor(li);
    return hz.empty() ? 1.0f : hz[std::clamp(voice, 0, (int)hz.size() - 1)];
}
float scModulation::lfoPhase(int li, int voice) const {
    const float off = perVoice && voice < (int)lfoPhases[li].size() ? lfoPhases[li][voice] : 0.0f;
    return lfoInit[li].get() + off;
}

void scModulation::updatePhases() {
    for(int li = 0; li < MAX_LFOS; li++){
        auto& ix = lfoIndexers[li];
        // the voice count can change before the node creates anything: the
        // indexer always as wide as the voices (else every voice the same phase)
        if((int)ix.getIndexs().size() != n()) ix.indexCountChanged(n());
        ix.numWaves_Param = lfoNWaves[li].get();
        ix.normalize_Param = lfoNorm[li].get();
        ix.indexInvert_Param = lfoInvert[li].get();
        ix.symmetry_Param = std::min(lfoSym[li].get(), n() / 2);
        ix.indexOffset_Param = lfoOffset[li].get();
        ix.indexQuant_Param = lfoQuant[li].get() >= lfoQuant[li].getMax() ? n() : std::clamp(lfoQuant[li].get(), 1, n());
        ix.combination_Param = lfoComb[li].get();
        ix.modulo_Param = lfoModulo[li].get() >= lfoModulo[li].getMax() ? n() : std::clamp(lfoModulo[li].get(), 1, n());
        if(ix.indexShuffle_Param != lfoShuffle[li].get()){ ix.indexShuffle_Param = lfoShuffle[li].get(); ix.indexShuffleChanged(ix.indexShuffle_Param); }
        if(ix.indexRand_Param != lfoRand[li].get()){ ix.indexRand_Param = lfoRand[li].get(); ix.indexRandChanged(ix.indexRand_Param); }
        ix.recomputeIndexs();
        lfoPhases[li] = expand(ix.getIndexs(), n());
        forStates([this, li](ofxSCServer*, ServerState& st){
            if(st.lfo[li]) st.lfo[li]->set("phoff", perVoice ? lfoPhases[li] : std::vector<float>(n(), 0.0f));
        });
    }
}

void scModulation::update() {
    applyParams();
    const bool forParams = paramsActive() && !liveHook;
    if(!showing && !forParams && !hasLfoOutputs()){ live.clear(); return; }
    // Read at 30 Hz for the editor or published output pins; every frame
    // while the sources move a control
    const float now = ofGetElapsedTimef();
    if(now < nextLiveRead && !forParams){ updateLfoOutputs(); return; }
    nextLiveRead = now + 1.0f / 30.0f;
    live.clear();
    for(auto& [server, st] : states){
        if(!st.created || !liveControlBus(server, st.srcBlock)) continue;
        live = st.srcBlock->readValues;
        // The source block reserves 16 channels for every possible source,
        // but the editor and parameter targets only use active sources and
        // the current voice count. Avoid a large /c_setn reply each refresh.
        for(int s = 0; s < sourceCount(); s++)
            if(sourceUsed(s)) server->requestControlValues(st.srcBlock->index + s * busWidth, n());
        break;
    }
    updateLfoOutputs();
}

scModulation::~scModulation() {
    // A removed pad / engine must also release its output pins. The containing
    // node owns its layout notification and is still alive here.
    if(host.node) for(int li = 0; li < MAX_LFOS; ++li)
        if(lfoOutputPins[li]) host.node->removeParameter(lfoOutputs[li].getName());
}

bool scModulation::hasLfoOutputs() const {
    for(const auto& pin : lfoOutputPins) if(pin) return true;
    return false;
}

bool scModulation::publishLfoOutput(int li, bool publish) {
    if(!host.node || li < 0 || li >= std::min((int)MAX_LFOS, maxLfos)) return false;
    auto& pin = lfoOutputPins[li];
    if(bool(pin) == publish) return false;
    if(publish){
        lfoOutputs[li].set("LFO " + ofToString(li + 1) + " Out" + nameSuffix,
                          std::vector<float>(perVoice ? n() : 1, 0.0f),
                          std::vector<float>{0.0f}, std::vector<float>{1.0f});
        pin = host.node->addOutputParameter(lfoOutputs[li],
            ofxOceanodeParameterFlags_DisableSavePreset | ofxOceanodeParameterFlags_DisableSaveProject);
        nextLiveRead = 0.0f;
        updateLfoOutputs();
    }else{
        if(pin->hasOutConnections()) return false;
        host.node->removeParameter(lfoOutputs[li].getName());
        pin.reset();
    }
    host.node->parameterGroupChanged.notify(host.node);
    return true;
}

void scModulation::updateLfoOutputs() {
    for(int li = 0; li < MAX_LFOS; ++li){
        if(!lfoOutputPins[li]) continue;
        // Match the top display, including AdditiveBox's per-partial bars.
        auto values = li < lfoCount && partialBars ? partialBars(li) : std::vector<float>{};
        if(values.empty()){
            values.resize(perVoice ? n() : 1, 0.0f);
            if(li < lfoCount) for(int v = 0; v < (int)values.size(); ++v) values[v] = liveValue(li, v);
        }
        for(auto& value : values) value = std::isfinite(value) ? ofClamp(value, 0.0f, 1.0f) : 0.0f;
        lfoOutputs[li].set(values);
    }
}

void scModulation::saveLfoOutputs(ofJson& j) const {
    auto& list = j["modulationLfoOutputs"][nameSuffix] = ofJson::array();
    for(int li = 0; li < MAX_LFOS; ++li) if(lfoOutputPins[li]) list.push_back(li);
}

void scModulation::loadLfoOutputs(const ofJson& j) {
    std::set<int> wanted;
    if(j.contains("modulationLfoOutputs") && j["modulationLfoOutputs"].is_object()){
        const auto& outputs = j["modulationLfoOutputs"];
        if(outputs.contains(nameSuffix) && outputs[nameSuffix].is_array())
            for(const auto& li : outputs[nameSuffix]) if(li.is_number_integer()) wanted.insert(li.get<int>());
    }
    for(int li = 0; li < std::min((int)MAX_LFOS, maxLfos); ++li) publishLfoOutput(li, wanted.count(li) != 0);
}

float scModulation::liveValue(int source, int voice) const {
    if(liveHook) return liveHook(source, voice);
    const int i = source * busWidth + voice;
    return i < (int)live.size() ? live[i] : 0.0f;
}

// ════════════════════════════════════════════════════════════════════════════
// Sources, presets
// ════════════════════════════════════════════════════════════════════════════

std::string scModulation::sourceName(int s) const {
    if(s >= NUM_SOURCES) return s < sourceCount() ? extraSources[s - NUM_SOURCES] : "";
    if(s < MAX_LFOS) return "LFO " + ofToString(s + 1);
    if(s < SRC_VEL) return "Env " + ofToString(s - SRC_ENV + 1);
    if(s == SRC_VEL) return "Velocity";
    if(s == SRC_RND) return "Random";
    if(s < SRC_VOICE) return "Ext " + ofToString(s - SRC_EXT + 1);
    return "Voice";
}
bool scModulation::sourceUsed(int s) const {
    if(s >= NUM_SOURCES) return s < sourceCount();
    if(s < MAX_LFOS) return s < lfoCount;
    if(s < SRC_VEL) return s - SRC_ENV < envCount;
    if(s == SRC_VEL) return velocity;
    if(s == SRC_RND) return random;
    if(s < SRC_VOICE) return s - SRC_EXT < extCount;
    return n() > 1;
}
ImU32 scModulation::sourceColor(int s) const {
    if(s >= NUM_SOURCES) return pal::lfo[(s - NUM_SOURCES + 3) % MAX_LFOS];
    if(s < MAX_LFOS) return pal::lfo[s];
    if(s < SRC_VEL) return pal::env[s - SRC_ENV];
    return pal::headerText;
}

void scModulation::save(ofJson& j, const std::string& prefix) const {
    j[prefix + "Matrix"] = matrix;
    j[prefix + "Lfos"] = lfoCount;
    j[prefix + "Envs"] = envCount;
    j[prefix + "Exts"] = extCount;
    j[prefix + "Params"] = paramsJson();
    j[prefix + "RandomShared"] = randomShared;
}

std::vector<float> scModulation::fitMatrix(const std::vector<float>& m, int sources, int targets) {
    if(sources <= 0 || (int)m.size() == sources * targets || m.empty() || m.size() % sources != 0) return m;
    const int old = (int)m.size() / sources;
    if(old > targets) return m;
    std::vector<float> out(sources * targets, 0.0f);
    for(int s = 0; s < sources; s++) for(int t = 0; t < old; t++) out[s * targets + t] = m[s * old + t];
    return out;
}

void scModulation::load(const ofJson& j, const std::string& prefix) {
    auto count = [&](const std::string& k, int& v, int lo, int hi){
        if(j.contains(prefix + k) && j[prefix + k].is_number()) v = std::clamp(j[prefix + k].get<int>(), lo, hi);
    };
    count("Lfos", lfoCount, 0, std::min((int)MAX_LFOS, maxLfos));   // the node's own limits (GrainBox: 4 / 2 / 4)
    count("Envs", envCount, 0, std::min((int)MAX_ENVS, maxEnvs));
    count("Exts", extCount, 0, std::min((int)MAX_EXT, maxExts));
    if(j.contains(prefix + "Matrix") && j[prefix + "Matrix"].is_array()){
        try {
            auto m = j[prefix + "Matrix"].get<std::vector<float>>();
            m = fitMatrix(m, sourceCount(), T());
            if((int)m.size() == sourceCount() * T()) matrix = m;
        } catch(...) {}
    }
    loadParamsJson(j.contains(prefix + "Params") ? j[prefix + "Params"] : ofJson::array());
    randomShared = j.value(prefix + "RandomShared", false);
}

void scModulation::afterLoad() {
    applyParamBases();
    notify();
    updatePhases();
    forStates([this](ofxSCServer* server, ServerState& st){ updateSources(server, st); });
    resend();
}

// ════════════════════════════════════════════════════════════════════════════
// Any control as a target
// ════════════════════════════════════════════════════════════════════════════

bool scModulation::resolveParam(const std::string& key, ParamTarget& pt) const {
    if(ownKeys.count(key) || targetOfKey.count(key) || !host.control) return false;
    ofAbstractParameter* q = host.control(key);
    if(!q) return false;
    const std::string t = q->valueType();
    if(t == typeid(float).name()){
        auto& f = q->cast<float>();
        pt.kind = 0; pt.lo = f.getMin(); pt.hi = f.getMax();
    }else if(t == typeid(int).name()){
        auto& f = q->cast<int>();
        pt.kind = 1; pt.lo = (float)f.getMin(); pt.hi = (float)f.getMax();
    }else if(t == typeid(std::vector<float>).name()){
        auto& f = q->cast<std::vector<float>>();
        pt.kind = 2; pt.lo = first(f.getMin(), 0.0f); pt.hi = first(f.getMax(), 1.0f);
    }else if(t == typeid(std::vector<int>).name()){
        auto& f = q->cast<std::vector<int>>();
        pt.kind = 3;
        pt.lo = f.getMin().empty() ? 0.0f : (float)f.getMin()[0];
        pt.hi = f.getMax().empty() ? 1.0f : (float)f.getMax()[0];
    }else return false;
    if(!(pt.hi > pt.lo) || !std::isfinite(pt.hi - pt.lo)) return false;
    pt.p = q;
    pt.key = key;
    pt.name = q->getName();
    return true;
}

std::vector<float> scModulation::readParam(const ParamTarget& pt) const {
    switch(pt.kind){
        case 0: return {pt.p->cast<float>().get()};
        case 1: return {(float)pt.p->cast<int>().get()};
        case 2: return pt.p->cast<std::vector<float>>().get();
        default: {
            std::vector<float> v;
            for(int x : pt.p->cast<std::vector<int>>().get()) v.push_back((float)x);
            return v;
        }
    }
}

void scModulation::writeParam(ParamTarget& pt, const std::vector<float>& v) {
    if(v.empty()) return;
    switch(pt.kind){
        case 0: pt.p->cast<float>().set(ofClamp(v[0], pt.lo, pt.hi)); break;
        case 1: pt.p->cast<int>().set((int)std::lround(ofClamp(v[0], pt.lo, pt.hi))); break;
        case 2: pt.p->cast<std::vector<float>>().set(v); break;
        default: {
            std::vector<int> iv;
            for(float x : v) iv.push_back((int)std::lround(x));
            pt.p->cast<std::vector<int>>().set(iv);
        }
    }
    pt.written = readParam(pt);
}

scModulation::ParamTarget* scModulation::findParam(const std::string& key) {
    for(auto& pt : params) if(pt.key == key) return &pt;
    return nullptr;
}

bool scModulation::isParamTarget(const std::string& key) const {
    for(auto& pt : params) if(pt.key == key) return true;
    return false;
}

float scModulation::morphVoiceValue(const ofParameter<float>& p,int voice,const scMorphPolicies& policies) const {
    const float current=voiceValue(p,voice);
    if(!policies.hasEffective(p))return current;
    float base=p.get();
    for(const auto& pt:params)if(pt.key==p.getEscapedName() && !pt.base.empty()){
        base=pt.base[size_t(std::max(0,voice))%pt.base.size()];break;
    }
    return policies.value(p)+(current-base)*policies.neutralGain(p);
}

float scModulation::voiceValue(const ofParameter<float>& p, int voice) const {
    for(const auto& pt : params)
        if(pt.key == p.getEscapedName() && !pt.voiceWritten.empty() && pt.written.size() == 1 && p.get() == pt.written[0])
            return pt.voiceWritten[ofClamp(voice, 0, int(pt.voiceWritten.size()) - 1)];
    return p.get();
}

std::vector<float> scModulation::voiceValues(const ofParameter<float>& p) const {
    std::vector<float> out(n(), p.get());
    for(const auto& pt : params){
        if(pt.key != p.getEscapedName() || pt.voiceWritten.empty() || pt.written.size() != 1 || p.get() != pt.written[0]) continue;
        for(size_t i = 0; i < out.size(); ++i) out[i] = pt.voiceWritten[std::min(i, pt.voiceWritten.size() - 1)];
        break;
    }
    return out;
}

// A source as it moves a control: LFOs, Random, Ext and Voice around the
// value (-0.5..0.5: amount 1 sweeps the whole range), envelopes and velocity up from it
float scModulation::paramSource(int s, int voice) const {
    if(s >= NUM_SOURCES) return liveValue(s, voice);    // a host's source: up from the value
    if(s < MAX_LFOS) return liveValue(s, perVoice ? voice : 0) - 0.5f;
    if(s < SRC_RND) return liveValue(s, voice);
    if(s == SRC_RND) return liveValue(s, voice) * 0.5f;
    if(s < SRC_VOICE){
        const auto& e = exts[s - SRC_EXT].get();
        return e.empty() ? 0.0f : ofClamp(e[std::min((size_t)voice, e.size() - 1)], -1.0f, 1.0f) * 0.5f;
    }
    return n() > 1 ? ((float)voice / (n() - 1) - 0.5f) : 0.0f;
}

bool scModulation::paramsActive() const {
    for(auto& pt : params)
        for(int s = 0; s < sourceCount(); s++) if(std::abs(pt.amount[s]) > 1e-4f && sourceUsed(s)) return true;
    return false;
}

void scModulation::applyParams() {
    const int nv = n();
    bool voicesDirty = false;
    applyParamBases();   // loaded ones (when the node did not call afterLoad)
    for(auto& pt : params){
        if(!pt.p) continue;
        const std::vector<float> cur = readParam(pt);
        // set by the user, a connection or a preset: that is the new base
        if(pt.applied && cur != pt.written) pt.applied = false;
        if(!pt.applied) pt.base = cur;
        bool on = false;
        for(int s = 0; s < sourceCount() && !on; s++) on = std::abs(pt.amount[s]) > 1e-4f && sourceUsed(s);
        if(!on || pt.base.empty()){
            if(!pt.voiceWritten.empty()) { pt.voiceWritten.clear(); voicesDirty = true; }
            if(pt.applied){ writeParam(pt, pt.base); pt.applied = false; }
            continue;
        }
        const bool voiceScalar = pt.kind == 0 && voiceParamKeys.count(pt.key);
        const size_t size = voiceScalar ? (size_t)nv : pt.kind >= 2 ? (pt.base.size() <= 1 ? (size_t)nv : pt.base.size()) : 1;
        const float span = pt.hi - pt.lo;
        std::vector<float> out(size);
        for(size_t i = 0; i < size; i++){
            const int voice = (int)(i % nv);
            float m = 0.0f;
            for(int s = 0; s < sourceCount(); s++)
                if(std::abs(pt.amount[s]) > 1e-4f && sourceUsed(s)){
                    // Random: this control's own value per note (salted by its key), unless shared
                    if(s == SRC_RND && !randomShared)
                        m += pt.amount[s] * 0.5f * decorrelate(liveValue(s, voice), float(std::hash<std::string>{}(pt.key) % 1000) * 0.913f + 50.0f);
                    else m += pt.amount[s] * paramSource(s, voice);
                }
            float v = ofClamp(pt.base[std::min(i, pt.base.size() - 1)] + m * span, pt.lo, pt.hi);
            if(pt.kind == 1 || pt.kind == 3) v = std::round(v);
            out[i] = v;
        }
        if(voiceScalar){
            if(out != pt.voiceWritten) { pt.voiceWritten = out; voicesDirty = true; }
            // Keep the scalar editor/connection value at its shared base.
            // Only the DSP reads the separate base + modulation per voice.
            pt.written = cur;
        }else if(out != cur) writeParam(pt, out); else pt.written = cur;
        pt.applied = true;
    }
    if(voicesDirty && host.voiceParamsChanged) host.voiceParamsChanged();
}

void scModulation::removeParam(size_t i) {
    if(i >= params.size()) return;
    auto& pt = params[i];
    const bool voice = !pt.voiceWritten.empty();
    pt.voiceWritten.clear();
    if(pt.applied && pt.p) writeParam(pt, pt.base);
    params.erase(params.begin() + i);
    if(voice && host.voiceParamsChanged) host.voiceParamsChanged();
}

void scModulation::patchBases(ofJson& controls) const {
    if(!controls.is_object()) return;
    for(auto& pt : params){
        if(!pt.applied || pt.base.empty() || !controls.contains(pt.key)) continue;
        ofJson& slot = controls[pt.key];
        if(pt.kind <= 1 || (pt.base.size() == 1 && !slot.is_array())){
            if(pt.kind == 1 || pt.kind == 3) slot = (int)std::lround(pt.base[0]); else slot = pt.base[0];
        }else if(pt.kind == 3){
            std::vector<int> iv;
            for(float x : pt.base) iv.push_back((int)std::lround(x));
            slot = iv;
        }else slot = pt.base;
    }
}

ofJson scModulation::paramsJson() const {
    ofJson a = ofJson::array();
    for(auto& pt : params){
        ofJson e;
        e["key"] = pt.key;
        e["base"] = pt.base;
        ofJson am = ofJson::object();
        for(int s = 0; s < sourceCount(); s++) if(std::abs(pt.amount[s]) > 1e-4f) am[ofToString(s)] = pt.amount[s];
        e["amounts"] = am;
        a.push_back(e);
    }
    return a;
}

void scModulation::loadParamsJson(const ofJson& j) {
    // the targets of before stop here (the loaded values are set by the node)
    params.clear();
    if(!j.is_array()) return;
    for(auto& e : j){
        if(!e.is_object() || !e.contains("key") || !e["key"].is_string()) continue;
        ParamTarget pt;
        if(!resolveParam(e["key"].get<std::string>(), pt)) continue;
        try { if(e.contains("base")) pt.base = e["base"].get<std::vector<float>>(); } catch(...) {}
        if(e.contains("amounts") && e["amounts"].is_object())
            for(auto it = e["amounts"].begin(); it != e["amounts"].end(); ++it){
                const int s = ofToInt(it.key());
                if(s >= 0 && s < sourceCount() && it.value().is_number()) pt.amount[s] = ofClamp(it.value().get<float>(), -1.0f, 1.0f);
            }
        pt.pending = !pt.base.empty();
        params.push_back(std::move(pt));
    }
}

void scModulation::applyParamBases() {
    for(auto& pt : params){
        if(!pt.pending) continue;
        pt.pending = false;
        pt.voiceWritten.clear();
        if(pt.p && !pt.base.empty()) writeParam(pt, pt.base);
        pt.applied = false;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Widgets (GrainBox's look)
// ════════════════════════════════════════════════════════════════════════════

void scModulation::sectionHeader(const char* title, float w, ImU32 accent) {
    const float z = zoom();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    const float bh = ImGui::GetFrameHeight() + 6.0f * z;
    dl->AddRectFilled(cur, ImVec2(cur.x + w, cur.y + bh), pal::header, 3.0f * z);
    dl->AddLine(ImVec2(cur.x, cur.y + bh), ImVec2(cur.x + w, cur.y + bh), accent, 1.5f * z);
    ImGui::SetCursorScreenPos(ImVec2(cur.x + 6.0f * z, cur.y + 3.0f * z));
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, pal::headerText);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(cur.x, cur.y + bh + 4.0f * z));
    ImGui::Dummy(ImVec2(w, 0.0f));
}

void scModulation::slider(ofParameter<float>& p, const char* label, float w, const char* fmt, bool log) {
    const float z = zoom();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if(host.publishedUnderline) host.publishedUnderline(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(30.0f * z, w - labelW * z));
    float v = p.get();
    if(ImGui::SliderFloat("##v", &v, p.getMin(), p.getMax(), fmt, log ? ImGuiSliderFlags_Logarithmic : 0)) p.set(v);
    if(host.publishMenu) host.publishMenu(key);
    ImGui::PopID();
}

void scModulation::sliderVec(ofParameter<std::vector<float>>& p, const char* label, float w, const char* fmt, bool log) {
    const float z = zoom();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if(host.publishedUnderline) host.publishedUnderline(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    const float sw = std::max(30.0f * z, w - labelW * z);
    const float lo = first(p.getMin(), 0.0f), hi = first(p.getMax(), 1.0f);
    const auto& values = p.get();
    if(values.size() > 1){
        const ImVec2 a = ImGui::GetCursorScreenPos(), size(sw, ImGui::GetFrameHeight());
        ImGui::InvisibleButton("##bars", size);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        canvas(dl, a, ImVec2(a.x + size.x, a.y + size.y));
        const float bw = size.x / values.size();
        for(size_t i = 0; i < values.size(); i++){
            const float t = ofClamp((values[i] - lo) / std::max(1e-6f, hi - lo), 0.0f, 1.0f);
            dl->AddRectFilled(ImVec2(a.x + i * bw + 1, a.y + size.y * (1 - t)), ImVec2(a.x + (i + 1) * bw - 1, a.y + size.y), pal::accent);
        }
        if(ImGui::IsItemActive()){
            const float t = ofClamp((ImGui::GetMousePos().x - a.x) / size.x, 0.0f, 1.0f);
            p.set({lo + t * (hi - lo)});
        }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("%d values, one per voice (from a connection)\nDrag: one value for every voice", (int)values.size());
    }else{
        ImGui::SetNextItemWidth(sw);
        float v = first(values, lo);
        if(ImGui::SliderFloat("##v", &v, lo, hi, fmt, log ? ImGuiSliderFlags_Logarithmic : 0)) p.set({v});
    }
    if(host.publishMenu) host.publishMenu(key);
    ImGui::PopID();
}

void scModulation::sliderInt(ofParameter<int>& p, const char* label, float w) {
    const float z = zoom();
    const std::string key = p.getEscapedName();
    ImGui::PushID(key.c_str());
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if(host.publishedUnderline) host.publishedUnderline(key);
    ImGui::SameLine();
    ImGui::SetCursorPosX(x0 + labelW * z);
    ImGui::SetNextItemWidth(std::max(30.0f * z, w - labelW * z));
    int v = p.get();
    if(ImGui::SliderInt("##v", &v, p.getMin(), p.getMax())) p.set(v);
    if(host.publishMenu) host.publishMenu(key);
    ImGui::PopID();
}

void scModulation::toggle(ofParameter<bool>& p, const char* label) {
    bool v = p.get();
    if(ImGui::Checkbox(label, &v)) p.set(v);
    if(host.publishedUnderline) host.publishedUnderline(p.getEscapedName());
    if(host.publishMenu) host.publishMenu(p.getEscapedName());
}

// Inside a control's right-click menu: route sources to it
void scModulation::modulateMenu(const std::string& key) {
    auto it = targetOfKey.find(key);
    if(it != targetOfKey.end()){
        const int t = it->second;
        ImGui::Separator();
        ImGui::TextDisabled("Modulate %s with", targetList[t].name.c_str());
        bool any = false;
        for(int s = 0; s < sourceCount(); s++){
            if(!sourceUsed(s)) continue;
            any = true;
            float& v = matrix[s * T() + t];
            char amount[24] = "";
            if(std::abs(v) > 1e-4f) std::snprintf(amount, sizeof(amount), "%+.2f", v);
            if(ImGui::MenuItem(sourceName(s).c_str(), amount, std::abs(v) > 1e-4f)){
                v = std::abs(v) > 1e-4f ? 0.0f : 0.5f;
                sendMatrix();
            }
        }
        ImGui::TextDisabled(any ? "(amounts: the Routing tab)" : "(add LFOs or envelopes first)");
        if(random && sourceUsed(SRC_RND)){
            if(ImGui::MenuItem("Random: one value for every control", nullptr, randomShared)){ randomShared = !randomShared; sendMatrix(); }
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Off: each control routed to Random gets its own random value per note\n"
                                                         "On: they all get the same one (they move together)");
        }
        return;
    }
    // any other control: a target of its own (a column in the Routing tab)
    ParamTarget probe;
    ParamTarget* pt = findParam(key);
    if(!pt && !resolveParam(key, probe)) return;
    const std::string name = pt ? pt->name : probe.name;
    ImGui::Separator();
    ImGui::TextDisabled("Modulate %s with", name.c_str());
    bool any = false;
    for(int s = 0; s < sourceCount(); s++){
        if(!sourceUsed(s)) continue;
        any = true;
        const float v = pt ? pt->amount[s] : 0.0f;
        char amount[24] = "";
        if(std::abs(v) > 1e-4f) std::snprintf(amount, sizeof(amount), "%+.2f", v);
        if(ImGui::MenuItem(sourceName(s).c_str(), amount, std::abs(v) > 1e-4f)){
            if(!pt){ params.push_back(probe); pt = &params.back(); pt->base = readParam(*pt); }
            pt->amount[s] = std::abs(v) > 1e-4f ? 0.0f : 0.25f;
            notify();
        }
    }
    if(random && sourceUsed(SRC_RND)){
        if(ImGui::MenuItem("Random: one value for every control", nullptr, randomShared)){ randomShared = !randomShared; sendMatrix(); }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Off: each control routed to Random gets its own random value per note\n"
                                                     "On: they all get the same one (they move together)");
    }
    if(pt && ImGui::MenuItem(("Stop modulating " + name).c_str())){
        for(size_t i = 0; i < params.size(); i++) if(params[i].key == key){ removeParam(i); break; }
        notify();
    }
    ImGui::TextDisabled(any ? "(amounts: the Routing tab)" : "(add LFOs or envelopes first)");
}

// ════════════════════════════════════════════════════════════════════════════
// Envelopes
// ════════════════════════════════════════════════════════════════════════════

void scModulation::drawEnvCard(int e, float w) {
    const float z = zoom();
    ImGui::PushID(e + 100);
    ImGui::BeginGroup();
    const ImU32 col = pal::env[e];
    sectionHeader(("Env " + ofToString(e + 1)).c_str(), w, col);
    auto& E = envs[e];
    labelW = 58.0f;
    {   // drag the points; Alt+drag a segment for its curve; each voice's level on the right
        const float h = 110 * z, meterW = 14.0f * z;
        const ImVec2 o = ImGui::GetCursorScreenPos();
        scEnvCurve::ADSR a{E.a.get(), E.d.get(), E.s.get(), E.r.get(), {E.ca.get(), E.cd.get(), E.cr.get()}};
        if(scEnvCurve::editADSR("##env", ImVec2(w - meterW - 4 * z, h), a, col, pal::canvas, false, E.r.getMax())){
            E.a = ofClamp(a.a, E.a.getMin(), E.a.getMax());
            E.d = ofClamp(a.d, E.d.getMin(), E.d.getMax());
            E.s = a.s;
            E.r = ofClamp(a.r, E.r.getMin(), E.r.getMax());
            E.ca = a.curve[0]; E.cd = a.curve[1]; E.cr = a.curve[2];
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 ma(o.x + w - meterW, o.y), mb(o.x + w, o.y + h);
        canvas(dl, ma, mb);
        for(int v = 0; v < n(); v++){
            const float level = ofClamp(liveValue(SRC_ENV + e, v), 0.0f, 1.0f);
            const float x = ma.x + meterW * (0.25f + 0.5f * (n() > 1 ? (float)v / (n() - 1) : 0.5f));
            dl->AddCircleFilled(ImVec2(x, mb.y - 4 * z - level * (h - 8 * z)), 2.2f * z, level > 0.001f ? col : pal::gridStrong);
        }
        ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + h + ImGui::GetStyle().ItemSpacing.y));
        ImGui::Dummy(ImVec2(w, 0));
    }
    slider(E.a, "Attack", w, "%.3f s", true);
    slider(E.d, "Decay", w, "%.3f s", true);
    slider(E.s, "Sustain", w, "%.2f");
    slider(E.r, "Release", w, "%.3f s", true);
    if(velocity){
        slider(E.vt, "Vel", w, "%.2f");
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Velocity tracking: 0 the same every note, 1 scaled by the note's velocity\n"
                                                     "(the Gate level)");
    }
    {
        ofParameter<float>* cs[3] = {&E.ca, &E.cd, &E.cr};
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Curves");
        ImGui::SameLine();
        ImGui::SetCursorPosX(x0 + labelW * z);
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        const float cw = (w - labelW * z - 2 * sp) / 3.0f;
        const char* fmts[3] = {"A %.1f", "D %.1f", "R %.1f"};
        for(int k = 0; k < 3; k++){
            if(k) ImGui::SameLine();
            ImGui::PushID(k);
            ImGui::SetNextItemWidth(cw);
            float v = cs[k]->get();
            if(ImGui::SliderFloat("##c", &v, -12.0f, 12.0f, fmts[k])) cs[k]->set(v);
            if(host.publishedUnderline) host.publishedUnderline(cs[k]->getEscapedName());
            if(host.publishMenu) host.publishMenu(cs[k]->getEscapedName());
            ImGui::PopID();
        }
    }
    std::string routes;
    for(int t = 0; t < T(); t++){
        const float v = matrix[(SRC_ENV + e) * T() + t];
        if(std::abs(v) > 1e-4f) routes += std::string(routes.empty() ? "" : ", ") + targetList[t].name + " " + ofToString(v, 2);
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    ImGui::TextColored(ImColor(pal::textDim), "%s", routes.empty() ? "Not routed yet: Routing tab, or right-click a control"
                                                                  : ("-> " + routes).c_str());
    ImGui::PopTextWrapPos();
    if(e == envCount - 1 && ImGui::SmallButton("Remove")){
        for(int t = 0; t < T(); t++) matrix[(SRC_ENV + e) * T() + t] = 0.0f;
        const std::string prefix = "Env" + ofToString(e + 1) + "_";
        const auto keys = host.publishedKeys();
        for(const auto& k : keys) if(k.rfind(prefix, 0) == 0 && ownsKey(k)) host.unpublish(k);
        envCount--;
        forStates([this](ofxSCServer* sv, ServerState& st){ updateSources(sv, st); syncRoutes(sv, st); });
    }
    labelW = 70.0f;
    ImGui::EndGroup();
    ImGui::PopID();
}

void scModulation::refresh() {
    forStates([this](ofxSCServer* sv, ServerState& st){ updateSources(sv, st); syncRoutes(sv, st); });
    notify();
}

void scModulation::drawEnvelopes(float w, const std::function<void(float)>& lead) {
    drawEnvelopeCards(w,lead ? std::vector<std::function<void(float)>>{lead} : std::vector<std::function<void(float)>>{});
}

void scModulation::drawEnvelopeCards(float w, const std::vector<std::function<void(float)>>& leads) {
    const float z = zoom();
    const float cw = colW(w, 4);
    const int first = leads.size();
    const int cards = first + envCount + (envCount < std::min((int)MAX_ENVS, maxEnvs) ? 1 : 0);
    for(int c = 0; c < cards; c++){
        if(c % 4) ImGui::SameLine(0, gapW());
        else if(c) ImGui::Spacing();
        if(c < first){ ImGui::BeginGroup(); leads[c](cw); ImGui::EndGroup(); continue; }
        if(c - first < envCount) drawEnvCard(c - first, cw);
        else{
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.13f, 0.15f, 1.0f));
            if(ImGui::Button("+  Add envelope", ImVec2(cw, 140 * z))){
                envCount++;
                forStates([this](ofxSCServer* sv, ServerState& st){ updateSources(sv, st); syncRoutes(sv, st); });
            }
            ImGui::PopStyleColor();
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("A new envelope, triggered by each voice's gate;\nroute it to anything in Routing");
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// LFOs
// ════════════════════════════════════════════════════════════════════════════

void scModulation::drawLfoCard(int li, float w) {
    const float z = zoom();
    const int nv = n();
    ImGui::PushID(li + 200);
    ImGui::BeginGroup();
    sectionHeader(("LFO " + ofToString(li + 1) + (effectiveNoise(li) ? "   noise" : "")).c_str(), w, pal::lfo[li]);
    if(unifiedLfoModels){
        static const char* models[] = {"Off", "Classic", "Noise", "Random"};
        int model = lfoModel[li].get();
        ImGui::SetNextItemWidth(w);
        if(ImGui::Combo("##model",&model,models,4)) lfoModel[li].set(model);
        if(host.publishMenu) host.publishMenu(lfoModel[li].getEscapedName());
    }
    {   // each voice's value now (bars across the whole display), the shape over them
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float h = 64.0f * z;
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
        canvas(dl, a, b);
        // per partial (routed to a per-partial target): one bar per partial
        const std::vector<float> pbars = partialBars ? partialBars(li) : std::vector<float>();
        const bool perPartial = !pbars.empty();
        const int bars = perPartial ? (int)pbars.size() : perVoice ? nv : 1;
        const float bw = (w - 8 * z) / std::max(1, bars);
        for(int v = 0; v < bars; v++){
            const float val = ofClamp(perPartial ? pbars[v] : liveValue(li, v), 0.0f, 1.0f);
            const float x0 = a.x + 4 * z + v * bw, y = b.y - 4 * z - val * (h - 8 * z);
            dl->AddRectFilled(ImVec2(x0 + 1, y), ImVec2(x0 + bw - 1, b.y - 4 * z), (pal::lfo[li] & 0x00FFFFFF) | 0x55000000, 2.0f * z);
            dl->AddLine(ImVec2(x0 + 1, y), ImVec2(x0 + bw - 1, y), pal::lfo[li], 2.0f * z);
        }
        const float rnd = first(lfoRound[li].get(), 0.5f), sk = first(lfoSkew[li].get()), pw = first(lfoPw[li].get(), 0.5f);
        const float pw2 = first(lfoPow[li].get()), bp = first(lfoBiPow[li].get()), gain = first(lfoGain[li].get(), 1.0f);
        auto value = [&](float ph){
            if(manualLfo(li)) return lfoManual[li].get();
            float v = effectiveRandom(li) > 0 ? rndPreview(li, ph, effectiveRandom(li), lfoRndSmooth[li].get())
                    : effectiveNoise(li) ? noise2(ph * 3.0f, 0.0f, li * 17.31f) : morph(ph, rnd, sk, pw, lfoCentered[li].get());
            v = ofClamp((v - 0.5f) * gain + 0.5f, 0.0f, 1.0f);
            if(std::abs(pw2) > 1e-4f) v = reflectUnit(customPow(v, pw2));
            if(std::abs(bp) > 1e-4f) v = biPow(v, bp);
            if(lfoSteps[li].get() >= 2){ const float q = (float)lfoSteps[li].get(); v = std::min(std::floor(v * q), q - 1.0f) / (q - 1.0f); }
            return v;
        };
        const std::array<float, 8> key{rnd, sk, pw + lfoSteps[li].get() * 3.0f, pw2, bp, manualLfo(li) ? -1.0f-lfoManual[li].get() : gain,
            (effectiveNoise(li) ? 1.0f : 0.0f) + effectiveRandom(li) * 2.0f + lfoRndSmooth[li].get() * 10.0f, lfoCentered[li].get() ? 1.0f : 0.0f};
        auto& preview = previewCache[li];
        if(!preview.valid || preview.key != key){
            for(int i = 0; i <= 96; i++) preview.values[i] = value(i / 96.0f);
            preview.key = key;
            preview.valid = true;
        }
        std::array<ImVec2, 97> pts;
        for(int i = 0; i <= 96; i++) pts[i] = ImVec2(a.x + w * i / 96.0f, b.y - 4 * z - preview.values[i] * (h - 8 * z));
        dl->AddPolyline(pts.data(), (int)pts.size(), (pal::lfo[li] & 0x00FFFFFF) | 0x80000000, 0, 1.5f * z);
        if(perVoice)
            for(int v = 0; v < nv && v < (int)lfoPhases[li].size(); v++)
                dl->AddCircleFilled(ImVec2(a.x + w * ofClamp(lfoPhases[li][v], 0.0f, 1.0f), a.y + 5 * z), 2.0f * z, pal::headerText);
        ImGui::Dummy(ImVec2(w, h));
        if(host.node && ImGui::BeginPopupContextItem("##lfoOutput")){
            if(!lfoOutputPins[li]){
                if(ImGui::MenuItem("Publish LFO output to Node GUI")) publishLfoOutput(li, true);
            }else if(lfoOutputPins[li]->hasOutConnections()){
                ImGui::TextDisabled("Disconnect the LFO output to unpublish");
            }else if(ImGui::MenuItem("Unpublish LFO output from Node GUI")) publishLfoOutput(li, false);
            ImGui::TextDisabled("0..1 values, updated at the node frame rate");
            ImGui::EndPopup();
        }
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nRight-click to publish the LFO values as a node output.",
                              perPartial ? "Bars: each partial's value now (voice 1; this LFO runs per partial).\nLine: the shape. Dots: each voice's phase offset"
                              : perVoice ? "Bars: each voice's value now. Line: the shape. Dots: each voice's phase offset"
                                         : "Bar: the value now. Line: the shape");
    }
    // AdditiveBox: this LFO to the per-partial targets (then it runs per partial)
    if(partialLfos){
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImColor(pal::textDim), "To partials");
        for(int t = 0; t < T(); t++){
            if(targetList[t].name.rfind("P.", 0) != 0) continue;
            ImGui::SameLine();
            float& v = matrix[li * T() + t];
            const bool on = std::abs(v) > 1e-4f;
            if(on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if(ImGui::SmallButton((targetList[t].name.substr(2) + "##pp" + ofToString(t)).c_str())){
                v = on ? 0.0f : 0.5f;
                sendMatrix();
            }
            if(on) ImGui::PopStyleColor();
            if(ImGui::IsItemHovered())
                ImGui::SetTooltip("Route this LFO to %s: it then runs once per partial, each partial at its own\n"
                                  "phase (Partials: Spread / Rand). Amount: the Routing tab.", targetList[t].name.c_str());
        }
    }
    if(manualLfo(li)){
        slider(lfoManual[li], "Value", w, "%.3f");
        ImGui::EndGroup(); ImGui::PopID(); return;
    }
    // two narrow columns: the LFO itself | how it spreads over the voices
    const float half = perVoice ? (w - 8.0f * z) * 0.5f : w;
    labelW = 40.0f;
    ImGui::BeginGroup();
    const bool region = grainExtras && lfoRegion[li].get();
    const bool random = effectiveRandom(li) > 0;
    bool sameLine = false;   // the toggles share a row; the rate slider goes below them
    if(!region){ toggle(lfoSync[li], "Sync"); sameLine = true; }
    if(!random && !unifiedLfoModels){
        if(sameLine) ImGui::SameLine();
        toggle(lfoNoise[li], "Noise");
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Smooth random values instead of a waveform; each voice follows its own trajectory");
        sameLine = true;
    }
    if(grainExtras){
        if(sameLine) ImGui::SameLine();
        toggle(lfoRegion[li], "Smp");
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("The cycle in lengths of the sample region (In..Out) instead of beats / Hz");
    }
    if(region) sliderVec(lfoBeats[li], "Regs", half, "%.3f", true);
    else if(lfoSync[li].get()) sliderVec(lfoBeats[li], "Beats", half, "%.3f", true);
    else sliderVec(lfoHz[li], "Rate", half, "%.3fHz", true);
    if(perVoice) sliderVec(lfoDetune[li], "Detune", half, "%.2f");
    slider(lfoInit[li], "Phase", half, "%.2f");
    if(!unifiedLfoModels){   // Random: per voice, a new value each cycle at the voice's own phase (not S&H's common clock)
        static const char* types[] = {"Off", "Stepped", "Smooth", "Jumps"};
        const std::string key = lfoRndType[li].getEscapedName();
        const float x0 = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Rnd");
        if(host.publishedUnderline) host.publishedUnderline(key);
        ImGui::SameLine();
        ImGui::SetCursorPosX(x0 + labelW * z);
        ImGui::SetNextItemWidth(std::max(30.0f * z, half - labelW * z));
        int t = std::clamp(effectiveRandom(li), 0, 3);
        if(ImGui::Combo(("##rnd" + key).c_str(), &t, types, 4)) lfoRndType[li].set(t);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Random instead of the waveform: each voice draws a new value every cycle,\n"
                                                     "at its own phase. Stepped: held (Smooth glides it); Smooth: a curve through\n"
                                                     "the values; Jumps: both added, wrapped (sudden leaps).");
        if(host.publishMenu) host.publishMenu(key);
        if(perVoice && random){
            toggle(lfoRndSame[li], "Same for all voices");
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Off: each voice its own random values\nOn: every voice the same values (at its own phase)");
        }
        if(effectiveRandom(li) == 1){
            slider(lfoRndSmooth[li], "Smooth", half, "%.3f s", true);
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("The stepped values glide (one-pole) over this time (0: they jump)");
        }
    }
    if(unifiedLfoModels && random){
        slider(lfoRndSmooth[li], "Smooth", half, "%.3f s", true);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("0: held random steps. Increase to glide between random values.");
        if(perVoice){
            toggle(lfoRndSame[li], "Same for all voices");
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Off: each voice its own random values\nOn: every voice the same values (at its own phase)");
        }
    }
    if(random){
    }else if(effectiveNoise(li)){
        if(perVoice || partialLfos) slider(lfoNScale[li], "Scale", half, "%.2f");
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("How different the voices' noises are: each voice reads the noise at\n"
                                                     "its own seed (its index x Scale). 0: all the same; 1 and up: independent.\n"
                                                     "(Per partial: the partials' indices.) Rate / Beats: how fast it moves.");
    }else{
        sliderVec(lfoRound[li], "Round", half, "%.2f");
        sliderVec(lfoSkew[li], "Skew", half, "%+.2f");
        sliderVec(lfoPw[li], "Width", half, "%.2f");
    }
    sliderVec(lfoGain[li], "Gain", half, "%.2f", true);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Stretches the shape around its middle and clips it at 0 and 1\n"
                                                 "(noise rarely reaches them). Before S&H, Pow and BiPow.");
    sliderVec(lfoPow[li], "Pow", half, "%+.2f");
    sliderVec(lfoBiPow[li], "BiPow", half, "%+.2f");
    sliderVec(lfoSH[li], "S&H", half, "%.2f");
    sliderInt(lfoSteps[li], "Steps", half);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Quantize the shape to this many levels (0: smooth)");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip(lfoSync[li].get() ? "Sample & hold: steps per beat" : "Sample & hold: steps per second");
    slider(lfoSHGlide[li], "Glide", half, "%.3f s", true);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("S&H glide: each held value slides to the next over this time (0: steps)");
    if(!effectiveNoise(li) && !random) toggle(lfoCentered[li], "Centered");
    ImGui::EndGroup();
    if(perVoice){
        ImGui::SameLine(0, 8.0f * z);
        ImGui::BeginGroup();
        ImGui::TextColored(ImColor(pal::textDim), "Voices");
        slider(lfoNWaves[li], "Spread", half, "%.2f");
        slider(lfoInvert[li], "Invert", half, "%.2f");
        sliderInt(lfoSym[li], "Sym", half);
        slider(lfoShuffle[li], "Shuf", half, "%.2f");
        slider(lfoRand[li], "Rand", half, "%.2f");
        slider(lfoOffset[li], "Offset", half, "%.2f");
        sliderInt(lfoQuant[li], "Quant", half);
        slider(lfoComb[li], "Comb", half, "%.2f");
        sliderInt(lfoModulo[li], "Mod", half);
        toggle(lfoNorm[li], "Normalize");
        if(partialLfos){
            ImGui::TextColored(ImColor(pal::textDim), "Partials");
            slider(lfoPSpread[li], "Spread", half, "%+.2f");
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Cycles across the partials: each partial its own phase\n(when this LFO goes to a per-partial target)");
            slider(lfoPRand[li], "Rand", half, "%.2f");
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("A random phase per partial");
        }
        ImGui::EndGroup();
    }
    labelW = 70.0f;
    ImGui::EndGroup();
    ImGui::PopID();
}

void scModulation::drawLfos(float w) {
    const float z = zoom();
    const float cw = colW(w, 4);
    float prevBottom = 0.0f;
    for(int li = 0; li < lfoCount; li++){
        if(li % 4) ImGui::SameLine(0, gapW());
        else if(li) ImGui::Spacing();
        drawLfoCard(li, cw);
        const ImVec2 c0 = ImGui::GetItemRectMin(), c1 = ImGui::GetItemRectMax();
        if(li % 4){   // a vertical line between this card and the previous one
            const float x = c0.x - gapW() * 0.5f;
            ImGui::GetWindowDrawList()->AddLine(ImVec2(x, c0.y), ImVec2(x, std::max(c1.y, prevBottom)), pal::grid, 1.0f * z);
        }
        prevBottom = c1.y;
    }
    ImGui::Spacing();
    if(lfoCount < std::min((int)MAX_LFOS, maxLfos)){
        if(ImGui::Button("+  Add LFO", ImVec2(160 * z, 0))){
            lfoCount++;
            forStates([this](ofxSCServer* sv, ServerState& st){ updateSources(sv, st); syncRoutes(sv, st); });
            notify();
        }
        ImGui::SameLine();
    }
    if(lfoCount > 0 && ImGui::Button(("Remove LFO " + ofToString(lfoCount)).c_str())){
        const int li = lfoCount - 1;
        for(int t = 0; t < T(); t++) matrix[li * T() + t] = 0.0f;
        const std::string prefix = "LFO" + ofToString(li + 1) + "_";
        const auto keys = host.publishedKeys();
        for(const auto& k : keys) if(k.rfind(prefix, 0) == 0 && ownsKey(k)) host.unpublish(k);
        lfoCount--;
        forStates([this](ofxSCServer* sv, ServerState& st){ updateSources(sv, st); syncRoutes(sv, st); });
        notify();
    }
    ImGui::SameLine();
    ImGui::TextColored(ImColor(pal::textDim), "Route them in Routing, or right-click a control: Modulate with.");
}

// ════════════════════════════════════════════════════════════════════════════
// Routing: sources (rows) x targets (columns); drag a cell left / right
// ════════════════════════════════════════════════════════════════════════════

void scModulation::drawRouting(float w) {
    const float z = zoom();
    const int nv = n();
    const float rowLabelW = 150.0f * z;
    // columns: the node's targets, then the controls added with "Modulate" (right-click any control)
    const int TT = T() + (int)params.size();
    auto colName = [this](int t) -> const std::string& { return t < T() ? targetList[t].name : params[t - T()].name; };
    auto amountRef = [this](int s, int t) -> float& { return t < T() ? matrix[s * T() + t] : params[t - T()].amount[s]; };
    auto amountChanged = [this](int t) { if(t < T()) sendMatrix(); else notify(); };
    const float cellW = (w - rowLabelW) / std::max(1, TT);
    int removeParamCol = -1;
    const float cellH = 26.0f * z;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::TextColored(ImColor(pal::textDim), "Drag a cell left / right to set how much a source moves a target "
                       "(Shift: fine; double-click: 0; right-click: type it).");
    if(random && sourceUsed(SRC_RND)){
        ImGui::SameLine(0, 16 * z);
        bool shared = randomShared;
        if(ImGui::Checkbox("Random: one value for every target", &shared)){ randomShared = shared; sendMatrix(); }
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Off: each target routed to Random gets its own random value per note\n"
                                                     "On: they all get the same one (they move together)");
    }
    const ImVec2 o = ImGui::GetCursorScreenPos();
    for(int t = 0; t < TT; t++){
        const char* name = colName(t).c_str();
        const ImVec2 ts = ImGui::CalcTextSize(name);
        const float x0 = o.x + rowLabelW + t * cellW;
        dl->PushClipRect(ImVec2(x0, o.y), ImVec2(x0 + cellW - 2 * z, o.y + ImGui::GetTextLineHeightWithSpacing()), true);
        dl->AddText(ImVec2(x0 + std::max(2.0f, (cellW - ts.x) * 0.5f), o.y), t < T() ? pal::headerText : pal::accent, name);
        dl->PopClipRect();
    }
    ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeightWithSpacing()));
    bool anySource = false;
    for(int s = 0; s < sourceCount(); s++){
        if(!sourceUsed(s)) continue;
        anySource = true;
        const ImVec2 r = ImGui::GetCursorScreenPos();
        const ImU32 sc = sourceColor(s);
        dl->AddRectFilled(r, ImVec2(r.x + rowLabelW - 6 * z, r.y + cellH - 2 * z), pal::header, 3.0f * z);
        dl->AddText(ImVec2(r.x + 6 * z, r.y + (cellH - 2 * z - ImGui::GetTextLineHeight()) * 0.5f), sc, sourceName(s).c_str());
        // the source now: one small bar per voice
        std::vector<float> values;
        const int shown = (s < MAX_LFOS && !perVoice) ? 1 : nv;
        if(s < SRC_VEL) for(int v = 0; v < shown; v++) values.push_back(liveValue(s, v));
        else if(s >= SRC_EXT && s < SRC_VOICE){
            const auto& e = exts[s - SRC_EXT].get();
            for(int v = 0; v < nv; v++) values.push_back((e.empty() ? 0.0f : e[std::min((size_t)v, e.size() - 1)]) * 0.5f + 0.5f);
        }else if(s == SRC_VEL) for(int v = 0; v < nv; v++) values.push_back(liveValue(s, v));
        else if(s == SRC_RND) for(int v = 0; v < nv; v++) values.push_back(liveValue(s, v) * 0.5f + 0.5f);
        if(!values.empty()){
            const float mx0 = r.x + 70 * z, mx1 = r.x + rowLabelW - 10 * z, my0 = r.y + 4 * z, my1 = r.y + cellH - 6 * z;
            const float bw = (mx1 - mx0) / values.size();
            dl->AddRectFilled(ImVec2(mx0, my0), ImVec2(mx1, my1), pal::canvas, 2.0f * z);
            for(size_t v = 0; v < values.size(); v++){
                const float val = ofClamp(values[v], 0.0f, 1.0f);
                dl->AddRectFilled(ImVec2(mx0 + v * bw + 1, my1 - val * (my1 - my0)), ImVec2(mx0 + (v + 1) * bw - 1, my1), sc, 1.0f);
            }
        }
        ImGui::Dummy(ImVec2(rowLabelW, cellH));
        for(int t = 0; t < TT; t++){
            const int idx = s * TT + t;
            ImGui::SameLine(0, 0);
            ImGui::PushID(idx);
            const ImVec2 c0 = ImGui::GetCursorScreenPos(), c1(c0.x + cellW - 2 * z, c0.y + cellH - 2 * z);
            ImGui::InvisibleButton("##cell", ImVec2(cellW, cellH));
            const float v = amountRef(s, t);
            const bool hov = ImGui::IsItemHovered();
            dl->AddRectFilled(c0, c1, (s + t) % 2 ? IM_COL32(20, 22, 27, 255) : IM_COL32(16, 18, 22, 255), 3.0f * z);
            const float midX = (c0.x + c1.x) * 0.5f;
            dl->AddLine(ImVec2(midX, c0.y + 3 * z), ImVec2(midX, c1.y - 3 * z), pal::grid);
            if(std::abs(v) > 1e-4f){
                const ImU32 col = v > 0 ? pal::accent : pal::negative;
                const float x = midX + v * (c1.x - c0.x - 4 * z) * 0.5f;
                dl->AddRectFilled(ImVec2(std::min(midX, x), c0.y + 4 * z), ImVec2(std::max(midX, x), c1.y - 4 * z),
                                  (col & 0x00FFFFFF) | 0xA0000000, 2.0f * z);
                char t2[16];
                std::snprintf(t2, sizeof(t2), "%+.2f", v);
                const ImVec2 ts = ImGui::CalcTextSize(t2);
                dl->AddText(ImVec2((c0.x + c1.x - ts.x) * 0.5f, (c0.y + c1.y - ts.y) * 0.5f), IM_COL32(255, 255, 255, 230), t2);
            }
            if(hov) dl->AddRect(c0, c1, pal::headerText, 3.0f * z);
            if(ImGui::IsItemActivated()){ matrixDrag = idx; matrixDragStart = ImGui::GetMousePos().x; matrixDragValue = v; }
            if(ImGui::IsItemActive() && matrixDrag == idx){
                const float fine = ImGui::GetIO().KeyShift ? 0.1f : 1.0f;
                const float nvv = ofClamp(matrixDragValue + (ImGui::GetMousePos().x - matrixDragStart) * 0.008f * fine, -1.0f, 1.0f);
                if(nvv != amountRef(s, t)){ amountRef(s, t) = nvv; amountChanged(t); }
            }
            if(ImGui::IsItemDeactivated()) matrixDrag = -1;
            if(hov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)){
                amountRef(s, t) = 0.0f;
                matrixDrag = idx;
                matrixDragValue = 0.0f;
                matrixDragStart = ImGui::GetMousePos().x;
                amountChanged(t);
            }
            if(hov && !ImGui::IsItemActive()){
                if(t < T())
                    ImGui::SetTooltip("%s -> %s: %+.2f  (%+.3g %s)\nDrag left / right (Shift: fine), double-click: 0",
                                      sourceName(s).c_str(), targetList[t].name.c_str(), v, v * targetList[t].range, targetList[t].unit.c_str());
                else{
                    const auto& pt = params[t - T()];
                    ImGui::SetTooltip("%s -> %s: %+.2f  (%+.3g of %.3g..%.3g)\nDrag left / right (Shift: fine), double-click: 0\n"
                                      "Right-click: remove this control from the matrix",
                                      sourceName(s).c_str(), pt.name.c_str(), v, v * (pt.hi - pt.lo), pt.lo, pt.hi);
                }
            }
            if(ImGui::BeginPopupContextItem("##cellMenu")){
                ImGui::Text("%s -> %s", sourceName(s).c_str(), colName(t).c_str());
                float e = amountRef(s, t);
                ImGui::SetNextItemWidth(120 * z);
                if(ImGui::InputFloat("##amount", &e, 0.05f, 0.1f, "%.3f")){ amountRef(s, t) = ofClamp(e, -1.0f, 1.0f); amountChanged(t); }
                if(ImGui::MenuItem("Clear this source's row")){
                    for(int k = 0; k < TT; k++) amountRef(s, k) = 0;
                    sendMatrix();
                }
                if(ImGui::MenuItem("Clear this target's column")){ for(int k = 0; k < sourceCount(); k++) amountRef(k, t) = 0; amountChanged(t); }
                if(t >= T() && ImGui::MenuItem(("Remove " + colName(t) + " from the matrix").c_str())) removeParamCol = t - T();
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    if(removeParamCol >= 0){ removeParam((size_t)removeParamCol); notify(); }
    if(!anySource) ImGui::TextColored(ImColor(pal::textDim), "No sources yet: add envelopes or LFOs in their tabs.");
    else ImGui::TextColored(ImColor(pal::textDim), "Any other control: right-click it -> Modulate with... (it joins the matrix as a column).");
    ImGui::Spacing();
    // external sources: Oceanode vectors (publish them), added one by one
    const float cw = colW(w, 4);
    sectionHeader("External sources   (publish them and connect Oceanode vectors, one value per voice)", w, pal::accent);
    for(int k = 0; k < extCount; k++){
        if(k % 4) ImGui::SameLine(0, gapW());
        ImGui::BeginGroup();
        sliderVec(exts[k], ("Ext " + ofToString(k + 1)).c_str(), cw, "%+.2f");
        ImGui::EndGroup();
    }
    const int extMax = std::min((int)MAX_EXT, maxExts);
    if(extCount < extMax && ImGui::Button("+  Add external source")){ extCount++; sendMatrix(); }
    if(extCount < extMax) ImGui::SameLine();
    if(extCount > 0 && ImGui::Button(("Remove Ext " + ofToString(extCount)).c_str())){
        const int k = extCount - 1;
        for(int t = 0; t < T(); t++) matrix[(SRC_EXT + k) * T() + t] = 0.0f;
        for(auto& pt : params) pt.amount[SRC_EXT + k] = 0.0f;
        host.unpublish(exts[k].getEscapedName());
        extCount--;
        sendMatrix();
    }
    ImGui::SameLine(0, 24 * z);
    if(ImGui::Button("Clear all routing")){
        std::fill(matrix.begin(), matrix.end(), 0.0f);
        while(!params.empty()) removeParam(params.size() - 1);
        sendMatrix();
    }
}
