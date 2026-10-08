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
constexpr float kTrackMaxGain = 4.0f; // +12.04 dB

const ofColor kTrackPalette[] = {
    ofColor(214, 92, 92),  ofColor(222, 150, 70), ofColor(214, 196, 84), ofColor(120, 190, 96),
    ofColor(80, 182, 160), ofColor(84, 150, 214), ofColor(132, 116, 214), ofColor(196, 104, 186)
};

ImU32 toU32(const ofColor& c, int alpha = 255) { return IM_COL32(c.r, c.g, c.b, alpha); }

// Echo note values, in beats (as GrainBox's echo)
struct NoteValue { float beats; const char* name; };
const NoteValue kNotes[] = {
    {1.0f / 16.0f, "64th"},  {1.0f / 12.0f, "32ndT"}, {1.0f / 8.0f, "32nd"},  {1.0f / 6.0f, "16thT"},
    {3.0f / 16.0f, "32ndD"}, {1.0f / 4.0f,  "16th"},  {1.0f / 3.0f, "8thT"},  {3.0f / 8.0f, "16thD"},
    {1.0f / 2.0f,  "8th"},   {2.0f / 3.0f,  "4thT"},  {3.0f / 4.0f, "8thD"},  {1.0f,        "4th"},
    {4.0f / 3.0f,  "halfT"}, {1.5f,         "4thD"},  {2.0f,        "half"},  {8.0f / 3.0f, "1 barT"},
    {3.0f,         "halfD"}, {4.0f,         "1 bar"}, {6.0f,        "1 barD"}, {8.0f,       "2 bars"},
    {16.0f,        "4 bars"}
};
constexpr int kNoteCount = (int)(sizeof(kNotes) / sizeof(kNotes[0]));
int nearestNote(float beats) {
    int best = 0; float bd = 1e9f;
    const float lb = std::log(std::max(1e-4f, beats));
    for(int i = 0; i < kNoteCount; i++) {
        const float d = std::abs(std::log(kNotes[i].beats) - lb);
        if(d < bd) { bd = d; best = i; }
    }
    return best;
}

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
                  "Dockable mixer window with VU / peak, fader, balance, mute / solo; per strip HP, EQ, DC, compressor, "
                  "saturation, echo and reverb; aux sends to return strips; master HP, EQ, maximizer and limiter "
                  "(every effect costs nothing until switched on).";

    addSeparator("Setup", ofColor(150, 180, 210));
    addParameter(showWindow.set("Show", false));
    addParameter(numChannels.set("N Chan", 2, 1, MAX_CHANNELS));
    addParameter(numTracks.set("Num Inputs", 4, 1, MAX_TRACKS));
    addParameter(showFaders.set("Show Faders", false));
    addSeparator("Mix Control", ofColor(180, 170, 210));
    addParameter(gainVec.set("Gain Vec", vector<float>(4, 1.0f), vector<float>(1, 0.0f), vector<float>(1, 2.0f)));
    addParameter(balanceVec.set("Balance Vec", vector<float>(4, 0.0f), vector<float>(1, -1.0f), vector<float>(1, 1.0f)));
    addParameter(masterLevel.set("Master Level", vector<float>(1, 1.0f), vector<float>(1, 0.0f), vector<float>(1, 2.0f)));

    masterVuOut.set("Master VU", vector<float>(2, 0.0f), vector<float>(1, 0.0f), vector<float>(1, 2.0f));
    addOutputAction(masterVuOut.getEscapedName(), masterVuOut);
    addInspectorParameter(vuAttack.set("VU Attack", 10.0f, 1.0f, 100.0f));
    addInspectorParameter(vuRelease.set("VU Release", 300.0f, 10.0f, 2000.0f));
    currentChannels = numChannels.get();

    addSeparator("Outputs", ofColor(150, 200, 170));
    for(int i = 0; i < (int)servers.size(); i++) scNode::addOutput("Out S" + ofToString(i + 1));

    addSeparator("Inputs", ofColor(210, 175, 135));
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
    // The checkbox can be changed from inside ofxOceanodeNodeGui::constructGui().
    // Adding/removing parameters there invalidates the vectors that constructGui
    // is currently traversing, so apply the structural change on the next update.
    nodeListeners.push(showFaders.newListener([this](bool&) { pendingFaderUpdate = true; }));
    auto vuTimes = [this](float&) {
        for(Track* tr : allStrips()) if(tr->synth) {
            tr->synth->set("vuAttackTime", vuAttack.get());
            tr->synth->set("vuReleaseTime", vuRelease.get());
        }
        for(auto& [server, state] : serverStates) if(state.master.out) {
            state.master.out->set("vuAttackTime", vuAttack.get());
            state.master.out->set("vuReleaseTime", vuRelease.get());
        }
    };
    nodeListeners.push(vuAttack.newListener(vuTimes));
    nodeListeners.push(vuRelease.newListener(vuTimes));

    // Master section (window controls, publishable like the strips')
    mHp.set("Master HP", false);
    mHpFreq.set("Master HP Freq", 30.0f, 10.0f, 1000.0f);
    mHp24.set("Master HP 24dB", true);
    mEq.set("Master EQ", false);
    for(int b = 0; b < scEQEditor::NUM_BANDS; b++) {
        const std::string prefix = std::string("Master EQ ") + kBandNames[b];
        const bool shelf = scEQEditor::isShelf(b);
        mEqFreq[b].set(prefix + " Freq", kBandFreq[b], scEQEditor::FREQ_MIN, scEQEditor::FREQ_MAX);
        mEqGain[b].set(prefix + " Gain", 0.0f, -24.0f, 24.0f);
        mEqShape[b].set(prefix + (shelf ? " Slope" : " Q"), 1.0f, 0.1f, shelf ? 4.0f : 20.0f);
    }
    mMax.set("Master Max", false);
    mMaxDrive.set("Master Max Drive", 3.0f, 0.0f, 24.0f);
    mMaxCeiling.set("Master Max Ceiling", -0.3f, -12.0f, 0.0f);
    mMaxCharacter.set("Master Max Character", 4.0f, 0.0f, 10.0f);
    mMaxIsp.set("Master Max ISP", true);
    mLim.set("Master Lim", false);
    mLimCeiling.set("Master Lim Ceiling", -0.3f, -24.0f, 0.0f);
    for(auto* q : {&mHp, &mEq, &mMax, &mLim})
        nodeListeners.push(q->newListener([this](bool&) { updateMasterAll(); }));
    auto masterSend = [this](int k) {
        for(auto& [server, state] : serverStates) if(state.active) sendMasterStage(server, k);
    };
    nodeListeners.push(mHp24.newListener([masterSend](bool&) { masterSend(MST_HP); }));
    nodeListeners.push(mHpFreq.newListener([masterSend](float&) { masterSend(MST_HP); }));
    nodeListeners.push(mMaxIsp.newListener([masterSend](bool&) { masterSend(MST_MAX); }));
    for(auto* q : {&mMaxDrive, &mMaxCeiling, &mMaxCharacter})
        nodeListeners.push(q->newListener([masterSend](float&) { masterSend(MST_MAX); }));
    nodeListeners.push(mLimCeiling.newListener([masterSend](float&) { masterSend(MST_LIM); }));
    for(int b = 0; b < scEQEditor::NUM_BANDS; b++)
        for(auto* q : {&mEqFreq[b], &mEqGain[b], &mEqShape[b]})
            nodeListeners.push(q->newListener([this, masterSend](float&) { mEqCurveDirty = true; masterSend(MST_EQ); }));
    for(auto* q : {&mHp, &mHp24, &mEq, &mMax, &mMaxIsp, &mLim}) addAction(q->getEscapedName(), *q);
    for(auto* q : {&mHpFreq, &mMaxDrive, &mMaxCeiling, &mMaxCharacter, &mLimCeiling}) addAction(q->getEscapedName(), *q);
    for(int b = 0; b < scEQEditor::NUM_BANDS; b++)
        for(auto* q : {&mEqFreq[b], &mEqGain[b], &mEqShape[b]}) addAction(q->getEscapedName(), *q);

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
                .parent_path().parent_path() / "synthdefs/Defaults/MixerPro/CompiledSynthdefs/mixerpro";
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
    Track* tp = tracks.back().get();
    tp->inputBadge.setName("Input Colour Key " + n);
    addCustomRegion(tp->inputBadge, [tp]() {
        const float zoom = ofxOceanodeShared::getZoomLevel();
        const float rowH = ImGui::GetFrameHeight();
        const float side = std::max(6.0f * zoom, std::min(10.0f * zoom, rowH - 4.0f * zoom));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(side, rowH));
        const float y = p.y + (rowH - side) * 0.5f;
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(p.x, y), ImVec2(p.x + side, y + side), toU32(tp->color), 1.5f * zoom);
        ImGui::SameLine(0.0f, 3.0f * zoom);
    });
    scNode::addInput("In " + n);
    registerTrackActions(t);
    syncTrackInputNames();
    syncFaderNames();
    if(showFaders.get()) updateFaders();

    // A new track starts on server 1; create its synth if that server runs us
    if(isServerActive(t.server)) createRuntime(t, serverAt(t.server));
}

