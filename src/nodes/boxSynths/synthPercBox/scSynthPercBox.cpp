#include "scSynthPercBox.h"
#include "../../../audio/scSynthControlBatch.h"
#include "../../../ofxOceanodeSuperColliderConfig.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <random>
#include <cstring>

namespace {
namespace pal {
const ImU32 bg        = IM_COL32( 16,  17,  20, 255);
const ImU32 rowBg     = IM_COL32( 24,  25,  30, 255);
const ImU32 border    = IM_COL32( 46,  50,  58, 255);
const ImU32 text      = IM_COL32(205, 205, 215, 255);
const ImU32 textDim   = IM_COL32(135, 135, 148, 255);
const ImU32 knobBody  = IM_COL32( 38,  39,  46, 255);
const ImU32 knobTrack = IM_COL32( 70,  72,  82, 255);
const ImU32 needle    = IM_COL32(235, 235, 240, 255);
}
// one colour per generator (rows, pads, the timeline's rows)
const unsigned char GEN_RGB[6][3] = {{236, 150, 80}, {230, 200, 90}, {120, 200, 140}, {108, 180, 232}, {228, 122, 146}, {186, 146, 232}};
ImU32 genCol(int g, int a = 255, float k = 1.0f) {
    return IM_COL32(int(GEN_RGB[g][0] * k), int(GEN_RGB[g][1] * k), int(GEN_RGB[g][2] * k), a);
}
float zoom() { return ofxOceanodeShared::getZoomLevel(); }
// the rows' own scale: knobs, pads, envelopes (user: a third bigger than the first version)
constexpr float ROW_SCALE = 1.33f;
float ui() { return zoom() * ROW_SCALE; }
bool liveBus(ofxSCServer* s, ofxSCBus* b, bool audio) {
    if(!s || !b || b->index < 0) return false;
    auto& buses = audio ? s->audioBusses : s->controlBusses;
    return b->index < int(buses.size()) && buses[b->index] == b;
}
void dropBus(ofxSCServer* s, ofxSCBus*& b, bool audio) {
    if(liveBus(s, b, audio)) b->free();
    delete b; b = nullptr;
}
float at(const std::vector<float>& v, int i, float fallback) { return i < int(v.size()) ? v[i] : fallback; }
const char* INPUT_MODES[] = {"Kit", "Notes"};
// Reaktor's key ranges (CLIX's file map overlaps NOIX's: the clean range is used)
const int FIRST_NOTE[6] = {0, 14, 28, 43, 59, 73};
// A Reaktor log time (10^(x/20) ms) as text
std::string msText(float x) {
    const float ms = std::pow(10.0f, x / 20.0f);
    return ms >= 1000 ? ofToString(ms / 1000, ms >= 10000 ? 0 : 2) + " s" : ofToString(ms, ms >= 100 ? 0 : ms >= 10 ? 1 : 2) + " ms";
}
}

int scSynthPercBox::Gen::param(const std::string& key) const {
    for(size_t k = 0; k < data->params.size(); ++k) if(key == data->params[k].key) return int(k);
    return -1;
}

scSynthPercBox::scSynthPercBox(std::vector<serverManager*> s) : scNode("synthPercBox"), servers(std::move(s)) {
    description = "Bits (Reaktor, 2008) as a Box: six percussion generators (K1 kick, STOMPS, NOIX noise, CLIX clicks, "
                  "CRAX crackles, QUAD quadratic map), 16 pads each. Click a pad to play it, Shift+click to store the "
                  "current sound in it. Gate channel n plays generator n, its Pitch picks the pad (60 = pad 1); "
                  "Input Notes uses Reaktor's key ranges instead.";
}
scSynthPercBox::~scSynthPercBox() {
    scheduled.disable();
    listeners.unsubscribeAll();
    for(auto& [s, st] : states) release(s, st);
}

void scSynthPercBox::resetToFactory(int g) {
    auto& gen = gens[g];
    const auto& d = *gen.data;
    for(int r = 0; r < ROWS; ++r) {
        // pads past the bank: the last factory sound (what Reaktor plays after it), not marked as stored
        const int src = std::min(r == LIVE ? 0 : r, int(d.pads.size()) - 1);
        gen.rows[r] = d.pads[src].values;
    }
    for(int p = 0; p < PADS; ++p) {
        gen.stored[p] = p < int(d.pads.size());
        gen.names[p] = gen.stored[p] ? d.pads[p].name : "";
    }
    gen.shown = 0;
}

