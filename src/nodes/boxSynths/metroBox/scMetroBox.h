//
//  scMetroBox.h
//  ofxOceanodeSuperCollider
//
//  Generative rhythm: up to 8 lanes of pulses locked to the global transport.
//  Outputs: Gates (every lane, one channel each: lane n = channel n, for a
//  SampleBox kit's pads) and a mono Gate per lane. A gate carries the hit's
//  level (a gate at 0.6 plays at 0.6).
//
//  Architecture (see the MetroBox design doc):
//    • The rhythm itself comes from metrobox::Engine (scMetroBoxEngine.h), a
//      pure function of (seed, lane settings, genome, cycle index), tested on
//      its own by tools/metrobox_test. The docked preview draws exactly what
//      will play, and a seek or loop replays the same rhythm.
//    • This node writes the cycles around the playhead into an onset buffer
//      (amplitude, duration per tick, 48 ticks per beat) on each server; the
//      MetroBoxPlayer synth follows the transport with scTransportSync
//      anchors and fires whatever the buffer holds under the playhead.
//    • The window shows one row per lane with its main settings (Every,
//      Offset, Chance, Length, Accent, Change, Smart); "More" opens the
//      lane's advanced panel under its row. Advanced settings keep acting when
//      the panel is closed, and the row lists the ones in use.
//    • Every lane setting in the window has a right-click menu: type a value,
//      or publish it to the node as "L<lane> <Setting>" (patchable). The
//      evolution actions (Mutate, Re-roll, Undo, Redo) publish as trigger
//      parameters.
//

#pragma once

#include "../../../ofxOceanodeSuperColliderConfig.h"


#include "../../../scNode.h"
#include "../../../timing/scTransportSync.h"
#include "scMetroBoxEngine.h"
#include "../../../ui/scTimelineMenu.h"
#include "../shared/scSnapshots.h"
#include "../../../ui/scDropdowns.h"
#include "ofxSCSynth.h"
#include "ofxSCBuffer.h"
#include "imgui.h"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

class scMetroBox : public scNode {
public:
    // its window (SC Mixer Pro's "Go to origin synth window" opens and focuses it)
    std::string getWindowTitle() override { return (canvasID == "Canvas" ? "" : canvasID + "/") + "MetroBox " + ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return &showWindow; }
private:
public:
    static constexpr int MAX_LAYERS        = metrobox::MAX_LAYERS;
    static constexpr int TICKS_PER_BEAT    = metrobox::TICKS_PER_BEAT;
    static constexpr int SLOTS             = 4;   // cycles held in the buffer per lane
    static constexpr int MAX_CYCLE_TICKS   = metrobox::MAX_CYCLE_TICKS;
    // Output ports: 0 = Gates (all lanes), n = Gate n

    using Layer = metrobox::Layer;

    // One lane setting as the window and the publish menu see it
    // F_NOTE: a length picked from note values (ticks in the lane, beats
    // when typed, an index into the note list when published)
    enum FieldKind : int { F_FLOAT, F_INT, F_BOOL, F_ENUM, F_NOTE, F_LANE, F_ACTION };
    enum Action : int { ACT_MUTATE = 0, ACT_REROLL, ACT_UNDO, ACT_REDO };
    using NoteList = std::vector<std::pair<int, std::string>>;   // ticks, label
    struct Field {
        std::string name;              // "Density"; published as "L<n> Density"
        FieldKind kind;
        float Layer::* f = nullptr;
        int   Layer::* i = nullptr;
        bool  Layer::* b = nullptr;
        int   action = -1;
        float min = 0.0f, max = 1.0f;
        std::vector<std::string> options;
        const NoteList* notes = nullptr;
        std::string tooltip;
    };

    scMetroBox();
    ~scMetroBox();

    void setup() override;
    void update(ofEventArgs &e) override;
    void draw(ofEventArgs &e) override;
    void setBpm(float bpm) override { currentBpm = std::max(1.0f, bpm); }   // Morph Time in beats

    void buildSynth(ofxSCServer* server) override;
    void createSynth(ofxSCServer* server) override;
    void moveSynthBefore(ofxSCServer* server, int nodeID) override;
    void free(ofxSCServer* server) override;
    void setOutputBus(ofxSCServer* server, int index, int bus) override;
    int  getOutputBusIndex(ofxSCServer* server, int index) override;
    int  getLastSynthID(ofxSCServer* server) override;
    // Gates: channel n is lane n
    std::string getOutputChannelName(int outputIndex, int channel) override {
        if(outputIndex != 0 || channel < 0 || channel >= numLayers.get()) return "";
        return layers[channel].name;
    }
    void activate() override;
    void deactivate() override;
    // The recorder writes the patch's state at score time zero: the onset
    // buffers too (written while it was arming, they never reached the score)
    void resendParametersForNRT() override { resendParams.notify(); invalidateSlots(-1); fillBuffers(); }

    void presetSave(ofJson &json) override;
    void loadBeforeConnections(ofJson &json) override;
    void presetRecallBeforeSettingParameters(ofJson &json) override;
    void presetRecallAfterSettingParameters(ofJson &json) override;

private:
    metrobox::Engine engine;
    std::array<Layer, MAX_LAYERS>& layers;   // engine.layers

    int64_t nowCycle(int layer) const;       // the lane's cycle under the playhead
    void doAction(int layer, int action);
    double mainBarBeats = 4.0;

    // ── Roles ─────────────────────────────────────────────────────────────────
    static const std::vector<std::string>& builtInRoleNames();
    static Layer builtInRole(int index);
    std::vector<std::string> userRoles;
    std::string rolesDir() const;
    void scanRoles();
    void applyRole(int layer, const Layer& role);
    void saveRole(int layer, const std::string& name);
    static ofJson layerToJson(const Layer& L, bool withState = true);
    static Layer layerFromJson(const ofJson& j);