// Parameters and listeners shared by tracks and buses (n: name suffix)
void scMixerPro::initStrip(Track& t, const std::string& n) {
    t.level.set("Level " + n, 1.0f, 0.0f, kTrackMaxGain);
    t.levelKey = t.level.getEscapedName();
    t.faderBadge.setName("Fader Colour Key " + t.levelKey);
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
    // Compressor (CompressorPro core) and reverb (SpaceMaster) inserts
    const std::string c = "Comp " + n, r = "Rvb " + n;
    t.comp.set(c, false);
    t.compThreshold.set(c + " Threshold", -18.0f, -60.0f, 0.0f);
    t.compRatio.set(c + " Ratio", 3.0f, 1.0f, 20.0f);
    t.compKnee.set(c + " Knee", 6.0f, 0.0f, 24.0f);
    t.compAttack.set(c + " Attack", 10.0f, 0.1f, 200.0f);
    t.compRelease.set(c + " Release", 150.0f, 5.0f, 2000.0f);
    t.compMakeup.set(c + " Makeup", 0.0f, -12.0f, 24.0f);
    t.compAuto.set(c + " Auto Makeup", false);
    t.compRms.set(c + " RMS", 0.0f, 0.0f, 1.0f);
    t.compHpf.set(c + " SC HPF", 20.0f, 20.0f, 1000.0f);
    t.compMix.set(c + " Mix", 1.0f, 0.0f, 1.0f);
    t.rvb.set(r, false);
    t.rvbMix.set(r + " Mix", 0.25f, 0.0f, 1.0f);
    t.rvbDecay.set(r + " Decay", 2.5f, 0.1f, 30.0f);
    t.rvbSize.set(r + " Size", 30.0f, 0.0f, 60.0f);
    t.rvbPredelay.set(r + " Predelay", 20.0f, 0.0f, 500.0f);
    t.rvbPosition.set(r + " Late", 0.8f, 0.0f, 1.0f);
    t.rvbSpread.set(r + " Spread", 0.15f, -1.0f, 1.0f);
    t.rvbLowpass.set(r + " Lowpass", 10000.0f, 200.0f, 20000.0f);
    t.rvbHighDamp.set(r + " Hi Damp", 6.0f, 0.0f, 24.0f);
    t.rvbLowDamp.set(r + " Lo Damp", 0.0f, 0.0f, 24.0f);
    t.rvbModFreq.set(r + " Mod Rate", 0.2f, 0.01f, 10.0f);
    t.rvbSpin.set(r + " Spin", 1.0f, 0.0f, 20.0f);
    t.rvbWander.set(r + " Wander", 0.25f, 0.0f, 20.0f);
    const std::string h = "HP " + n, sa = "Sat " + n, e = "Echo " + n;
    t.hp.set(h, false);
    t.hpFreq.set(h + " Freq", 80.0f, 10.0f, 1000.0f);
    t.hp24.set(h + " 24dB", false);
    t.sat.set(sa, false);
    t.satDrive.set(sa + " Drive", 6.0f, 0.0f, 36.0f);
    t.satBias.set(sa + " Warmth", 0.0f, 0.0f, 1.0f);
    t.satTone.set(sa + " Tone", 20000.0f, 500.0f, 20000.0f);
    t.satOutput.set(sa + " Output", 0.0f, -24.0f, 24.0f);
    t.satAuto.set(sa + " Auto Gain", true);
    t.satMix.set(sa + " Mix", 1.0f, 0.0f, 1.0f);
    t.echo.set(e, false);
    t.echoTime.set(e + " Delay", 0.375f, 0.005f, 4.0f);
    t.echoBeats.set(e + " Beats", true);
    t.echoBeatVal.set(e + " Delay Beats", 0.75f, 1.0f / 16.0f, 16.0f);
    t.echoFeed.set(e + " Feed", 0.45f, 0.0f, 0.98f);
    t.echoCutoff.set(e + " Cutoff", 96.0f, 12.0f, 130.0f);
    t.echoResonance.set(e + " Resonance", 0.1f, 0.0f, 0.9f);
    t.echoFilter.set(e + " Filter", 0, 0, 3);
    t.echoPingPong.set(e + " Ping Pong", 0.0f, 0.0f, 1.0f);
    // A return is a wet effect: its echo and reverb start fully wet
    t.echoMix.set(e + " Mix", t.isReturn ? 1.0f : 0.35f, 0.0f, 1.0f);
    if(t.isReturn) t.rvbMix.set(1.0f);
    for(int k = 0; k < MAX_RETURNS; k++) {
        const std::string sn = "Send " + n + " Aux" + ofToString(k + 1);
        t.sendLevel[k].set(sn, 0.0f, 0.0f, 1.0f);
        t.sendPre[k].set(sn + " Pre", false);
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
    t.listeners.push(t.hp.newListener([this, tp](bool&) { updateFx(*tp, FX_HP); }));
    t.listeners.push(t.comp.newListener([this, tp](bool&) { updateFx(*tp, FX_COMP); }));
    t.listeners.push(t.sat.newListener([this, tp](bool&) { updateFx(*tp, FX_SAT); }));
    t.listeners.push(t.echo.newListener([this, tp](bool&) { updateFx(*tp, FX_ECHO); }));
    t.listeners.push(t.rvb.newListener([this, tp](bool&) { updateFx(*tp, FX_RVB); }));
    t.listeners.push(t.hp24.newListener([this, tp](bool&) { sendHP(*tp); }));
    t.listeners.push(t.hpFreq.newListener([this, tp](float&) { sendHP(*tp); }));
    t.listeners.push(t.satAuto.newListener([this, tp](bool&) { sendSat(*tp); }));
    for(auto* q : {&t.satDrive, &t.satBias, &t.satTone, &t.satOutput, &t.satMix})
        t.listeners.push(q->newListener([this, tp](float&) { sendSat(*tp); }));
    t.listeners.push(t.echoBeats.newListener([this, tp](bool&) { sendEcho(*tp); }));
    t.listeners.push(t.echoFilter.newListener([this, tp](int&) { sendEcho(*tp); }));
    for(auto* q : {&t.echoTime, &t.echoBeatVal, &t.echoFeed, &t.echoCutoff, &t.echoResonance, &t.echoPingPong, &t.echoMix})
        t.listeners.push(q->newListener([this, tp](float&) { sendEcho(*tp); }));
    for(int k = 0; k < MAX_RETURNS; k++) {
        t.listeners.push(t.sendLevel[k].newListener([this, tp](float&) { updateSends(*tp); }));
        t.listeners.push(t.sendPre[k].newListener([this, tp](bool&) { updateSends(*tp); }));
    }
    t.listeners.push(t.compAuto.newListener([this, tp](bool&) { sendComp(*tp); }));
    for(auto* q : {&t.compThreshold, &t.compRatio, &t.compKnee, &t.compAttack, &t.compRelease,
                   &t.compMakeup, &t.compRms, &t.compHpf, &t.compMix})
        t.listeners.push(q->newListener([this, tp](float&) { sendComp(*tp); }));
    for(auto* q : {&t.rvbMix, &t.rvbDecay, &t.rvbSize, &t.rvbPredelay, &t.rvbPosition, &t.rvbSpread,
                   &t.rvbLowpass, &t.rvbHighDamp, &t.rvbLowDamp, &t.rvbModFreq, &t.rvbSpin, &t.rvbWander})
        t.listeners.push(q->newListener([this, tp](float&) { sendRvb(*tp); }));
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
    removeParameter(t.inputBadge.getEscapedName());
    scNode::removeInput((int)tracks.size() - 1);
    tracks.pop_back();
    reorderNodeParameters();
    updateMutes();
}

// Input parameter objects are renamed in place. Oceanode connections point to
// those objects, not to their names, so an already-wired track remains wired.
// A temporary pass also makes swaps ("Kick" <-> "Snare") collision-free.
void scMixerPro::syncTrackInputNames() {
    const int count = std::min((int)tracks.size(), (int)inputs.size());
    for(int i = 0; i < count; i++)
        inputs[i].setName("__MixerPro input slot " + ofToString(i + 1));

    auto& group = getParameterGroup();
    for(int i = 0; i < count; i++) {
        std::string base = tracks[i]->name.empty() ? "Track " + ofToString(i + 1) : tracks[i]->name;
        std::string candidate = base;
        int suffix = 2;
        while(group.contains(candidate)) candidate = base + " [" + ofToString(suffix++) + "]";
        if(!inputs[i].setName(candidate))
            ofLogWarning("scMixerPro") << "Could not rename input " << i + 1 << " to '" << candidate << "'";
    }
    reorderNodeParameters();
}

bool scMixerPro::canEncapsulateSubgraphFrom(ofxOceanodeAbstractParameter& parameter) const {
    // Only the dynamically-created audio inputs participate.  Comparing the
    // parameter name is safe here: every candidate belongs to this model and
    // syncTrackInputNames() keeps the input names unique within the group.
    return std::any_of(inputs.begin(), inputs.end(), [&](const ofParameter<nodePort>& input) {
        return input.getName() == parameter.getName();
    });
}

// Old Mixer Pro projects stored their connection endpoints as "In 1", etc.
// Keep those aliases until their connections have been restored, then switch
// to the track labels in presetRecallAfterSettingParameters().
void scMixerPro::useLegacyInputNames() {
    const int count = std::min((int)tracks.size(), (int)inputs.size());
    for(int i = 0; i < count; i++)
        inputs[i].setName("__MixerPro legacy input slot " + ofToString(i + 1));
    for(int i = 0; i < count; i++) inputs[i].setName("In " + ofToString(i + 1));
    reorderNodeParameters();
}

// Faders share the node parameter group with their corresponding inputs, so
// their internal names need to differ even though their visible labels should
// be identical. A zero-width suffix keeps the UI label exact while levelKey
// remains the stable identity used by publishing and preset state.
void scMixerPro::syncFaderNames() {
    auto& group = getParameterGroup();
    auto strips = allStrips();
    for(Track* tr : strips) tr->level.setName("__MixerPro fader slot " + tr->levelKey);

    const std::string zeroWidth = "\xE2\x80\x8B";
    std::vector<std::string> assigned;
    for(Track* tr : strips) {
        std::string base = tr->name.empty()
            ? (tr->isBus ? "Bus" : "Track " + ofToString(tr->index + 1))
            : tr->name;
        std::string candidate = base + zeroWidth;
        while(group.contains(candidate) ||
              std::find(assigned.begin(), assigned.end(), candidate) != assigned.end())
            candidate += zeroWidth;
        tr->level.setName(candidate);
        assigned.push_back(candidate);
    }
    reorderNodeParameters();
}

// Older projects restore their level connections against "Level N"/"Level BN".
// Adopt those names until connection restoration has completed.
void scMixerPro::useLegacyFaderNames() {
    for(Track* tr : allStrips()) tr->level.setName("__MixerPro legacy fader " + tr->levelKey);
    for(Track* tr : allStrips()) tr->level.setName(tr->levelKey);
    reorderNodeParameters();
}

// Keep the fixed sections and all dynamically-created inputs in their visual
// position. ofParameterGroup::reorder() retains the parameter objects, which
// is the important part: moving an inlet never drops its connection.
void scMixerPro::reorderNodeParameters() {
    auto& group = getParameterGroup();
    std::vector<std::string> order;
    auto append = [&](const std::string& name) {
        if(group.contains(name) && std::find(order.begin(), order.end(), name) == order.end())
            order.push_back(name);
    };
    auto appendSeparator = [&](const std::string& label) {
        const std::string prefix = "SEPARATOR:|" + label + "|";
        for(int i = 0; i < (int)group.size(); i++) {
            const std::string name = group.get(i).getName();
            if(name.rfind(prefix, 0) == 0) { append(name); return; }
        }
    };

    appendSeparator("Setup");
    append(showWindow.getName());
    append(numChannels.getName());
    append(numTracks.getName());
    append(showFaders.getName());
    appendSeparator("Mix Control");
    append(gainVec.getName());
    append(balanceVec.getName());
    append(masterLevel.getName());
    appendSeparator("Outputs");
    for(auto& output : outputs) append(output.getName());
    appendSeparator("Inputs");
    for(int i = 0; i < (int)tracks.size() && i < (int)inputs.size(); i++) {
        append(tracks[i]->inputBadge.getName());
        append(inputs[i].getName());
    }

    appendSeparator("Published");
    for(const auto& key : publishedKeys) {
        auto handle = nodeHandles.find(key);
        if(handle != nodeHandles.end() && handle->second) {
            if(Track* tr = stripForLevelKey(key)) append(tr->faderBadge.getName());
            append(handle->second->getName());
        }
    }
    appendSeparator("Faders");
    for(const auto& key : faderKeys) {
        auto handle = nodeHandles.find(key);
        if(handle != nodeHandles.end() && handle->second) {
            Track* tr = stripForLevelKey(key);
            if(tr && tr->isBus) continue;
            if(tr) append(tr->faderBadge.getName());
            append(handle->second->getName());
        }
    }
    appendSeparator("Submasters");
    for(const auto& key : faderKeys) {
        auto handle = nodeHandles.find(key);
        if(handle != nodeHandles.end() && handle->second) {
            Track* tr = stripForLevelKey(key);
            if(!tr || !tr->isBus) continue;
            append(tr->faderBadge.getName());
            append(handle->second->getName());
        }
    }

    // Retain any future or connection-held controls in their relative order.
    for(int i = 0; i < (int)group.size(); i++) append(group.get(i).getName());
    group.reorder(order);
    parameterGroupChanged.notify(this);
}

void scMixerPro::setStripServer(Track& tr, int server) {
    server = ofClamp(server, 0, std::max(0, (int)servers.size() - 1));
    if(tr.server == server) return;
    destroyRuntime(tr, true);
    tr.server = server;
    tr.srcBus = -1;
    if(isServerActive(server)) createRuntime(tr, serverAt(server));
    if(tr.isReturn) refreshSends();
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
    for(auto* q : fxParams(t)) p.push_back(q);
    return p;
}

// The insert and send parameters (a fixed order: reorder "roles")
std::vector<ofAbstractParameter*> scMixerPro::fxParams(Track& t) {
    std::vector<ofAbstractParameter*> p = {
        &t.comp, &t.compThreshold, &t.compRatio, &t.compKnee, &t.compAttack, &t.compRelease,
        &t.compMakeup, &t.compAuto, &t.compRms, &t.compHpf, &t.compMix,
        &t.rvb, &t.rvbMix, &t.rvbDecay, &t.rvbSize, &t.rvbPredelay, &t.rvbPosition, &t.rvbSpread,
        &t.rvbLowpass, &t.rvbHighDamp, &t.rvbLowDamp, &t.rvbModFreq, &t.rvbSpin, &t.rvbWander,
        &t.hp, &t.hpFreq, &t.hp24,
        &t.sat, &t.satDrive, &t.satBias, &t.satTone, &t.satOutput, &t.satAuto, &t.satMix,
        &t.echo, &t.echoTime, &t.echoBeats, &t.echoBeatVal, &t.echoFeed, &t.echoCutoff,
        &t.echoResonance, &t.echoFilter, &t.echoPingPong, &t.echoMix
    };
    for(int k = 0; k < MAX_RETURNS; k++) { p.push_back(&t.sendLevel[k]); p.push_back(&t.sendPre[k]); }
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
    auto roleKey = [](Track& tr, ofAbstractParameter* parameter) {
        return parameter == &tr.level ? tr.levelKey : parameter->getEscapedName();
    };

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
            const std::string key = roleKey(tr, params[r]);
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
    for(int j = 0; j < oldCount; j++) {
        Track& tr = *tracks[j];
        for(auto* p : stripParams(tr)) {
            const std::string key = roleKey(tr, p);
            if(isPublished(key)) unpublishKey(key);
        }
    }

    const vector<float> oldGain = gainVec.get(), oldBalance = balanceVec.get();
    std::vector<int> newPosition(oldCount, -1);
    for(int i = 0; i < (int)order.size(); i++) newPosition[order[i]] = i;
    const int oldEq = selectedEqTrack;

    // 3. New count, then each position takes its track's settings
    setTrackCount((int)order.size());
    for(int i = 0; i < (int)order.size(); i++) loadStrip(*tracks[i], snaps[order[i]].json);
    syncTrackInputNames();
    syncFaderNames();
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
            if(snap.published[r]) publishKey(roleKey(*tracks[i], params[r]));
    }
    if(showFaders.get()) updateFaders();
    for(int i = 0; i < (int)order.size(); i++) {
        const Snapshot& snap = snaps[order[i]];
        if(snap.inputSource && i < (int)inputs.size())
            if(auto* in = paramByName(inputs[i].getEscapedName()))
                container->createConnection(*snap.inputSource, *in);
        const auto params = stripParams(*tracks[i]);
        for(size_t r = 0; r < params.size(); r++) {
            auto handle = nodeHandles.find(roleKey(*tracks[i], params[r]));
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
    syncFaderNames();
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
    all.reserve(tracks.size() + buses.size() + returns.size());
    for(auto& t : tracks) all.push_back(t.get());
    for(auto& b : buses) all.push_back(b.get());
    for(auto& r : returns) all.push_back(r.get());
    return all;
}

// ── Aux returns ─────────────────────────────────────────────────────────────
// A return is a submaster (isBus) fed by the strips' sends instead of their
// outputs (isReturn): same inserts, runs after the submasters, solo-safe.

void scMixerPro::setReturnCount(int count) {
    count = ofClamp(count, 0, MAX_RETURNS);
    while((int)returns.size() < count) addReturn();
    while((int)returns.size() > count) removeLastReturn();
}

void scMixerPro::addReturn() {
    auto rp = std::make_unique<Track>();
    Track& r = *rp;
    r.isBus = true;
    r.isReturn = true;
    r.index = (int)returns.size();
    r.name = "Aux " + ofToString(r.index + 1);
    static const ofColor kReturnColours[MAX_RETURNS] = {
        ofColor(70, 190, 190), ofColor(90, 170, 220), ofColor(120, 200, 150), ofColor(170, 200, 90)
    };
    r.color = kReturnColours[r.index % MAX_RETURNS];
    r.server = 0;
    initStrip(r, "R" + ofToString(r.index + 1));
    returns.push_back(std::move(rp));
    registerTrackActions(r);
    syncFaderNames();
    if(showFaders.get()) updateFaders();
    if(isServerActive(r.server)) createRuntime(r, serverAt(r.server));
    refreshSends();
}

void scMixerPro::removeLastReturn() {
    if(returns.empty()) return;
    Track& r = *returns.back();
    r.listeners.unsubscribeAll();
    destroyRuntime(r, true);
    unregisterTrackActions(r);
    if(selectedEqTrack == eqId(r)) selectedEqTrack = -1;
    returns.pop_back();
    refreshSends();
}

// The strips' sends follow the returns (created, removed, moved)
void scMixerPro::refreshSends() {
    for(auto& t : tracks) updateSends(*t);
    for(auto& b : buses) updateSends(*b);
}

// A send synth only while the strip sends something to a live return on its
// server; the track synth's taps (\sendBus) are off otherwise
void scMixerPro::updateSends(Track& tr) {
    if(tr.isReturn || !tr.synth || !tr.liveServer) return;
    ofxSCServer* server = tr.liveServer;
    int target[MAX_RETURNS];
    bool any = false;
    for(int k = 0; k < MAX_RETURNS; k++) {
        target[k] = -1;
        if(k >= (int)returns.size() || tr.sendLevel[k].get() <= 0.0f) continue;
        Track& r = *returns[k];
        if(r.synth && r.mixBus && r.liveServer == server) { target[k] = r.mixBus->index; any = true; }
    }
    if(!any) {
        if(tr.sendSynth && tr.sendRunning) {
            tr.synth->set("sendBus", -1);
            tr.sendSynth->run(false);
            tr.sendRunning = false;
        }
        return;
    }
    if(!tr.sendBus) tr.sendBus = new ofxSCBus(RATE_AUDIO, 2 * currentChannels, server);
    auto sendTo = [&](ofxSCSynth* s) {
        s->set("in", tr.sendBus->index);
        for(int k = 0; k < MAX_RETURNS; k++) {
            const std::string i = ofToString(k);
            s->set("ret" + i, target[k]);
            s->set("gain" + i, target[k] >= 0 ? ofClamp(tr.sendLevel[k].get(), 0.0f, 1.0f) : 0.0f);
            s->set("pre" + i, tr.sendPre[k].get() ? 1.0f : 0.0f);
        }
    };
    if(!tr.sendSynth) {
        tr.sendSynth = new ofxSCSynth("mixerProSend" + ofToString(currentChannels), server);
        sendTo(tr.sendSynth);
        tr.sendSynth->createAndRun(3, tr.synth->nodeID, getActive());   // addAfter
        orderInsertAfter(server, tr.sendSynth, tr.synth);
    } else {
        sendTo(tr.sendSynth);
        if(!tr.sendRunning && getActive()) tr.sendSynth->run(true);
    }
    tr.sendRunning = true;
    tr.synth->set("sendBus", tr.sendBus->index);
}

void scMixerPro::dropSends(Track& tr, bool sendFree) {
    if(tr.sendSynth) {
        if(tr.liveServer) orderRemove(tr.liveServer, tr.sendSynth);
        if(sendFree) tr.sendSynth->free();
        delete tr.sendSynth;
        tr.sendSynth = nullptr;
    }
    if(tr.sendBus) {
        if(sendFree) tr.sendBus->free();
        delete tr.sendBus;
        tr.sendBus = nullptr;
    }
    tr.sendRunning = false;
    if(tr.synth) tr.synth->set("sendBus", -1);
}

// Where a strip synth of a zone goes: before the first synth of a later zone
// (2: returns and master, 3: master), nullptr = at the end
ofxSCSynth* scMixerPro::firstSynthOfZone(ofxSCServer* server, int zone) {
    std::vector<ofxSCSynth*> later;
    auto addStrip = [&](Track& t) {
        if(t.liveServer != server) return;
        if(t.adapt) later.push_back(t.adapt);
        if(t.insert) later.push_back(t.insert);
        for(auto& f : t.fx) if(f.synth) later.push_back(f.synth);
        if(t.synth) later.push_back(t.synth);
        if(t.sendSynth) later.push_back(t.sendSynth);
    };
    if(zone <= 2) for(auto& r : returns) addStrip(*r);
    MasterRuntime& m = serverStates[server].master;
    for(auto& f : m.st) if(f.synth) later.push_back(f.synth);
    if(m.out) later.push_back(m.out);
    for(ofxSCSynth* s : serverStates[server].order)
        if(std::find(later.begin(), later.end(), s) != later.end()) return s;
    return nullptr;
}

void scMixerPro::orderInsertAfter(ofxSCServer* server, ofxSCSynth* added, ofxSCSynth* after) {
    auto& order = serverStates[server].order;
    auto it = std::find(order.begin(), order.end(), after);
    order.insert(it == order.end() ? it : it + 1, added);
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
    return masterInput(tr.liveServer);
}

// ════════════════════════════════════════════════════════════════════════════
// Master section: HP -> EQ -> Maximizer -> Limiter -> MasterOut, per server,
// only while one of the stages is on (otherwise the strips write straight to
// the output and there is no master synth)
// ════════════════════════════════════════════════════════════════════════════

std::vector<ofAbstractParameter*> scMixerPro::masterParams() {
    std::vector<ofAbstractParameter*> p = {
        &mHp, &mHpFreq, &mHp24, &mEq, &mMax, &mMaxDrive, &mMaxCeiling, &mMaxCharacter, &mMaxIsp, &mLim, &mLimCeiling
    };
    for(int b = 0; b < scEQEditor::NUM_BANDS; b++) { p.push_back(&mEqFreq[b]); p.push_back(&mEqGain[b]); p.push_back(&mEqShape[b]); }
    return p;
}

bool scMixerPro::masterWanted(int k) const {
    switch(k) {
        case MST_HP:  return mHp.get();
        case MST_EQ:  return mEq.get();
        case MST_MAX: return mMax.get();
        default:      return mLim.get();
    }
}

// Where the strips that go to the master write on a server
int scMixerPro::masterInput(ofxSCServer* server) {
    auto it = serverStates.find(server);
    if(it != serverStates.end() && it->second.master.active && it->second.master.bus)
        return it->second.master.bus->index;
    return outBusFor(server);
}

void scMixerPro::refreshStripOuts(ofxSCServer* server) {
    for(Track* tr : allStrips())
        if(tr->synth && tr->liveServer == server) tr->synth->set("out", trackOutBus(*tr));
}

void scMixerPro::updateMasterAll() {
    for(auto& [server, state] : serverStates) if(state.active) updateMaster(server);
}

void scMixerPro::updateMaster(ofxSCServer* server) {
    auto it = serverStates.find(server);
    if(it == serverStates.end() || !it->second.active) return;
    ServerState& state = it->second;
    MasterRuntime& m = state.master;
    static const char* bases[MST_COUNT] = {"mixerProHP", "mixerProEQ", "mixerProMax", "mixerProLim"};
    const std::string n = ofToString(currentChannels);

    bool wanted = false;
    for(int k = 0; k < MST_COUNT; k++) wanted = wanted || masterWanted(k);
    if(wanted) {
        // MasterOut first: the stages go before it
        if(!m.bus) m.bus = new ofxSCBus(RATE_AUDIO, currentChannels, server);
        if(!m.vuBus) m.vuBus = new ofxSCBus(RATE_CONTROL, currentChannels, server);
        if(!m.out) {
            m.out = new ofxSCSynth("mixerProMasterOut" + n, server);
            m.out->set("in", m.bus->index);
            m.out->set("out", outBusFor(server));
            m.out->set("vuBus", m.vuBus->index);
            m.out->set("vuAttackTime", vuAttack.get());
            m.out->set("vuReleaseTime", vuRelease.get());
            if(state.order.empty()) m.out->createAndRun(0, 1, getActive());
            else m.out->createAndRun(3, state.order.back()->nodeID, getActive());   // addAfter: last
            state.order.push_back(m.out);
        } else if(!m.outRunning && getActive()) {
            m.out->run(true);
        }
        m.outRunning = true;
    }

    const float now = ofGetElapsedTimef();
    for(int k = 0; k < MST_COUNT; k++) {
        FxStage& f = m.st[k];
        if(!masterWanted(k)) {
            if(f.synth && f.running && f.offAt < 0.0f) {
                f.offAt = now + (k == MST_EQ ? 0.0f : 0.15f);   // the EQ has no fade
                sendMasterStage(server, k);
            }
            continue;
        }
        f.offAt = -1.0f;
        if(!f.bus) f.bus = new ofxSCBus(RATE_AUDIO, currentChannels, server);
        if(!f.synth) {
            ofxSCSynth* before = m.out;
            for(int j = k + 1; j < MST_COUNT; j++) if(m.st[j].synth) { before = m.st[j].synth; break; }
            f.synth = new ofxSCSynth(bases[k] + n, server);
            f.synth->set("out", f.bus->index);
            sendMasterStage(server, k);
            f.synth->createAndRun(2, before->nodeID, getActive());   // addBefore
            orderInsertBefore(server, f.synth, before);
        } else {
            sendMasterStage(server, k);
            if(!f.running && getActive()) f.synth->run(true);
        }
        f.running = true;
    }

    bool running = false;
    for(auto& f : m.st) running = running || f.running;
    masterRouting(server);
    if(running && !m.active) {
        m.active = true;
        refreshStripOuts(server);
    } else if(!running && m.active) {
        m.active = false;
        refreshStripOuts(server);   // straight to the output again, then pause
    }
    if(!running && m.out && m.outRunning) {
        m.out->run(false);
        m.outRunning = false;
    }
}

// Each running stage reads the previous one (the first the master bus);
// MasterOut reads the last
void scMixerPro::masterRouting(ofxSCServer* server) {
    MasterRuntime& m = serverStates[server].master;
    if(!m.bus) return;
    int bus = m.bus->index;
    for(auto& f : m.st) {
        if(!(f.synth && f.running && f.bus)) continue;
        f.synth->set("in", bus);
        f.synth->set("out", f.bus->index);
        bus = f.bus->index;
    }
    if(m.out) m.out->set("in", bus);
}

void scMixerPro::sendMasterStage(ofxSCServer* server, int k) {
    MasterRuntime& m = serverStates[server].master;
    ofxSCSynth* s = m.st[k].synth;
    if(!s) return;
    const float on = (masterWanted(k) && m.st[k].offAt < 0.0f) ? 1.0f : 0.0f;
    switch(k) {
        case MST_HP:
            s->set("freq", ofClamp(mHpFreq.get(), 10.0f, 20000.0f));
            s->set("slope", mHp24.get() ? 1.0f : 0.0f);
            s->set("on", on);
            break;
        case MST_EQ:
            for(int b = 0; b < scEQEditor::NUM_BANDS; b++) {
                const std::string key = "b" + ofToString(b + 1);
                s->set(key + "gain", mEqGain[b].get());
                s->set(key + "freq", mEqFreq[b].get());
                if(scEQEditor::isShelf(b))
                    s->set(key + "slope", 1.0f / scEQEditor::safeShelfSlope(mEqShape[b].get(), mEqGain[b].get()));
                else
                    s->set(key + "rq", 1.0f / std::max(0.01f, mEqShape[b].get()));
            }
            break;
        case MST_MAX:
            s->set("drive", ofClamp(mMaxDrive.get(), 0.0f, 24.0f));
            s->set("ceiling", ofClamp(mMaxCeiling.get(), -12.0f, 0.0f));
            s->set("character", ofClamp(mMaxCharacter.get(), 0.0f, 10.0f));
            s->set("isp", mMaxIsp.get() ? 1.0f : 0.0f);
            s->set("on", on);
            break;
        default:
            s->set("ceiling", ofClamp(mLimCeiling.get(), -24.0f, 0.0f));
            s->set("on", on);
            break;
    }
}

void scMixerPro::destroyMaster(ofxSCServer* server, bool sendFree) {
    MasterRuntime& m = serverStates[server].master;
    const bool wasActive = m.active;
    auto drop = [&](ofxSCSynth*& s) {
        if(!s) return;
        orderRemove(server, s);
        if(sendFree) s->free();
        delete s;
        s = nullptr;
    };
    auto dropBus = [&](ofxSCBus*& b) {
        if(!b) return;
        if(sendFree) b->free();
        delete b;
        b = nullptr;
    };
    for(auto& f : m.st) { drop(f.synth); dropBus(f.bus); f.running = false; f.offAt = -1.0f; }
    drop(m.out);
    dropBus(m.bus);
    dropBus(m.vuBus);
    m.outRunning = false;
    m.active = false;
    m.vu.clear();
    if(wasActive && sendFree) refreshStripOuts(server);
}

// Where every live track writes, and its master level (1 into a bus)
void scMixerPro::refreshTrackOutputs() {
    for(Track* tr : allStrips()) {
        if(!tr->synth) continue;
        tr->synth->set("out", trackOutBus(*tr));
        sendMasterLevel(*tr);
    }
    updateMutes();
}

scMixerPro::Track* scMixerPro::eqTarget() {
    if(selectedEqTrack >= MASTER_PANEL) return nullptr;
    if(selectedEqTrack >= 1500) {
        const int r = selectedEqTrack - 1500;
        return r < (int)returns.size() ? returns[r].get() : nullptr;
    }
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
        auto replaceFx = [&](FxStage& fx, const char* base) {
            if(fx.bus) {
                fx.bus->free(); delete fx.bus;
                fx.bus = new ofxSCBus(RATE_AUDIO, channels, server);
            }
            if(!fx.synth) return;
            auto* synth = new ofxSCSynth(base + ofToString(channels), server);
            synth->createAndRun(4, fx.synth->nodeID, getActive() && fx.running);
            orderReplace(server, fx.synth, synth);
            delete fx.synth;
            fx.synth = synth;
        };
        static const char* bases[FX_COUNT] = {"mixerProHP", "mixerProComp", "mixerProSat", "mixerProEcho", "mixerProRvb"};
        for(int k = 0; k < FX_COUNT; k++) replaceFx(tr.fx[k], bases[k]);
        dropSends(tr, true);   // its def and bus depend on the width; updateSends below
        tr.sentMute = -1.0f;
        sendAll(tr);
        updateAdapter(tr);
    }
    // The master chains are rebuilt at the new width
    for(auto& [server, state] : serverStates) {
        if(!state.active) continue;
        destroyMaster(server, true);
        updateMaster(server);
    }
    for(Track* tr : allStrips()) updateSends(*tr);
    refreshTrackOutputs();
    masterVU.assign(channels, 0.0f);
    masterPeak.assign(channels, 0.0f);
    masterPeakAge.assign(channels, 0.0f);
}

bool scMixerPro::anySolo() const {
    for(const auto& tr : tracks) if(tr->solo.get()) return true;
    for(const auto& b : buses) if(b->solo.get()) return true;
    return false;   // returns are solo-safe and have no solo
}

// A soloed bus opens all of its tracks until one or more of those tracks are
// soloed; then the child solos narrow that bus. A soloed track always keeps
// its parent bus audible.
bool scMixerPro::isSilenced(const Track& tr) const {
    if(tr.mute.get()) return true;
    if(tr.isReturn) return false;   // solo-safe: a soloed track keeps its reverb
    if(!anySolo() || tr.solo.get()) return false;
    if(!tr.isBus) {
        Track* b = busFor(tr);
        if(!(b && b->solo.get())) return true;
        for(const auto& sibling : tracks)
            if(sibling->solo.get() && busFor(*sibling) == b) return true;
        return false;
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
        // Submasters run after every track, returns after the submasters,
        // the master section last
        ofxSCSynth* before = firstSynthOfZone(server, tr.isReturn ? 3 : 2);
        if(before) {
            tr.synth->createAndRun(2, before->nodeID, getActive());   // addBefore
            orderInsertBefore(server, tr.synth, before);
        } else {
            tr.synth->createAndRun(3, order.back()->nodeID, getActive());   // addAfter
            order.push_back(tr.synth);
        }
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
    for(int k = 0; k < FX_COUNT; k++) if(fxSwitch(tr, k).get()) updateFx(tr, k);
    tr.detectedInputs = sourceChannels(tr);
    updateAdapter(tr);
    if(tr.isReturn) refreshSends();
    else updateSends(tr);
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
    for(auto& f : tr.fx) drop(f.synth);
    drop(tr.sendSynth);
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
    for(auto& f : tr.fx) dropBus(f.bus);
    dropBus(tr.sendBus);
    dropBus(tr.mixBus);
    tr.insertVariant = 0;
    tr.insertRunning = false;
    for(auto& f : tr.fx) { f.running = false; f.offAt = -1.0f; }
    tr.sendRunning = false;
    tr.gr = 0.0f;
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
        tr.insert->set("in", stageInput(tr, 1));
        tr.insert->set("out", tr.insertBus->index);
    }
    for(int k = 0; k < FX_COUNT; k++) {
        const FxStage& f = tr.fx[k];
        if(!(f.synth && f.running && f.bus)) continue;
        f.synth->set("in", stageInput(tr, fxPos(k)));
        f.synth->set("out", f.bus->index);
    }
    tr.synth->set("in", stageInput(tr, CHAIN_TRACK));
}

// A chain position's output bus while it runs (-1: not in the chain)
int scMixerPro::stageOutBus(const Track& tr, int pos) const {
    if(pos == 1) return (tr.insert && tr.insertRunning && tr.insertBus) ? tr.insertBus->index : -1;
    const int k = fxAtPos(pos);
    if(k < 0) return -1;
    const FxStage& f = tr.fx[k];
    return (f.synth && f.running && f.bus) ? f.bus->index : -1;
}

ofxSCSynth* scMixerPro::stageSynth(const Track& tr, int pos) const {
    if(pos == 1) return tr.insert;
    const int k = fxAtPos(pos);
    return k >= 0 ? tr.fx[k].synth : nullptr;
}

// The bus a chain position reads: the output of the last running stage
// before it, else the source / adapter
int scMixerPro::stageInput(const Track& tr, int pos) const {
    int bus = chainInput(tr);
    for(int p = 0; p < pos && p < CHAIN_TRACK; p++) {
        const int b = stageOutBus(tr, p);
        if(b >= 0) bus = b;
    }
    return bus;
}

// The first existing synth after a chain position (-1: the adapter): where a
// new stage is added before, so the chain keeps its order
ofxSCSynth* scMixerPro::chainSynthAfter(const Track& tr, int pos) const {
    for(int p = pos + 1; p < CHAIN_TRACK; p++)
        if(ofxSCSynth* s = stageSynth(tr, p)) return s;
    return tr.synth;
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
            ofxSCSynth* first = chainSynthAfter(tr, -1);              // head of the track's chain
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
            tr.insertRunning = false;
            applyInputRouting(tr);
            tr.insert->run(false);
        }
        return;
    }

    if(!tr.insertBus) tr.insertBus = new ofxSCBus(RATE_AUDIO, currentChannels, server);
    if(!tr.insert) {
        tr.insert = new ofxSCSynth(insertDefName(variant), server);
        tr.insert->set("in", chainInput(tr));
        tr.insert->set("out", tr.insertBus->index);
        sendEQ(tr);
        ofxSCSynth* before = chainSynthAfter(tr, 1);
        tr.insert->createAndRun(2, before->nodeID, getActive());
        orderInsertBefore(server, tr.insert, before);
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
    applyInputRouting(tr);
}