void scSynthPercBox::setup() {
    addOnToggle();
    const auto& data = synthPercData::generators();
    for(int g = 0; g < GENS; ++g) { gens[g].data = &data[g]; resetToFactory(g); }

    addParameter(show.set("Show", false));
    addParameterDropdown(inputMode, "Input", 0, {"Kit", "Notes"});
    addParameter(gainDb.set("Gain", 18.0f, -24.0f, 36.0f));
    gateIn.attach(addParameter(gate.set("Gate", std::vector<float>(GENS, 0.0f), {0.0f}, {1.0f})), availableInputs, [this](){ for(auto& output : outputs) output = output; });
    pitchIn.attach(addParameter(pitch.set("Pitch", std::vector<float>(GENS, 60.0f), {0.0f}, {127.0f})), availableInputs, [this](){ for(auto& output : outputs) output = output; });
    addOutput("Out");
    scheduled.add(gateIn.handle, gate); scheduled.add(pitchIn.handle, pitch);
    scheduled.enable(this, [this](){ for(auto& [s, st] : states) if(st.created) return true; return false; });

    auto toIn = [this](){
        if(scScheduling::isBackendSendSuppressed()) return;
        for(auto& [s, st] : states) sendIn(s, st);
    };
    listeners.push(gate.newListener([toIn](std::vector<float>&){ toIn(); }));
    listeners.push(pitch.newListener([toIn](std::vector<float>&){ toIn(); }));
    listeners.push(inputMode.newListener([this](int&){ sendAllGens(false); }));
    listeners.push(gainDb.newListener([this](float&){ sendAllGens(false); }));

    snaps.setupParameters();
    scSnapshots::Host sh;
    sh.folder = "SynthPercBox"; sh.title = "SynthPercBox";
    sh.capture = [this](){ return capture(); };
    sh.apply = [this](const ofJson& a, const ofJson& b, float t, bool sw){ load(sw || t >= 0.5f ? b : a); };
    sh.factory = [this](){
        // the ensemble as it came: every generator's Reaktor bank
        const auto keep = gens;
        for(int g = 0; g < GENS; ++g) resetToFactory(g);
        ofJson j = capture(); j["name"] = "Bits (Reaktor)";
        gens = keep;
        return std::vector<ofJson>{j};
    };
    sh.bpm = [this](){ return bpm; };
    snaps.setup(sh);
    requireSynthdefs();
}

void scSynthPercBox::requireSynthdefs() {
    std::filesystem::path folder;
    try {
        const std::filesystem::path data = ofToDataPath(SYNTHDEF_DIRECTORY, true);
        if(std::filesystem::exists(data)) for(const auto& f : std::filesystem::recursive_directory_iterator(data, std::filesystem::directory_options::skip_permission_denied))
            if(f.is_regular_file() && f.path().filename() == "SPK1.scsyndef") { folder = f.path().parent_path(); break; }
        if(folder.empty()) folder = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path().parent_path() / "synthdefs/Defaults/SynthPercBox/CompiledSynthdefs/SynthPercBox";
        if(!std::filesystem::exists(folder / "SPK1.scsyndef")) { ofLogError("synthPercBox") << "Compile synthdefs/Defaults/SynthPercBox first"; return; }
        for(auto* sm : servers) if(sm) sm->requireSynthdefFolder(folder.string());
    } catch(const std::exception& e) { ofLogError("synthPercBox") << e.what(); }
}

// ── Server ───────────────────────────────────────────────────────────────────
float scSynthPercBox::gainLinear() const { return std::pow(10.0f, gainDb.get() / 20.0f); }

void scSynthPercBox::sendIn(ofxSCServer* server, State& st) {
    if(!st.created || !st.in) return;
    auto now = scheduled.inEffectNow();
    scSynthControlBatch b(st.in, server);
    auto setv = [&](const std::string& name, std::vector<float> v) {
        auto it = st.inSent.find(name);
        if(it != st.inSent.end() && it->second == v) return;
        st.inSent[name] = v; b.set(name, v);
    };
    std::vector<float> g(NV, 0.0f), p(NV, 60.0f);
    for(int i = 0; i < NV; ++i) { g[i] = std::max(0.0f, at(gate.get(), i, 0.0f)); p[i] = at(pitch.get(), i, 60.0f); }
    setv("pitches", p); setv("gates", g);
    const int gb = gateIn.bus(server), pb = pitchIn.bus(server);
    const int gn = std::clamp(gateIn.channels() > 0 ? gateIn.channels() : 1, 1, NV);
    const int pn = std::clamp(pitchIn.channels() > 0 ? pitchIn.channels() : 1, 1, NV);
    if(gb >= 0) st.in->mapan("gate_ar", gb, gn);
    if(pb >= 0) st.in->mapan("pitch_ar", pb, pn);
    setv("gate_sel", {gb >= 0 ? 1.0f : 0.0f}); setv("pitch_sel", {pb >= 0 ? 1.0f : 0.0f});
    setv("gate_n", {float(gb >= 0 ? gn : NV)}); setv("pitch_n", {float(pb >= 0 ? pn : NV)});
    setv("out", {float(st.inBus->index)});
    b.send();
}

void scSynthPercBox::sendGen(ofxSCServer* server, State& st, int g, bool table) {
    if(!st.created || !st.gen[g]) return;
    auto& gen = gens[g];
    const bool space = spaceChannels(g) > 0;
    const float gain = gen.mute ? 0.0f : gainLinear();
    {
        scSynthControlBatch b(st.gen[g], server);
        b.set("gin", st.inBus->index);
        b.set("inmode", inputMode.get()); b.set("ch", g); b.set("lo", FIRST_NOTE[g]);
        b.set("rowbus", st.rowBus->index + g);
        b.set("bps", bpm / 60.0f);
        b.set("out", space ? st.dry[g]->index : std::max(0, st.out));
        if(space) b.set("pbus", st.pbus[g]->index);
        b.set("gain", space ? 1.0f : gain);
        if(table) {
            const auto& params = gen.data->params;
            for(size_t k = 0; k < params.size(); ++k) {
                std::vector<float> v(ROWS);
                for(int r = 0; r < ROWS; ++r) v[r] = gen.rows[r][k];
                b.set(std::string("p_") + params[k].key, v);
            }
        }
        b.send();
    }
    if(space && st.space[g]) {
        scSynthControlBatch b(st.space[g], server);
        b.set("in", st.dry[g]->index); b.set("pbus", st.pbus[g]->index);
        b.set("out", std::max(0, st.out)); b.set("gain", gain);
        b.send();
    }
}
void scSynthPercBox::sendAllGens(bool table) { for(auto& [s, st] : states) for(int g = 0; g < GENS; ++g) sendGen(s, st, g, table); }
void scSynthPercBox::sendParam(int g, int k) {
    const auto& key = gens[g].data->params[k].key;
    for(auto& [s, st] : states) if(st.created && st.gen[g]) {
        std::vector<float> v(ROWS);
        for(int r = 0; r < ROWS; ++r) v[r] = gens[g].rows[r][k];
        scSynthControlBatch b(st.gen[g], s);
        b.set(std::string("p_") + key, v);
        b.send();
    }
}

