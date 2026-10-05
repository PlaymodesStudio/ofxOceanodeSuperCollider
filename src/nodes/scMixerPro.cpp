//
//  scMixerPro.cpp
//  ofxOceanodeSuperCollider
//
//  See scMixerPro.h for the design.
//

#include "scMixerPro.h"
#include "ofxOceanodeContainer.h"
#include "ofxOceanodeConnection.h"
#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxSuperCollider.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace {
const char* kBandNames[scEQEditor::NUM_BANDS] = {"Lo", "LoMid", "Mid", "HiMid", "Hi"};
const float kBandFreq[scEQEditor::NUM_BANDS]  = {80.0f, 250.0f, 1000.0f, 4000.0f, 12000.0f};

const ofColor kTrackPalette[] = {
    ofColor(214, 92, 92),  ofColor(222, 150, 70), ofColor(214, 196, 84), ofColor(120, 190, 96),
    ofColor(80, 182, 160), ofColor(84, 150, 214), ofColor(132, 116, 214), ofColor(196, 104, 186)
};

ImU32 toU32(const ofColor& c, int alpha = 255) { return IM_COL32(c.r, c.g, c.b, alpha); }

// Button colours for a state: hovering / pressing only brightens the state's
// own colour, so an "on" button doesn't look "off" under the mouse
void pushButtonColours(const ImVec4& c) {
    auto lighter = [&](float k) {
        return ImVec4(std::min(1.0f, c.x + (1.0f - c.x) * k), std::min(1.0f, c.y + (1.0f - c.y) * k),
                      std::min(1.0f, c.z + (1.0f - c.z) * k), c.w);
    };
    ImGui::PushStyleColor(ImGuiCol_Button, c);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, lighter(0.15f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, lighter(0.30f));
}
}

// ════════════════════════════════════════════════════════════════════════════
// Construction
// ════════════════════════════════════════════════════════════════════════════

scMixerPro::scMixerPro(std::vector<serverManager*> _servers)
: scNode("Mixer Pro")
, servers(std::move(_servers))
{}

scMixerPro::~scMixerPro() {
    nodeListeners.unsubscribeAll();
    for(Track* tr : allStrips()) {
        tr->listeners.unsubscribeAll();
        destroyRuntime(*tr, true);
    }
}

void scMixerPro::setup() {
    description = "Multichannel, multi-server mixer: each track runs on its own server, one output per server. "
                  "Dockable mixer window with VU / peak, fader, balance, mute / solo, per-track EQ and DC correction.";

    addParameter(showWindow.set("Show", false));
    addParameter(numChannels.set("N Chan", 2, 1, MAX_CHANNELS));
    addParameter(numTracks.set("Num Inputs", 4, 1, MAX_TRACKS));
    addParameter(showFaders.set("Show Faders", false));
    addParameter(gainVec.set("Gain Vec", vector<float>(4, 1.0f), vector<float>(1, 0.0f), vector<float>(1, 2.0f)));
    addParameter(balanceVec.set("Balance Vec", vector<float>(4, 0.0f), vector<float>(1, -1.0f), vector<float>(1, 1.0f)));
    addParameter(masterLevel.set("Master Level", vector<float>(1, 1.0f), vector<float>(1, 0.0f), vector<float>(1, 2.0f)));

    masterVuOut.set("Master VU", vector<float>(2, 0.0f), vector<float>(1, 0.0f), vector<float>(1, 2.0f));
    addOutputAction(masterVuOut.getEscapedName(), masterVuOut);
    addInspectorParameter(vuAttack.set("VU Attack", 10.0f, 1.0f, 100.0f));
    addInspectorParameter(vuRelease.set("VU Release", 300.0f, 10.0f, 2000.0f));
    currentChannels = numChannels.get();

    // One output per server, ahead of the inputs so that inputs added later
    // stay together at the end of the node
    for(int i = 0; i < (int)servers.size(); i++) scNode::addOutput("Out S" + ofToString(i + 1));

    setTrackCount(numTracks.get());

    nodeListeners.push(numTracks.newListener([this](int& n) {
        if(rebuildingTracks) return;
        pendingTrackCount = ofClamp(n, 1, MAX_TRACKS);
    }));
    nodeListeners.push(numChannels.newListener([this](int& n) {
        setChannelCount(ofClamp(n, 1, MAX_CHANNELS));
    }));
    // As the PolyMixer: Gain Vec is an independent per-track multiplier of the
    // faders (index t; a missing entry is 1)...
    nodeListeners.push(gainVec.newListener([this](vector<float>&) {
        for(auto& tr : tracks) sendLevel(*tr);
    }));
    // ...and Balance Vec sets each track's balance (index t; not synced back
    // when a balance knob moves)
    nodeListeners.push(balanceVec.newListener([this](vector<float>& v) {
        for(size_t t = 0; t < tracks.size() && t < v.size(); t++) {
            const float b = ofClamp(v[t], -1.0f, 1.0f);
            if(tracks[t]->balance.get() != b) tracks[t]->balance.set(b);
        }
    }));
    nodeListeners.push(masterLevel.newListener([this](vector<float>&) {
        for(Track* tr : allStrips()) sendMasterLevel(*tr);
    }));
    nodeListeners.push(showFaders.newListener([this](bool&) { updateFaders(); }));
    auto vuTimes = [this](float&) {
        for(Track* tr : allStrips()) if(tr->synth) {
            tr->synth->set("vuAttackTime", vuAttack.get());
            tr->synth->set("vuReleaseTime", vuRelease.get());
        }
    };
    nodeListeners.push(vuAttack.newListener(vuTimes));
    nodeListeners.push(vuRelease.newListener(vuTimes));

    requireSynthdefs();
}

void scMixerPro::requireSynthdefs() {
    // Like BeatRepeat Pro: find the compiled folder anywhere under the
    // SynthDefs directory, else next to the addon's source.
    std::filesystem::path folder;
    const std::string expected = "mixerProTrack2.scsyndef";
    try {
        const std::filesystem::path dataRoot = ofToDataPath(SYNTHDEF_DIRECTORY, true);
        if(std::filesystem::exists(dataRoot)) {
            for(const auto& entry : std::filesystem::recursive_directory_iterator(
                    dataRoot, std::filesystem::directory_options::skip_permission_denied)) {
                if(entry.is_regular_file() && entry.path().filename() == expected) {
                    folder = entry.path().parent_path();
                    break;
                }
            }
        }
        if(folder.empty()) {
            const std::filesystem::path bundled = std::filesystem::path(__FILE__).parent_path()
                .parent_path() / "sc/MixerPro/CompiledSynthdefs/mixerpro";
            if(std::filesystem::exists(bundled / expected)) folder = bundled;
        }
    } catch(const std::exception& error) {
        ofLogWarning("scMixerPro") << "SynthDef search failed: " << error.what();
    }
    if(folder.empty()) {
        ofLogError("scMixerPro") << "SC Mixer Pro SynthDefs (mixerProTrackN) were not found: "
                                 << "run mixerpro.scd and copy CompiledSynthdefs/mixerpro into "
                                 << SYNTHDEF_DIRECTORY;
        return;
    }
    for(auto* manager : servers) if(manager) manager->requireSynthdefFolder(folder.string());
}

// ════════════════════════════════════════════════════════════════════════════
// Servers
// ════════════════════════════════════════════════════════════════════════════

int scMixerPro::serverIndexOf(ofxSCServer* server) const {
    for(int i = 0; i < (int)servers.size(); i++)
        if(servers[i] && servers[i]->getServer() == server) return i;
    return -1;
}

ofxSCServer* scMixerPro::serverAt(int index) const {
    if(index < 0 || index >= (int)servers.size() || !servers[index]) return nullptr;
    return servers[index]->getServer();
}

bool scMixerPro::isServerActive(int index) const {
    ofxSCServer* server = serverAt(index);
    if(!server) return false;
    auto it = serverStates.find(server);
    return it != serverStates.end() && it->second.active;
}

// Same walk as scNode::appendOrderedNodes, restricted to the tracks of the
// server being built. Without a known server (never the case when called by
// serverManager) every input is followed, as any node does.
bool scMixerPro::appendOrderedNodes(vector<scNode*>& nodesList,
                                    map<scNode*, std::pair<int, vector<int>>>& visitedNodeChilds,
                                    vector<scNode*> parents) {
    const int k = serverIndexOf(getGraphTraversalServer());
    parents.push_back(this);
    for(size_t t = 0; t < tracks.size() && t < availableInputs.size(); t++) {
        if(k >= 0 && tracks[t]->server != k) continue;
        scNode* source = availableInputs[t]->getNodeRef();
        if(source == nullptr) continue;
        if(std::find(parents.begin(), parents.end(), source) != parents.end()) continue;
        source->appendOrderedNodes(nodesList, visitedNodeChilds, parents);
    }
    if(std::find(nodesList.begin(), nodesList.end(), this) == nodesList.end()) {
        nodesList.push_back(this);
        return true;
    }
    return false;
}

void scMixerPro::getConnections(std::map<nodePort, vector<scNode*>>& connections) {
    const int k = serverIndexOf(getGraphTraversalServer());
    for(size_t t = 0; t < tracks.size() && t < availableInputs.size(); t++) {
        if(k >= 0 && tracks[t]->server != k) continue;
        if(availableInputs[t]->getNodeRef() != nullptr)
            connections[*availableInputs[t]].push_back(this);
    }
}

// Re-evaluate the graph of every server: re-sending the outputs reaches the
// Outputs connected to them, each of which recomputes its own server.
void scMixerPro::refreshGraph() {
    if(holdGraphRefresh) return;   // reorderTracks refreshes once at the end
    for(auto& output : outputs) output = output;
}

// ════════════════════════════════════════════════════════════════════════════
// Tracks
// ════════════════════════════════════════════════════════════════════════════

void scMixerPro::setTrackCount(int count) {
    count = ofClamp(count, 1, MAX_TRACKS);
    if(count == (int)tracks.size()) return;
    rebuildingTracks = true;
    const bool removing = count < (int)tracks.size();
    while((int)tracks.size() < count) addTrack();
    while((int)tracks.size() > count) removeLastTrack();
    if(numTracks.get() != count) numTracks.set(count);
    // Both vectors follow the track count (as the PolyMixer)
    vector<float> g = gainVec.get();
    g.resize(tracks.size(), 1.0f);
    gainVec.setWithoutEventNotifications(g);
    vector<float> b = balanceVec.get();
    b.resize(tracks.size(), 0.0f);
    balanceVec.setWithoutEventNotifications(b);
    rebuildingTracks = false;
    if(removing) refreshGraph();
}

void scMixerPro::addTrack() {
    auto tr = std::make_unique<Track>();
    Track& t = *tr;
    t.index = (int)tracks.size();
    const std::string n = ofToString(t.index + 1);
    t.name = "Track " + n;
    t.color = kTrackPalette[t.index % (sizeof(kTrackPalette) / sizeof(kTrackPalette[0]))];
    t.server = 0;

    initStrip(t, n);

    tracks.push_back(std::move(tr));
    scNode::addInput("In " + n);
    registerTrackActions(t);
    if(showFaders.get()) updateFaders();

    // A new track starts on server 1; create its synth if that server runs us
    if(isServerActive(t.server)) createRuntime(t, serverAt(t.server));
}

// Parameters and listeners shared by tracks and buses (n: name suffix)
void scMixerPro::initStrip(Track& t, const std::string& n) {
    t.level.set("Level " + n, 1.0f, 0.0f, 2.0f);
    t.balance.set("Balance " + n, 0.0f, -1.0f, 1.0f);
    t.mute.set("Mute " + n, false);
    t.solo.set("Solo " + n, false);
    t.eq.set("EQ " + n, false);
    t.dc.set("DC " + n, false);
    t.vuOut.set("VU " + n, vector<float>(currentChannels, 0.0f), vector<float>(1, 0.0f), vector<float>(1, 2.0f));
    t.scSource.set("SC Source " + n, 0, 0, MAX_TRACKS);
    t.scStrength.set("SC Strength " + n, 1.0f, 0.0f, 1.0f);
    t.scAttack.set("SC Attack " + n, 10.0f, 0.1f, 500.0f);
    t.scRelease.set("SC Release " + n, 200.0f, 1.0f, 3000.0f);
    t.scThreshold.set("SC Threshold " + n, -18.0f, -60.0f, 0.0f);
    t.adaptMode.set("Adapt Mode " + n, 0, 0, 3);
    t.inChannels.set("In Ch " + n, 0, 0, MAX_CHANNELS);
    t.rotate.set("Rotate " + n, 0, 0, MAX_CHANNELS - 1);
    for(int b = 0; b < scEQEditor::NUM_BANDS; b++) {
        const std::string prefix = "EQ " + n + " " + kBandNames[b];
        const bool shelf = scEQEditor::isShelf(b);
        t.eqFreq[b].set(prefix + " Freq", kBandFreq[b], scEQEditor::FREQ_MIN, scEQEditor::FREQ_MAX);
        t.eqGain[b].set(prefix + " Gain", 0.0f, -24.0f, 24.0f);
        t.eqShape[b].set(prefix + (shelf ? " Slope" : " Q"), 1.0f, shelf ? 0.1f : 0.1f, shelf ? 4.0f : 20.0f);
    }
    t.vu.assign(currentChannels, 0.0f);
    t.peak.assign(currentChannels, 0.0f);
    t.peakAge.assign(currentChannels, 0.0f);

    Track* tp = &t;
    t.output.set("Out " + n, 0, 0, MAX_BUSES);
    t.listeners.push(t.output.newListener([this](int&) { refreshTrackOutputs(); }));
    t.listeners.push(t.level.newListener([this, tp](float&) { sendLevel(*tp); }));
    t.listeners.push(t.balance.newListener([tp](float& v) {
        if(tp->synth) tp->synth->set("balance", ofClamp(v, -1.0f, 1.0f));
    }));
    t.listeners.push(t.mute.newListener([this](bool&) { updateMutes(); }));
    t.listeners.push(t.solo.newListener([this](bool&) { updateMutes(); }));
    t.listeners.push(t.eq.newListener([this, tp](bool&) { updateInsert(*tp); }));
    t.listeners.push(t.dc.newListener([this, tp](bool&) { updateInsert(*tp); }));
    // Source changes are routed by update(); the shape goes straight out
    auto onShape = [this, tp](float&) { sendSidechainShape(*tp); };
    t.listeners.push(t.scStrength.newListener(onShape));
    t.listeners.push(t.scAttack.newListener(onShape));
    t.listeners.push(t.scRelease.newListener(onShape));
    t.listeners.push(t.scThreshold.newListener(onShape));
    auto onAdapt = [this, tp](int&) { updateAdapter(*tp); };
    t.listeners.push(t.adaptMode.newListener(onAdapt));
    t.listeners.push(t.inChannels.newListener(onAdapt));
    t.listeners.push(t.rotate.newListener(onAdapt));
    for(int b = 0; b < scEQEditor::NUM_BANDS; b++) {
        auto onBand = [this, tp](float&) { tp->eqCurveDirty = true; sendEQ(*tp); };
        t.listeners.push(t.eqFreq[b].newListener(onBand));
        t.listeners.push(t.eqGain[b].newListener(onBand));
        t.listeners.push(t.eqShape[b].newListener(onBand));
    }
}