// ── Strip inserts (HP, Comp, Sat, Echo, Rvb) ───────────────────────────────

ofParameter<bool>& scMixerPro::fxSwitch(Track& tr, int k) {
    switch(k) {
        case FX_HP:   return tr.hp;
        case FX_COMP: return tr.comp;
        case FX_SAT:  return tr.sat;
        case FX_ECHO: return tr.echo;
        default:      return tr.rvb;
    }
}

void scMixerPro::sendFx(Track& tr, int k) {
    switch(k) {
        case FX_HP:   sendHP(tr); break;
        case FX_COMP: sendComp(tr); break;
        case FX_SAT:  sendSat(tr); break;
        case FX_ECHO: sendEcho(tr); break;
        default:      sendRvb(tr); break;
    }
}

// Switched on: create (first time) or resume, and route through it.
// Switched off: \on 0 (HP / Comp / Sat fade to dry, Echo / Rvb stop feeding
// their loop), and update() pauses it and routes around it once faded or
// rung out.
void scMixerPro::updateFx(Track& tr, int k) {
    if(!tr.synth || !tr.liveServer) return;   // applied when the synth is created
    ofxSCServer* server = tr.liveServer;
    static const char* bases[FX_COUNT] = {"mixerProHP", "mixerProComp", "mixerProSat", "mixerProEcho", "mixerProRvb"};
    FxStage& f = tr.fx[k];

    if(!fxSwitch(tr, k).get()) {
        if(f.synth && f.running && f.offAt < 0.0f) {
            f.offAt = ofGetElapsedTimef() + fxOffDelay(tr, k);
            sendFx(tr, k);   // on 0
        }
        return;
    }

    f.offAt = -1.0f;
    if(!f.bus) f.bus = new ofxSCBus(RATE_AUDIO, currentChannels, server);
    if(!f.synth) {
        ofxSCSynth* before = chainSynthAfter(tr, fxPos(k));
        f.synth = new ofxSCSynth(bases[k] + ofToString(currentChannels), server);
        f.synth->set("in", stageInput(tr, fxPos(k)));
        f.synth->set("out", f.bus->index);
        sendFx(tr, k);   // stored as /s_new arguments
        f.synth->createAndRun(2, before->nodeID, getActive());   // addBefore
        orderInsertBefore(server, f.synth, before);
    } else {
        sendFx(tr, k);   // on 1: fades back in from where it was paused
        if(!f.running && getActive()) f.synth->run(true);
    }
    f.running = true;
    applyInputRouting(tr);
}