void scSynthPercBox::buildSynth(ofxSCServer* server) {
    if(!server) return;
    auto& st = states[server]; release(server, st); st.root = new ofxSCGroup(server);
}
void scSynthPercBox::createSynth(ofxSCServer* server) {
    auto it = states.find(server); if(it == states.end() || !it->second.root) return;
    auto& st = it->second;
    st.root->create(0, 1);
    st.inBus = new ofxSCBus(RATE_AUDIO, 2 * NV, server);
    st.rowBus = new ofxSCBus(RATE_CONTROL, GENS, server);
    bool ok = liveBus(server, st.inBus, true) && liveBus(server, st.rowBus, false);
    for(int g = 0; g < GENS && ok; ++g) if(const int n = spaceChannels(g)) {
        st.dry[g] = new ofxSCBus(RATE_AUDIO, n, server);
        st.pbus[g] = new ofxSCBus(RATE_CONTROL, 9, server);
        ok = liveBus(server, st.dry[g], true) && liveBus(server, st.pbus[g], false);
    }
    if(!ok) { release(server, st); return; }
    st.created = true;
    st.lastRow.fill(-1);
    st.in = new ofxSCSynth("SPIn", server);
    sendIn(server, st);
    st.in->createAndRun(1, st.root->nodeID, true);
    st.inSent.clear(); sendIn(server, st);   // again: /s_new sets only a vector control's first value
    for(int g = 0; g < GENS; ++g) {
        st.gen[g] = new ofxSCSynth(gens[g].data->def, server);
        if(const int n = spaceChannels(g)) st.space[g] = new ofxSCSynth("SPSpace" + ofToString(n), server);
        sendGen(server, st, g, true);   // set before /s_new: the synth starts with its pads
        st.gen[g]->createAndRun(1, st.root->nodeID, true);
        if(st.space[g]) st.space[g]->createAndRun(1, st.root->nodeID, true);
        sendGen(server, st, g, true);   // again: /s_new sets only a vector control's first value
    }
    if(!runsOn(server)) st.root->run(false);
}
void scSynthPercBox::release(ofxSCServer* server, State& st) {
    if(st.created && st.root) st.root->free();
    delete st.in; st.in = nullptr;
    for(auto*& s : st.gen) { delete s; s = nullptr; }
    for(auto*& s : st.space) { delete s; s = nullptr; }
    delete st.root; st.root = nullptr; st.created = false;
    st.inSent.clear();
    dropBus(server, st.inBus, true); dropBus(server, st.rowBus, false);
    for(auto*& b : st.dry) dropBus(server, b, true);
    for(auto*& b : st.pbus) dropBus(server, b, false);
}
void scSynthPercBox::free(ofxSCServer* s) { auto it = states.find(s); if(it != states.end()) { release(s, it->second); it->second.out = -1; } }
void scSynthPercBox::moveSynthBefore(ofxSCServer* s, int id) { auto it = states.find(s); if(it != states.end() && it->second.created) it->second.root->moveBefore(id); }
int scSynthPercBox::getLastSynthID(ofxSCServer* s) { auto it = states.find(s); return it != states.end() && it->second.created ? it->second.root->nodeID : -1; }
void scSynthPercBox::setOutputBus(ofxSCServer* s, int, int bus) {
    auto& st = states[s]; st.out = bus;
    for(int g = 0; g < GENS; ++g) sendGen(s, st, g, false);
}
int scSynthPercBox::getOutputBusIndex(ofxSCServer* s, int) { auto it = states.find(s); return it == states.end() ? -1 : it->second.out; }
void scSynthPercBox::setInputBus(ofxSCServer* s, scNode* node, int bus) {
    auto& st = states[s];
    if(gateIn.take(s, node, bus) | pitchIn.take(s, node, bus)) { st.inSent.clear(); sendIn(s, st); }
}
void scSynthPercBox::resetInputBusses(ofxSCServer* s, int) {
    gateIn.reset(s); pitchIn.reset(s);
    auto& st = states[s]; st.inSent.clear(); sendIn(s, st);
}
void scSynthPercBox::activate() { for(auto& [s, st] : states) if(st.created) st.root->run(runsOn(s)); }
void scSynthPercBox::deactivate() { for(auto& [s, st] : states) if(st.created) st.root->run(false); }
void scSynthPercBox::applyGraphSuspended(ofxSCServer* server, bool suspended) {
    setGraphSuspendedFlag(server, suspended);
    auto it = states.find(server);
    if(it != states.end() && it->second.created && it->second.root) it->second.root->run(runsOn(server));
}
void scSynthPercBox::setBpm(float value) {
    const float b = std::max(1.0f, value);
    if(b == bpm) return;
    bpm = b;
    for(auto& [s, st] : states) for(int g = 0; g < GENS; ++g) if(st.created && st.gen[g]) st.gen[g]->set("bps", bpm / 60.0f);
}
void scSynthPercBox::resendParametersForNRT() {
    for(auto& [s, st] : states) { st.inSent.clear(); sendIn(s, st); }
    sendAllGens(true);
}
#ifdef OFXOCEANODE_HAS_TIMELINE_INSTRUMENT
bool scSynthPercBox::getTimelineInstrument(ofxOceanodeTimelineInstrument& out) {
    if(!gateIn.handle || !pitchIn.handle) return false;
    out.gate = gateIn.handle.get(); out.pitch = pitchIn.handle.get(); out.velocity = nullptr;
    if(inputMode.get() == 1) { out.kit = false; out.voices = NV; return true; }   // Notes: a keyboard / piano roll
    // Kit: a row per generator, a pad per hit (Pitch 60 + k plays pad k + 1)
    out.kit = true; out.voices = GENS;
    out.rowNames.clear(); out.rowColors.clear();
    for(int g = 0; g < GENS; ++g) {
        out.rowNames.push_back(gens[g].data->name);
        out.rowColors.push_back(ofColor(GEN_RGB[g][0], GEN_RGB[g][1], GEN_RGB[g][2]));
    }
#ifdef OFXOCEANODE_HAS_TIMELINE_ROW_VARIANTS
    out.rowVariants.assign(GENS, {});
    for(int g = 0; g < GENS; ++g) for(int p = 0; p < PADS; ++p)
        out.rowVariants[g].push_back(gens[g].names[p].empty() ? "Pad " + ofToString(p + 1) : gens[g].names[p]);
#endif
    return true;
}
#endif

