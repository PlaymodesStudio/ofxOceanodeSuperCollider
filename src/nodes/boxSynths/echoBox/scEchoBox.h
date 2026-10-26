//
//  scEchoBox.h
//  ofxOceanodeSuperCollider
//
//  EchoBox: a multi-tap echo, N channels in, N channels out.
//
//  • Taps (1..8), each with its own time (ms, or beats following the
//    tempo), feedback, filter, low cut, diffusion (each repeat more diffuse),
//    saturation, shimmer (pitch-shifted repeats), ping-pong / multichannel
//    bounce, wow / flutter and freeze. One synth per tap (EchoBoxTap<N>,
//    synthdefs/Defaults/EchoBox): a tap that is not there costs nothing.
//  • The echoes then go through MicroSpace reverb (boxMicroSpace<N>,
//    while Reverb is on) and EchoBoxMix<N>: dry / wet, ducking (the echoes
//    step back while the input plays), output gain.
//  • Modulation (the shared scModulation): LFOs with one oscillator per
//    channel (each channel its own phase: the LFO's Spread / Invert /
//    Rand...), external sources, routed to the taps' Time, Feedback, Cutoff,
//    Level, Ping-pong, Drive, Shimmer, Diffusion and Send (per channel, the
//    same for every tap) and to Mix (LFOs / Routing tabs).
//  • The node shows Show, Channels, Mix and whatever is published. The
//    dockable window has the echo map (every tap's repeats on a timeline,
//    one lane per channel: drag a tap's first echo to set its time), a tab
//    per tap and the output and reverb controls. Right-click any control to
//    publish it to the node.
//

#pragma once