void scMixerPro::removeLastTrack() {
    if(tracks.empty()) return;
    Track& t = *tracks.back();
    t.listeners.unsubscribeAll();
    destroyRuntime(t, true);
    unregisterTrackActions(t);
    if(selectedEqTrack == t.index) selectedEqTrack = -1;
    scNode::removeInput((int)tracks.size() - 1);
    tracks.pop_back();
    updateMutes();
}

void scMixerPro::setStripServer(Track& tr, int server) {
    server = ofClamp(server, 0, std::max(0, (int)servers.size() - 1));
    if(tr.server == server) return;
    destroyRuntime(tr, true);
    tr.server = server;
    tr.srcBus = -1;
    if(isServerActive(server)) createRuntime(tr, serverAt(server));
    refreshTrackOutputs();
    // A track's source chain moves with it: both servers rebuild
    if(!tr.isBus) refreshGraph();
}

// ── Reorder / delete ────────────────────────────────────────────────────────

// Every per-strip parameter, in a fixed order (a "role" per position)
std::vector<ofAbstractParameter*> scMixerPro::stripParams(Track& t) {
    std::vector<ofAbstractParameter*> p = {
        &t.level, &t.balance, &t.mute, &t.solo, &t.eq, &t.dc, &t.vuOut,
        &t.scSource, &t.scStrength, &t.scAttack, &t.scRelease, &t.scThreshold,
        &t.adaptMode, &t.inChannels, &t.rotate, &t.output
    };
    for(int b = 0; b < scEQEditor::NUM_BANDS; b++) {
        p.push_back(&t.eqFreq[b]);
        p.push_back(&t.eqGain[b]);
        p.push_back(&t.eqShape[b]);
    }
    return p;
}

// Rebuild the tracks in a new order (order[new position] = old index; an
// old index left out is deleted). A track's numbered parameters belong to its
// position, so everything that makes a track moves with it: its settings, the
// source connected to its input, its published parameters and their
// connections, its Gain / Balance Vec entries and sidechain references.
void scMixerPro::reorderTracks(const std::vector<int>& order) {
    ofxOceanodeContainer* container = getHostContainer();
    if(order.empty() || container == nullptr) return;
    const int oldCount = (int)tracks.size();

    auto paramByName = [this](const std::string& escaped) -> ofxOceanodeAbstractParameter* {
        auto& group = getParameterGroup();
        if(!group.contains(escaped)) return nullptr;
        return dynamic_cast<ofxOceanodeAbstractParameter*>(&group.get(escaped));
    };
    auto isOurs = [this](ofxOceanodeAbstractParameter& p) { return p.getNodeModel() == this; };

    struct Snapshot {
        ofJson json;
        ofxOceanodeAbstractParameter* inputSource = nullptr;
        std::vector<bool> published;
        std::vector<ofxOceanodeAbstractParameter*> roleIn;
        std::vector<std::vector<ofxOceanodeAbstractParameter*>> roleOut;
    };
    std::vector<Snapshot> snaps(oldCount);

    // No graph rebuild per (dis)connection: a source briefly without a path to
    // an Output would be freed and recreated, with new buses
    suppressInputRefresh = true;
    holdGraphRefresh = true;

    // 1. Remember and disconnect
    for(int j = 0; j < oldCount; j++) {
        Track& tr = *tracks[j];
        Snapshot& snap = snaps[j];
        snap.json = saveStrip(tr);
        if(j < (int)inputs.size()) {
            if(auto* in = paramByName(inputs[j].getEscapedName())) {
                if(auto* c = in->getInConnection()) {
                    if(!isOurs(c->getSourceParameter())) snap.inputSource = &c->getSourceParameter();
                    c->deleteSelf();
                }
            }
        }
        const auto params = stripParams(tr);
        snap.published.assign(params.size(), false);
        snap.roleIn.assign(params.size(), nullptr);
        snap.roleOut.assign(params.size(), {});
        for(size_t r = 0; r < params.size(); r++) {
            const std::string key = params[r]->getEscapedName();
            snap.published[r] = isPublished(key);
            auto handle = nodeHandles.find(key);
            if(handle == nodeHandles.end() || !handle->second) continue;
            if(auto* c = handle->second->getInConnection()) {
                if(!isOurs(c->getSourceParameter())) snap.roleIn[r] = &c->getSourceParameter();
                c->deleteSelf();
            }
            for(auto* c : handle->second->getOutConnections()) {
                if(!isOurs(c->getSinkParameter())) snap.roleOut[r].push_back(&c->getSinkParameter());
                c->deleteSelf();
            }
        }
    }
    // 2. Unpublish every track parameter (republished by position below)
    for(int j = 0; j < oldCount; j++)
        for(auto* p : stripParams(*tracks[j]))
            if(isPublished(p->getEscapedName())) unpublishKey(p->getEscapedName());

    const vector<float> oldGain = gainVec.get(), oldBalance = balanceVec.get();
    std::vector<int> newPosition(oldCount, -1);
    for(int i = 0; i < (int)order.size(); i++) newPosition[order[i]] = i;
    const int oldEq = selectedEqTrack;

    // 3. New count, then each position takes its track's settings
    setTrackCount((int)order.size());
    for(int i = 0; i < (int)order.size(); i++) loadStrip(*tracks[i], snaps[order[i]].json);
    // Sidechain sources refer to track numbers
    for(Track* tr : allStrips()) {
        const int s = tr->scSource.get() - 1;
        if(s < 0) continue;
        tr->scSource.set(s < oldCount && newPosition[s] >= 0 ? newPosition[s] + 1 : 0);
    }
    vector<float> g(order.size(), 1.0f), b(order.size(), 0.0f);
    for(int i = 0; i < (int)order.size(); i++) {
        if(order[i] < (int)oldGain.size()) g[i] = oldGain[order[i]];
        if(order[i] < (int)oldBalance.size()) b[i] = oldBalance[order[i]];
    }
    gainVec.setWithoutEventNotifications(g);
    balanceVec.setWithoutEventNotifications(b);
    for(auto& tr : tracks) sendLevel(*tr);
    if(oldEq >= 0 && oldEq < 1000) selectedEqTrack = oldEq < oldCount ? newPosition[oldEq] : -1;

    // 4. Republish and reconnect, by position
    for(int i = 0; i < (int)order.size(); i++) {
        const Snapshot& snap = snaps[order[i]];
        const auto params = stripParams(*tracks[i]);
        for(size_t r = 0; r < params.size(); r++)
            if(snap.published[r]) publishKey(params[r]->getEscapedName());
    }
    if(showFaders.get()) updateFaders();
    for(int i = 0; i < (int)order.size(); i++) {
        const Snapshot& snap = snaps[order[i]];
        if(snap.inputSource && i < (int)inputs.size())
            if(auto* in = paramByName(inputs[i].getEscapedName()))
                container->createConnection(*snap.inputSource, *in);
        const auto params = stripParams(*tracks[i]);
        for(size_t r = 0; r < params.size(); r++) {
            auto handle = nodeHandles.find(params[r]->getEscapedName());
            if(handle == nodeHandles.end() || !handle->second) continue;
            if(snap.roleIn[r]) container->createConnection(*snap.roleIn[r], *handle->second);
            for(auto* sink : snap.roleOut[r]) container->createConnection(*handle->second, *sink);
        }
    }
    parameterGroupChanged.notify(this);

    // One rebuild. When the same sources still feed the mixer, serverManager
    // sees no change and sends no input buses: take them from the ports.
    suppressInputRefresh = false;
    holdGraphRefresh = false;
    refreshGraph();
    for(auto& trp : tracks) {
        Track& tr = *trp;
        if(!tr.liveServer) continue;
        int bus = -1;
        if(tr.index < (int)availableInputs.size() && availableInputs[tr.index]->getNodeRef())
            bus = availableInputs[tr.index]->getBusIndex(tr.liveServer);
        if(bus < 0) bus = serverStates[tr.liveServer].silentBus;
        tr.srcBus = bus;
        applyInputRouting(tr);
        updateAdapter(tr);
    }
}

// ── Buses (submasters) ──────────────────────────────────────────────────────

void scMixerPro::setBusCount(int count) {
    count = ofClamp(count, 0, MAX_BUSES);
    while((int)buses.size() < count) addBus();
    while((int)buses.size() > count) removeLastBus();
}

void scMixerPro::addBus() {
    auto bp = std::make_unique<Track>();
    Track& b = *bp;
    b.isBus = true;
    b.index = (int)buses.size();
    b.name = std::string("Bus ") + char('A' + b.index);
    b.color = kTrackPalette[(b.index + 3) % (sizeof(kTrackPalette) / sizeof(kTrackPalette[0]))];
    b.server = 0;
    initStrip(b, "B" + ofToString(b.index + 1));
    buses.push_back(std::move(bp));
    registerTrackActions(b);
    if(showFaders.get()) updateFaders();
    if(isServerActive(b.server)) createRuntime(b, serverAt(b.server));
    refreshTrackOutputs();
}

void scMixerPro::removeLastBus() {
    if(buses.empty()) return;
    Track& b = *buses.back();
    b.listeners.unsubscribeAll();
    destroyRuntime(b, true);
    unregisterTrackActions(b);
    if(selectedEqTrack == eqId(b)) selectedEqTrack = -1;
    buses.pop_back();
    refreshTrackOutputs();
}

std::vector<scMixerPro::Track*> scMixerPro::allStrips() {
    std::vector<Track*> all;
    all.reserve(tracks.size() + buses.size());
    for(auto& t : tracks) all.push_back(t.get());
    for(auto& b : buses) all.push_back(b.get());
    return all;
}

scMixerPro::Track* scMixerPro::busFor(const Track& tr) const {
    if(tr.isBus) return nullptr;
    const int k = tr.output.get() - 1;
    if(k < 0 || k >= (int)buses.size()) return nullptr;
    Track* b = buses[k].get();
    return b->server == tr.server ? b : nullptr;   // buses can't cross servers
}

bool scMixerPro::routedToBus(const Track& tr) const {
    Track* b = busFor(tr);
    return b && b->synth && b->mixBus && b->liveServer == tr.liveServer;
}

int scMixerPro::trackOutBus(Track& tr) {
    if(routedToBus(tr)) return busFor(tr)->mixBus->index;
    return outBusFor(tr.liveServer);
}

// Where every live track writes, and its master level (1 into a bus)
void scMixerPro::refreshTrackOutputs() {
    for(auto& tr : tracks) {
        if(!tr->synth) continue;
        tr->synth->set("out", trackOutBus(*tr));
        sendMasterLevel(*tr);
    }
    updateMutes();
}

scMixerPro::Track* scMixerPro::eqTarget() {
    if(selectedEqTrack >= 1000) {
        const int b = selectedEqTrack - 1000;
        return b < (int)buses.size() ? buses[b].get() : nullptr;
    }
    if(selectedEqTrack >= 0 && selectedEqTrack < (int)tracks.size()) return tracks[selectedEqTrack].get();
    return nullptr;
}

void scMixerPro::setChannelCount(int channels) {
    if(channels == currentChannels) return;
    currentChannels = channels;
    for(Track* trp : allStrips()) {
        Track& tr = *trp;
        tr.vu.assign(channels, 0.0f);
        tr.peak.assign(channels, 0.0f);
        tr.peakAge.assign(channels, 0.0f);
        if(!tr.synth || !tr.liveServer) continue;
        ofxSCServer* server = tr.liveServer;
        dropAdapter(tr, true);   // its def and bus depend on the width

        // Buses of the new width
        if(tr.vuBus) { tr.vuBus->free(); delete tr.vuBus; }
        tr.vuBus = new ofxSCBus(RATE_CONTROL, vuBusChannels(), server);
        if(tr.mixBus) {
            tr.mixBus->free(); delete tr.mixBus;
            tr.mixBus = new ofxSCBus(RATE_AUDIO, channels, server);
            tr.srcBus = tr.mixBus->index;
        }
        if(tr.insertBus) {
            tr.insertBus->free(); delete tr.insertBus;
            tr.insertBus = new ofxSCBus(RATE_AUDIO, channels, server);
        }

        // Replace in place (kAddAction_replace), keeping the execution order
        auto* synth = new ofxSCSynth(trackDefName(), server);
        synth->createAndRun(4, tr.synth->nodeID, getActive());
        orderReplace(server, tr.synth, synth);
        delete tr.synth;
        tr.synth = synth;
        if(tr.insert) {
            auto* insert = new ofxSCSynth(insertDefName(tr.insertVariant), server);
            insert->createAndRun(4, tr.insert->nodeID, getActive() && tr.insertRunning);
            orderReplace(server, tr.insert, insert);
            delete tr.insert;
            tr.insert = insert;
        }
        tr.sentMute = -1.0f;
        sendAll(tr);
        updateAdapter(tr);
    }
    refreshTrackOutputs();
    masterVU.assign(channels, 0.0f);
    masterPeak.assign(channels, 0.0f);
    masterPeakAge.assign(channels, 0.0f);
}