// ── Playing and editing ──────────────────────────────────────────────────────
// a pad played from the window: held while the mouse is down (STOMPS and CRAX sound while held)
void scSynthPercBox::press(int g, int pad) {
    auto& gen = gens[g];
    gen.held = pad;
    if(pad < PADS) { gen.shown = pad; gen.flash[pad] = ofGetElapsedTimef(); }
    for(auto& [s, st] : states) if(st.created && st.gen[g]) {
        scSynthControlBatch b(st.gen[g], s);
        b.set("cpad", float(pad)); b.set("cgate", 1.0f); b.send();
    }
}
void scSynthPercBox::releasePress(int g) {
    gens[g].held = -1;
    for(auto& [s, st] : states) if(st.created && st.gen[g]) st.gen[g]->set("cgate", 0.0f);
}
// a control edited: the live row takes the shown pad's sound the first time, then the edit
void scSynthPercBox::edit(int g, int k, float v) {
    auto& gen = gens[g];
    if(gen.shown != LIVE) {
        gen.rows[LIVE] = gen.rows[gen.shown];
        gen.shown = LIVE;
        for(auto& [s, st] : states) sendGen(s, st, g, true);
    }
    gen.rows[LIVE][k] = v;
    sendParam(g, k);
    for(auto& [s, st] : states) if(st.created && st.gen[g]) st.gen[g]->set("t_live", 1.0f);
}
void scSynthPercBox::storePad(int g, int pad) {
    auto& gen = gens[g];
    gen.rows[pad] = gen.rows[gen.shown];
    if(!gen.stored[pad] || gen.names[pad].empty()) gen.names[pad] = "Pad " + ofToString(pad + 1);
    gen.stored[pad] = true;
    gen.shown = pad;
    for(auto& [s, st] : states) sendGen(s, st, g, true);
}
// CRAX's Randomize: the 14 sound controls its randomizers reach (Reaktor CC 50..63), anywhere in
// their range, then a hit of the live sound
void scSynthPercBox::randomize(int g) {
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<float> u(0.05f, 1.0f);
    auto& gen = gens[g];
    for(const char* key : {"owave", "p", "pw", "plus", "nrnd", "rrate", "rnd", "lwave", "bpmr", "width", "lamp", "ftype", "cut", "res"}) {
        const int k = gen.param(key); if(k < 0) continue;
        const auto& p = gen.data->params[k];
        float v = p.lo + u(rng) * (p.hi - p.lo);
        if(p.kind == 'l') {
            const int n = int(std::count(p.options, p.options + std::strlen(p.options), '|')) + 1;
            v = std::min(float(n - 1), std::floor(u(rng) * n));
        } else if(p.step > 0) v = p.lo + std::floor((v - p.lo) / p.step) * p.step;
        edit(g, k, v);
    }
    press(g, LIVE);
}

// ── Frame ────────────────────────────────────────────────────────────────────
void scSynthPercBox::update(ofEventArgs&) {
    snaps.update();
    // the pads the generators play (any source: cables, the timeline, notes): the window follows
    const float now = ofGetElapsedTimef();
    for(auto& [s, st] : states) if(st.created && liveBus(s, st.rowBus, false)) {
        const auto rows = st.rowBus->readValues;
        if(now >= st.nextRead) { st.nextRead = now + 1.0f / 30; s->requestControlValues(st.rowBus->index, GENS); }
        for(int g = 0; g < GENS && g < int(rows.size()); ++g) {
            const int r = int(std::lround(rows[g]));
            if(r == st.lastRow[g]) continue;
            const bool first = st.lastRow[g] < 0;
            st.lastRow[g] = r;
            if(first || r < 0 || r >= LIVE) continue;
            gens[g].shown = r; gens[g].flash[r] = now;
        }
        break;
    }
}
void scSynthPercBox::draw(ofEventArgs&) {
    if(show.get()) drawWindow();
}