// How long a switched-off insert keeps running before it is paused
float scMixerPro::fxOffDelay(const Track& tr, int k) const {
    if(k == FX_RVB) return rvbTail(tr);
    if(k == FX_ECHO) {
        // repeats down to -60 dB: 60 / (-20 log10 feed) of them
        const float fb = ofClamp(tr.echoFeed.get(), 0.0f, 0.98f);
        const float repeats = fb < 0.01f ? 1.0f : std::min(200.0f, 3.0f / -std::log10(fb));
        return std::min(30.0f, echoDelaySec(tr) * (repeats + 1.0f) + 0.2f);
    }
    return 0.15f;
}

// How long a switched-off reverb keeps running: its tail down to -60 dB
float scMixerPro::rvbTail(const Track& tr) const {
    return std::min(30.0f, tr.rvbDecay.get() + tr.rvbPredelay.get() * 0.001f + 0.3f);
}

// Echo delay: seconds, or a note value (beats) at the node's tempo (max 4 s)
float scMixerPro::echoDelaySec(const Track& tr) const {
    if(!tr.echoBeats.get()) return ofClamp(tr.echoTime.get(), 0.005f, 4.0f);
    return ofClamp(tr.echoBeatVal.get() * 60.0f / std::max(1.0f, currentBpm), 0.005f, 4.0f);
}

void scMixerPro::setBpm(float bpm) {
    currentBpm = bpm;
    for(Track* tr : allStrips()) if(tr->echoBeats.get()) sendEcho(*tr);
}

void scMixerPro::processFxTimers() {
    const float now = ofGetElapsedTimef();
    for(Track* tr : allStrips()) {
        for(FxStage& f : tr->fx) {
            if(f.offAt < 0.0f || now < f.offAt) continue;
            f.offAt = -1.0f;
            if(!f.synth || !f.running || !tr->synth) continue;
            f.running = false;
            applyInputRouting(*tr);   // around it first, then pause: no gap
            f.synth->run(false);
        }
    }
    // Master stages: the same, then the master itself when nothing is left
    for(auto& [server, state] : serverStates) {
        MasterRuntime& m = state.master;
        bool changed = false;
        for(FxStage& f : m.st) {
            if(f.offAt < 0.0f || now < f.offAt) continue;
            f.offAt = -1.0f;
            if(!f.synth || !f.running) continue;
            f.running = false;
            changed = true;
        }
        if(!changed) continue;
        masterRouting(server);
        for(FxStage& f : m.st) if(f.synth && !f.running) f.synth->run(false);
        updateMaster(server);   // deactivates the master once every stage is off
    }
}

void scMixerPro::sendHP(Track& tr) {
    ofxSCSynth* s = tr.fx[FX_HP].synth;
    if(!s) return;
    s->set("freq", ofClamp(tr.hpFreq.get(), 10.0f, 20000.0f));
    s->set("slope", tr.hp24.get() ? 1.0f : 0.0f);
    s->set("on", (tr.hp.get() && tr.fx[FX_HP].offAt < 0.0f) ? 1.0f : 0.0f);
}

void scMixerPro::sendComp(Track& tr) {
    ofxSCSynth* s = tr.fx[FX_COMP].synth;
    if(!s) return;
    s->set("threshold", tr.compThreshold.get());
    s->set("ratio", std::max(1.0f, tr.compRatio.get()));
    s->set("knee", std::max(0.0f, tr.compKnee.get()));
    s->set("attack", std::max(0.01f, tr.compAttack.get()));
    s->set("release", std::max(1.0f, tr.compRelease.get()));
    s->set("makeup", tr.compMakeup.get());
    s->set("automakeup", tr.compAuto.get() ? 1.0f : 0.0f);
    s->set("rms", ofClamp(tr.compRms.get(), 0.0f, 1.0f));
    s->set("schpf", ofClamp(tr.compHpf.get(), 10.0f, 2000.0f));
    s->set("mix", ofClamp(tr.compMix.get(), 0.0f, 1.0f));
    s->set("grBus", tr.vuBus ? tr.vuBus->index + currentChannels + 2 : -1);
    s->set("on", (tr.comp.get() && tr.fx[FX_COMP].offAt < 0.0f) ? 1.0f : 0.0f);
}

void scMixerPro::sendSat(Track& tr) {
    ofxSCSynth* s = tr.fx[FX_SAT].synth;
    if(!s) return;
    s->set("drive", ofClamp(tr.satDrive.get(), 0.0f, 36.0f));
    s->set("bias", ofClamp(tr.satBias.get(), 0.0f, 1.0f));
    s->set("tone", ofClamp(tr.satTone.get(), 500.0f, 20000.0f));
    s->set("output", ofClamp(tr.satOutput.get(), -24.0f, 24.0f));
    s->set("autogain", tr.satAuto.get() ? 1.0f : 0.0f);
    s->set("mix", ofClamp(tr.satMix.get(), 0.0f, 1.0f));
    s->set("on", (tr.sat.get() && tr.fx[FX_SAT].offAt < 0.0f) ? 1.0f : 0.0f);
}

void scMixerPro::sendEcho(Track& tr) {
    ofxSCSynth* s = tr.fx[FX_ECHO].synth;
    if(!s) return;
    s->set("delay", echoDelaySec(tr));
    s->set("feed", ofClamp(tr.echoFeed.get(), 0.0f, 0.98f));
    s->set("cutoff", ofClamp(tr.echoCutoff.get(), 12.0f, 130.0f));
    s->set("resonance", ofClamp(tr.echoResonance.get(), 0.0f, 0.9f));
    s->set("filtertype", (float)ofClamp(tr.echoFilter.get(), 0, 3));
    s->set("pingpong", ofClamp(tr.echoPingPong.get(), 0.0f, 1.0f));
    s->set("mix", ofClamp(tr.echoMix.get(), 0.0f, 1.0f));
    s->set("on", (tr.echo.get() && tr.fx[FX_ECHO].offAt < 0.0f) ? 1.0f : 0.0f);
}

void scMixerPro::sendRvb(Track& tr) {
    ofxSCSynth* s = tr.fx[FX_RVB].synth;
    if(!s) return;
    s->set("mix", ofClamp(tr.rvbMix.get(), 0.0f, 1.0f));
    s->set("decay", std::max(0.05f, tr.rvbDecay.get()));
    s->set("size", ofClamp(tr.rvbSize.get(), 0.0f, 60.0f));
    s->set("predelay", ofClamp(tr.rvbPredelay.get(), 0.0f, 500.0f));
    s->set("position", ofClamp(tr.rvbPosition.get(), 0.0f, 1.0f));
    s->set("spread", ofClamp(tr.rvbSpread.get(), -1.0f, 1.0f));
    s->set("lowpass", ofClamp(tr.rvbLowpass.get(), 20.0f, 20000.0f));
    s->set("highdamp", ofClamp(tr.rvbHighDamp.get(), 0.0f, 60.0f));
    s->set("lowdamp", ofClamp(tr.rvbLowDamp.get(), 0.0f, 60.0f));
    s->set("modfreq", ofClamp(tr.rvbModFreq.get(), 0.001f, 20.0f));
    s->set("spin", ofClamp(tr.rvbSpin.get(), 0.0f, 20.0f));
    s->set("wander", ofClamp(tr.rvbWander.get(), 0.0f, 20.0f));
    s->set("on", (tr.rvb.get() && tr.fx[FX_RVB].offAt < 0.0f) ? 1.0f : 0.0f);
}

// Fader times Gain Vec (a missing entry counts as 1)
void scMixerPro::sendLevel(Track& tr) {
    if(!tr.synth) return;
    const auto& v = gainVec.get();
    const float g = (!tr.isBus && tr.index < (int)v.size()) ? v[tr.index] : 1.0f;
    tr.synth->setMultiple("level", ofClamp(tr.level.get(), 0.0f, kTrackMaxGain) * ofClamp(g, 0.0f, 2.0f), currentChannels);
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
    for(int k = 0; k < FX_COUNT; k++) sendFx(tr, k);
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
    for(auto& [server, state] : serverStates)
        for(int k = 0; k < MST_COUNT; k++) sendMasterStage(server, k);
}

void scMixerPro::activate() {
    for(Track* tr : allStrips()) {
        if(tr->synth) tr->synth->run(true);
        if(tr->insert && tr->insertRunning) tr->insert->run(true);
        for(auto& f : tr->fx) if(f.synth && f.running) f.synth->run(true);
        if(tr->sendSynth && tr->sendRunning) tr->sendSynth->run(true);
        if(tr->adapt && tr->adaptRunning) tr->adapt->run(true);
    }
    for(auto& [server, state] : serverStates) {
        for(auto& f : state.master.st) if(f.synth && f.running) f.synth->run(true);
        if(state.master.out && state.master.outRunning) state.master.out->run(true);
    }
}

void scMixerPro::deactivate() {
    for(Track* tr : allStrips()) {
        if(tr->synth) tr->synth->run(false);
        if(tr->insert) tr->insert->run(false);
        for(auto& f : tr->fx) if(f.synth) f.synth->run(false);
        if(tr->sendSynth) tr->sendSynth->run(false);
        if(tr->adapt) tr->adapt->run(false);
    }
    for(auto& [server, state] : serverStates) {
        for(auto& f : state.master.st) if(f.synth) f.synth->run(false);
        if(state.master.out) state.master.out->run(false);
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
    destroyMaster(server, false);
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
    // Then the buses, each after everything else (in bus order), the returns
    // and the master section
    for(auto& b : buses) if(b->server == k) createRuntime(*b, server);
    for(auto& r : returns) if(r->server == k) createRuntime(*r, server);
    updateMaster(server);
    refreshTrackOutputs();
}

void scMixerPro::free(ofxSCServer* server) {
    for(Track* tr : allStrips()) if(tr->liveServer == server) destroyRuntime(*tr, true);
    destroyMaster(server, true);
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
    if(MasterRuntime& m = serverStates[server].master; m.out) m.out->set("out", bus);
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
    addAction(tr.levelKey, tr.level);
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
    for(auto* q : {&tr.comp, &tr.compAuto, &tr.rvb, &tr.hp, &tr.hp24, &tr.sat, &tr.satAuto, &tr.echo, &tr.echoBeats})
        addAction(q->getEscapedName(), *q);
    addAction(tr.echoFilter.getEscapedName(), tr.echoFilter);
    for(auto* q : {&tr.hpFreq, &tr.satDrive, &tr.satBias, &tr.satTone, &tr.satOutput, &tr.satMix,
                   &tr.echoTime, &tr.echoBeatVal, &tr.echoFeed, &tr.echoCutoff, &tr.echoResonance,
                   &tr.echoPingPong, &tr.echoMix})
        addAction(q->getEscapedName(), *q);
    if(!tr.isReturn) {
        for(int k = 0; k < MAX_RETURNS; k++) {
            addAction(tr.sendLevel[k].getEscapedName(), tr.sendLevel[k]);
            addAction(tr.sendPre[k].getEscapedName(), tr.sendPre[k]);
        }
    }
    for(auto* q : {&tr.compThreshold, &tr.compRatio, &tr.compKnee, &tr.compAttack, &tr.compRelease,
                   &tr.compMakeup, &tr.compRms, &tr.compHpf, &tr.compMix,
                   &tr.rvbMix, &tr.rvbDecay, &tr.rvbSize, &tr.rvbPredelay, &tr.rvbPosition, &tr.rvbSpread,
                   &tr.rvbLowpass, &tr.rvbHighDamp, &tr.rvbLowDamp, &tr.rvbModFreq, &tr.rvbSpin, &tr.rvbWander})
        addAction(q->getEscapedName(), *q);
}

void scMixerPro::unregisterTrackActions(Track& tr) {
    std::vector<std::string> keys = {
        tr.levelKey, tr.balance.getEscapedName(), tr.mute.getEscapedName(),
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
    for(auto* q : fxParams(tr)) keys.push_back(q->getEscapedName());
    for(const auto& key : keys) {
        // The parameter is about to disappear: take it out of the node even
        // if connected (as removing a track input does)
        auto it = nodeHandles.find(key);
        if(it != nodeHandles.end()) {
            removeParameter(it->second->getName());
            nodeHandles.erase(it);
        }
        publishedKeys.erase(std::remove(publishedKeys.begin(), publishedKeys.end(), key), publishedKeys.end());
        faderKeys.erase(std::remove(faderKeys.begin(), faderKeys.end(), key), faderKeys.end());
        publishActions.erase(key);
    }
    removeFaderBadge(tr);
    if(publishedKeys.empty() && publishedSeparatorAdded) { removeSeparator("Published"); publishedSeparatorAdded = false; }
    syncFaderSeparators();
    reorderNodeParameters();
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
    if(!publishedSeparatorAdded) { addSeparator("Published", ofColor(200)); publishedSeparatorAdded = true; }
    if(nodeHandles.count(key) == 0) {      // not already in the node as a fader
        nodeHandles[key] = action->second.add();
    }
    if(Track* tr = stripForLevelKey(key)) ensureFaderBadge(*tr);
    publishedKeys.push_back(key);
    reorderNodeParameters();
    return true;
}

bool scMixerPro::unpublishKey(const std::string& key) {
    if(!isPublished(key)) return false;
    const bool fader = std::find(faderKeys.begin(), faderKeys.end(), key) != faderKeys.end();
    if(!fader && hasNodeConnection(key)) return false;
    publishedKeys.erase(std::remove(publishedKeys.begin(), publishedKeys.end(), key), publishedKeys.end());
    if(!fader) removeNodeHandleIfUnused(key);
    if(publishedKeys.empty() && publishedSeparatorAdded) { removeSeparator("Published"); publishedSeparatorAdded = false; }
    reorderNodeParameters();
    return true;
}

void scMixerPro::removeNodeHandleIfUnused(const std::string& key) {
    auto it = nodeHandles.find(key);
    if(it == nodeHandles.end()) return;
    if(isPublished(key)) return;
    if(std::find(faderKeys.begin(), faderKeys.end(), key) != faderKeys.end()) return;
    if(hasNodeConnection(key)) return;
    removeParameter(it->second->getName());
    nodeHandles.erase(it);
    if(Track* tr = stripForLevelKey(key)) removeFaderBadge(*tr);
}

scMixerPro::Track* scMixerPro::stripForLevelKey(const std::string& key) {
    for(Track* tr : allStrips()) if(tr->levelKey == key) return tr;
    return nullptr;
}

void scMixerPro::ensureFaderBadge(Track& tr) {
    if(getParameterGroup().contains(tr.faderBadge.getName())) return;
    Track* tp = &tr;
    addCustomRegion(tr.faderBadge, [tp]() {
        const float zoom = ofxOceanodeShared::getZoomLevel();
        const float rowH = ImGui::GetFrameHeight();
        const float side = std::max(6.0f * zoom, std::min(10.0f * zoom, rowH - 4.0f * zoom));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(side, rowH));
        const float y = p.y + (rowH - side) * 0.5f;
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(p.x, y), ImVec2(p.x + side, y + side), toU32(tp->color), 1.5f * zoom);
        ImGui::SameLine(0.0f, 3.0f * zoom);
    });
}