#include "../../../scNode.h"
#include "../../../serverManager.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "ofxSCGroup.h"
#include "../shared/scModulation.h"
#include "../../../ui/scTimelineMenu.h"
#include "../../../ui/scDropdowns.h"
#include "../shared/scSnapshots.h"
#include "../../../audio/scSynthControlBatch.h"
#include "imgui.h"
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class scEchoBox : public scNode {
public:
    // its window (SC Mixer Pro's "Go to origin synth window" opens and focuses it)
    std::string getWindowTitle() override { return "EchoBox " + ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return &showWindow; }
private:
public:
    static constexpr int MAX_TAPS = 8;
    static constexpr int MAX_CHANNELS = 16;

    explicit scEchoBox(std::vector<serverManager*> servers);
    ~scEchoBox();

    void setup() override;
    void update(ofEventArgs&) override;
    void draw(ofEventArgs&) override;
    void setBpm(float bpm) override;
    void activate() override;
    void deactivate() override;

    void buildSynth(ofxSCServer* s) override;
    void createSynth(ofxSCServer* s) override;
    void free(ofxSCServer* s) override;
    void moveSynthBefore(ofxSCServer* s, int nodeID) override;
    int  getLastSynthID(ofxSCServer* s) override;
    void setOutputBus(ofxSCServer* s, int idx, int bus) override;
    int  getOutputBusIndex(ofxSCServer* s, int idx) override;
    void setInputBus(ofxSCServer* s, scNode* node, int bus) override;
    void resetInputBusses(ofxSCServer* s, int targetBus) override;
    void resendParametersForNRT() override;

    void presetSave(ofJson& j) override;
    void loadBeforeConnections(ofJson& j) override;
    void presetRecallAfterSettingParameters(ofJson& j) override;

private:
    struct Tap {
        ofParameter<int> unit;                  // 0 ms, 1 beats
        ofParameter<float> ms, beats, level, send, feed;
        ofParameter<bool> freeze;
        ofParameter<float> pingpong, spread, wowRate, wowDepth;
        ofParameter<int> rotate, bounce;        // bounce: 0 forwards (+1), 1 backwards (-1)
        ofParameter<int> ftype;                 // 0 off, 1 LP, 2 HP, 3 BP, 4 peak
        ofParameter<float> cutoff, res, lowcut, blur, blurTime, drive, shift, shiftMix;
    };
    struct ServerState {
        std::array<ofxSCSynth*, MAX_TAPS> taps{};
        ofxSCSynth* mix = nullptr;
        ofxSCSynth* rvb = nullptr;
        bool rvbRunning = false;
        float rvbOffAt = -1.0f;
        // what the reverb / mix already have: morph frames send only changes
        scSynthControlBatch::Cache rvbSent, mixRouteSent;
        ofxSCBus* wetA = nullptr;            // taps -> (reverb)
        ofxSCBus* wetB = nullptr;            // reverb -> mix
        ofxSCBus* vis = nullptr;             // taps' levels, input, wet, duck
        // modulation: a group at the head (LFO sources, then routes)
        ofxSCGroup* modGroup = nullptr;
        ofxSCGroup* modSources = nullptr;
        ofxSCGroup* modRoutes = nullptr;
        bool modCreated = false;
        int inBus = -1, outBus = -1, builtChannels = 0;
    };
    std::vector<serverManager*> servers;
    std::map<ofxSCServer*, ServerState> states;

    // ── Publishable controls ─────────────────────────────────────────────────
    struct Control {
        std::string key;
        ofAbstractParameter* param = nullptr;
        std::function<std::shared_ptr<ofxOceanodeAbstractParameter>()> add;
        std::function<ofJson()> save;
        std::function<void(const ofJson&)> load;
    };
    std::vector<Control> controls;
    std::map<std::string, std::shared_ptr<ofxOceanodeAbstractParameter>> published;
    scDropdowns dropdowns;   // the combos' names (a published combo is a dropdown)
    std::vector<std::string> publishedKeys;
    scTimelineMenu timeline;              // "Add to Timeline" from the window
    bool publishedSeparator = false;
    bool restoring = false;
    template<typename T> void reg(ofParameter<T>& p, std::function<void()> send);
    void publish(const std::string& key);
    void unpublish(const std::string& key);
    bool isConnected(const std::string& key) const;
    void publishMenu(const std::string& key);
    void publishedUnderline(const std::string& key) const;

    // ── Engine ───────────────────────────────────────────────────────────────
    int n() const { return channels.get(); }
    float tapSeconds(int t) const;
    void sendTap(int t);
    void sendTap(ofxSCSynth* s, int t);
    void sendMix(ofxSCSynth* s);
    void sendRvb(ofxSCSynth* s);
    void createTap(ofxSCServer* server, ServerState& st, int t);
    void addTap();
    void removeLastTap();
    void ensureBuses(ofxSCServer* server, ServerState& st);
    void updateReverb(ofxSCServer* server, ServerState& st);
    void routeWet(ServerState& st);
    void rebuildChannels();
    float reverbTail() const;
    void requireSynthdefs();

    // ── Parameters ───────────────────────────────────────────────────────────
    ofParameter<bool> showWindow;
    ofParameter<int> channels;
    ofParameter<float> mix, gain, duck, duckRel;
    std::array<Tap, MAX_TAPS> taps;
    int tapCount = 1;
    ofParameter<bool> rvbOn;
    ofParameter<float> rvbMix, rvbDecay, rvbSize, rvbPredelay, rvbLate, rvbSpread, rvbLowpass,
                       rvbHiDamp, rvbLoDamp, rvbModRate, rvbSpin, rvbWander;
    float currentBpm = 120.0f;
    scModulation modulation;
    scSnapshots snaps;                    // the shared snapshots: library, morph, presets
    ofJson initial;                       // the sound at setup (the factory's base)
    ofJson appliedMod;                    // the modulation a snapshot last loaded
    ofJson captureSnapshot() const;
    void applySnapshot(const ofJson& a, const ofJson& b, float t, bool sw);
    std::vector<ofJson> factorySnapshots() const;
    void setTapCount(int taps);
    void createMod(ofxSCServer* server, ServerState& st);
    void freeMod(ofxSCServer* server, ServerState& st, bool sendFree);
    void setModArgs(ofxSCServer* server, ServerState& st, ofxSCSynth* s, int tap);
    std::vector<float> visValues;
    float nextVisRead = 0.0f;
    ofEventListeners listeners;

    // ── Window ───────────────────────────────────────────────────────────────
    void drawWindow();
    void drawEchoMap(float w);
    void drawTapControls(int t, float w);
    void drawOutput(float w);
    void sectionHeader(const char* title, float w, ofParameter<bool>* on = nullptr, ImU32 accent = 0);
    void slider(ofParameter<float>& p, const char* label, float w, const char* fmt = "%.2f", bool log = false);
    void sliderInt(ofParameter<int>& p, const char* label, float w);
    void combo(ofParameter<int>& p, const char* label, float w);   // its names: dropdowns
    void toggle(ofParameter<bool>& p, const char* label);
    void beatsSlider(Tap& tap, float w);
    float labelW = 70.0f;
    int selectedTap = 0;
    int selectTabNext = -1;
    float mapHeight = 0.0f;
    int mapDragTap = -1;
};