// ── Presets and snapshots ────────────────────────────────────────────────────
ofJson scSynthPercBox::capture() const {
    ofJson j = ofJson::object();
    ofJson all = ofJson::array();
    for(const auto& gen : gens) {
        ofJson gj;
        gj["name"] = gen.data->name;
        ofJson pads = ofJson::array();
        for(int r = 0; r < ROWS; ++r) {
            ofJson pj = ofJson::object();
            for(size_t k = 0; k < gen.data->params.size(); ++k) pj[gen.data->params[k].key] = gen.rows[r][k];
            if(r < PADS) { pj["#name"] = gen.names[r]; pj["#stored"] = gen.stored[r]; }
            pads.push_back(pj);
        }
        gj["pads"] = pads;
        gj["shown"] = gen.shown;
        gj["mute"] = gen.mute;
        all.push_back(gj);
    }
    j["generators"] = all;
    return j;
}
void scSynthPercBox::load(const ofJson& j) {
    if(!j.contains("generators") || !j["generators"].is_array()) return;
    for(const auto& gj : j["generators"]) {
        const std::string name = gj.value("name", "");
        int g = -1;
        for(int i = 0; i < GENS; ++i) if(name == gens[i].data->name) g = i;
        if(g < 0 || !gj.contains("pads") || !gj["pads"].is_array()) continue;
        auto& gen = gens[g];
        const auto& pads = gj["pads"];
        for(int r = 0; r < ROWS && r < int(pads.size()); ++r) {
            const auto& pj = pads[r];
            for(size_t k = 0; k < gen.data->params.size(); ++k) {
                const char* key = gen.data->params[k].key;
                if(pj.contains(key) && pj[key].is_number()) gen.rows[r][k] = pj[key].get<float>();
            }
            if(r < PADS) { gen.names[r] = pj.value("#name", gen.names[r]); gen.stored[r] = pj.value("#stored", gen.stored[r]); }
        }
        gen.shown = std::clamp(gj.value("shown", 0), 0, LIVE);
        gen.mute = gj.value("mute", false);
    }
    sendAllGens(true);
}
void scSynthPercBox::presetSave(ofJson& j) {
    j["synthPercBox"] = capture();
    j["synthPercBoxSnapshots"] = snaps.settingsJson();
}
void scSynthPercBox::presetRecallAfterSettingParameters(ofJson& j) {
    if(j.contains("synthPercBox")) load(j["synthPercBox"]);
    if(j.contains("synthPercBoxSnapshots")) snaps.loadSettings(j["synthPercBoxSnapshots"]);
}