void scMixerPro::removeFaderBadge(Track& tr) {
    if(getParameterGroup().contains(tr.faderBadge.getName()))
        removeParameter(tr.faderBadge.getName());
}

void scMixerPro::syncFaderSeparators() {
    bool hasTrackFader = false;
    bool hasSubmasterFader = false;
    for(const auto& key : faderKeys) {
        if(Track* tr = stripForLevelKey(key)) {
            if(tr->isBus) hasSubmasterFader = true;
            else hasTrackFader = true;
        }
    }

    if(hasTrackFader && !faderSeparatorAdded) {
        addSeparator("Faders", ofColor(200));
        faderSeparatorAdded = true;
    } else if(!hasTrackFader && faderSeparatorAdded) {
        removeSeparator("Faders");
        faderSeparatorAdded = false;
    }
    if(hasSubmasterFader && !submasterFaderSeparatorAdded) {
        addSeparator("Submasters", ofColor(175, 190, 215));
        submasterFaderSeparatorAdded = true;
    } else if(!hasSubmasterFader && submasterFaderSeparatorAdded) {
        removeSeparator("Submasters");
        submasterFaderSeparatorAdded = false;
    }
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
            const std::string& key = tr->levelKey;
            if(std::find(faderKeys.begin(), faderKeys.end(), key) != faderKeys.end()) continue;
            if(nodeHandles.count(key) == 0) {
                nodeHandles[key] = publishActions[key].add();
            }
            ensureFaderBadge(*tr);
            faderKeys.push_back(key);
        }
    } else {
        const auto keys = faderKeys;
        faderKeys.clear();
        for(const auto& key : keys) removeNodeHandleIfUnused(key);
        // Connected faders stay, still counted as faders
        for(const auto& key : keys)
            if(nodeHandles.count(key) && !isPublished(key)) faderKeys.push_back(key);
    }
    syncFaderSeparators();
    reorderNodeParameters();
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
    processFxTimers();
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
    if(pendingReturnCount >= 0) {
        const int count = pendingReturnCount;
        pendingReturnCount = -1;
        setReturnCount(count);
    }
    if(pendingTrackLabelsUpdate) {
        pendingTrackLabelsUpdate = false;
        syncTrackInputNames();
        syncFaderNames();
    }
    if(pendingFaderUpdate) {
        pendingFaderUpdate = false;
        updateFaders();
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
                const int g = currentChannels + 2;
                tr->gr = (tr->fx[FX_COMP].running && g < (int)v.size() && std::isfinite(v[g])) ? ofClamp(v[g], 0.0f, 60.0f) : 0.0f;
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
    // PolyMixer's master meter); a server with the master section on is
    // metered after it (MasterOut)
    masterVU.assign(currentChannels, 0.0f);
    const auto& m = masterLevel.get();
    for(auto& [server, state] : serverStates) {
        MasterRuntime& mr = state.master;
        if(!(mr.active && mr.vuBus)) continue;
        const auto& v = mr.vuBus->readValues;
        mr.vu.assign(currentChannels, 0.0f);
        for(int c = 0; c < currentChannels && c < (int)v.size(); c++) mr.vu[c] = std::isfinite(v[c]) ? v[c] : 0.0f;
        mr.vuBus->requestValues();
    }
    auto meteredAfterMaster = [this](const Track& tr) {
        auto it = serverStates.find(tr.liveServer);
        return it != serverStates.end() && it->second.master.active;
    };
    // (tracks into a running bus are counted through the bus)
    for(Track* tr : allStrips()) {
        if(!tr->synth || isSilenced(*tr) || routedToBus(*tr) || meteredAfterMaster(*tr)) continue;
        for(int c = 0; c < currentChannels && c < (int)tr->vu.size(); c++) masterVU[c] += tr->vu[c];
    }
    for(int c = 0; c < currentChannels; c++) {
        const float level = m.empty() ? 1.0f : m[std::min(c, (int)m.size() - 1)];
        masterVU[c] = masterVU[c] * level;
    }
    for(auto& [server, state] : serverStates)
        if(state.master.active)
            for(int c = 0; c < currentChannels && c < (int)state.master.vu.size(); c++) masterVU[c] += state.master.vu[c];
    for(float& v : masterVU) v = ofClamp(v, 0.0f, 2.0f);
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
    t["comp"] = {
        {"on", tr.comp.get()}, {"threshold", tr.compThreshold.get()}, {"ratio", tr.compRatio.get()},
        {"knee", tr.compKnee.get()}, {"attack", tr.compAttack.get()}, {"release", tr.compRelease.get()},
        {"makeup", tr.compMakeup.get()}, {"auto", tr.compAuto.get()}, {"rms", tr.compRms.get()},
        {"hpf", tr.compHpf.get()}, {"mix", tr.compMix.get()}
    };
    t["rvb"] = {
        {"on", tr.rvb.get()}, {"mix", tr.rvbMix.get()}, {"decay", tr.rvbDecay.get()}, {"size", tr.rvbSize.get()},
        {"predelay", tr.rvbPredelay.get()}, {"late", tr.rvbPosition.get()}, {"spread", tr.rvbSpread.get()},
        {"lowpass", tr.rvbLowpass.get()}, {"hidamp", tr.rvbHighDamp.get()}, {"lodamp", tr.rvbLowDamp.get()},
        {"modrate", tr.rvbModFreq.get()}, {"spin", tr.rvbSpin.get()}, {"wander", tr.rvbWander.get()}
    };
    t["hp"] = {{"on", tr.hp.get()}, {"freq", tr.hpFreq.get()}, {"24dB", tr.hp24.get()}};
    t["sat"] = {
        {"on", tr.sat.get()}, {"drive", tr.satDrive.get()}, {"warmth", tr.satBias.get()}, {"tone", tr.satTone.get()},
        {"output", tr.satOutput.get()}, {"auto", tr.satAuto.get()}, {"mix", tr.satMix.get()}
    };
    t["echo"] = {
        {"on", tr.echo.get()}, {"delay", tr.echoTime.get()}, {"beats", tr.echoBeats.get()}, {"delayBeats", tr.echoBeatVal.get()},
        {"feed", tr.echoFeed.get()}, {"cutoff", tr.echoCutoff.get()}, {"resonance", tr.echoResonance.get()},
        {"filter", tr.echoFilter.get()}, {"pingpong", tr.echoPingPong.get()}, {"mix", tr.echoMix.get()}
    };
    if(!tr.isReturn) {
        t["sends"] = ofJson::array();
        for(int k = 0; k < MAX_RETURNS; k++) t["sends"].push_back({tr.sendLevel[k].get(), tr.sendPre[k].get()});
    }
    return t;
}

void scMixerPro::presetSave(ofJson& json) {
    ofJson state;
    state["inputNamesFollowTracks"] = true;
    state["faderNamesFollowTracks"] = true;
    state["numTracks"] = (int)tracks.size();
    state["numChannels"] = currentChannels;
    state["showFaders"] = showFaders.get();
    state["published"] = publishedKeys;
    state["tracks"] = ofJson::array();
    for(auto& tr : tracks) state["tracks"].push_back(saveStrip(*tr));
    state["numBuses"] = (int)buses.size();
    state["buses"] = ofJson::array();
    for(auto& bus : buses) state["buses"].push_back(saveStrip(*bus));
    state["numReturns"] = (int)returns.size();
    state["returns"] = ofJson::array();
    for(auto& r : returns) state["returns"].push_back(saveStrip(*r));
    ofJson master;
    master["hp"] = {{"on", mHp.get()}, {"freq", mHpFreq.get()}, {"24dB", mHp24.get()}};
    master["eq"] = {{"on", mEq.get()}, {"bands", ofJson::array()}};
    for(int b = 0; b < scEQEditor::NUM_BANDS; b++)
        master["eq"]["bands"].push_back({mEqFreq[b].get(), mEqGain[b].get(), mEqShape[b].get()});
    master["max"] = {{"on", mMax.get()}, {"drive", mMaxDrive.get()}, {"ceiling", mMaxCeiling.get()},
                     {"character", mMaxCharacter.get()}, {"isp", mMaxIsp.get()}};
    master["lim"] = {{"on", mLim.get()}, {"ceiling", mLimCeiling.get()}};
    state["master"] = master;
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
    // Comp / Rvb: values first, then the switches (a missing block: off,
    // defaults -- a preset from before the inserts existed)
    const ofJson none = ofJson::object();
    const ofJson& c = (j.contains("comp") && j["comp"].is_object()) ? j["comp"] : none;
    const ofJson& r = (j.contains("rvb") && j["rvb"].is_object()) ? j["rvb"] : none;
    auto num = [](const ofJson& o, const char* k, ofParameter<float>& p, float def) {
        p.set(ofClamp(o.value(k, def), p.getMin(), p.getMax()));
    };
    num(c, "threshold", tr.compThreshold, -18.0f); num(c, "ratio", tr.compRatio, 3.0f);
    num(c, "knee", tr.compKnee, 6.0f);            num(c, "attack", tr.compAttack, 10.0f);
    num(c, "release", tr.compRelease, 150.0f);    num(c, "makeup", tr.compMakeup, 0.0f);
    num(c, "rms", tr.compRms, 0.0f);              num(c, "hpf", tr.compHpf, 20.0f);
    num(c, "mix", tr.compMix, 1.0f);
    tr.compAuto.set(c.value("auto", false));
    num(r, "mix", tr.rvbMix, 0.25f);         num(r, "decay", tr.rvbDecay, 2.5f);
    num(r, "size", tr.rvbSize, 30.0f);       num(r, "predelay", tr.rvbPredelay, 20.0f);
    num(r, "late", tr.rvbPosition, 0.8f);    num(r, "spread", tr.rvbSpread, 0.15f);
    num(r, "lowpass", tr.rvbLowpass, 10000.0f); num(r, "hidamp", tr.rvbHighDamp, 6.0f);
    num(r, "lodamp", tr.rvbLowDamp, 0.0f);   num(r, "modrate", tr.rvbModFreq, 0.2f);
    num(r, "spin", tr.rvbSpin, 1.0f);        num(r, "wander", tr.rvbWander, 0.25f);
    if(!(j.contains("rvb") && j["rvb"].is_object())) tr.rvbMix.set(tr.isReturn ? 1.0f : 0.25f);
    const ofJson& hj = (j.contains("hp") && j["hp"].is_object()) ? j["hp"] : none;
    const ofJson& sj = (j.contains("sat") && j["sat"].is_object()) ? j["sat"] : none;
    const ofJson& ej = (j.contains("echo") && j["echo"].is_object()) ? j["echo"] : none;
    num(hj, "freq", tr.hpFreq, 80.0f);
    tr.hp24.set(hj.value("24dB", false));
    num(sj, "drive", tr.satDrive, 6.0f);   num(sj, "warmth", tr.satBias, 0.0f);
    num(sj, "tone", tr.satTone, 20000.0f); num(sj, "output", tr.satOutput, 0.0f);
    num(sj, "mix", tr.satMix, 1.0f);
    tr.satAuto.set(sj.value("auto", true));
    num(ej, "delay", tr.echoTime, 0.375f); num(ej, "delayBeats", tr.echoBeatVal, 0.75f);
    num(ej, "feed", tr.echoFeed, 0.45f);   num(ej, "cutoff", tr.echoCutoff, 96.0f);
    num(ej, "resonance", tr.echoResonance, 0.1f); num(ej, "pingpong", tr.echoPingPong, 0.0f);
    num(ej, "mix", tr.echoMix, tr.isReturn ? 1.0f : 0.35f);
    tr.echoBeats.set(ej.value("beats", true));
    tr.echoFilter.set(ofClamp(ej.value("filter", 0), 0, 3));
    for(int k = 0; k < MAX_RETURNS; k++) {
        float level = 0.0f; bool pre = false;
        if(j.contains("sends") && j["sends"].is_array() && k < (int)j["sends"].size()) {
            const ofJson& sk = j["sends"][k];
            if(sk.is_array() && sk.size() >= 2) { level = sk[0].get<float>(); pre = sk[1].get<bool>(); }
        }
        tr.sendLevel[k].set(ofClamp(level, 0.0f, 1.0f));
        tr.sendPre[k].set(pre);
    }
    tr.hp.set(hj.value("on", false));
    tr.comp.set(c.value("on", false));
    tr.sat.set(sj.value("on", false));
    tr.echo.set(ej.value("on", false));
    tr.rvb.set(r.value("on", false));
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

        // Returns and buses first: the tracks' sends and outputs refer to them
        setReturnCount(state.value("numReturns", 0));
        if(state.contains("returns") && state["returns"].is_array()) {
            const ofJson& list = state["returns"];
            for(size_t k = 0; k < returns.size() && k < list.size(); k++) loadStrip(*returns[k], list[k]);
        }
        setBusCount(state.value("numBuses", 0));
        if(state.contains("buses") && state["buses"].is_array()) {
            const ofJson& list = state["buses"];
            for(size_t k = 0; k < buses.size() && k < list.size(); k++) loadStrip(*buses[k], list[k]);
        }
        if(state.contains("tracks") && state["tracks"].is_array()) {
            const ofJson& list = state["tracks"];
            for(size_t t = 0; t < tracks.size() && t < list.size(); t++) loadStrip(*tracks[t], list[t]);
        }
        {
            const ofJson none = ofJson::object();
            const ofJson& ms = (state.contains("master") && state["master"].is_object()) ? state["master"] : none;
            auto obj = [&](const char* k) -> const ofJson& { return (ms.contains(k) && ms[k].is_object()) ? ms[k] : none; };
            auto num = [](const ofJson& o, const char* k, ofParameter<float>& p, float def) {
                p.set(ofClamp(o.value(k, def), p.getMin(), p.getMax()));
            };
            const ofJson& hp = obj("hp"); const ofJson& eq = obj("eq"); const ofJson& mx = obj("max"); const ofJson& lm = obj("lim");
            num(hp, "freq", mHpFreq, 30.0f);
            mHp24.set(hp.value("24dB", true));
            for(int b = 0; b < scEQEditor::NUM_BANDS; b++) {
                float f = kBandFreq[b], g = 0.0f, q = 1.0f;
                if(eq.contains("bands") && eq["bands"].is_array() && b < (int)eq["bands"].size()) {
                    const ofJson& band = eq["bands"][b];
                    if(band.is_array() && band.size() >= 3) { f = band[0].get<float>(); g = band[1].get<float>(); q = band[2].get<float>(); }
                }
                mEqFreq[b].set(f); mEqGain[b].set(g); mEqShape[b].set(q);
            }
            num(mx, "drive", mMaxDrive, 3.0f); num(mx, "ceiling", mMaxCeiling, -0.3f);
            num(mx, "character", mMaxCharacter, 4.0f);
            mMaxIsp.set(mx.value("isp", true));
            num(lm, "ceiling", mLimCeiling, -0.3f);
            mEqCurveDirty = true;
            mHp.set(hp.value("on", false));
            mEq.set(eq.value("on", false));
            mMax.set(mx.value("on", false));
            mLim.set(lm.value("on", false));
        }
        if(state.value("inputNamesFollowTracks", false)) syncTrackInputNames();
        else useLegacyInputNames();
        if(state.value("faderNamesFollowTracks", false)) syncFaderNames();
        else useLegacyFaderNames();
        const bool faders = state.value("showFaders", false);
        if(showFaders.get() != faders) showFaders.set(faders);
        // Connections are restored immediately after this hook, so fader
        // parameters must exist now rather than waiting for update().
        pendingFaderUpdate = false;
        updateFaders();
        if(state.contains("published") && state["published"].is_array())
            syncPublished(state["published"].get<std::vector<std::string>>());
    } catch(const std::exception& e) {
        ofLogError("scMixerPro") << "Could not load the mixer state: " << e.what();
    }
}

