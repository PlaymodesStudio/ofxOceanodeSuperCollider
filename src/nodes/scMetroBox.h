//
//  scMetroBox.h
//  ofxOceanodeSuperCollider
//
//  Generative rhythm: up to 8 lanes of pulses locked to the global transport,
//  each with its own mono Trig, Gate and Amp outputs.
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
//    • Every lane setting in the window has a right-click menu: type a value,
//      or publish it to the node as "L<lane> <Setting>" (patchable). The
//      evolution actions (Mutate, Re-roll, Undo, Redo, Like) publish as
//      trigger parameters.
//

#pragma once

#include "ofxOceanodeSuperColliderConfig.h"

#if OFXOCEANODESC_HAS_TRANSPORT

#include "scNode.h"
#include "scTransportSync.h"
#include "scMetroBoxEngine.h"
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
    static constexpr int MAX_LAYERS        = metrobox::MAX_LAYERS;
    static constexpr int TICKS_PER_BEAT    = metrobox::TICKS_PER_BEAT;
    static constexpr int SLOTS             = 4;   // cycles held in the buffer per lane
    static constexpr int MAX_CYCLE_TICKS   = metrobox::MAX_CYCLE_TICKS;
    static constexpr int OUTPUTS_PER_LAYER = 3;   // Trig, Gate, Amp

    using Layer = metrobox::Layer;

    // One lane setting as the window and the publish menu see it
    enum FieldKind : int { F_FLOAT, F_INT, F_BOOL, F_ENUM, F_TICKS, F_LANE, F_ACTION };
    enum Action : int { ACT_MUTATE = 0, ACT_REROLL, ACT_UNDO, ACT_REDO, ACT_LIKE };
    struct Field {
        std::string name;              // "Density"; published as "L<n> Density"
        FieldKind kind;
        float Layer::* f = nullptr;
        int   Layer::* i = nullptr;
        bool  Layer::* b = nullptr;
        int   action = -1;
        float min = 0.0f, max = 1.0f;  // F_TICKS: in beats
        std::vector<std::string> options;
        std::string tooltip;
    };

    scMetroBox();
    ~scMetroBox();

    void setup() override;
    void update(ofEventArgs &e) override;
    void draw(ofEventArgs &e) override;

    void buildSynth(ofxSCServer* server) override;
    void createSynth(ofxSCServer* server) override;
    void moveSynthBefore(ofxSCServer* server, int nodeID) override;
    void free(ofxSCServer* server) override;
    void setOutputBus(ofxSCServer* server, int index, int bus) override;
    int  getOutputBusIndex(ofxSCServer* server, int index) override;
    int  getLastSynthID(ofxSCServer* server) override;
    void activate() override;
    void deactivate() override;
    void resendParametersForNRT() override { resendParams.notify(); }

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
    float fieldValue(int layer, const Field& f) const;          // as shown (beats for ticks)
    void setFieldValue(int layer, const Field& f, float value); // clamps, regenerates
    bool drawField(int layer, const std::string& name, const char* label = nullptr);
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
    std::vector<std::string> publishedOrder;
    bool publishedSeparatorAdded = false;
    bool publish(int layer, const std::string& field);
    bool unpublish(const std::string& key);
    bool hasConnection(const std::string& key) const;
    void syncPublished(const std::vector<std::string>& keys);
    void refreshPublished(int layer);   // push lane values into its published parameters

    // ── Auto-evolve ───────────────────────────────────────────────────────────
    std::array<int64_t, MAX_LAYERS> lastPhrase;
    std::array<int, MAX_LAYERS> phrasesSinceAuto;
    uint32_t autoCounter = 0;
    void autoEvolve();

    // ── Ports: Trig N, Gate N, Amp N per active lane ─────────────────────────
    int portLayers = 0;
    void syncOutputPorts();
    static std::string busName(int outputIndex);

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
    ofParameter<bool>          trigAmp;
    ofEvent<void> resendParams;
    ofEventListeners listeners;

    // ── GUI ───────────────────────────────────────────────────────────────────
    int   selectedLayer = 0;
    int   viewBars = 4;
    char  roleNameBuf[64] = "";
    int   crossA = 0, crossB = 1;
    void drawWindow();
    void drawTimeline(float width);
    void drawLayerEditor(int layer);
    double beatsPerBar() const;
};

#endif // OFXOCEANODESC_HAS_TRANSPORT