bool scMixerPro::anySolo() const {
    for(const auto& tr : tracks) if(tr->solo.get()) return true;
    for(const auto& b : buses) if(b->solo.get()) return true;
    return false;
}

// A soloed bus keeps its tracks audible; a soloed track keeps its bus audible
bool scMixerPro::isSilenced(const Track& tr) const {
    if(tr.mute.get()) return true;
    if(!anySolo() || tr.solo.get()) return false;
    if(!tr.isBus) {
        Track* b = busFor(tr);
        return !(b && b->solo.get());
    }
    for(const auto& t : tracks) if(t->solo.get() && busFor(*t) == &tr) return false;
    return true;
}

void scMixerPro::updateMutes() {
    for(Track* tr : allStrips()) {
        if(!tr->synth) continue;
        const float muted = isSilenced(*tr) ? 1.0f : 0.0f;
        if(muted == tr->sentMute) continue;
        tr->synth->set("mute", muted);
        tr->sentMute = muted;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Synths
// ════════════════════════════════════════════════════════════════════════════

std::string scMixerPro::trackDefName() const {
    return "mixerProTrack" + ofToString(currentChannels);
}

std::string scMixerPro::insertDefName(int variant) const {
    const char* base = variant == 3 ? "mixerProEQDC" : (variant == 2 ? "mixerProDC" : "mixerProEQ");
    return base + ofToString(currentChannels);
}

int scMixerPro::outBusFor(ofxSCServer* server) {
    const int k = serverIndexOf(server);
    auto& state = serverStates[server];
    auto it = state.outputBuses.find(k);
    return it != state.outputBuses.end() ? it->second : -1;
}

void scMixerPro::orderInsertBefore(ofxSCServer* server, ofxSCSynth* added, ofxSCSynth* before) {
    auto& order = serverStates[server].order;
    auto it = std::find(order.begin(), order.end(), before);
    order.insert(it, added);
}

void scMixerPro::orderReplace(ofxSCServer* server, ofxSCSynth* oldSynth, ofxSCSynth* newSynth) {
    auto& order = serverStates[server].order;
    std::replace(order.begin(), order.end(), oldSynth, newSynth);
}

void scMixerPro::orderRemove(ofxSCServer* server, ofxSCSynth* synth) {
    auto& order = serverStates[server].order;
    order.erase(std::remove(order.begin(), order.end(), synth), order.end());
}

// The track synth goes to the head of the default group, which is where a
// node's synths start (serverManager orders them afterwards); the insert, if
// any, right before it.
void scMixerPro::createRuntime(Track& tr, ofxSCServer* server) {
    if(!server || tr.synth) return;
    tr.liveServer = server;
    const int n = currentChannels;
    tr.vuBus = new ofxSCBus(RATE_CONTROL, vuBusChannels(), server);
    const int variant = (tr.eq.get() ? 1 : 0) | (tr.dc.get() ? 2 : 0);
    if(variant != 0) tr.insertBus = new ofxSCBus(RATE_AUDIO, n, server);

    if(tr.isBus) {
        tr.mixBus = new ofxSCBus(RATE_AUDIO, n, server);
        tr.srcBus = tr.mixBus->index;
    }
    tr.synth = new ofxSCSynth(trackDefName(), server);
    tr.sentMute = -1.0f;
    sendAll(tr);   // stored as /s_new arguments
    tr.synth->set("in", variant != 0 ? tr.insertBus->index : chainInput(tr));
    auto& order = serverStates[server].order;
    if(tr.isBus && !order.empty()) {
        // Buses run after every track of the node
        tr.synth->createAndRun(3, order.back()->nodeID, getActive());   // addAfter
        order.push_back(tr.synth);
    } else {
        tr.synth->createAndRun(0, 1, getActive());
        order.insert(order.begin(), tr.synth);
    }

    if(variant != 0) {
        tr.insert = new ofxSCSynth(insertDefName(variant), server);
        tr.insertVariant = variant;
        tr.insert->set("in", chainInput(tr));
        tr.insert->set("out", tr.insertBus->index);
        sendEQ(tr);
        tr.insert->createAndRun(2, tr.synth->nodeID, getActive());   // addBefore
        orderInsertBefore(server, tr.insert, tr.synth);
        tr.insertRunning = true;
    }
    tr.detectedInputs = sourceChannels(tr);
    updateAdapter(tr);
    if(tr.isBus) refreshTrackOutputs();
}

void scMixerPro::destroyRuntime(Track& tr, bool sendFree) {
    ofxSCServer* server = tr.liveServer;
    auto drop = [&](ofxSCSynth*& s) {
        if(!s) return;
        if(server) orderRemove(server, s);
        if(sendFree) s->free();
        delete s;
        s = nullptr;
    };
    dropAdapter(tr, sendFree);
    drop(tr.insert);
    drop(tr.synth);
    // After a server reboot the allocators were reset: the addresses are no
    // longer ours to give back
    auto dropBus = [&](ofxSCBus*& b) {
        if(!b) return;
        if(sendFree) b->free();
        delete b;
        b = nullptr;
    };
    dropBus(tr.vuBus);
    dropBus(tr.insertBus);
    dropBus(tr.mixBus);
    tr.insertVariant = 0;
    tr.insertRunning = false;
    tr.liveServer = nullptr;
    tr.sentMute = -1.0f;
    std::fill(tr.vu.begin(), tr.vu.end(), 0.0f);
    if(tr.isBus) { tr.srcBus = -1; refreshTrackOutputs(); }   // its tracks go to the master
}

int scMixerPro::chainInput(const Track& tr) const {
    return (tr.adapt && tr.adaptRunning && tr.adaptBus) ? tr.adaptBus->index : tr.srcBus;
}

void scMixerPro::applyInputRouting(Track& tr) {
    if(!tr.synth) return;
    if(tr.adapt && tr.adaptRunning && tr.adaptBus) {
        tr.adapt->set("in", tr.srcBus);
        tr.adapt->set("out", tr.adaptBus->index);
    }
    if(tr.insert && tr.insertRunning && tr.insertBus) {
        tr.insert->set("in", chainInput(tr));
        tr.insert->set("out", tr.insertBus->index);
        tr.synth->set("in", tr.insertBus->index);
    } else {
        tr.synth->set("in", chainInput(tr));
    }
}

// ── Channel adapter ─────────────────────────────────────────────────────────

// The source's channel count: In Ch if set, else the "N Chan" of the node
// connected to the track's input, else the mixer's own count.
int scMixerPro::sourceChannels(Track& tr) {
    if(tr.isBus) return currentChannels;
    if(tr.inChannels.get() > 0) return ofClamp(tr.inChannels.get(), 1, MAX_CHANNELS);
    if(tr.index < (int)availableInputs.size()) {
        scNode* node = availableInputs[tr.index]->getNodeRef();
        if(node) {
            auto& group = node->getParameterGroup();
            if(group.contains("N_Chan")) {
                // Oceanode keeps its own wrapper in the group, not the ofParameter
                ofAbstractParameter& param = group.get("N_Chan");
                if(auto* p = dynamic_cast<ofxOceanodeParameter<int>*>(&param))
                    return ofClamp(p->getParameter().get(), 1, MAX_CHANNELS);
                if(auto* p = dynamic_cast<ofxOceanodeParameter<float>*>(&param))
                    return ofClamp((int)std::round(p->getParameter().get()), 1, MAX_CHANNELS);
                if(auto* p = dynamic_cast<ofParameter<int>*>(&param))
                    return ofClamp(p->get(), 1, MAX_CHANNELS);
            }
        }
    }
    return currentChannels;
}

// Gains, row-major (input i to output j at i*N+j)
std::vector<float> scMixerPro::adaptMatrix(const Track& tr, int M) const {
    const int N = currentChannels;
    std::vector<float> g(M * N, 0.0f);
    auto at = [&](int i, int j) -> float& { return g[i * N + j]; };
    const float halfPi = 1.57079632679f;
    switch(tr.adaptMode.get()) {
        case 1:   // Wrap: L R L R L R  /  folding: input i to output i mod N
            if(M <= N) { for(int j = 0; j < N; j++) at(j % M, j) = 1.0f; }
            else       { for(int i = 0; i < M; i++) at(i, i % N) = 1.0f; }
            break;
        case 2:   // Blocks: L L L R R R  /  folding contiguous groups
            if(M <= N) { for(int j = 0; j < N; j++) at(std::min(M - 1, j * M / N), j) = 1.0f; }
            else       { for(int i = 0; i < M; i++) at(i, std::min(N - 1, i * N / M)) = 1.0f; }
            break;
        case 3:   // Stretch: equal-power interpolation across the outputs
            if(M == 1)      { for(int j = 0; j < N; j++) at(0, j) = 1.0f; }
            else if(N == 1) { for(int i = 0; i < M; i++) at(i, 0) = 1.0f; }
            else if(M <= N) {
                for(int j = 0; j < N; j++) {
                    const float x = (float)j * (M - 1) / (N - 1);
                    const int i0 = std::min(M - 2, (int)std::floor(x));
                    const float f = x - i0;
                    at(i0, j) += std::cos(f * halfPi);
                    at(i0 + 1, j) += std::sin(f * halfPi);
                }
            } else {
                for(int i = 0; i < M; i++) {
                    const float y = (float)i * (N - 1) / (M - 1);
                    const int j0 = std::min(N - 2, (int)std::floor(y));
                    const float f = y - j0;
                    at(i, j0) += std::cos(f * halfPi);
                    at(i, j0 + 1) += std::sin(f * halfPi);
                }
            }
            break;
        default:  // Direct
            for(int i = 0; i < std::min(M, N); i++) at(i, i) = 1.0f;
            break;
    }
    // Several inputs summed into one output: keep the power (1/sqrt(sum g^2))
    if(M > N) {
        for(int j = 0; j < N; j++) {
            float power = 0.0f;
            for(int i = 0; i < M; i++) power += at(i, j) * at(i, j);
            if(power > 1.0f) {
                const float k = 1.0f / std::sqrt(power);
                for(int i = 0; i < M; i++) at(i, j) *= k;
            }
        }
    }
    // Rotate the outputs
    const int r = ((tr.rotate.get() % N) + N) % N;
    if(r != 0) {
        std::vector<float> rotated(M * N, 0.0f);
        for(int i = 0; i < M; i++)
            for(int j = 0; j < N; j++) rotated[i * N + (j + r) % N] = at(i, j);
        g.swap(rotated);
    }
    return g;
}

void scMixerPro::dropAdapter(Track& tr, bool sendFree) {
    if(tr.adapt) {
        if(tr.liveServer) orderRemove(tr.liveServer, tr.adapt);
        if(sendFree) tr.adapt->free();
        delete tr.adapt;
        tr.adapt = nullptr;
    }
    if(tr.adaptBus) {
        if(sendFree) tr.adaptBus->free();
        delete tr.adaptBus;
        tr.adaptBus = nullptr;
    }
    tr.adaptInputs = 0;
    tr.adaptRunning = false;
    tr.sentMatrix.clear();
}

void scMixerPro::updateAdapter(Track& tr) {
    if(tr.isBus || !tr.synth || !tr.liveServer) return;   // applied when the synth is created
    ofxSCServer* server = tr.liveServer;
    const int M = sourceChannels(tr);
    tr.detectedInputs = M;
    const std::vector<float> matrix = adaptMatrix(tr, M);

    // The direct matrix is what the track already does on its own (it reads
    // the first N channels): no adapter, it costs nothing
    bool direct = true;
    for(int i = 0; i < M && direct; i++)
        for(int j = 0; j < currentChannels; j++)
            if(matrix[i * currentChannels + j] != (i == j ? 1.0f : 0.0f)) { direct = false; break; }

    if(direct) {
        if(tr.adapt && tr.adaptRunning) {
            tr.adapt->run(false);
            tr.adaptRunning = false;
            applyInputRouting(tr);
        }
        return;
    }

    if(!tr.adaptBus) tr.adaptBus = new ofxSCBus(RATE_AUDIO, currentChannels, server);
    const std::string def = "mixerProAdapt" + ofToString(M) + "_" + ofToString(currentChannels);
    if(!tr.adapt || tr.adaptInputs != M) {
        auto* adapt = new ofxSCSynth(def, server);
        adapt->set("in", tr.srcBus);
        adapt->set("out", tr.adaptBus->index);
        adapt->set("matrix", matrix);
        if(tr.adapt) {
            adapt->createAndRun(4, tr.adapt->nodeID, getActive());   // replace in place
            orderReplace(server, tr.adapt, adapt);
            delete tr.adapt;
        } else {
            ofxSCSynth* first = tr.insert ? tr.insert : tr.synth;     // head of the track's chain
            adapt->createAndRun(2, first->nodeID, getActive());        // addBefore
            orderInsertBefore(server, adapt, first);
        }
        tr.adapt = adapt;
        tr.adaptInputs = M;
    } else {
        if(matrix != tr.sentMatrix) tr.adapt->set("matrix", matrix);
        if(!tr.adaptRunning && getActive()) tr.adapt->run(true);
    }
    tr.sentMatrix = matrix;
    tr.adaptRunning = true;
    applyInputRouting(tr);
}

void scMixerPro::updateInsert(Track& tr) {
    if(!tr.synth || !tr.liveServer) return;   // applied when the synth is created
    ofxSCServer* server = tr.liveServer;
    const int variant = (tr.eq.get() ? 1 : 0) | (tr.dc.get() ? 2 : 0);

    if(variant == 0) {
        // Paused: costs nothing; the track reads its source directly again
        if(tr.insert && tr.insertRunning) {
            tr.synth->set("in", chainInput(tr));
            tr.insert->run(false);
            tr.insertRunning = false;
        }
        return;
    }

    if(!tr.insertBus) tr.insertBus = new ofxSCBus(RATE_AUDIO, currentChannels, server);
    if(!tr.insert) {
        tr.insert = new ofxSCSynth(insertDefName(variant), server);
        tr.insert->set("in", chainInput(tr));
        tr.insert->set("out", tr.insertBus->index);
        sendEQ(tr);
        tr.insert->createAndRun(2, tr.synth->nodeID, getActive());
        orderInsertBefore(server, tr.insert, tr.synth);
    } else if(tr.insertVariant != variant) {
        auto* insert = new ofxSCSynth(insertDefName(variant), server);
        insert->set("in", chainInput(tr));
        insert->set("out", tr.insertBus->index);
        ofxSCSynth* old = tr.insert;
        tr.insert = insert;
        sendEQ(tr);
        insert->createAndRun(4, old->nodeID, getActive());   // replace in place
        orderReplace(server, old, insert);
        delete old;
    } else {
        tr.insert->set("in", chainInput(tr));
        if(getActive()) tr.insert->run(true);
    }
    tr.insertVariant = variant;
    tr.insertRunning = true;
    tr.synth->set("in", tr.insertBus->index);
}

// Fader times Gain Vec (a missing entry counts as 1)
void scMixerPro::sendLevel(Track& tr) {
    if(!tr.synth) return;
    const auto& v = gainVec.get();
    const float g = (!tr.isBus && tr.index < (int)v.size()) ? v[tr.index] : 1.0f;
    tr.synth->setMultiple("level", ofClamp(tr.level.get(), 0.0f, 2.0f) * ofClamp(g, 0.0f, 2.0f), currentChannels);
}

void scMixerPro::sendMasterLevel(Track& tr) {
    if(!tr.synth) return;
    if(routedToBus(tr)) {   // the bus applies the master level
        tr.synth->setMultiple("masterLevel", 1.0f, currentChannels);
        return;
    }
    const auto& m = masterLevel.get();
    if(m.size() <= 1) {
        tr.synth->setMultiple("masterLevel", m.empty() ? 1.0f : ofClamp(m[0], 0.0f, 2.0f), currentChannels);
    } else {
        vector<float> v(currentChannels);
        for(int c = 0; c < currentChannels; c++) v[c] = ofClamp(m[std::min(c, (int)m.size() - 1)], 0.0f, 2.0f);
        tr.synth->set("masterLevel", v);
    }
}

void scMixerPro::sendEQ(Track& tr) {
    if(!tr.insert || !(tr.insertVariant & 1)) return;
    for(int b = 0; b < scEQEditor::NUM_BANDS; b++) {
        const std::string k = "b" + ofToString(b + 1);
        tr.insert->set(k + "gain", tr.eqGain[b].get());
        tr.insert->set(k + "freq", tr.eqFreq[b].get());
        // As GrainBox / Graphic EQ: shelves take rs = 1/S, kept where the
        // shelf stays defined; peaks take rq = 1/Q
        if(scEQEditor::isShelf(b))
            tr.insert->set(k + "slope", 1.0f / scEQEditor::safeShelfSlope(tr.eqShape[b].get(), tr.eqGain[b].get()));
        else
            tr.insert->set(k + "rq", 1.0f / std::max(0.01f, tr.eqShape[b].get()));
    }
}

void scMixerPro::sendAll(Track& tr) {
    if(!tr.synth) return;
    ofxSCServer* server = tr.liveServer;
    tr.synth->set("out", trackOutBus(tr));
    if(tr.vuBus) tr.synth->set("vuBus", tr.vuBus->index);
    tr.synth->set("vuAttackTime", vuAttack.get());
    tr.synth->set("vuReleaseTime", vuRelease.get());
    tr.synth->set("balance", ofClamp(tr.balance.get(), -1.0f, 1.0f));
    sendLevel(tr);
    sendMasterLevel(tr);
    tr.sentMute = isSilenced(tr) ? 1.0f : 0.0f;
    tr.synth->set("mute", tr.sentMute);
    applyInputRouting(tr);
    sendEQ(tr);
    sendSidechainShape(tr);
    // The source bus / key are sent by update() (the source may not exist yet)
    tr.synth->set("scBus", -1);
    tr.synth->set("scKey", 0.0f);
    tr.sentScBus = -1;
    tr.sentScKey = 0.0f;
}

void scMixerPro::sendSidechainShape(Track& tr) {
    if(!tr.synth) return;
    tr.synth->set("scStrength", ofClamp(tr.scStrength.get(), 0.0f, 1.0f));
    tr.synth->set("scAttack", std::max(0.1f, tr.scAttack.get()));
    tr.synth->set("scRelease", std::max(0.1f, tr.scRelease.get()));
    tr.synth->set("scThresh", ofClamp(std::pow(10.0f, tr.scThreshold.get() / 20.0f), 0.0001f, 1.0f));
}

// Which key each ducked track reads: the source's key bus when both run on
// the same server, else the key relayed by the node (scKey).
void scMixerPro::routeSidechains() {
    for(auto& tr : tracks) tr->keyNeeded = false;
    for(Track* trp : allStrips()) {
        Track& tr = *trp;
        const int s = tr.scSource.get() - 1;
        Track* src = (s >= 0 && s < (int)tracks.size() && (tr.isBus || s != tr.index) && tr.scStrength.get() > 0.0f)
                   ? tracks[s].get() : nullptr;
        int bus = -1;
        tr.scMode = 0;
        if(src) {
            if(tr.synth && src->synth && src->vuBus && src->liveServer == tr.liveServer) {
                bus = src->vuBus->index + currentChannels;
                tr.scMode = 1;
            } else {
                tr.scMode = 2;
                if(tr.synth) src->keyNeeded = true;
            }
        }
        if(tr.synth && bus != tr.sentScBus) {
            tr.synth->set("scBus", bus);
            tr.sentScBus = bus;
        }
    }
}

void scMixerPro::sendSidechainKeys() {
    for(Track* trp : allStrips()) {
        Track& tr = *trp;
        if(!tr.synth) continue;
        float key = 0.0f;
        if(tr.scMode == 2) {
            const int s = tr.scSource.get() - 1;
            if(s >= 0 && s < (int)tracks.size() && tracks[s]->synth) key = tracks[s]->key;
        }
        if(std::abs(key - tr.sentScKey) > 1e-4f) {
            tr.synth->set("scKey", key);
            tr.sentScKey = key;
        }
    }
}

void scMixerPro::resendParametersForNRT() {
    for(Track* tr : allStrips()) if(tr->synth) sendAll(*tr);
}

void scMixerPro::activate() {
    for(Track* tr : allStrips()) {
        if(tr->synth) tr->synth->run(true);
        if(tr->insert && tr->insertRunning) tr->insert->run(true);
        if(tr->adapt && tr->adaptRunning) tr->adapt->run(true);
    }
}

void scMixerPro::deactivate() {
    for(Track* tr : allStrips()) {
        if(tr->synth) tr->synth->run(false);
        if(tr->insert) tr->insert->run(false);
        if(tr->adapt) tr->adapt->run(false);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// serverManager interface
// ════════════════════════════════════════════════════════════════════════════

void scMixerPro::buildSynth(ofxSCServer* server) {
    // Anything still live here belongs to a server life that has ended (a
    // reboot clears the graph without freeing the nodes): forget it without
    // freeing, its node IDs and bus addresses are gone.
    for(Track* tr : allStrips()) if(tr->liveServer == server) destroyRuntime(*tr, false);
    serverStates[server].order.clear();
}

void scMixerPro::createSynth(ofxSCServer* server) {
    auto& state = serverStates[server];
    state.active = true;
    const int k = serverIndexOf(server);
    // Each at the head: created last-to-first, they end up in track order
    for(int t = (int)tracks.size() - 1; t >= 0; t--) {
        if(tracks[t]->server == k) createRuntime(*tracks[t], server);
    }
    // Then the buses, each after everything else (in bus order)
    for(auto& b : buses) if(b->server == k) createRuntime(*b, server);
    refreshTrackOutputs();
}

void scMixerPro::free(ofxSCServer* server) {
    for(Track* tr : allStrips()) if(tr->liveServer == server) destroyRuntime(*tr, true);
    auto& state = serverStates[server];
    state.order.clear();
    state.active = false;
}

void scMixerPro::moveSynthBefore(ofxSCServer* server, int nodeID) {
    // Every parameter is sent as it changes; the bus wiring was just sent by
    // setOutputBus / setInputBus. Only the execution order is left.
    auto& order = serverStates[server].order;
    int target = nodeID;
    for(auto it = order.rbegin(); it != order.rend(); ++it) {
        if(!*it) continue;
        (*it)->moveBefore(target);
        target = (*it)->nodeID;
    }
}

int scMixerPro::getLastSynthID(ofxSCServer* server) {
    auto it = serverStates.find(server);
    if(it == serverStates.end() || it->second.order.empty()) return -1;
    return it->second.order.front()->nodeID;
}

void scMixerPro::setOutputBus(ofxSCServer* server, int index, int bus) {
    serverStates[server].outputBuses[index] = bus;
    if(index != serverIndexOf(server)) return;
    for(Track* tr : allStrips()) if(tr->liveServer == server && tr->synth) tr->synth->set("out", trackOutBus(*tr));
}

int scMixerPro::getOutputBusIndex(ofxSCServer* server, int index) {
    auto it = serverStates.find(server);
    if(it == serverStates.end()) return -1;
    auto bus = it->second.outputBuses.find(index);
    return bus != it->second.outputBuses.end() ? bus->second : -1;
}

void scMixerPro::setInputBus(ofxSCServer* server, scNode* node, int bus) {
    // A source can feed several tracks, through several of its outputs: match
    // the port, not only the node
    // Recorded for the tracks of this server even before their synths exist:
    // when the graph is first built (a preset load) serverManager sends the
    // input buses before createSynth.
    const int k = serverIndexOf(server);
    for(size_t t = 0; t < tracks.size() && t < availableInputs.size(); t++) {
        Track& tr = *tracks[t];
        if(tr.server != k) continue;
        if(availableInputs[t]->getNodeRef() != node) continue;
        if(availableInputs[t]->getBusIndex(server) != bus) continue;
        tr.srcBus = bus;
        if(tr.liveServer == server) applyInputRouting(tr);
    }
}

void scMixerPro::resetInputBusses(ofxSCServer* server, int targetBus) {
    serverStates[server].silentBus = targetBus;
    const int k = serverIndexOf(server);
    for(auto& tr : tracks) {
        if(tr->server != k) continue;
        tr->srcBus = targetBus;
        if(tr->liveServer == server) applyInputRouting(*tr);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Publishing (same behaviour as GrainBox / BeatRepeat Pro)
// ════════════════════════════════════════════════════════════════════════════

void scMixerPro::registerTrackActions(Track& tr) {
    addAction(tr.level.getEscapedName(), tr.level);
    addAction(tr.balance.getEscapedName(), tr.balance);
    addAction(tr.mute.getEscapedName(), tr.mute);
    addAction(tr.solo.getEscapedName(), tr.solo);
    addAction(tr.eq.getEscapedName(), tr.eq);
    addAction(tr.dc.getEscapedName(), tr.dc);
    addOutputAction(tr.vuOut.getEscapedName(), tr.vuOut);
    addAction(tr.scSource.getEscapedName(), tr.scSource);
    addAction(tr.scStrength.getEscapedName(), tr.scStrength);
    addAction(tr.scAttack.getEscapedName(), tr.scAttack);
    addAction(tr.scRelease.getEscapedName(), tr.scRelease);
    addAction(tr.scThreshold.getEscapedName(), tr.scThreshold);
    if(!tr.isBus) addAction(tr.output.getEscapedName(), tr.output);
    addAction(tr.adaptMode.getEscapedName(), tr.adaptMode);
    addAction(tr.inChannels.getEscapedName(), tr.inChannels);
    addAction(tr.rotate.getEscapedName(), tr.rotate);
    for(int b = 0; b < scEQEditor::NUM_BANDS; b++) {
        addAction(tr.eqFreq[b].getEscapedName(), tr.eqFreq[b]);
        addAction(tr.eqGain[b].getEscapedName(), tr.eqGain[b]);
        addAction(tr.eqShape[b].getEscapedName(), tr.eqShape[b]);
    }
}

void scMixerPro::unregisterTrackActions(Track& tr) {
    std::vector<std::string> keys = {
        tr.level.getEscapedName(), tr.balance.getEscapedName(), tr.mute.getEscapedName(),
        tr.solo.getEscapedName(), tr.eq.getEscapedName(), tr.dc.getEscapedName(),
        tr.vuOut.getEscapedName(), tr.scSource.getEscapedName(), tr.scStrength.getEscapedName(),
        tr.scAttack.getEscapedName(), tr.scRelease.getEscapedName(), tr.scThreshold.getEscapedName(),
        tr.adaptMode.getEscapedName(), tr.inChannels.getEscapedName(), tr.rotate.getEscapedName(),
        tr.output.getEscapedName()
    };
    for(int b = 0; b < scEQEditor::NUM_BANDS; b++) {
        keys.push_back(tr.eqFreq[b].getEscapedName());
        keys.push_back(tr.eqGain[b].getEscapedName());
        keys.push_back(tr.eqShape[b].getEscapedName());
    }
    for(const auto& key : keys) {
        // The parameter is about to disappear: take it out of the node even
        // if connected (as removing a track input does)
        auto it = nodeHandles.find(key);
        if(it != nodeHandles.end()) {
            removeParameter(key);
            nodeHandles.erase(it);
        }
        publishedKeys.erase(std::remove(publishedKeys.begin(), publishedKeys.end(), key), publishedKeys.end());
        faderKeys.erase(std::remove(faderKeys.begin(), faderKeys.end(), key), faderKeys.end());
        publishActions.erase(key);
    }
    if(publishedKeys.empty() && publishedSeparatorAdded) { removeSeparator("Published"); publishedSeparatorAdded = false; }
    if(faderKeys.empty() && faderSeparatorAdded) { removeSeparator("Faders"); faderSeparatorAdded = false; }
    parameterGroupChanged.notify(this);
}

bool scMixerPro::isPublished(const std::string& key) const {
    return std::find(publishedKeys.begin(), publishedKeys.end(), key) != publishedKeys.end();
}

bool scMixerPro::hasNodeConnection(const std::string& key) const {
    auto it = nodeHandles.find(key);
    return it != nodeHandles.end() && it->second
        && (it->second->hasInConnection() || it->second->hasOutConnections());
}

bool scMixerPro::publishKey(const std::string& key) {
    auto action = publishActions.find(key);
    if(action == publishActions.end() || isPublished(key)) return false;
    if(nodeHandles.count(key) == 0) {      // not already in the node as a fader
        if(!publishedSeparatorAdded) { addSeparator("Published", ofColor(200)); publishedSeparatorAdded = true; }
        nodeHandles[key] = action->second.add();
    }
    publishedKeys.push_back(key);
    parameterGroupChanged.notify(this);
    return true;
}

bool scMixerPro::unpublishKey(const std::string& key) {
    if(!isPublished(key)) return false;
    const bool fader = std::find(faderKeys.begin(), faderKeys.end(), key) != faderKeys.end();
    if(!fader && hasNodeConnection(key)) return false;
    publishedKeys.erase(std::remove(publishedKeys.begin(), publishedKeys.end(), key), publishedKeys.end());
    if(!fader) removeNodeHandleIfUnused(key);
    if(publishedKeys.empty() && publishedSeparatorAdded) { removeSeparator("Published"); publishedSeparatorAdded = false; }
    parameterGroupChanged.notify(this);
    return true;
}

void scMixerPro::removeNodeHandleIfUnused(const std::string& key) {
    auto it = nodeHandles.find(key);
    if(it == nodeHandles.end()) return;
    if(isPublished(key)) return;
    if(std::find(faderKeys.begin(), faderKeys.end(), key) != faderKeys.end()) return;
    if(hasNodeConnection(key)) return;
    removeParameter(key);
    nodeHandles.erase(it);
}

void scMixerPro::syncPublished(const std::vector<std::string>& keys) {
    const auto current = publishedKeys;
    for(const auto& key : current)
        if(std::find(keys.begin(), keys.end(), key) == keys.end() && !hasNodeConnection(key)) unpublishKey(key);
    for(const auto& key : keys) if(!isPublished(key)) publishKey(key);
}

// "Show Faders": every track's Level in the node, under its own separator.
// A Level that is also published, or connected, stays when they are hidden.
void scMixerPro::updateFaders() {
    if(showFaders.get()) {
        for(Track* tr : allStrips()) {
            const std::string key = tr->level.getEscapedName();
            if(std::find(faderKeys.begin(), faderKeys.end(), key) != faderKeys.end()) continue;
            if(nodeHandles.count(key) == 0) {
                if(!faderSeparatorAdded) { addSeparator("Faders", ofColor(200)); faderSeparatorAdded = true; }
                nodeHandles[key] = publishActions[key].add();
            }
            faderKeys.push_back(key);
        }
    } else {
        const auto keys = faderKeys;
        faderKeys.clear();
        for(const auto& key : keys) removeNodeHandleIfUnused(key);
        // Connected faders stay, still counted as faders
        for(const auto& key : keys)
            if(nodeHandles.count(key) && !isPublished(key)) faderKeys.push_back(key);
        if(faderKeys.empty() && faderSeparatorAdded) { removeSeparator("Faders"); faderSeparatorAdded = false; }
    }
    parameterGroupChanged.notify(this);
}

void scMixerPro::markPublished(const std::string& key) const {
    if(!isPublished(key)) return;
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y - 1.0f), ImVec2(b.x, b.y - 1.0f),
                                        ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
}

// Value field + publish / unpublish, inside an already open popup
void scMixerPro::drawPublishItems(const std::string& key, const char* title, bool focus) {
    auto action = publishActions.find(key);
    if(action == publishActions.end() || !action->second.parameter) return;
    ofAbstractParameter* parameter = action->second.parameter;
    if(action->second.output) {
        // An output: only publish / unpublish
        if(isPublished(key)) {
            if(hasNodeConnection(key)) ImGui::TextDisabled("Published (disconnect to unpublish)");
            else if(ImGui::MenuItem(("Unpublish VU output##" + key).c_str())) unpublishKey(key);
        } else if(ImGui::MenuItem(("Publish VU output to Node GUI##" + key).c_str())) {
            publishKey(key);
        }
        return;
    }
    static std::map<std::string, std::array<char, 128>> buffers;
    auto& buffer = buffers[key];
    if(ImGui::IsWindowAppearing()) std::snprintf(buffer.data(), buffer.size(), "%s", parameter->toString().c_str());
    ImGui::TextDisabled("%s", title ? title : "Value");
    ImGui::SetNextItemWidth(160.0f * ofxOceanodeShared::getZoomLevel());
    if(focus && ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::PushID(key.c_str());
    const bool enter = ImGui::InputText("##value", buffer.data(), buffer.size(),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopID();
    if(enter || ImGui::IsItemDeactivatedAfterEdit()) {
        try { parameter->fromString(std::string(buffer.data())); } catch(...) {}
        if(enter) ImGui::CloseCurrentPopup();
    }
    if(isPublished(key)) {
        const bool fader = std::find(faderKeys.begin(), faderKeys.end(), key) != faderKeys.end();
        if(!fader && hasNodeConnection(key)) {
            ImGui::TextDisabled("Published (disconnect to unpublish)");
        } else if(ImGui::MenuItem(("Unpublish from Node##" + key).c_str())) {
            unpublishKey(key);
        }
    } else if(ImGui::MenuItem(("Publish to Node GUI##" + key).c_str())) {
        publishKey(key);
    }
}

void scMixerPro::drawVuPublishMenu(const std::string& key) {
    markPublished(key);
    if(!ImGui::BeginPopupContextItem(("##mixerProVu_" + key).c_str())) return;
    ImGui::TextDisabled("Click: reset peaks");
    ImGui::Separator();
    drawPublishItems(key, nullptr, false);
    ImGui::EndPopup();
}

void scMixerPro::drawPublishPopup(const std::string& key) {
    markPublished(key);
    if(!ImGui::BeginPopupContextItem(("##mixerProPublish_" + key).c_str())) return;
    drawPublishItems(key, nullptr, true);
    ImGui::EndPopup();
}

// ════════════════════════════════════════════════════════════════════════════
// Update: meters
// ════════════════════════════════════════════════════════════════════════════

void scMixerPro::updatePeaks(std::vector<float>& vu, std::vector<float>& peak, std::vector<float>& age, float dt) {
    if(peak.size() != vu.size()) { peak.assign(vu.size(), 0.0f); age.assign(vu.size(), 0.0f); }
    // Peak hold: the maximum stays until the user clears it (click on the meter)
    for(size_t c = 0; c < vu.size(); c++) {
        if(vu[c] >= peak[c]) { peak[c] = vu[c]; age[c] = 0.0f; }
        else age[c] += dt;
    }
}

void scMixerPro::update(ofEventArgs&) {
    if(pendingDelete >= 0) {
        const int k = pendingDelete;
        pendingDelete = -1;
        if(k < (int)tracks.size() && tracks.size() > 1) {
            std::vector<int> order;
            for(int t = 0; t < (int)tracks.size(); t++) if(t != k) order.push_back(t);
            reorderTracks(order);
        }
    }
    if(pendingMoveFrom >= 0) {
        const int from = pendingMoveFrom, to = pendingMoveTo;
        pendingMoveFrom = pendingMoveTo = -1;
        if(from < (int)tracks.size() && to >= 0 && to < (int)tracks.size() && from != to) {
            std::vector<int> order;
            for(int t = 0; t < (int)tracks.size(); t++) order.push_back(t);
            order.erase(order.begin() + from);
            order.insert(order.begin() + to, from);
            reorderTracks(order);
        }
    }
    if(pendingTrackCount > 0) {
        const int count = pendingTrackCount;
        pendingTrackCount = -1;
        setTrackCount(count);
    }
    if(pendingBusCount >= 0) {
        const int count = pendingBusCount;
        pendingBusCount = -1;
        setBusCount(count);
    }
    const bool visible = windowVisible;
    windowVisible = false;   // set again by draw() while the window is shown
    // Meters are only read while someone looks at them, or while a VU is
    // published as a node output
    bool vuPublished = false;
    for(const auto& key : publishedKeys) {
        auto action = publishActions.find(key);
        if(action != publishActions.end() && action->second.output) { vuPublished = true; break; }
    }
    // A source's channel count can change (its N Chan, a new connection)
    for(auto& tr : tracks) {
        if(!tr->synth) continue;
        if(sourceChannels(*tr) != tr->detectedInputs) updateAdapter(*tr);
    }

    routeSidechains();
    bool keysNeeded = false;
    for(auto& tr : tracks) if(tr->keyNeeded) { keysNeeded = true; break; }
    const bool meters = visible || vuPublished;
    if(!meters && !keysNeeded) { sendSidechainKeys(); return; }

    const float dt = std::min(0.1f, (float)ofGetLastFrameTime());
    for(Track* tr : allStrips()) {
        if(tr->vuBus && tr->synth) {
            if(meters || tr->keyNeeded) {
                const auto& v = tr->vuBus->readValues;
                for(int c = 0; c < currentChannels && c < (int)tr->vu.size(); c++)
                    tr->vu[c] = c < (int)v.size() && std::isfinite(v[c]) ? v[c] : 0.0f;
                const int k = currentChannels + 1;
                tr->key = k < (int)v.size() && std::isfinite(v[k]) ? ofClamp(v[k], 0.0f, 4.0f) : 0.0f;
                tr->vuBus->requestValues();
            }
        } else {
            std::fill(tr->vu.begin(), tr->vu.end(), 0.0f);
            tr->key = 0.0f;
        }
        if(meters) updatePeaks(tr->vu, tr->peak, tr->peakAge, dt);
    }
    sendSidechainKeys();
    if(!meters) return;

    // Master: the audible tracks summed, times the master level (as the
    // PolyMixer's master meter)
    masterVU.assign(currentChannels, 0.0f);
    const auto& m = masterLevel.get();
    // (tracks into a running bus are counted through the bus)
    for(Track* tr : allStrips()) {
        if(!tr->synth || isSilenced(*tr) || routedToBus(*tr)) continue;
        for(int c = 0; c < currentChannels && c < (int)tr->vu.size(); c++) masterVU[c] += tr->vu[c];
    }
    for(int c = 0; c < currentChannels; c++) {
        const float level = m.empty() ? 1.0f : m[std::min(c, (int)m.size() - 1)];
        masterVU[c] = ofClamp(masterVU[c] * level, 0.0f, 2.0f);
    }
    updatePeaks(masterVU, masterPeak, masterPeakAge, dt);

    if(vuPublished) {
        for(Track* tr : allStrips()) if(isPublished(tr->vuOut.getEscapedName())) tr->vuOut = tr->vu;
        if(isPublished(masterVuOut.getEscapedName())) masterVuOut = masterVU;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Presets
// ════════════════════════════════════════════════════════════════════════════

ofJson scMixerPro::saveStrip(Track& tr) {
    ofJson t;
    t["name"] = tr.name;
    t["color"] = {tr.color.r, tr.color.g, tr.color.b};
    t["server"] = tr.server;
    t["level"] = tr.level.get();
    t["balance"] = tr.balance.get();
    t["mute"] = tr.mute.get();
    t["solo"] = tr.solo.get();
    t["eq"] = tr.eq.get();
    t["dc"] = tr.dc.get();
    t["sidechain"] = {tr.scSource.get(), tr.scStrength.get(), tr.scAttack.get(), tr.scRelease.get(), tr.scThreshold.get()};
    t["adapt"] = {tr.adaptMode.get(), tr.inChannels.get(), tr.rotate.get()};
    t["bands"] = ofJson::array();
    for(int b = 0; b < scEQEditor::NUM_BANDS; b++)
        t["bands"].push_back({tr.eqFreq[b].get(), tr.eqGain[b].get(), tr.eqShape[b].get()});
    if(!tr.isBus) t["output"] = tr.output.get();
    return t;
}

void scMixerPro::presetSave(ofJson& json) {
    ofJson state;
    state["numTracks"] = (int)tracks.size();
    state["numChannels"] = currentChannels;
    state["showFaders"] = showFaders.get();
    state["published"] = publishedKeys;
    state["tracks"] = ofJson::array();
    for(auto& tr : tracks) state["tracks"].push_back(saveStrip(*tr));
    state["numBuses"] = (int)buses.size();
    state["buses"] = ofJson::array();
    for(auto& bus : buses) state["buses"].push_back(saveStrip(*bus));
    json["mixerPro"] = state;
}

void scMixerPro::loadStrip(Track& tr, const ofJson& j) {
    tr.name = j.value("name", tr.name);
    if(j.contains("color") && j["color"].is_array() && j["color"].size() == 3)
        tr.color = ofColor(j["color"][0].get<int>(), j["color"][1].get<int>(), j["color"][2].get<int>());
    const int server = ofClamp(j.value("server", 0), 0, std::max(0, (int)servers.size() - 1));
    if(server != tr.server) setStripServer(tr, server);
    tr.level.set(j.value("level", tr.level.get()));
    tr.balance.set(j.value("balance", tr.balance.get()));
    tr.mute.set(j.value("mute", false));
    tr.solo.set(j.value("solo", false));
    if(j.contains("bands") && j["bands"].is_array()) {
        for(int b = 0; b < scEQEditor::NUM_BANDS && b < (int)j["bands"].size(); b++) {
            const ofJson& band = j["bands"][b];
            if(!band.is_array() || band.size() < 3) continue;
            tr.eqFreq[b].set(band[0].get<float>());
            tr.eqGain[b].set(band[1].get<float>());
            tr.eqShape[b].set(band[2].get<float>());
        }
    }
    tr.eq.set(j.value("eq", false));
    tr.dc.set(j.value("dc", false));
    if(j.contains("adapt") && j["adapt"].is_array() && j["adapt"].size() >= 3) {
        tr.adaptMode.set(ofClamp(j["adapt"][0].get<int>(), 0, 3));
        tr.inChannels.set(ofClamp(j["adapt"][1].get<int>(), 0, MAX_CHANNELS));
        tr.rotate.set(ofClamp(j["adapt"][2].get<int>(), 0, MAX_CHANNELS - 1));
    } else {
        tr.adaptMode.set(0); tr.inChannels.set(0); tr.rotate.set(0);
    }
    if(j.contains("sidechain") && j["sidechain"].is_array() && j["sidechain"].size() >= 4) {
        const ofJson& sc = j["sidechain"];
        tr.scSource.set(ofClamp(sc[0].get<int>(), 0, MAX_TRACKS));
        tr.scStrength.set(sc[1].get<float>());
        tr.scAttack.set(sc[2].get<float>());
        tr.scRelease.set(sc[3].get<float>());
        if(sc.size() >= 5) tr.scThreshold.set(sc[4].get<float>());
    } else {
        tr.scSource.set(0);
    }
    tr.eqCurveDirty = true;
    if(!tr.isBus) tr.output.set(ofClamp(j.value("output", 0), 0, MAX_BUSES));
}

// Before connections: the inputs and the published parameters must exist
// for their connections to be restored.
void scMixerPro::loadBeforeConnections(ofJson& json) {
    if(!json.contains("mixerPro") || !json["mixerPro"].is_object()) return;
    const ofJson& state = json["mixerPro"];
    try {
        const int channels = ofClamp(state.value("numChannels", currentChannels), 1, MAX_CHANNELS);
        if(numChannels.get() != channels) numChannels.set(channels);
        setTrackCount(state.value("numTracks", (int)tracks.size()));

        // Buses first: the tracks' outputs refer to them
        setBusCount(state.value("numBuses", 0));
        if(state.contains("buses") && state["buses"].is_array()) {
            const ofJson& list = state["buses"];
            for(size_t k = 0; k < buses.size() && k < list.size(); k++) loadStrip(*buses[k], list[k]);
        }
        if(state.contains("tracks") && state["tracks"].is_array()) {
            const ofJson& list = state["tracks"];
            for(size_t t = 0; t < tracks.size() && t < list.size(); t++) loadStrip(*tracks[t], list[t]);
        }
        const bool faders = state.value("showFaders", false);
        if(showFaders.get() != faders) showFaders.set(faders);
        if(state.contains("published") && state["published"].is_array())
            syncPublished(state["published"].get<std::vector<std::string>>());
    } catch(const std::exception& e) {
        ofLogError("scMixerPro") << "Could not load the mixer state: " << e.what();
    }
}

void scMixerPro::presetRecallAfterSettingParameters(ofJson&) {
    // Values that arrived as node parameters are already applied by their
    // listeners; live synths (a paste into a running patch) get everything.
    for(Track* tr : allStrips()) if(tr->synth) sendAll(*tr);
}

// ════════════════════════════════════════════════════════════════════════════
// Window
// ════════════════════════════════════════════════════════════════════════════

float scMixerPro::ampToDb(float amp) { return amp <= 1e-6f ? -120.0f : 20.0f * std::log10(amp); }

// Fader / meter scale: -60 dB at the bottom (0 = silence) to +6 dB at the top
float scMixerPro::gainToPos(float gain) {
    if(gain <= 1e-6f) return 0.0f;
    return ofClamp((ampToDb(gain) + 60.0f) / 66.0f, 0.0f, 1.0f);
}

float scMixerPro::posToGain(float pos) {
    if(pos <= 0.002f) return 0.0f;
    return ofClamp(std::pow(10.0f, (pos * 66.0f - 60.0f) / 20.0f), 0.0f, 2.0f);
}

void scMixerPro::draw(ofEventArgs&) {
    if(showWindow.get()) drawWindow();
}

void scMixerPro::drawWindow() {
    const float zoom = ofxOceanodeShared::getZoomLevel();
    const std::string title = (canvasID == "Canvas" ? "" : canvasID + "/") + "SC Mixer Pro " + ofToString(getNumIdentifier());
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(900.0f * zoom, 560.0f * zoom), ImGuiCond_FirstUseEver);
    if(!ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        if(!open) showWindow = false;
        return;
    }
    if(!open) { ImGui::End(); showWindow = false; return; }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if(avail.x < 200.0f * zoom || avail.y < 180.0f * zoom) { ImGui::End(); return; }
    windowVisible = true;

    drawToolbar();

    const bool eqOpen = eqTarget() != nullptr;
    const float eqH = eqOpen ? 190.0f * zoom : 0.0f;
    const float bodyH = std::max(160.0f * zoom, ImGui::GetContentRegionAvail().y - eqH - (eqOpen ? 6.0f * zoom : 0.0f));

    const float masterW = 130.0f * zoom;
    drawMasterStrip(masterW, bodyH);
    ImGui::SameLine(0, 8.0f * zoom);
    ImGui::BeginChild("##mixerProStrips", ImVec2(0, bodyH), false, ImGuiWindowFlags_HorizontalScrollbar);
    const float stripW = 92.0f * zoom;
    const float stripH = std::max(150.0f * zoom, ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ScrollbarSize);
    for(size_t t = 0; t < tracks.size(); t++) {
        ImGui::PushID((int)t);
        // Strips scrolled out of view only take their space
        if(ImGui::IsRectVisible(ImVec2(stripW, stripH))) drawTrackStrip(*tracks[t], stripW, stripH);
        else ImGui::Dummy(ImVec2(stripW, stripH));
        ImGui::PopID();
        if(t + 1 < tracks.size()) ImGui::SameLine(0, 5.0f * zoom);
    }
    // Buses, after a gap
    for(size_t k = 0; k < buses.size(); k++) {
        ImGui::SameLine(0, k == 0 ? 16.0f * zoom : 5.0f * zoom);
        ImGui::PushID(1000 + (int)k);
        if(ImGui::IsRectVisible(ImVec2(stripW, stripH))) drawTrackStrip(*buses[k], stripW, stripH);
        else ImGui::Dummy(ImVec2(stripW, stripH));
        ImGui::PopID();
    }
    ImGui::EndChild();

    // The strips can have closed (or switched) the EQ panel this frame:
    // check the selection again rather than trusting eqOpen
    if(eqOpen && eqTarget())
        drawEqPanel(ImGui::GetContentRegionAvail().x, eqH);
    ImGui::End();
}

void scMixerPro::drawToolbar() {
    const float zoom = ofxOceanodeShared::getZoomLevel();
    // InputInt does not support EnterReturnsTrue (ImGui asserts): typed
    // values apply when the field is left, the +/- buttons immediately.
    auto countField = [&](const char* label, int& edit, int current, int step, int fastStep,
                          const std::function<void(int)>& apply) {
        ImGui::SetNextItemWidth(90.0f * zoom);
        const bool changed = ImGui::InputInt(label, &edit, step, fastStep);
        const bool typing = ImGui::IsItemActive();
        if(ImGui::IsItemDeactivatedAfterEdit() || (changed && !typing)) apply(edit);
        if(!typing) edit = current;
    };
    countField("Inputs", editTracks, (int)tracks.size(), 1, 4,
               [this](int v) { numTracks.set(ofClamp(v, 1, MAX_TRACKS)); });
    ImGui::SameLine(0, 14.0f * zoom);
    countField("Buses", editBuses, (int)buses.size(), 1, 2,
               [this](int v) { pendingBusCount = ofClamp(v, 0, MAX_BUSES); });
    ImGui::SameLine(0, 14.0f * zoom);
    countField("Channels", editChannels, currentChannels, 1, 2,
               [this](int v) { numChannels.set(ofClamp(v, 1, MAX_CHANNELS)); });
    ImGui::SameLine(0, 14.0f * zoom);
    bool faders = showFaders.get();
    if(ImGui::Checkbox("Show Faders", &faders)) showFaders.set(faders);
    ImGui::SameLine(0, 14.0f * zoom);
    if(ImGui::Button("Clear Peaks")) {
        for(Track* tr : allStrips()) std::fill(tr->peak.begin(), tr->peak.end(), 0.0f);
        std::fill(masterPeak.begin(), masterPeak.end(), 0.0f);
    }

    // Servers: which ones run this mixer (an Output on them is connected)
    for(int i = 0; i < (int)servers.size(); i++) {
        ImGui::SameLine(0, i == 0 ? 20.0f * zoom : 6.0f * zoom);
        const bool active = isServerActive(i);
        int count = 0;
        for(auto& tr : tracks) if(tr->server == i) count++;
        ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(0.55f, 0.9f, 0.55f, 1.0f)
                                                    : ImVec4(0.6f, 0.6f, 0.62f, 1.0f));
        ImGui::Text("S%d: %d", i + 1, count);
        ImGui::PopStyleColor();
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip(active ? "Server %d: %d tracks, mixed into Out S%d"
                                     : "Server %d: %d tracks -- Out S%d is not connected to an Output on this server",
                              i + 1, count, i + 1);
    }
}

void scMixerPro::drawMeters(const std::vector<float>& vu, const std::vector<float>& peak, ImVec2 pos, ImVec2 size, bool bus) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int channels = std::max(1, (int)vu.size());
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(10, 10, 12, 255), 2.0f);
    const float gap = channels > 8 ? 0.0f : 1.0f;
    const float w = std::max(1.0f, (size.x - gap * (channels + 1)) / channels);
    const float y0dB = pos.y + size.y * (1.0f - gainToPos(1.0f));
    for(int c = 0; c < channels; c++) {
        const float x = pos.x + gap + c * (w + gap);
        const float v = c < (int)vu.size() ? vu[c] : 0.0f;
        const float p = gainToPos(v);
        const float yTop = pos.y + size.y * (1.0f - p);
        // green up to -12 dB, yellow up to -3 dB, red above
        const float yGreen  = pos.y + size.y * (1.0f - gainToPos(std::pow(10.0f, -12.0f / 20.0f)));
        const float yYellow = pos.y + size.y * (1.0f - gainToPos(std::pow(10.0f, -3.0f / 20.0f)));
        const float yBottom = pos.y + size.y;
        if(yTop < yBottom) {
            dl->AddRectFilled(ImVec2(x, std::max(yTop, yGreen)), ImVec2(x + w, yBottom), bus ? IM_COL32(70, 140, 235, 255) : IM_COL32(70, 200, 90, 255));
            if(yTop < yGreen) dl->AddRectFilled(ImVec2(x, std::max(yTop, yYellow)), ImVec2(x + w, yGreen), IM_COL32(220, 200, 60, 255));
            if(yTop < yYellow) dl->AddRectFilled(ImVec2(x, yTop), ImVec2(x + w, yYellow), IM_COL32(230, 70, 60, 255));
        }
        if(c < (int)peak.size() && peak[c] > 1e-4f) {
            const float yp = pos.y + size.y * (1.0f - gainToPos(peak[c]));
            dl->AddLine(ImVec2(x, yp), ImVec2(x + w, yp),
                        peak[c] >= 1.0f ? IM_COL32(255, 80, 70, 255) : IM_COL32(230, 230, 230, 220), 1.5f);
        }
    }
    dl->AddLine(ImVec2(pos.x, y0dB), ImVec2(pos.x + size.x, y0dB), IM_COL32(255, 255, 255, 60), 1.0f);
}

bool scMixerPro::drawFader(const char* id, float& gain, ImVec2 pos, ImVec2 size) {
    const float zoom = ofxOceanodeShared::getZoomLevel();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton(id, size);
    bool changed = false;
    float p = gainToPos(gain);
    if(ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 0.0f)) {
        const float speed = ImGui::GetIO().KeyShift ? 0.1f : 1.0f;
        p = ofClamp(p - ImGui::GetIO().MouseDelta.y * speed / std::max(1.0f, size.y), 0.0f, 1.0f);
        gain = posToGain(p);
        changed = true;
    }
    if(ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { gain = 1.0f; changed = true; }
    if(ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
        p = ofClamp(p + ImGui::GetIO().MouseWheel * 0.01f, 0.0f, 1.0f);
        gain = posToGain(p);
        changed = true;
    }
    p = gainToPos(gain);
    const float cx = pos.x + size.x * 0.5f;
    dl->AddRectFilled(ImVec2(cx - 2.0f * zoom, pos.y), ImVec2(cx + 2.0f * zoom, pos.y + size.y), IM_COL32(8, 8, 10, 255), 2.0f);
    const float y0 = pos.y + size.y * (1.0f - gainToPos(1.0f));
    dl->AddLine(ImVec2(pos.x, y0), ImVec2(pos.x + size.x, y0), IM_COL32(255, 255, 255, 70), 1.0f);
    const float yc = pos.y + size.y * (1.0f - p);
    const float capH = 10.0f * zoom;
    const bool hot = ImGui::IsItemActive() || ImGui::IsItemHovered();
    dl->AddRectFilled(ImVec2(pos.x + 1.0f, yc - capH * 0.5f), ImVec2(pos.x + size.x - 1.0f, yc + capH * 0.5f),
                      hot ? IM_COL32(235, 235, 240, 255) : IM_COL32(190, 190, 198, 255), 2.0f);
    dl->AddLine(ImVec2(pos.x + 3.0f, yc), ImVec2(pos.x + size.x - 3.0f, yc), IM_COL32(30, 30, 34, 255), 1.0f);
    if(ImGui::IsItemActive() || ImGui::IsItemHovered())
        ImGui::SetTooltip("%s dB  (drag, Shift: fine, wheel, double-click: 0 dB)",
                          gain <= 1e-6f ? "-inf" : ofToString(ampToDb(gain), 1).c_str());
    return changed;
}

bool scMixerPro::drawKnob(const char* id, float& value, float radius) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(radius * 2.0f, radius * 2.0f));
    bool changed = false;
    if(ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 0.0f)) {
        const float speed = ImGui::GetIO().KeyShift ? 0.001f : 0.01f;
        value = ofClamp(value - ImGui::GetIO().MouseDelta.y * speed, -1.0f, 1.0f);
        changed = true;
    }
    if(ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { value = 0.0f; changed = true; }
    const ImVec2 c(p.x + radius, p.y + radius);
    const float a0 = (float)M_PI * 0.75f, a1 = (float)M_PI * 2.25f;
    const float aMid = (a0 + a1) * 0.5f;
    const float a = aMid + value * (a1 - a0) * 0.5f;
    dl->AddCircleFilled(c, radius, IM_COL32(36, 36, 42, 255), 24);
    dl->PathArcTo(c, radius - 2.0f, a0, a1, 24);
    dl->PathStroke(IM_COL32(70, 70, 80, 255), 0, 2.5f);
    dl->PathArcTo(c, radius - 2.0f, std::min(aMid, a), std::max(aMid, a), 16);
    dl->PathStroke(IM_COL32(110, 190, 255, 255), 0, 2.5f);
    dl->AddLine(c, ImVec2(c.x + std::cos(a) * (radius - 3.0f), c.y + std::sin(a) * (radius - 3.0f)),
                IM_COL32(230, 230, 235, 255), 2.0f);
    if(ImGui::IsItemActive() || ImGui::IsItemHovered())
        ImGui::SetTooltip("Balance %.2f  (drag, Shift: fine, double-click: centre)", value);
    return changed;
}

void scMixerPro::recomputeEqCurve(Track& tr) {
    if(!tr.eqCurveDirty) return;
    for(int b = 0; b < scEQEditor::NUM_BANDS; b++)
        tr.eqEditor.bands[b] = { tr.eqFreq[b].get(), tr.eqGain[b].get(), tr.eqShape[b].get() };
    float sr = 48000.0f;
    if(ofxSCServer* server = serverAt(tr.server)) {
        const float s = (float)serverManager::getSampleRateForServer(server);
        if(s > 0.0f) sr = s;
    }
    tr.eqEditor.dbRange = 24.0f;
    tr.eqEditor.recompute(sr);
    tr.eqCurveDirty = false;
}

void scMixerPro::drawMiniCurve(Track& tr, ImVec2 pos, ImVec2 size) {
    recomputeEqCurve(tr);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(12, 14, 20, 255), 2.0f);
    const float midY = pos.y + size.y * 0.5f;
    dl->AddLine(ImVec2(pos.x, midY), ImVec2(pos.x + size.x, midY), IM_COL32(60, 60, 75, 255), 1.0f);
    const ImU32 col = tr.eq.get() ? IM_COL32(80, 210, 255, 255) : IM_COL32(90, 90, 100, 255);
    const auto& curve = tr.eqEditor.curveDb;
    const int N = (int)curve.size();
    const float range = 24.0f;
    for(int i = 0; i + 1 < N; i++) {
        const float x1 = pos.x + size.x * (float)i / (N - 1);
        const float x2 = pos.x + size.x * (float)(i + 1) / (N - 1);
        const float y1 = midY - ofClamp(curve[i] / range, -1.0f, 1.0f) * size.y * 0.5f;
        const float y2 = midY - ofClamp(curve[i + 1] / range, -1.0f, 1.0f) * size.y * 0.5f;
        dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), col, 1.0f);
    }
}