void scMixerPro::presetRecallAfterSettingParameters(ofJson&) {
    // Legacy projects restored their connections against "In N" above. Now
    // that those objects are wired, adopting the track labels is connection-safe.
    syncTrackInputNames();
    syncFaderNames();
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
    ImGui::SetNextWindowSize(ImVec2(960.0f * zoom, 660.0f * zoom), ImGuiCond_FirstUseEver);
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

    const bool eqOpen = eqTarget() != nullptr || selectedEqTrack == MASTER_PANEL;
    const float remainingH = std::max(1.0f, ImGui::GetContentRegionAvail().y);
    const float panelGap = eqOpen ? 6.0f * zoom : 0.0f;
    // Keep the editor inside the window and let the strip viewport become
    // genuinely short. Its contents are clipped below instead of overlapping
    // the editor when the mixer window is reduced vertically.
    const float eqH = eqOpen
        ? std::min(190.0f * zoom, std::max(80.0f * zoom, remainingH - panelGap - 40.0f * zoom))
        : 0.0f;
    const float bodyH = std::max(1.0f, remainingH - eqH - panelGap);

    const float masterW = 130.0f * zoom;
    drawMasterStrip(masterW, bodyH);
    ImGui::SameLine(0, 8.0f * zoom);
    ImGui::BeginChild("##mixerProStrips", ImVec2(0, bodyH), false, ImGuiWindowFlags_HorizontalScrollbar);
    const float stripW = 92.0f * zoom;
    const float stripH = std::max(1.0f, ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ScrollbarSize);
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
    // Returns, after another gap
    for(size_t k = 0; k < returns.size(); k++) {
        ImGui::SameLine(0, k == 0 ? 16.0f * zoom : 5.0f * zoom);
        ImGui::PushID(1500 + (int)k);
        if(ImGui::IsRectVisible(ImVec2(stripW, stripH))) drawTrackStrip(*returns[k], stripW, stripH);
        else ImGui::Dummy(ImVec2(stripW, stripH));
        ImGui::PopID();
    }
    ImGui::EndChild();

    // The strips can have closed (or switched) the EQ panel this frame:
    // check the selection again rather than trusting eqOpen
    if(eqOpen && selectedEqTrack == MASTER_PANEL)
        drawMasterPanel(ImGui::GetContentRegionAvail().x, eqH);
    else if(eqOpen && eqTarget())
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
    countField("Returns", editReturns, (int)returns.size(), 1, 1,
               [this](int v) { pendingReturnCount = ofClamp(v, 0, MAX_RETURNS); });
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Aux returns (max %d): every strip gets a send knob per return", MAX_RETURNS);
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

bool scMixerPro::drawFader(const char* id, float& gain, ImVec2 pos, ImVec2 size, float maxGain) {
    const float zoom = ofxOceanodeShared::getZoomLevel();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float topDb = ampToDb(std::max(1.0f, maxGain));
    auto faderToPos = [topDb](float value) {
        if(value <= 1e-6f) return 0.0f;
        return ofClamp((ampToDb(value) + 60.0f) / (60.0f + topDb), 0.0f, 1.0f);
    };
    auto posToFader = [topDb, maxGain](float value) {
        if(value <= 0.002f) return 0.0f;
        return ofClamp(std::pow(10.0f, (value * (60.0f + topDb) - 60.0f) / 20.0f), 0.0f, maxGain);
    };
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton(id, size);
    bool changed = false;
    float p = faderToPos(gain);
    if(ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 0.0f)) {
        const float speed = ImGui::GetIO().KeyShift ? 0.1f : 1.0f;
        p = ofClamp(p - ImGui::GetIO().MouseDelta.y * speed / std::max(1.0f, size.y), 0.0f, 1.0f);
        gain = posToFader(p);
        changed = true;
    }
    if(ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { gain = 1.0f; changed = true; }
    if(ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
        p = ofClamp(p + ImGui::GetIO().MouseWheel * 0.01f, 0.0f, 1.0f);
        gain = posToFader(p);
        changed = true;
    }
    p = faderToPos(gain);
    const float cx = pos.x + size.x * 0.5f;
    dl->AddRectFilled(ImVec2(cx - 2.0f * zoom, pos.y), ImVec2(cx + 2.0f * zoom, pos.y + size.y), IM_COL32(8, 8, 10, 255), 2.0f);
    const float y0 = pos.y + size.y * (1.0f - faderToPos(1.0f));
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
    // Master primitives are drawn directly in the parent window, so they need
    // their own clip rectangle when the bottom editor reduces this area.
    ImGui::PushClipRect(start, end, true);
    dl->AddRectFilled(start, end, IM_COL32(24, 24, 28, 255), 4.0f * zoom);
    dl->AddRect(start, end, IM_COL32(70, 70, 78, 220), 4.0f * zoom);
    dl->AddRectFilled(start, ImVec2(end.x, start.y + 20.0f * zoom), IM_COL32(180, 180, 188, 255), 4.0f * zoom);
    dl->AddText(ImVec2(start.x + 6.0f * zoom, start.y + 3.0f * zoom), IM_COL32(20, 20, 24, 255), "MASTER");

    // Per server: its audible tracks, summed, as one small meter
    float y = start.y + 26.0f * zoom;
    for(int i = 0; i < (int)servers.size(); i++) {
        std::vector<float> sum(currentChannels, 0.0f);
        auto st = serverStates.find(serverAt(i));
        if(st != serverStates.end() && st->second.master.active) {
            sum = st->second.master.vu;   // after the master section
        } else {
            for(Track* tr : allStrips()) {
                if(tr->server != i || !tr->synth || isSilenced(*tr) || routedToBus(*tr)) continue;
                for(int c = 0; c < currentChannels && c < (int)tr->vu.size(); c++) sum[c] += tr->vu[c];
            }
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

    // Master section: HP EQ / MAX LIM (click: on / off, right-click: edit / publish)
    {
        const float bw = (w - 16.0f * zoom - 3.0f * zoom) * 0.5f, bh = 18.0f * zoom;
        bool anyRunning = false;
        for(auto& [server, state] : serverStates) for(auto& f : state.master.st) anyRunning = anyRunning || f.running;
        auto masterToggle = [&](const char* label, ofParameter<bool>& p, ImVec4 on, int tab, const char* what) {
            const bool v = p.get();
            pushButtonColours(v ? on : ImVec4(0.22f, 0.22f, 0.25f, 1.0f));
            if(ImGui::Button(label, ImVec2(bw, bh))) p.set(!v);
            ImGui::PopStyleColor(3);
            const std::string key = p.getEscapedName();
            markPublished(key);
            if(ImGui::IsItemHovered())
                ImGui::SetTooltip("Master %s%s\nClick: on / off, right-click: edit / publish", what, v ? "" : ": off");
            if(ImGui::BeginPopupContextItem(("##masterMenu_" + key).c_str())) {
                if(ImGui::MenuItem((std::string("Edit master ") + what + "...").c_str())) {
                    selectedEqTrack = MASTER_PANEL;
                    masterTab = tab;
                }
                ImGui::Separator();
                drawPublishItems(key, nullptr, false);
                ImGui::EndPopup();
            }
        };
        y += 4.0f * zoom;
        ImGui::SetCursorScreenPos(ImVec2(start.x + 8.0f * zoom, y));
        masterToggle("HP##mhp", mHp, ImVec4(0.32f, 0.5f, 0.62f, 1.0f), MST_HP, "high-pass");
        ImGui::SameLine(0, 3.0f * zoom);
        masterToggle("EQ##meq", mEq, ImVec4(0.2f, 0.5f, 0.75f, 1.0f), MST_EQ, "EQ");
        y += bh + 3.0f * zoom;
        ImGui::SetCursorScreenPos(ImVec2(start.x + 8.0f * zoom, y));
        masterToggle("MAX##mmax", mMax, ImVec4(0.75f, 0.35f, 0.2f, 1.0f), MST_MAX, "maximizer");
        ImGui::SameLine(0, 3.0f * zoom);
        masterToggle("LIM##mlim", mLim, ImVec4(0.75f, 0.2f, 0.25f, 1.0f), MST_LIM, "limiter");
        y += bh + 3.0f * zoom;
        ImGui::SetCursorScreenPos(ImVec2(start.x + 8.0f * zoom, y));
        pushButtonColours(selectedEqTrack == MASTER_PANEL ? ImVec4(0.34f, 0.34f, 0.4f, 1.0f) : ImVec4(0.18f, 0.18f, 0.21f, 1.0f));
        if(ImGui::Button("MASTER FX##mpanel", ImVec2(bw * 2.0f + 3.0f * zoom, bh)))
            selectedEqTrack = selectedEqTrack == MASTER_PANEL ? -1 : MASTER_PANEL;
        ImGui::PopStyleColor(3);
        if(ImGui::IsItemHovered())
            ImGui::SetTooltip(anyRunning ? "Master section running (metered after it)" : "Master section off: no master synth");
        y += bh + 2.0f * zoom;
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
    ImGui::PopClipRect();
}

void scMixerPro::drawTrackStrip(Track& tr, float w, float h) {
    const float zoom = ofxOceanodeShared::getZoomLevel();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 end(start.x + w, start.y + h);
    // Several controls (knobs, meters, peak text and curves) use raw draw-list
    // primitives. Clip those to the strip, intersected with the child viewport,
    // so they cannot paint over an open bottom editor.
    ImGui::PushClipRect(start, end, true);
    const float pad = 4.0f * zoom;
    const float innerW = w - 2.0f * pad;
    const bool silenced = isSilenced(tr);

    const ImU32 bg = tr.isReturn ? IM_COL32(22, 44, 46, 255) : (tr.isBus ? IM_COL32(36, 30, 62, 255) : IM_COL32(24, 24, 28, 255));
    const ImU32 edge = tr.isReturn ? IM_COL32(80, 170, 165, 230) : (tr.isBus ? IM_COL32(125, 105, 200, 230) : IM_COL32(70, 70, 78, 220));
    dl->AddRectFilled(start, end, bg, 4.0f * zoom);
    dl->AddRect(start, end, selectedEqTrack == eqId(tr) ? IM_COL32(110, 190, 255, 220) : edge, 4.0f * zoom);

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
    if(ImGui::InputText("##name", nameBuffer, sizeof(nameBuffer))) {
        tr.name = nameBuffer;
        // Both inlet and fader labels are node parameters. Defer renaming them
        // until update(), outside either GUI's parameter traversal.
        pendingTrackLabelsUpdate = true;
    }
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
    if(tr.isReturn) {
        int fed = 0;
        for(Track* t : allStrips()) if(!t->isReturn && tr.index < MAX_RETURNS && t->sendLevel[tr.index].get() > 0.0f) fed++;
        const std::string text = "AUX RETURN  " + ofToString(fed) + (fed == 1 ? " send" : " sends");
        dl->AddText(ImVec2(start.x + pad, y + 2.0f * zoom), IM_COL32(150, 210, 205, 255), text.c_str());
    } else if(tr.isBus) {
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
    if(ImGui::IsItemClicked()) {
        const bool closing = selectedEqTrack == eqId(tr) && panelTab == 0;
        selectedEqTrack = closing ? -1 : eqId(tr);
        panelTab = 0;
    }
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Click: edit this track's EQ (Comp / Reverb: tabs in the panel)");
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
    // Inserts, two per row in chain order: HP EQ / DC COMP / SAT ECHO / RVB FX.
    // EQ / DC: click switches. The others: click switches, right-click edits
    // (the panel's tab) or publishes; a dim colour: off, still ringing out.
    auto fxToggle = [&](const char* label, ofParameter<bool>& p, ImVec4 on, int tab, const char* what, bool running) {
        const bool v = p.get();
        const bool tail = !v && running;
        pushButtonColours(v ? on : (tail ? ImVec4(on.x * 0.5f, on.y * 0.5f, on.z * 0.5f, 1.0f) : ImVec4(0.22f, 0.22f, 0.25f, 1.0f)));
        if(ImGui::Button(label, ImVec2(halfW, btnH))) p.set(!v);
        ImGui::PopStyleColor(3);
        const std::string key = p.getEscapedName();
        markPublished(key);
        if(ImGui::IsItemHovered()) {
            if(tab == 2 && v) ImGui::SetTooltip("Compressor: GR %.1f dB\nClick: on / off, right-click: edit / publish", tr.gr);
            else ImGui::SetTooltip("%s%s\nClick: on / off, right-click: edit / publish",
                                   what, tail ? " (ringing out)" : (v ? "" : ": off, costs nothing"));
        }
        if(ImGui::BeginPopupContextItem(("##fxMenu_" + key).c_str())) {
            if(ImGui::MenuItem((std::string("Edit ") + what + "...").c_str())) {
                selectedEqTrack = eqId(tr);
                panelTab = tab;
            }
            ImGui::Separator();
            drawPublishItems(key, nullptr, false);
            ImGui::EndPopup();
        }
    };
    ImGui::SetCursorScreenPos(ImVec2(start.x + pad, y));
    fxToggle("HP##hp", tr.hp, ImVec4(0.32f, 0.5f, 0.62f, 1.0f), 1, "High-pass", tr.fx[FX_HP].running);
    ImGui::SameLine(0, 3.0f * zoom);
    toggle("EQ", tr.eq, ImVec4(0.2f, 0.5f, 0.75f, 1.0f));
    y += btnH + 3.0f * zoom;
    ImGui::SetCursorScreenPos(ImVec2(start.x + pad, y));
    toggle("DC", tr.dc, ImVec4(0.45f, 0.35f, 0.7f, 1.0f));
    ImGui::SameLine(0, 3.0f * zoom);
    fxToggle("COMP##comp", tr.comp, ImVec4(0.78f, 0.45f, 0.14f, 1.0f), 2, "Compressor", tr.fx[FX_COMP].running);
    if(tr.fx[FX_COMP].running && tr.gr > 0.05f) {
        // gain reduction: a bar growing from the right edge (0..24 dB)
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const float frac = ofClamp(tr.gr / 24.0f, 0.0f, 1.0f);
        dl->AddRectFilled(ImVec2(b.x - (b.x - a.x) * frac, b.y - 3.0f * zoom), b, IM_COL32(255, 200, 80, 255));
    }
    y += btnH + 3.0f * zoom;
    ImGui::SetCursorScreenPos(ImVec2(start.x + pad, y));
    fxToggle("SAT##sat", tr.sat, ImVec4(0.72f, 0.3f, 0.26f, 1.0f), 3, "Saturation", tr.fx[FX_SAT].running);
    ImGui::SameLine(0, 3.0f * zoom);
    fxToggle("ECHO##echo", tr.echo, ImVec4(0.3f, 0.48f, 0.72f, 1.0f), 4, "Echo", tr.fx[FX_ECHO].running);
    y += btnH + 3.0f * zoom;
    ImGui::SetCursorScreenPos(ImVec2(start.x + pad, y));
    fxToggle("RVB##rvb", tr.rvb, ImVec4(0.18f, 0.55f, 0.52f, 1.0f), 5, "Reverb", tr.fx[FX_RVB].running);
    ImGui::SameLine(0, 3.0f * zoom);
    pushButtonColours(selectedEqTrack == eqId(tr) ? ImVec4(0.34f, 0.34f, 0.4f, 1.0f) : ImVec4(0.18f, 0.18f, 0.21f, 1.0f));
    if(ImGui::Button("FX##fxPanel", ImVec2(halfW, btnH))) {
        const bool closing = selectedEqTrack == eqId(tr);
        selectedEqTrack = closing ? -1 : eqId(tr);
    }
    ImGui::PopStyleColor(3);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Open / close this strip's panel (EQ, HP, Comp, Sat, Echo, Reverb)");
    y += btnH + 3.0f * zoom;

    // Aux sends: one knob per return (ring: pre-fader)
    if(!tr.isReturn && !returns.empty()) {
        const int R = (int)returns.size();
        const float cellW = innerW / R;
        const float knobR = std::min(9.0f * zoom, cellW * 0.5f - 1.0f * zoom);
        for(int k = 0; k < R; k++) {
            Track& r = *returns[k];
            const bool reachable = r.server == tr.server;
            ImGui::SetCursorScreenPos(ImVec2(start.x + pad + k * cellW + (cellW - 2.0f * knobR) * 0.5f, y));
            ImGui::PushID(100 + k);
            float v = tr.sendLevel[k].get();
            if(drawSendKnob("##send", v, knobR, tr.sendPre[k].get(), toU32(r.color), !reachable)) tr.sendLevel[k].set(v);
            const std::string key = tr.sendLevel[k].getEscapedName();
            markPublished(key);
            if(ImGui::IsItemHovered() && !ImGui::IsItemActive())
                ImGui::SetTooltip("Send to %s: %s%s%s\nDrag, double-click: off, right-click: pre / post, publish",
                                  r.name.c_str(), v <= 0.0f ? "off" : (ofToString(ampToDb(v), 1) + " dB").c_str(),
                                  tr.sendPre[k].get() ? " (pre-fader)" : " (post-fader)",
                                  reachable ? "" : "\nOn another server: not sent");
            if(ImGui::BeginPopupContextItem("##sendMenu")) {
                ImGui::TextDisabled("Send to %s", r.name.c_str());
                bool pre = tr.sendPre[k].get();
                if(ImGui::Checkbox("Pre-fader", &pre)) tr.sendPre[k].set(pre);
                ImGui::Separator();
                drawPublishItems(key, "Level (0..1)", false);
                ImGui::Separator();
                drawPublishItems(tr.sendPre[k].getEscapedName(), "Pre-fader (0 / 1)", false);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        y += 2.0f * knobR + 4.0f * zoom;
    }

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
    if(!tr.isReturn) {   // returns are solo-safe
        ImGui::SameLine(0, 3.0f * zoom);
        toggle("S", tr.solo, ImVec4(0.82f, 0.72f, 0.18f, 1.0f));
    }
    y += btnH + 6.0f * zoom;

    // Fader + meters
    const float bottom = end.y - 20.0f * zoom;
    const float areaH = std::max(30.0f * zoom, bottom - y);
    const float faderW = 22.0f * zoom;
    float g = tr.level.get();
    if(drawFader("##fader", g, ImVec2(start.x + pad, y), ImVec2(faderW, areaH), kTrackMaxGain)) tr.level.set(g);
    drawPublishPopup(tr.levelKey);
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
    ImGui::PopClipRect();
}

void scMixerPro::drawEqPanel(float w, float h) {
    Track* target = eqTarget();
    if(!target) return;
    const float zoom = ofxOceanodeShared::getZoomLevel();
    Track& tr = *target;
    ImGui::BeginChild("##mixerProEq", ImVec2(w, h), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextColored(ImVec4(tr.color.r / 255.0f, tr.color.g / 255.0f, tr.color.b / 255.0f, 1.0f), "%s", tr.name.c_str());
    // Tabs (green text: that insert is on)
    const char* tabNames[] = {"EQ##tab", "HP##tab", "Comp##tab", "Sat##tab", "Echo##tab", "Reverb##tab"};
    const bool tabOn[] = {tr.eq.get() || tr.dc.get(), tr.hp.get(), tr.comp.get(), tr.sat.get(), tr.echo.get(), tr.rvb.get()};
    for(int i = 0; i < 6; i++) {
        ImGui::SameLine(0, i == 0 ? 12.0f * zoom : 2.0f * zoom);
        pushButtonColours(panelTab == i ? ImVec4(0.32f, 0.32f, 0.38f, 1.0f) : ImVec4(0.15f, 0.15f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, tabOn[i] ? ImVec4(0.55f, 0.92f, 0.6f, 1.0f) : ImVec4(0.75f, 0.75f, 0.78f, 1.0f));
        if(ImGui::SmallButton(tabNames[i])) panelTab = i;
        ImGui::PopStyleColor(4);
    }
    ImGui::SameLine(0, 14.0f * zoom);
    if(panelTab >= 1 && panelTab <= 5) {
        static const int kTabFx[6] = {-1, FX_HP, FX_COMP, FX_SAT, FX_ECHO, FX_RVB};
        static const char* kTabLabel[6] = {"", "High-pass", "Compressor", "Saturation", "Echo", "Reverb"};
        const int k = kTabFx[panelTab];
        ofParameter<bool>& sw = fxSwitch(tr, k);
        bool on = sw.get();
        if(ImGui::Checkbox(kTabLabel[panelTab], &on)) sw.set(on);
        drawPublishPopup(sw.getEscapedName());
        ImGui::SameLine(0, 12.0f * zoom);
        if(!on && tr.fx[k].running) ImGui::TextDisabled(k == FX_ECHO || k == FX_RVB ? "ringing out..." : "fading out...");
        else if(!on) ImGui::TextDisabled("off: costs nothing");
        else if(k == FX_COMP) ImGui::Text("GR %4.1f dB", tr.gr);
        else if(k == FX_RVB) ImGui::TextDisabled("SpaceMaster  -  Decay = RT60");
        else if(k == FX_ECHO) ImGui::TextDisabled("%.0f ms%s", echoDelaySec(tr) * 1000.0f, tr.echoBeats.get() ? " at the node tempo" : "");
        ImGui::SameLine(0, 12.0f * zoom);
        ImGui::TextDisabled("right-click a control: value / publish");
    } else {
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
    }
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 22.0f * zoom);
    if(ImGui::SmallButton("x")) selectedEqTrack = -1;

    if(panelTab == 1) { drawHpPanel(tr); ImGui::EndChild(); return; }
    if(panelTab == 2) { drawCompPanel(tr); ImGui::EndChild(); return; }
    if(panelTab == 3) { drawSatPanel(tr); ImGui::EndChild(); return; }
    if(panelTab == 4) { drawEchoPanel(tr); ImGui::EndChild(); return; }
    if(panelTab == 5) { drawRvbPanel(tr); ImGui::EndChild(); return; }

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


// Compressor tab: transfer curve + GR meter, then the controls
void scMixerPro::drawCompPanel(Track& tr) {
    const float zoom = ofxOceanodeShared::getZoomLevel();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float side = std::max(60.0f * zoom, std::min(avail.y - 6.0f * zoom, 220.0f * zoom));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + side, p0.y + side);
    dl->AddRectFilled(p0, p1, IM_COL32(20, 20, 24, 255), 3.0f * zoom);
    // -60..0 dB on both axes, grid every 12 dB
    auto px = [&](float db) { return p0.x + (db + 60.0f) / 60.0f * side; };
    auto py = [&](float db) { return p1.y - (ofClamp(db, -60.0f, 0.0f) + 60.0f) / 60.0f * side; };
    for(int g = -48; g < 0; g += 12) {
        dl->AddLine(ImVec2(px((float)g), p0.y), ImVec2(px((float)g), p1.y), IM_COL32(45, 45, 52, 255));
        dl->AddLine(ImVec2(p0.x, py((float)g)), ImVec2(p1.x, py((float)g)), IM_COL32(45, 45, 52, 255));
    }
    dl->AddLine(ImVec2(p0.x, p1.y), ImVec2(p1.x, p0.y), IM_COL32(70, 70, 80, 255));
    const float thr = tr.compThreshold.get(), knee = std::max(0.01f, tr.compKnee.get());
    const float slope = 1.0f - 1.0f / std::max(1.0f, tr.compRatio.get());
    auto grAt = [&](float x) {   // as mixerProComp
        const float over = x - thr;
        const float xk = ofClamp(over + knee * 0.5f, 0.0f, knee);
        return std::min(40.0f, slope * (xk * xk / (2.0f * knee) + std::max(0.0f, over - knee * 0.5f)));
    };
    const float makeup = tr.compMakeup.get() + (tr.compAuto.get() ? 0.5f * grAt(0.0f) : 0.0f);
    ImVec2 prev;
    for(int i = 0; i <= 120; i++) {
        const float x = -60.0f + i * 0.5f;
        const ImVec2 pt(px(x), py(x - grAt(x) + makeup));
        if(i > 0) dl->AddLine(prev, pt, IM_COL32(255, 190, 90, 255), 2.0f * zoom);
        prev = pt;
    }
    dl->AddLine(ImVec2(px(thr), p0.y), ImVec2(px(thr), p1.y), IM_COL32(255, 190, 90, 70));
    dl->AddText(ImVec2(p0.x + 4.0f * zoom, p0.y + 2.0f * zoom), IM_COL32(150, 150, 160, 255), "out");
    dl->AddText(ImVec2(p1.x - 18.0f * zoom, p1.y - 16.0f * zoom), IM_COL32(150, 150, 160, 255), "in");
    ImGui::Dummy(ImVec2(side, side));
    ImGui::SameLine(0, 6.0f * zoom);

    // GR meter (0..24 dB, from the top)
    const ImVec2 m0 = ImGui::GetCursorScreenPos();
    const float mw = 10.0f * zoom;
    dl->AddRectFilled(m0, ImVec2(m0.x + mw, m0.y + side), IM_COL32(20, 20, 24, 255), 2.0f * zoom);
    if(tr.fx[FX_COMP].running)
        dl->AddRectFilled(m0, ImVec2(m0.x + mw, m0.y + side * ofClamp(tr.gr / 24.0f, 0.0f, 1.0f)), IM_COL32(255, 200, 80, 255), 2.0f * zoom);
    for(int g = 6; g < 24; g += 6) {
        const float yy = m0.y + side * g / 24.0f;
        dl->AddLine(ImVec2(m0.x, yy), ImVec2(m0.x + mw, yy), IM_COL32(60, 60, 68, 255));
    }
    ImGui::Dummy(ImVec2(mw, side));
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Gain reduction %.1f dB (scale 0..24 dB)", tr.gr);
    ImGui::SameLine(0, 14.0f * zoom);

    const float fieldW = 150.0f * zoom;
    auto slider = [&](const char* name, ofParameter<float>& p, const char* format, ImGuiSliderFlags flags = 0) {
        float v = p.get();
        ImGui::SetNextItemWidth(fieldW);
        if(ImGui::SliderFloat(name, &v, p.getMin(), p.getMax(), format, flags)) p.set(v);
        drawPublishPopup(p.getEscapedName());
    };
    ImGui::BeginGroup();
    slider("Threshold", tr.compThreshold, "%.1f dB");
    slider("Ratio", tr.compRatio, "%.1f : 1", ImGuiSliderFlags_Logarithmic);
    slider("Knee", tr.compKnee, "%.1f dB");
    slider("Makeup", tr.compMakeup, "%.1f dB");
    bool a = tr.compAuto.get();
    if(ImGui::Checkbox("Auto makeup", &a)) tr.compAuto.set(a);
    drawPublishPopup(tr.compAuto.getEscapedName());
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Adds back half the reduction a 0 dBFS signal gets");
    ImGui::EndGroup();
    ImGui::SameLine(0, 18.0f * zoom);
    ImGui::BeginGroup();
    slider("Attack", tr.compAttack, "%.1f ms", ImGuiSliderFlags_Logarithmic);
    slider("Release", tr.compRelease, "%.0f ms", ImGuiSliderFlags_Logarithmic);
    slider("Peak/RMS", tr.compRms, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Detector: 0 peak (tracks, drums), 1 RMS (buses, glue)");
    slider("SC HPF", tr.compHpf, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("High-pass on the detector only: less pumping from the low end");
    slider("Mix", tr.compMix, "%.2f");
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Parallel compression: 1 fully compressed, 0 dry");
    ImGui::EndGroup();
}

// Reverb tab: the SpaceMaster controls
void scMixerPro::drawRvbPanel(Track& tr) {
    const float zoom = ofxOceanodeShared::getZoomLevel();
    const float fieldW = 150.0f * zoom;
    auto slider = [&](const char* name, ofParameter<float>& p, const char* format, ImGuiSliderFlags flags = 0, const char* tip = nullptr) {
        float v = p.get();
        ImGui::SetNextItemWidth(fieldW);
        if(ImGui::SliderFloat(name, &v, p.getMin(), p.getMax(), format, flags)) p.set(v);
        drawPublishPopup(p.getEscapedName());
        if(tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    ImGui::Dummy(ImVec2(0, 2.0f * zoom));
    ImGui::BeginGroup();
    slider("Mix", tr.rvbMix, "%.2f", 0, "Equal-power dry / wet");
    slider("Decay", tr.rvbDecay, "%.2f s", ImGuiSliderFlags_Logarithmic, "Reverb time (RT60)");
    slider("Size", tr.rvbSize, "%.1f", 0, "Room size: diffuser and tank delay lengths");
    slider("Predelay", tr.rvbPredelay, "%.0f ms");
    ImGui::EndGroup();
    ImGui::SameLine(0, 18.0f * zoom);
    ImGui::BeginGroup();
    slider("Late", tr.rvbPosition, "%.2f", 0, "Early reflections (0) to late tail (1)");
    slider("Spread", tr.rvbSpread, "%.2f", 0, "Left / right predelay offset");
    slider("Lowpass", tr.rvbLowpass, "%.0f Hz", ImGuiSliderFlags_Logarithmic, "Tone of the reverb input");
    slider("Hi Damp", tr.rvbHighDamp, "%.1f dB", 0, "High-shelf loss per pass in the tank: darker tail");
    ImGui::EndGroup();
    ImGui::SameLine(0, 18.0f * zoom);
    ImGui::BeginGroup();
    slider("Lo Damp", tr.rvbLowDamp, "%.1f dB", 0, "Low-shelf loss per pass in the tank: thinner tail");
    slider("Mod Rate", tr.rvbModFreq, "%.2f Hz", ImGuiSliderFlags_Logarithmic, "Tank delay modulation rate");
    slider("Spin", tr.rvbSpin, "%.2f", 0, "Sine modulation depth (ms / 5)");
    slider("Wander", tr.rvbWander, "%.2f", 0, "Random modulation depth (ms / 5)");
    ImGui::EndGroup();
}


// Aux send knob (0..1): the arc in the return's colour, a ring when pre-fader
bool scMixerPro::drawSendKnob(const char* id, float& value, float radius, bool pre, ImU32 colour, bool disabled) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(radius * 2.0f, radius * 2.0f));
    bool changed = false;
    if(ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 0.0f)) {
        const float speed = ImGui::GetIO().KeyShift ? 0.001f : 0.006f;
        value = ofClamp(value - ImGui::GetIO().MouseDelta.y * speed, 0.0f, 1.0f);
        changed = true;
    }
    if(ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { value = 0.0f; changed = true; }
    const ImVec2 c(p.x + radius, p.y + radius);
    const float a0 = (float)M_PI * 0.75f, a1 = (float)M_PI * 2.25f;
    dl->AddCircleFilled(c, radius, IM_COL32(36, 36, 42, 255), 20);
    dl->PathArcTo(c, radius - 1.5f, a0, a1, 20);
    dl->PathStroke(IM_COL32(64, 64, 72, 255), 0, 2.0f);
    if(value > 0.0f) {
        dl->PathArcTo(c, radius - 1.5f, a0, a0 + (a1 - a0) * value, 20);
        dl->PathStroke(disabled ? IM_COL32(110, 110, 115, 255) : colour, 0, 2.5f);
    }
    if(pre) dl->AddCircle(c, radius * 0.45f, IM_COL32(240, 210, 90, 255), 12, 1.5f);
    else dl->AddCircleFilled(c, radius * 0.22f, value > 0.0f ? colour : IM_COL32(80, 80, 88, 255), 8);
    return changed;
}

namespace {
// A float slider with the publish menu, for the panels
template<typename F>
void panelSlider(const char* name, ofParameter<float>& p, const char* format, ImGuiSliderFlags flags, float width,
                 const char* tip, F&& publish) {
    float v = p.get();
    ImGui::SetNextItemWidth(width);
    if(ImGui::SliderFloat(name, &v, p.getMin(), p.getMax(), format, flags)) p.set(v);
    publish(p.getEscapedName());
    if(tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
}
}

void scMixerPro::drawHpPanel(Track& tr) {
    const float zoom = ofxOceanodeShared::getZoomLevel();
    auto pub = [this](const std::string& k) { drawPublishPopup(k); };
    ImGui::Dummy(ImVec2(0, 2.0f * zoom));
    panelSlider("Frequency", tr.hpFreq, "%.0f Hz", ImGuiSliderFlags_Logarithmic, 220.0f * zoom, "Cutoff (-3 dB)", pub);
    bool steep = tr.hp24.get();
    if(ImGui::Checkbox("24 dB/oct", &steep)) tr.hp24.set(steep);
    drawPublishPopup(tr.hp24.getEscapedName());
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Butterworth, 12 or 24 dB per octave");
}

void scMixerPro::drawSatPanel(Track& tr) {
    const float zoom = ofxOceanodeShared::getZoomLevel();
    const float fieldW = 150.0f * zoom;
    auto pub = [this](const std::string& k) { drawPublishPopup(k); };
    ImGui::Dummy(ImVec2(0, 2.0f * zoom));
    ImGui::BeginGroup();
    panelSlider("Drive", tr.satDrive, "%.1f dB", 0, fieldW, "Gain into the soft clipper (anti-aliased tanh)", pub);
    panelSlider("Warmth", tr.satBias, "%.2f", 0, fieldW, "Asymmetry: adds even harmonics", pub);
    panelSlider("Tone", tr.satTone, "%.0f Hz", ImGuiSliderFlags_Logarithmic, fieldW, "Low-pass after the clipper", pub);
    ImGui::EndGroup();
    ImGui::SameLine(0, 18.0f * zoom);
    ImGui::BeginGroup();
    panelSlider("Output", tr.satOutput, "%.1f dB", 0, fieldW, nullptr, pub);
    panelSlider("Mix", tr.satMix, "%.2f", 0, fieldW, "Parallel saturation", pub);
    bool a = tr.satAuto.get();
    if(ImGui::Checkbox("Auto gain", &a)) tr.satAuto.set(a);
    drawPublishPopup(tr.satAuto.getEscapedName());
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("Quiet signals stay at the same level whatever the drive");
    ImGui::EndGroup();
}

void scMixerPro::drawEchoPanel(Track& tr) {
    const float zoom = ofxOceanodeShared::getZoomLevel();
    const float fieldW = 150.0f * zoom;
    auto pub = [this](const std::string& k) { drawPublishPopup(k); };
    ImGui::Dummy(ImVec2(0, 2.0f * zoom));
    ImGui::BeginGroup();
    {   // Delay: a note value at the node tempo (b) or seconds (s)
        const bool beats = tr.echoBeats.get();
        if(ImGui::Button(beats ? "b##echoUnit" : "s##echoUnit", ImVec2(18.0f * zoom, 0))) tr.echoBeats.set(!beats);
        drawPublishPopup(tr.echoBeats.getEscapedName());
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("Delay as a note value (b, follows the tempo) or in seconds (s); max 4 s");
        ImGui::SameLine(0, 2.0f * zoom);
        ImGui::SetNextItemWidth(fieldW - 20.0f * zoom);
        if(beats) {
            int k = nearestNote(tr.echoBeatVal.get());
            if(ImGui::SliderInt("Delay", &k, 0, kNoteCount - 1, kNotes[k].name, ImGuiSliderFlags_AlwaysClamp))
                tr.echoBeatVal.set(kNotes[k].beats);
            drawPublishPopup(tr.echoBeatVal.getEscapedName());
        } else {
            float v = tr.echoTime.get();
            if(ImGui::SliderFloat("Delay", &v, tr.echoTime.getMin(), tr.echoTime.getMax(), "%.3f s", ImGuiSliderFlags_Logarithmic))
                tr.echoTime.set(v);
            drawPublishPopup(tr.echoTime.getEscapedName());
        }
    }
    panelSlider("Feed", tr.echoFeed, "%.2f", 0, fieldW, "Feedback: how many repeats", pub);
    panelSlider("Mix", tr.echoMix, "%.2f", 0, fieldW, "Equal-power dry / echoes (returns: 1)", pub);
    ImGui::EndGroup();
    ImGui::SameLine(0, 18.0f * zoom);
    ImGui::BeginGroup();
    {
        static const char* filters[4] = {"LowPass", "HighPass", "BandPass", "PeakEQ"};
        int f = ofClamp(tr.echoFilter.get(), 0, 3);
        ImGui::SetNextItemWidth(fieldW);
        if(ImGui::Combo("Filter", &f, filters, 4)) tr.echoFilter.set(f);
        drawPublishPopup(tr.echoFilter.getEscapedName());
        if(ImGui::IsItemHovered()) ImGui::SetTooltip("In the feedback loop: every repeat goes through it again");
    }
    panelSlider("Cutoff", tr.echoCutoff, "%.0f (note)", 0, fieldW, "Filter frequency as a MIDI note", pub);
    panelSlider("Resonance", tr.echoResonance, "%.2f", 0, fieldW, nullptr, pub);
    panelSlider("Ping Pong", tr.echoPingPong, "%.2f", 0, fieldW, "Cross-feeds stereo pairs: repeats alternate left / right", pub);
    ImGui::EndGroup();
}

// Master panel: HP / EQ / Maximizer / Limiter, the same on every server
void scMixerPro::drawMasterPanel(float w, float h) {
    const float zoom = ofxOceanodeShared::getZoomLevel();
    const float fieldW = 170.0f * zoom;
    auto pub = [this](const std::string& k) { drawPublishPopup(k); };
    ImGui::BeginChild("##mixerProMaster", ImVec2(w, h), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.9f, 1.0f), "MASTER");
    const char* tabNames[MST_COUNT] = {"HP##mtab", "EQ##mtab", "Maximizer##mtab", "Limiter##mtab"};
    ofParameter<bool>* sw[MST_COUNT] = {&mHp, &mEq, &mMax, &mLim};
    for(int i = 0; i < MST_COUNT; i++) {
        ImGui::SameLine(0, i == 0 ? 12.0f * zoom : 2.0f * zoom);
        pushButtonColours(masterTab == i ? ImVec4(0.32f, 0.32f, 0.38f, 1.0f) : ImVec4(0.15f, 0.15f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, sw[i]->get() ? ImVec4(0.55f, 0.92f, 0.6f, 1.0f) : ImVec4(0.75f, 0.75f, 0.78f, 1.0f));
        if(ImGui::SmallButton(tabNames[i])) masterTab = i;
        ImGui::PopStyleColor(4);
    }
    masterTab = ofClamp(masterTab, 0, MST_COUNT - 1);
    ImGui::SameLine(0, 14.0f * zoom);
    static const char* labels[MST_COUNT] = {"High-pass", "EQ", "Maximizer", "Limiter"};
    bool on = sw[masterTab]->get();
    if(ImGui::Checkbox(labels[masterTab], &on)) sw[masterTab]->set(on);
    drawPublishPopup(sw[masterTab]->getEscapedName());
    ImGui::SameLine(0, 12.0f * zoom);
    static const char* notes[MST_COUNT] = {
        "after the master level, on every server",
        "the same curve on every channel",
        "drive into a lookahead peak limiter (7 ms latency)",
        "brickwall safety limiter (6 ms latency)"
    };
    ImGui::TextDisabled("%s  -  right-click a control: value / publish", notes[masterTab]);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 22.0f * zoom);
    if(ImGui::SmallButton("x##mclose")) selectedEqTrack = -1;

    ImGui::Dummy(ImVec2(0, 2.0f * zoom));
    switch(masterTab) {
        case MST_HP: {
            panelSlider("Frequency", mHpFreq, "%.0f Hz", ImGuiSliderFlags_Logarithmic, fieldW, "Cutoff (-3 dB): rumble / DC protection", pub);
            bool steep = mHp24.get();
            if(ImGui::Checkbox("24 dB/oct", &steep)) mHp24.set(steep);
            drawPublishPopup(mHp24.getEscapedName());
            break;
        }
        case MST_EQ: {
            if(ImGui::SmallButton("Flat##meq")) for(int b = 0; b < scEQEditor::NUM_BANDS; b++) mEqGain[b].set(0.0f);
            ImGui::SameLine(0, 12.0f * zoom);
            ImGui::TextDisabled("drag: freq / gain, wheel: Q / slope, double-click: 0 dB, right-click: values / publish");
            if(mEqCurveDirty) {
                for(int b = 0; b < scEQEditor::NUM_BANDS; b++)
                    mEqEditor.bands[b] = { mEqFreq[b].get(), mEqGain[b].get(), mEqShape[b].get() };
                float sr = 48000.0f;
                if(ofxSCServer* server = serverAt(0)) {
                    const float r = (float)serverManager::getSampleRateForServer(server);
                    if(r > 0.0f) sr = r;
                }
                mEqEditor.dbRange = 24.0f;
                mEqEditor.recompute(sr);
                mEqCurveDirty = false;
            }
            mEqEditor.gainLimit = 24.0f;
            mEqEditor.qMin = 0.1f;  mEqEditor.qMax = 20.0f;
            mEqEditor.slopeMin = 0.1f; mEqEditor.slopeMax = 4.0f;
            mEqEditor.bandMenu = [this](int b) {
                ImGui::TextUnformatted(scEQEditor::bandName(b));
                ImGui::Separator();
                drawPublishItems(mEqFreq[b].getEscapedName(), "Freq (Hz)", true);
                ImGui::Separator();
                drawPublishItems(mEqGain[b].getEscapedName(), "Gain (dB)", false);
                ImGui::Separator();
                drawPublishItems(mEqShape[b].getEscapedName(), scEQEditor::isShelf(b) ? "Slope" : "Q", false);
            };
            static scEQEditor::Colors colors = scEQEditor::defaultColors();
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            if(mEqEditor.draw(std::max(120.0f * zoom, avail.x - 6.0f * zoom), std::max(60.0f * zoom, avail.y - 10.0f * zoom),
                              colors, true, nullptr, 0, "##mixerProMasterEqEditor")) {
                for(int b = 0; b < scEQEditor::NUM_BANDS; b++) {
                    mEqFreq[b].set(mEqEditor.bands[b].freqHz);
                    mEqGain[b].set(mEqEditor.bands[b].gainDb);
                    mEqShape[b].set(mEqEditor.bands[b].shape);
                }
            }
            break;
        }
        case MST_MAX: {
            ImGui::BeginGroup();
            panelSlider("Drive", mMaxDrive, "%.1f dB", 0, fieldW, "Gain into the limiter: louder, more limiting", pub);
            panelSlider("Ceiling", mMaxCeiling, "%.1f dB", 0, fieldW, "Output peak level", pub);
            ImGui::EndGroup();
            ImGui::SameLine(0, 18.0f * zoom);
            ImGui::BeginGroup();
            panelSlider("Character", mMaxCharacter, "%.1f", 0, fieldW, "Knee and release: 0 tight and fast, 10 soft and slow", pub);
            bool isp = mMaxIsp.get();
            if(ImGui::Checkbox("True peak (ISP)", &isp)) mMaxIsp.set(isp);
            drawPublishPopup(mMaxIsp.getEscapedName());
            if(ImGui::IsItemHovered()) ImGui::SetTooltip("Detects inter-sample peaks with 4x oversampling");
            ImGui::EndGroup();
            break;
        }
        default:
            panelSlider("Ceiling", mLimCeiling, "%.1f dB", 0, fieldW, "No peak above this level", pub);
            break;
    }
    ImGui::EndChild();
}