    // ── Fields, typed values, publishing ──────────────────────────────────────
    static const std::vector<Field>& fields();
    static const Field* findField(const std::string& name);
    static std::string publishKey(int layer, const std::string& field);
    float fieldValue(int layer, const Field& f) const;          // as typed (beats for notes)
    void setFieldValue(int layer, const Field& f, float value); // clamps, regenerates
    bool drawField(int layer, const std::string& name, const char* label = nullptr);
    std::vector<const std::pair<int, std::string>*> notesFor(int layer, const Field& f) const;
    void fieldMenu(int layer, const Field& f);

    struct Published {
        int layer = 0;
        std::string field;
        ofParameter<float> pf;
        ofParameter<int>   pi;
        ofParameter<bool>  pb;
        ofParameter<void>  pv;
        std::shared_ptr<ofxOceanodeAbstractParameter> handle;
        ofEventListener listener;
        bool updating = false;
    };
    std::map<std::string, std::unique_ptr<Published>> published;
    // Snapshots (the shared system); their controls are published on their own
    scSnapshots snaps;
    ofJson initial;                       // the groove at setup (the factory's base)
    ofJson appliedLanes;                  // the lanes a snapshot last loaded whole
    float currentBpm = 120.0f;
    std::map<std::string, std::function<std::shared_ptr<ofxOceanodeAbstractParameter>()>> snapParams;
    std::map<std::string, std::shared_ptr<ofxOceanodeAbstractParameter>> snapPublished;
    ofJson captureSnapshot() const;
    void applySnapshot(const ofJson& a, const ofJson& b, float t, bool sw);
    std::vector<ofJson> factorySnapshots() const;
    void publishSnap(const std::string& key);
    void unpublishSnap(const std::string& key);
    void snapMenu(const std::string& key);
    std::vector<std::string> publishedOrder;
    scTimelineMenu timeline;                  // "Add to Timeline" from the window
    void publishKey(const std::string& key);  // "L<n> <Setting>"
    bool publishedSeparatorAdded = false;
    bool publish(int layer, const std::string& field);
    bool unpublish(const std::string& key);
    bool hasConnection(const std::string& key) const;
    void syncPublished(const std::vector<std::string>& keys);
    void refreshPublished(int layer);   // push lane values into its published parameters
    std::vector<std::string> laneNames() const;   // a published lane link's dropdown
    void syncLaneNames();                      // ... as lanes are renamed / added

    // ── Change: Evolving mutates the loop every few phrases ───────────────────
    std::array<int64_t, MAX_LAYERS> lastPhrase;
    std::array<int, MAX_LAYERS> phrasesSinceAuto;
    std::array<int64_t, MAX_LAYERS> lastPassPhrase;   // Seed 0: the phrase each lane is in (leaving one re-draws it)
    uint32_t autoCounter = 0;
    void autoEvolve();

    // ── Ports: Trig N, Gate N, Amp N per active lane ─────────────────────────
    int portLayers = 0;
    void syncOutputPorts();
    static std::string busName(int outputIndex);
    static constexpr int NUM_OUTPUTS = 1 + MAX_LAYERS;

    // ── Server side ───────────────────────────────────────────────────────────
    struct ServerState {
        ofxSCSynth*  synth = nullptr;
        ofxSCBuffer* buffer = nullptr;
        bool  allocPending = false;
        float allocTime = 0.0f;
        std::map<int, int> outputBuses;
        // Which cycle each slot holds, and the cycle length it was written with
        std::array<std::array<int64_t, SLOTS>, MAX_LAYERS> slotCycle;
        std::array<int, MAX_LAYERS> slotCycleTicks;
    };
    std::map<ofxSCServer*, ServerState> servers;

    void invalidateSlots(int layer);           // -1 = all lanes
    void fillBuffers();                        // keep the cycles around the playhead written
    void writeSlot(ServerState& st, int layer, int64_t cycle);
    std::vector<int64_t> wantedCycles(int layer, const ofxOceanodeTransportState& t) const;
    void sendStaticControls(ServerState& st);
    bool bufferLive(ofxSCServer* server, const ServerState& st) const;

    // ── Transport ─────────────────────────────────────────────────────────────
    scTransportSync::Follower  follower;
    scTransportSync::StepClock stepClock;
    int  anchorIdCounter = 0;
    int  anchorHardCounter = 0;
    bool forceHardAnchor = true;
    std::array<int, MAX_LAYERS> anchoredCycleTicks;
    void sendTransportAnchors();

    // ── Parameters ────────────────────────────────────────────────────────────
    ofParameter<bool>          showWindow;
    ofParameter<int>           numLayers;
    ofParameter<int>           globalSeed;
    ofParameter<vector<float>> densityIn;
    ofEvent<void> resendParams;
    ofEventListeners listeners;

    // ── GUI ───────────────────────────────────────────────────────────────────
    int   viewBars = 4;
    char  roleNameBuf[64] = "";
    void drawWindow();
    void drawTimeline(float width);
    void drawLanes();
    void drawLaneRow(int layer);
    void drawAdvanced(int layer);
    void roleCombo(int layer, const char* label);
    // Drag a lane (its name in the rows or the timeline) onto another to move it there
    void laneDragSource(int layer);
    void laneDropTarget(int layer);
    int pendingMoveFrom = -1, pendingMoveTo = -1;
    void moveLane(int from, int to);
    std::vector<std::string> advancedSummary(int layer) const;   // advanced settings in use
    double beatsPerBar() const;
};