void scMixerPro::drawMasterStrip(float w, float h) {
    const float zoom = ofxOceanodeShared::getZoomLevel();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 end(start.x + w, start.y + h);
    dl->AddRectFilled(start, end, IM_COL32(24, 24, 28, 255), 4.0f * zoom);
    dl->AddRect(start, end, IM_COL32(70, 70, 78, 220), 4.0f * zoom);
    dl->AddRectFilled(start, ImVec2(end.x, start.y + 20.0f * zoom), IM_COL32(180, 180, 188, 255), 4.0f * zoom);
    dl->AddText(ImVec2(start.x + 6.0f * zoom, start.y + 3.0f * zoom), IM_COL32(20, 20, 24, 255), "MASTER");

    // Per server: its audible tracks, summed, as one small meter
    float y = start.y + 26.0f * zoom;
    for(int i = 0; i < (int)servers.size(); i++) {
        std::vector<float> sum(currentChannels, 0.0f);
        for(Track* tr : allStrips()) {
            if(tr->server != i || !tr->synth || isSilenced(*tr) || routedToBus(*tr)) continue;
            for(int c = 0; c < currentChannels && c < (int)tr->vu.size(); c++) sum[c] += tr->vu[c];
        }
        float mx = 0.0f;
        for(float v : sum) mx = std::max(mx, v);
        char label[16];
        std::snprintf(label, sizeof(label), "S%d", i + 1);
        dl->AddText(ImVec2(start.x + 6.0f * zoom, y), isServerActive(i) ? IM_COL32(150, 230, 150, 255) : IM_COL32(130, 130, 136, 255), label);
        const ImVec2 bp(start.x + 28.0f * zoom, y + 3.0f * zoom);
        const ImVec2 bs(w - 34.0f * zoom, 8.0f * zoom);
        dl->AddRectFilled(bp, ImVec2(bp.x + bs.x, bp.y + bs.y), IM_COL32(10, 10, 12, 255), 2.0f);
        dl->AddRectFilled(bp, ImVec2(bp.x + bs.x * gainToPos(mx), bp.y + bs.y),
                          mx >= 1.0f ? IM_COL32(230, 70, 60, 255) : IM_COL32(70, 200, 90, 255), 2.0f);
        y += 16.0f * zoom;
    }

    const float top = y + 6.0f * zoom;
    const float bottom = end.y - 24.0f * zoom;
    const float areaH = std::max(40.0f * zoom, bottom - top);
    const float faderW = 26.0f * zoom;
    const ImVec2 meterPos(start.x + 8.0f * zoom + faderW + 8.0f * zoom, top);
    const ImVec2 meterSize(w - (meterPos.x - start.x) - 8.0f * zoom, areaH);

    auto m = masterLevel.get();
    float g = m.empty() ? 1.0f : m[0];
    if(m.size() <= 1) {
        if(drawFader("##masterFader", g, ImVec2(start.x + 8.0f * zoom, top), ImVec2(faderW, areaH)))
            masterLevel.set(vector<float>(1, g));
    } else {
        ImGui::SetCursorScreenPos(ImVec2(start.x + 6.0f * zoom, top));
        ImGui::TextDisabled("vec");
    }
    ImGui::SetCursorScreenPos(meterPos);
    ImGui::InvisibleButton("##masterMeters", meterSize);
    if(ImGui::IsItemClicked()) std::fill(masterPeak.begin(), masterPeak.end(), 0.0f);
    drawVuPublishMenu(masterVuOut.getEscapedName());
    drawMeters(masterVU, masterPeak, meterPos, meterSize);

    float pk = 0.0f;
    for(float v : masterPeak) pk = std::max(pk, v);
    const std::string db = m.size() <= 1 ? (g <= 1e-6f ? "-inf" : ofToString(ampToDb(g), 1)) : "vec";
    const std::string text = db + " | " + (pk <= 1e-6f ? "-inf" : ofToString(ampToDb(pk), 1));
    dl->AddText(ImVec2(start.x + 6.0f * zoom, bottom + 4.0f * zoom),
                pk >= 1.0f ? IM_COL32(255, 90, 80, 255) : IM_COL32(210, 210, 215, 255), text.c_str());

    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(ImVec2(w, h));
}