// ── Window ───────────────────────────────────────────────────────────────────
// A small knob: drag up / down (Shift: fine), double-click: the factory pad 1 value,
// right-click: type a value. Returns true when changed.
bool scSynthPercBox::knob(const char* id, float& v, float lo, float hi, float step, const char* label, const char* tip, float defValue) {
    const float z = ui();
    const float r = 9.0f * z, cellW = 34.0f * z, cellH = 2 * r + 13.0f * z;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::InvisibleButton("##k", ImVec2(cellW, cellH));
    bool changed = false;
    const float span = hi - lo;
    if(ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 0.0f) && span != 0) {
        const float speed = (ImGui::GetIO().KeyShift ? 0.0015f : 0.008f) * span;
        float nv = std::clamp(v - ImGui::GetIO().MouseDelta.y * speed, std::min(lo, hi), std::max(lo, hi));
        if(nv != v) { v = nv; changed = true; }
    }
    if(ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { v = defValue; changed = true; }
    if(ImGui::BeginPopupContextItem("##type")) {
        ImGui::TextUnformatted(label);
        float t = v;
        ImGui::SetNextItemWidth(110 * z);
        if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if(ImGui::InputFloat("##val", &t, 0, 0, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
            v = std::clamp(t, std::min(lo, hi), std::max(lo, hi)); changed = true; ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    const ImVec2 c(p.x + cellW * 0.5f, p.y + r + 1.0f * z);
    const float a0 = float(M_PI) * 0.75f, a1 = float(M_PI) * 2.25f;
    const float n = span != 0 ? std::clamp((v - lo) / span, 0.0f, 1.0f) : 0.0f;
    const float a = a0 + n * (a1 - a0);
    dl->AddCircleFilled(c, r, pal::knobBody, 20);
    dl->PathArcTo(c, r - 1.5f * z, a0, a1, 20); dl->PathStroke(pal::knobTrack, 0, 2.0f * z);
    // bipolar ranges draw their arc from the centre
    const float zeroN = (lo < 0 && hi > 0) ? -lo / span : 0.0f;
    const float az = a0 + zeroN * (a1 - a0);
    dl->PathArcTo(c, r - 1.5f * z, std::min(az, a), std::max(az, a), 16);
    dl->PathStroke(ImGui::GetColorU32(ImGuiCol_SliderGrabActive), 0, 2.0f * z);
    dl->AddLine(c, ImVec2(c.x + std::cos(a) * (r - 2.5f * z), c.y + std::sin(a) * (r - 2.5f * z)), pal::needle, 1.5f * z);
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize() * 0.95f;
    const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, label);
    dl->AddText(font, fs, ImVec2(c.x - ts.x * 0.5f, p.y + 2 * r + 2.0f * z), pal::textDim, label);
    if(ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetTooltip("%s", tip);
    ImGui::PopID();
    return changed;
}

// An envelope drawn as its curve: drag the corners (A, D, R: left-right; S: up-down).
// ks = the generator's A, D, [S], R parameter indices (Reaktor log times; S linear).
bool scSynthPercBox::envelope(int g, const std::vector<int>& ks, float w, float h) {
    auto& gen = gens[g];
    const float z = ui();
    const bool hasS = ks.size() == 4;
    const int kA = ks[0], kD = ks[1], kS = hasS ? ks[2] : -1, kR = ks.back();
    const auto& P = gen.data->params;
    auto norm = [&](int k) { const auto& p = P[k]; return std::clamp((gen.value(k) - p.lo) / (p.hi - p.lo), 0.0f, 1.0f); };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::PushID(kA);
    ImGui::InvisibleButton("##env", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), pal::bg, 3.0f * z);
    dl->AddRect(o, ImVec2(o.x + w, o.y + h), pal::border, 3.0f * z);
    // each time segment takes up to a quarter (third without S) of the width, by its log value
    const float seg = (w - 6 * z) / (hasS ? 4.0f : 3.0f);
    const float sx = o.x + 3 * z, top = o.y + 4 * z, bot = o.y + h - 4 * z;
    const float s = hasS ? gen.value(kS) : 0.0f;
    const float xA = sx + norm(kA) * seg, xD = xA + norm(kD) * seg;
    const float yS = bot - s * (bot - top);
    const float xS = hasS ? xD + seg * 0.6f : xD;
    const float xR = xS + norm(kR) * seg;
    std::vector<ImVec2> pts{{sx, bot}, {xA, top}};
    auto expo = [&](ImVec2 a, ImVec2 b, int n) {    // Reaktor's decay / release: exponential
        for(int i = 1; i <= n; ++i) { const float t = float(i) / n; pts.push_back({a.x + (b.x - a.x) * t, b.y + (a.y - b.y) * std::exp(-5 * t)}); }
    };
    expo({xA, top}, {xD, yS}, 10);
    if(hasS) pts.push_back({xS, yS});
    expo({xS, yS}, {xR, bot}, 10);
    const ImU32 col = genCol(g);
    std::vector<ImVec2> fill = pts; fill.push_back({xR, bot});
    dl->AddConvexPolyFilled(fill.data(), int(fill.size()), genCol(g, 40));
    dl->AddPolyline(pts.data(), int(pts.size()), col, 0, 1.5f * z);
    // the handle nearest to the mouse: which value a drag changes
    static int dragging = -1;
    const ImVec2 m = ImGui::GetIO().MousePos;
    struct H { ImVec2 p; int k; bool vertical; };
    std::vector<H> handles{{{xA, top}, kA, false}, {{xD, yS}, kD, false}};
    if(hasS) handles.push_back({{xS, yS}, kS, true});
    handles.push_back({{xR, bot}, kR, false});
    int near = -1; float best = 1e9f;
    for(int i = 0; i < int(handles.size()); ++i) {
        const float d = std::hypot(handles[i].p.x - m.x, handles[i].p.y - m.y);
        if(d < best) { best = d; near = i; }
    }
    if(ImGui::IsItemActivated()) dragging = near;
    if(!active && dragging >= 0 && !ImGui::IsMouseDown(0)) dragging = -1;
    const int hl = active ? dragging : (hovered ? near : -1);
    for(int i = 0; i < int(handles.size()); ++i)
        dl->AddCircleFilled(handles[i].p, (i == hl ? 3.5f : 2.5f) * z, i == hl ? pal::needle : col);
    bool changed = false;
    if(active && dragging >= 0 && dragging < int(handles.size()) && ImGui::IsMouseDragging(0, 0.0f)) {
        const auto& hd = handles[dragging];
        const auto& p = P[hd.k];
        const float span = p.hi - p.lo;
        const float d = hd.vertical ? -ImGui::GetIO().MouseDelta.y / (bot - top) : ImGui::GetIO().MouseDelta.x / seg;
        const float fine = ImGui::GetIO().KeyShift ? 0.2f : 1.0f;
        const float nv = std::clamp(gen.value(hd.k) + d * span * fine, std::min(p.lo, p.hi), std::max(p.lo, p.hi));
        if(nv != gen.value(hd.k)) { edit(g, hd.k, nv); changed = true; }
    }
    if(hovered || active) {
        std::string tip = std::string(P[kA].label) + " " + msText(gen.value(kA)) + "   " + P[kD].label + " " + msText(gen.value(kD));
        if(hasS) tip += "   S " + ofToString(gen.value(kS), 2);
        tip += "   " + std::string(P[kR].label) + " " + msText(gen.value(kR)) + "\nDrag a corner (Shift: fine)";
        ImGui::SetTooltip("%s", tip.c_str());
    }
    ImGui::PopID();
    return changed;
}

void scSynthPercBox::drawWindow() {
    const float z = zoom();
    ImGui::SetNextWindowSize(ImVec2(1500 * z, 900 * z), ImGuiCond_FirstUseEver);
    bool open = show.get();
    if(!ImGui::Begin(getWindowId().c_str(), &open)) { ImGui::End(); if(!open) show = false; return; }
    if(!open) { show = false; ImGui::End(); return; }
    const float w = ImGui::GetContentRegionAvail().x;
    // header: snapshots (the whole kit), input, gain
    snaps.drawBar(w, false, false);
    ImGui::SetNextItemWidth(80 * z);
    int mode = inputMode.get();
    if(ImGui::Combo("Input", &mode, INPUT_MODES, 2)) inputMode = mode;
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Kit: Gate channel n plays generator n, its Pitch picks the pad (60 = pad 1): the timeline's Drum Grid.\n"
                                                 "Notes: Reaktor's key ranges (K1 0-13, STOMPS 14-27, NOIX 28-41, CLIX 43-56, CRAX 59-72, QUAD 73-86).");
    ImGui::SameLine(0, 16 * z);
    ImGui::SetNextItemWidth(140 * z);
    float gdb = gainDb.get();
    if(ImGui::SliderFloat("Gain", &gdb, gainDb.getMin(), gainDb.getMax(), "%.1f dB")) gainDb = gdb;
    ImGui::SameLine(0, 16 * z);
    ImGui::TextDisabled("Click a pad: play  ·  Shift+click: store the sound  ·  right-click: rename / revert");
    ImGui::Separator();
    ImGui::BeginChild("##rows", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    for(int g = 0; g < GENS; ++g) drawRow(g, w);
    ImGui::EndChild();
    ImGui::End();
}

void scSynthPercBox::drawRow(int g, float w) {
    auto& gen = gens[g];
    const auto& P = gen.data->params;
    const float z = ui();
    const float knobH = 2 * 9.0f * z + 13.0f * z, rowH = 2 * knobH + 16.0f * z;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::PushID(g);
    dl->AddRectFilled(o, ImVec2(o.x + std::max(w, ImGui::GetContentRegionAvail().x), o.y + rowH), pal::rowBg, 4.0f * z);
    dl->AddRectFilled(o, ImVec2(o.x + 3 * z, o.y + rowH), genCol(g), 2.0f * z);

    // ── name, play, mute ──
    ImGui::SetCursorScreenPos(ImVec2(o.x + 8 * z, o.y + 6 * z));
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, genCol(g));
    ImGui::TextUnformatted(gen.data->name);
    ImGui::PopStyleColor();
    const std::string shownName = gen.shown == LIVE ? "edited" : (gen.names[gen.shown].empty() ? "Pad " + ofToString(gen.shown + 1) : gen.names[gen.shown]);
    ImGui::TextDisabled("%s", shownName.c_str());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3 * z, 1 * z));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3 * z, 3 * z));
    ImGui::Button("Play");
    if(ImGui::IsItemActivated()) press(g, gen.shown);
    if(ImGui::IsItemDeactivated()) releasePress(g);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Play the sound the controls show (held while pressed)");
    ImGui::SameLine();
    if(gen.mute) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(170, 60, 60, 255));
    const bool wasMuted = gen.mute;
    if(ImGui::Button("M")) { gen.mute = !gen.mute; for(auto& [s, st] : states) sendGen(s, st, g, false); }
    if(wasMuted) ImGui::PopStyleColor();
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Mute %s", gen.data->name);
    if(g == 4) {   // CRAX's Randomize
        ImGui::SameLine();
        ImGui::Button("Rnd");
        if(ImGui::IsItemActivated()) randomize(g);
        if(ImGui::IsItemDeactivated()) releasePress(g);
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Randomize: the oscillator, jitter, LFO and filter anywhere in their range, and play (Reaktor's Randomize)");
    }
    ImGui::PopStyleVar(2);
    ImGui::EndGroup();

    // ── pads: 8 x 2 ──
    const float px = o.x + 108 * z, cell = 17.0f * z, gap = 3.0f * z;
    const float padTop = o.y + (rowH - (2 * cell + gap)) * 0.5f;
    const float now = ofGetElapsedTimef();
    for(int p = 0; p < PADS; ++p) {
        const int cx = p % 8, cy = p / 8;
        const ImVec2 a(px + cx * (cell + gap), padTop + cy * (cell + gap)), b(a.x + cell, a.y + cell);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(p);
        ImGui::InvisibleButton("##pad", ImVec2(cell, cell));
        if(ImGui::IsItemActivated()) {
            if(ImGui::GetIO().KeyShift) storePad(g, p);
            else press(g, p);
        }
        if(ImGui::IsItemDeactivated() && gen.held >= 0) releasePress(g);
        const float lit = std::clamp(1.0f - (now - gen.flash[p]) / 0.35f, 0.0f, 1.0f);
        const bool sel = gen.shown == p;
        ImU32 fillC = gen.stored[p] ? genCol(g, 255, 0.35f + 0.25f * float(sel)) : IM_COL32(40, 41, 48, 255);
        dl->AddRectFilled(a, b, fillC, 3.0f * z);
        if(lit > 0) dl->AddRectFilled(a, b, genCol(g, int(220 * lit)), 3.0f * z);
        dl->AddRect(a, b, sel ? pal::needle : pal::border, 3.0f * z, 0, sel ? 1.5f * z : 1.0f);
        const std::string num = ofToString(p + 1);
        const float fs = ImGui::GetFontSize() * 0.9f;
        const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, num.c_str());
        dl->AddText(ImGui::GetFont(), fs, ImVec2(a.x + (cell - ts.x) * 0.5f, a.y + (cell - ts.y) * 0.5f),
                    gen.stored[p] ? pal::text : pal::textDim, num.c_str());
        if(ImGui::IsItemHovered()) {
            const bool notes = inputMode.get() == 1;
            const std::string how = notes ? (p < 14 ? "note " + ofToString(FIRST_NOTE[g] + p) : "no key (Notes: 14 per generator)")
                                          : "pitch " + ofToString(60 + p);
            ImGui::SetTooltip("%s\n%s  ·  %s\nClick: play  ·  Shift+click: store the current sound here",
                              gen.names[p].empty() ? ("Pad " + ofToString(p + 1)).c_str() : gen.names[p].c_str(),
                              gen.stored[p] ? "stored" : "empty (plays the bank's last sound)", how.c_str());
        }
        if(ImGui::BeginPopupContextItem("##padMenu")) {
            static char buf[64];
            if(ImGui::IsWindowAppearing()) { std::snprintf(buf, sizeof(buf), "%s", gen.names[p].c_str()); ImGui::SetKeyboardFocusHere(); }
            ImGui::SetNextItemWidth(140 * z);
            if(ImGui::InputText("Name", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) { gen.names[p] = buf; ImGui::CloseCurrentPopup(); }
            if(ImGui::MenuItem("Store the current sound here")) storePad(g, p);
            const auto& bank = gen.data->pads;
            if(ImGui::MenuItem("Revert to the factory sound", nullptr, false, true)) {
                const int src = std::min(p, int(bank.size()) - 1);
                gen.rows[p] = bank[src].values;
                gen.stored[p] = p < int(bank.size());
                gen.names[p] = gen.stored[p] ? bank[p].name : "";
                for(auto& [s, st] : states) sendGen(s, st, g, true);
            }
            if(ImGui::MenuItem("Revert the whole generator")) { resetToFactory(g); for(auto& [s, st] : states) sendGen(s, st, g, true); }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    // ── controls by section ──
    float x = px + 8 * (cell + gap) + 12 * z;
    std::vector<std::string> sections;
    for(const auto& p : P) if(std::find(sections.begin(), sections.end(), p.section) == sections.end()) sections.push_back(p.section);
    const float cellW = 34.0f * z, top = o.y + 16 * z;
    for(const auto& sec : sections) {
        std::vector<int> ks, env;
        for(int k = 0; k < int(P.size()); ++k) if(sec == P[k].section) (P[k].kind == 'e' ? env : ks).push_back(k);
        // the section's width: its knobs in two lines, its envelope beside them
        const int cols = (int(ks.size()) + 1) / 2;
        const float envW = env.empty() ? 0 : 86 * z;
        const float secW = cols * cellW + envW + (cols && !env.empty() ? 4 * z : 0);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.95f, ImVec2(x, o.y + 3 * z), genCol(g, 200), sec.c_str());
        dl->AddLine(ImVec2(x + secW + 5 * z, o.y + 6 * z), ImVec2(x + secW + 5 * z, o.y + rowH - 6 * z), pal::border);
        float ex = x;
        for(int i = 0; i < int(ks.size()); ++i) {
            const int k = ks[i];
            const auto& p = P[k];
            const ImVec2 cpos(x + (i / 2) * cellW, top + (i % 2) * knobH);
            ImGui::SetCursorScreenPos(cpos);
            ImGui::PushID(k);
            float v = gen.value(k);
            const float def = gen.data->pads.front().values[k];
            if(p.kind == 'l') {
                // a list: its entries, click to cycle (right-click: pick)
                std::vector<std::string> opts = ofSplitString(p.options, "|");
                const int idx = std::clamp(int(std::lround(v)), 0, int(opts.size()) - 1);
                const ImVec2 bs(cellW - 3 * z, 18 * z);
                ImGui::SetCursorScreenPos(ImVec2(cpos.x, cpos.y + 2 * z));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1, 1));
                ImGui::SetWindowFontScale(0.95f);
                if(ImGui::Button(opts[idx].c_str(), bs)) edit(g, k, float((idx + 1) % int(opts.size())));
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PopStyleVar();
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s  (click: next, right-click: list)", p.label, opts[idx].c_str());
                if(ImGui::BeginPopupContextItem("##list")) {
                    for(int o2 = 0; o2 < int(opts.size()); ++o2) if(ImGui::MenuItem(opts[o2].c_str(), nullptr, o2 == idx)) edit(g, k, float(o2));
                    ImGui::EndPopup();
                }
                const float fs = ImGui::GetFontSize() * 0.95f;
                const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, p.label);
                dl->AddText(ImGui::GetFont(), fs, ImVec2(cpos.x + (cellW - ts.x) * 0.5f, cpos.y + 2 * 9.0f * z + 2 * z), pal::textDim, p.label);
            } else if(p.kind == 't') {
                bool on = v > 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(cpos.x + (cellW - ImGui::GetFrameHeight()) * 0.5f, cpos.y + 1 * z));
                if(ImGui::Checkbox("##t", &on)) edit(g, k, on ? 1.0f : 0.0f);
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s %s", p.label, on ? "on" : "off");
                const float fs = ImGui::GetFontSize() * 0.95f;
                const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, p.label);
                dl->AddText(ImGui::GetFont(), fs, ImVec2(cpos.x + (cellW - ts.x) * 0.5f, cpos.y + 2 * 9.0f * z + 2 * z), pal::textDim, p.label);
            } else {
                const std::string tip = std::string(sec) + " " + p.label + ": " + ofToString(v, std::fabs(p.hi - p.lo) > 50 ? 1 : 3)
                                      + "\nDrag (Shift: fine) · double-click: pad 1's · right-click: type";
                if(knob("##k", v, p.lo, p.hi, p.step, p.label, tip.c_str(), def)) edit(g, k, v);
            }
            ImGui::PopID();
            ex = std::max(ex, cpos.x + cellW);
        }
        if(!env.empty()) {
            ImGui::SetCursorScreenPos(ImVec2(ks.empty() ? x : ex + 4 * z, top + 2 * z));
            envelope(g, env, envW, 2 * knobH - 8 * z);
        }
        x += secW + 12 * z;
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + rowH + 4 * z));
    ImGui::Dummy(ImVec2(x - o.x, 0));
    ImGui::PopID();
}