void scMixerPro::drawTrackStrip(Track& tr, float w, float h) {
    const float zoom = ofxOceanodeShared::getZoomLevel();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 end(start.x + w, start.y + h);
    const float pad = 4.0f * zoom;
    const float innerW = w - 2.0f * pad;
    const bool silenced = isSilenced(tr);

    dl->AddRectFilled(start, end, tr.isBus ? IM_COL32(36, 30, 62, 255) : IM_COL32(24, 24, 28, 255), 4.0f * zoom);
    dl->AddRect(start, end, selectedEqTrack == eqId(tr) ? IM_COL32(110, 190, 255, 220) : (tr.isBus ? IM_COL32(125, 105, 200, 230) : IM_COL32(70, 70, 78, 220)), 4.0f * zoom);

    // Name on the track colour (right-click: colour / delete). Tracks have a
    // grip on the left: drag it onto another strip to move the track there.
    dl->AddRectFilled(start, ImVec2(end.x, start.y + 20.0f * zoom), toU32(tr.color), 4.0f * zoom);
    auto deleteItem = [&]() {
        if(tr.isBus) return;
        ImGui::BeginDisabled(tracks.size() <= 1);
        if(ImGui::MenuItem(("Delete " + tr.name).c_str())) { pendingDelete = tr.index; ImGui::CloseCurrentPopup(); }
        ImGui::EndDisabled();
    };
    float nameX = start.x + pad;
    if(!tr.isBus) {
        const float gripW = 10.0f * zoom;
        const ImVec2 gp(start.x + 2.0f * zoom, start.y + 2.0f * zoom);
        ImGui::SetCursorScreenPos(gp);
        ImGui::InvisibleButton("##grip", ImVec2(gripW, 16.0f * zoom));
        const ImU32 dot = ImGui::IsItemHovered() || ImGui::IsItemActive() ? IM_COL32(255, 255, 255, 230) : IM_COL32(15, 15, 18, 200);
        for(int r = 0; r < 3; r++) for(int c = 0; c < 2; c++)
            dl->AddCircleFilled(ImVec2(gp.x + (3.0f + c * 4.0f) * zoom, gp.y + (4.0f + r * 4.0f) * zoom), 1.2f * zoom, dot);
        if(ImGui::IsItemHovered() && !ImGui::IsMouseDown(0)) ImGui::SetTooltip("Drag to move the track");
        if(ImGui::BeginDragDropSource()) {
            const int idx = tr.index;
            ImGui::SetDragDropPayload("MIXERPRO_TRACK", &idx, sizeof(int));
            ImGui::Text("Move %s", tr.name.c_str());
            ImGui::EndDragDropSource();
        }
        if(ImGui::BeginPopupContextItem("##gripMenu")) { deleteItem(); ImGui::EndPopup(); }
        nameX = gp.x + gripW + 2.0f * zoom;
    }
    ImGui::SetCursorScreenPos(ImVec2(nameX, start.y + 2.0f * zoom));
    char nameBuffer[64];
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", tr.name.c_str());
    ImGui::SetNextItemWidth(end.x - pad - nameX);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.06f, 0.06f, 0.07f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f * zoom, 1.0f * zoom));
    if(ImGui::InputText("##name", nameBuffer, sizeof(nameBuffer))) tr.name = nameBuffer;
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    if(ImGui::BeginPopupContextItem("##colour")) {
        if(!tr.isBus) { deleteItem(); ImGui::Separator(); }
        float col[3] = { tr.color.r / 255.0f, tr.color.g / 255.0f, tr.color.b / 255.0f };
        if(ImGui::ColorPicker3("##trackColour", col, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview))
            tr.color = ofColor(col[0] * 255, col[1] * 255, col[2] * 255);
        ImGui::EndPopup();
    }

    float y = start.y + 24.0f * zoom;

    // Server (+ Out, for tracks)
    ImGui::SetCursorScreenPos(ImVec2(start.x + pad, y));
    const float serverW = tr.isBus ? innerW : (innerW - 3.0f * zoom) * 0.45f;
    ImGui::SetNextItemWidth(serverW);
    const std::string preview = "S" + ofToString(tr.server + 1);
    const bool serverOk = isServerActive(tr.server);
    if(!serverOk) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
    if(ImGui::BeginCombo("##server", preview.c_str())) {
        for(int i = 0; i < (int)servers.size(); i++) {
            const std::string label = "Server " + ofToString(i + 1) + (isServerActive(i) ? "" : "  (no output)");
            if(ImGui::Selectable(label.c_str(), tr.server == i)) setStripServer(tr, i);
        }
        ImGui::EndCombo();
    }
    if(!serverOk) {
        ImGui::PopStyleColor();
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Out S%d is not connected to an Output on server %d: this track is silent", tr.server + 1, tr.server + 1);
    }
    if(!tr.isBus) {
        ImGui::SameLine(0, 3.0f * zoom);
        ImGui::SetNextItemWidth(innerW - serverW - 3.0f * zoom);
        const int out = tr.output.get();
        Track* bus = busFor(tr);
        const bool badBus = out > 0 && !bus;   // missing, or on another server
        std::string outPreview = out == 0 ? "Mst" : (out - 1 < (int)buses.size() ? std::string(1, char('A' + out - 1)) : "?");
        if(badBus) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
        if(ImGui::BeginCombo("##out", outPreview.c_str(), ImGuiComboFlags_NoArrowButton)) {
            if(ImGui::Selectable("Master", out == 0)) tr.output.set(0);
            for(auto& b : buses) {
                std::string l = b->name;
                if(b->server != tr.server) l += "  (S" + ofToString(b->server + 1) + ": other server)";
                ImGui::BeginDisabled(b->server != tr.server);
                if(ImGui::Selectable((l + "##bus" + ofToString(b->index)).c_str(), out == b->index + 1)) tr.output.set(b->index + 1);
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        if(badBus) ImGui::PopStyleColor();
        drawPublishPopup(tr.output.getEscapedName());
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip(badBus ? "Bus not available on this track's server: going to the master"
                                     : (out == 0 ? "Output: master" : "Output: %s"), bus ? bus->name.c_str() : "");
    }
    y += 24.0f * zoom;

    // Channel adapter: button (M -> N and mode) + popup. Buses: a label
    if(tr.isBus) {
        int fed = 0;
        for(auto& t : tracks) if(busFor(*t) == &tr) fed++;
        const std::string text = "SUBMASTER  " + ofToString(fed) + (fed == 1 ? " track" : " tracks");
        dl->AddText(ImVec2(start.x + pad, y + 2.0f * zoom), IM_COL32(170, 160, 210, 255), text.c_str());
    } else {
        static const char* modeNames[] = {"Direct", "Wrap", "Blocks", "Stretch"};
        const int mode = ofClamp(tr.adaptMode.get(), 0, 3);
        const int M = tr.synth ? tr.detectedInputs : sourceChannels(tr);
        const bool adapting = tr.adapt && tr.adaptRunning;
        std::string label = ofToString(M) + " > " + ofToString(currentChannels);
        if(adapting) label += " " + std::string(modeNames[mode]);
        if(tr.rotate.get() != 0) label += " R" + ofToString(tr.rotate.get());
        label += "##adapt";
        ImGui::SetCursorScreenPos(ImVec2(start.x + pad, y));
        pushButtonColours(adapting ? ImVec4(0.18f, 0.5f, 0.45f, 1.0f) : ImVec4(0.22f, 0.22f, 0.25f, 1.0f));
        if(ImGui::Button(label.c_str(), ImVec2(innerW, 18.0f * zoom))) ImGui::OpenPopup("##adaptPopup");
        ImGui::PopStyleColor(3);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip("Input channels: %d%s -> %d", M, tr.inChannels.get() > 0 ? "" : " (auto)", currentChannels);
        if(ImGui::BeginPopup("##adaptPopup")) {
            const float fieldW = 150.0f * zoom;
            ImGui::TextDisabled("Channels: %d in -> %d out", M, currentChannels);
            int m = mode;
            ImGui::SetNextItemWidth(fieldW);
            if(ImGui::Combo("Mode", &m, modeNames, 4)) tr.adaptMode.set(m);
            drawPublishPopup(tr.adaptMode.getEscapedName());
            int in = tr.inChannels.get();
            const std::string inFormat = in == 0 ? "Auto (" + ofToString(M) + ")" : std::string("%d");
            ImGui::SetNextItemWidth(fieldW);
            if(ImGui::SliderInt("In Ch", &in, 0, MAX_CHANNELS, inFormat.c_str())) tr.inChannels.set(in);
            drawPublishPopup(tr.inChannels.getEscapedName());
            int r = tr.rotate.get();
            ImGui::SetNextItemWidth(fieldW);
            if(ImGui::SliderInt("Rotate", &r, 0, std::max(0, currentChannels - 1))) tr.rotate.set(r);
            drawPublishPopup(tr.rotate.getEscapedName());
            const char* help[] = {
                "First channels as they are",
                M <= currentChannels ? "Repeat the inputs: 1 2 1 2 ..." : "Fold: input i to output i mod N",
                M <= currentChannels ? "Spread in blocks: 1 1 1 2 2 2" : "Fold contiguous groups",
                "Interpolate across the outputs (equal power)"
            };
            ImGui::TextDisabled("%s", help[m]);
            ImGui::EndPopup();
        }
    }
    y += 18.0f * zoom + 4.0f * zoom;

    // EQ curve (click: open the editor) + EQ / DC switches
    const ImVec2 curvePos(start.x + pad, y);
    const ImVec2 curveSize(innerW, 26.0f * zoom);
    ImGui::SetCursorScreenPos(curvePos);
    ImGui::InvisibleButton("##eqCurve", curveSize);
    if(ImGui::IsItemClicked()) selectedEqTrack = (selectedEqTrack == eqId(tr)) ? -1 : eqId(tr);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Click: edit this track's EQ");
    drawMiniCurve(tr, curvePos, curveSize);
    y += curveSize.y + 3.0f * zoom;

    const float halfW = (innerW - 3.0f * zoom) * 0.5f;
    const float btnH = 18.0f * zoom;
    auto toggle = [&](const char* label, ofParameter<bool>& p, ImVec4 on) {
        const bool v = p.get();
        pushButtonColours(v ? on : ImVec4(0.22f, 0.22f, 0.25f, 1.0f));
        if(ImGui::Button(label, ImVec2(halfW, btnH))) p.set(!v);
        ImGui::PopStyleColor(3);
        drawPublishPopup(p.getEscapedName());
    };
    ImGui::SetCursorScreenPos(ImVec2(start.x + pad, y));
    toggle("EQ", tr.eq, ImVec4(0.2f, 0.5f, 0.75f, 1.0f));
    ImGui::SameLine(0, 3.0f * zoom);
    toggle("DC", tr.dc, ImVec4(0.45f, 0.35f, 0.7f, 1.0f));
    y += btnH + 3.0f * zoom;

    // Sidechain: button (shows the source) + popup with the controls
    {
        const int s = tr.scSource.get() - 1;
        const bool valid = s >= 0 && s < (int)tracks.size() && (tr.isBus || s != tr.index);
        std::string label = "SC";
        if(valid) label = (tr.scMode == 2 ? "SC~ " : "SC < ") + ofToString(s + 1);
        label += "##sc";
        ImGui::SetCursorScreenPos(ImVec2(start.x + pad, y));
        pushButtonColours(valid ? ImVec4(0.78f, 0.45f, 0.12f, 1.0f) : ImVec4(0.22f, 0.22f, 0.25f, 1.0f));
        if(ImGui::Button(label.c_str(), ImVec2(innerW, btnH))) ImGui::OpenPopup("##sidechain");
        ImGui::PopStyleColor(3);
        if(ImGui::IsItemHovered() && valid)
            ImGui::SetTooltip("Ducked by %d: %s%s", s + 1, tracks[s]->name.c_str(),
                              tr.scMode == 2 ? "\nOther server: relayed via Oceanode (~20-40 ms)" : "");
        if(ImGui::BeginPopup("##sidechain")) {
            const float fieldW = 170.0f * zoom;
            ImGui::TextDisabled("Sidechain: duck \"%s\" by", tr.name.c_str());
            ImGui::SetNextItemWidth(fieldW);
            const std::string current = valid ? ofToString(s + 1) + ": " + tracks[s]->name : std::string("Off");
            if(ImGui::BeginCombo("Source", current.c_str())) {
                if(ImGui::Selectable("Off", !valid)) tr.scSource.set(0);
                for(auto& other : tracks) {
                    if(!tr.isBus && other->index == tr.index) continue;
                    std::string l = ofToString(other->index + 1) + ": " + other->name;
                    if(other->server != tr.server) l += "  (S" + ofToString(other->server + 1) + ", via Oceanode)";
                    if(ImGui::Selectable(l.c_str(), other->index == s)) tr.scSource.set(other->index + 1);
                }
                ImGui::EndCombo();
            }
            drawPublishPopup(tr.scSource.getEscapedName());
            auto slider = [&](const char* name, ofParameter<float>& p, const char* format, ImGuiSliderFlags flags) {
                float v = p.get();
                ImGui::SetNextItemWidth(fieldW);
                if(ImGui::SliderFloat(name, &v, p.getMin(), p.getMax(), format, flags)) p.set(v);
                drawPublishPopup(p.getEscapedName());
            };
            slider("Strength", tr.scStrength, "%.2f", 0);
            slider("Threshold", tr.scThreshold, "%.1f dB", 0);
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Source level at which the ducking is full");
            slider("Attack", tr.scAttack, "%.1f ms", ImGuiSliderFlags_Logarithmic);
            slider("Release", tr.scRelease, "%.0f ms", ImGuiSliderFlags_Logarithmic);
            if(valid) {
                if(tr.scMode == 1) ImGui::TextDisabled("Same server: sample-accurate (1 block)");
                else if(tr.scMode == 2) ImGui::TextDisabled("Other server: relayed via Oceanode (~20-40 ms)");
                else if(tr.scStrength.get() <= 0.0f) ImGui::TextDisabled("Strength 0: off");
            }
            ImGui::EndPopup();
        }
    }
    y += btnH + 6.0f * zoom;

    // Balance
    const float radius = 13.0f * zoom;
    ImGui::SetCursorScreenPos(ImVec2(start.x + w * 0.5f - radius, y));
    float bal = tr.balance.get();
    if(drawKnob("##balance", bal, radius)) tr.balance.set(bal);
    drawPublishPopup(tr.balance.getEscapedName());
    y += radius * 2.0f + 4.0f * zoom;

    // Mute / Solo
    ImGui::SetCursorScreenPos(ImVec2(start.x + pad, y));
    toggle("M", tr.mute, ImVec4(0.75f, 0.16f, 0.16f, 1.0f));
    ImGui::SameLine(0, 3.0f * zoom);
    toggle("S", tr.solo, ImVec4(0.82f, 0.72f, 0.18f, 1.0f));
    y += btnH + 6.0f * zoom;

    // Fader + meters
    const float bottom = end.y - 20.0f * zoom;
    const float areaH = std::max(30.0f * zoom, bottom - y);
    const float faderW = 22.0f * zoom;
    float g = tr.level.get();
    if(drawFader("##fader", g, ImVec2(start.x + pad, y), ImVec2(faderW, areaH))) tr.level.set(g);
    drawPublishPopup(tr.level.getEscapedName());
    const ImVec2 meterPos(start.x + pad + faderW + 5.0f * zoom, y);
    const ImVec2 meterSize(end.x - pad - meterPos.x, areaH);
    ImGui::SetCursorScreenPos(meterPos);
    ImGui::InvisibleButton("##meters", meterSize);
    if(ImGui::IsItemClicked()) std::fill(tr.peak.begin(), tr.peak.end(), 0.0f);
    drawVuPublishMenu(tr.vuOut.getEscapedName());
    drawMeters(tr.vu, tr.peak, meterPos, meterSize, tr.isBus);
    if(silenced) dl->AddRectFilled(meterPos, ImVec2(meterPos.x + meterSize.x, meterPos.y + meterSize.y), IM_COL32(0, 0, 0, 110));

    // Level | peak (dB)
    float pk = 0.0f;
    for(float v : tr.peak) pk = std::max(pk, v);
    const std::string text = (g <= 1e-6f ? std::string("-inf") : ofToString(ampToDb(g), 1)) + " | "
                           + (pk <= 1e-6f ? std::string("-inf") : ofToString(ampToDb(pk), 1));
    dl->AddText(ImVec2(start.x + pad, bottom + 3.0f * zoom),
                pk >= 1.0f ? IM_COL32(255, 90, 80, 255) : IM_COL32(200, 200, 206, 255), text.c_str());

    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(ImVec2(w, h));
    // Drop a dragged track here: it takes this position
    if(!tr.isBus && ImGui::BeginDragDropTarget()) {
        if(const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MIXERPRO_TRACK")) {
            const int from = *static_cast<const int*>(payload->Data);
            if(from != tr.index) { pendingMoveFrom = from; pendingMoveTo = tr.index; }
        }
        ImGui::EndDragDropTarget();
    }
}

void scMixerPro::drawEqPanel(float w, float h) {
    Track* target = eqTarget();
    if(!target) return;
    const float zoom = ofxOceanodeShared::getZoomLevel();
    Track& tr = *target;
    ImGui::BeginChild("##mixerProEq", ImVec2(w, h), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextColored(ImVec4(tr.color.r / 255.0f, tr.color.g / 255.0f, tr.color.b / 255.0f, 1.0f), "%s", tr.name.c_str());
    ImGui::SameLine();
    bool eq = tr.eq.get();
    if(ImGui::Checkbox("EQ", &eq)) tr.eq.set(eq);
    drawPublishPopup(tr.eq.getEscapedName());
    ImGui::SameLine();
    bool dc = tr.dc.get();
    if(ImGui::Checkbox("DC correction", &dc)) tr.dc.set(dc);
    drawPublishPopup(tr.dc.getEscapedName());
    ImGui::SameLine();
    if(ImGui::SmallButton("Flat")) {
        for(int b = 0; b < scEQEditor::NUM_BANDS; b++) tr.eqGain[b].set(0.0f);
    }
    ImGui::SameLine(0, 12.0f * zoom);
    ImGui::TextDisabled("drag: freq / gain, wheel: Q / slope, double-click: 0 dB, right-click: values / publish");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 22.0f * zoom);
    if(ImGui::SmallButton("x")) selectedEqTrack = -1;

    recomputeEqCurve(tr);
    tr.eqEditor.gainLimit = 24.0f;
    tr.eqEditor.qMin = 0.1f;  tr.eqEditor.qMax = 20.0f;
    tr.eqEditor.slopeMin = 0.1f; tr.eqEditor.slopeMax = 4.0f;
    Track* tp = &tr;
    tr.eqEditor.bandMenu = [this, tp](int b) {
        ImGui::TextUnformatted(scEQEditor::bandName(b));
        ImGui::Separator();
        drawPublishItems(tp->eqFreq[b].getEscapedName(), "Freq (Hz)", true);
        ImGui::Separator();
        drawPublishItems(tp->eqGain[b].getEscapedName(), "Gain (dB)", false);
        ImGui::Separator();
        drawPublishItems(tp->eqShape[b].getEscapedName(), scEQEditor::isShelf(b) ? "Slope" : "Q", false);
    };
    static scEQEditor::Colors colors = scEQEditor::defaultColors();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if(tr.eqEditor.draw(std::max(120.0f * zoom, avail.x - 6.0f * zoom), std::max(60.0f * zoom, avail.y - 10.0f * zoom),
                        colors, true, nullptr, 0, "##mixerProEqEditor")) {
        for(int b = 0; b < scEQEditor::NUM_BANDS; b++) {
            tr.eqFreq[b].set(tr.eqEditor.bands[b].freqHz);
            tr.eqGain[b].set(tr.eqEditor.bands[b].gainDb);
            tr.eqShape[b].set(tr.eqEditor.bands[b].shape);
        }
    }
    ImGui::EndChild();
}
