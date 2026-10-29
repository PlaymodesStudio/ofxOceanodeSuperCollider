//
//  scSampleBox.h
//  ofxOceanodeSuperCollider
//
//  SampleBox: a kit of sampler pads (1 to 16), each with its own sample, its
//  own sound settings and modulation, and its own mixer channel.
//
//  • Playing: the Trig/Gate Audio input and the Trigger/Gate values are
//    multichannel, one channel per pad: a pad plays the channel set in its
//    "Channel" (Auto: its row, so row 3 plays channel 3, as a MetroBox lane 3
//    or an EuclideanBox track 3 on their Gates output). Pitch and Velocity
//    are per channel too (one value: every channel). A rising edge starts a
//    note; a gate is a level (0.2 plays at 0.2). One-shot plays the region
//    once (fading out over its last 4 ms), Loop repeats it until Stop, Gate
//    Mode holds the note while the gate is high. Polyphonic rotates over up
//    to 8 voices; mono chokes the previous note (5 ms) instead of jumping it
//    back to the start. Pads in the same Choke group cut each other.
//  • Tempo (a pad's Sound tab): Fit makes the region last a number of beats
//    at the current tempo, kept as the tempo changes (Resample: the pitch
//    follows, like tape; Stretch: it does not). Sync To Transport reads the
//    region at the transport's position while it plays (sample accurate,
//    scTransportSync anchors): a break stays in phase with the bars through
//    seeks, loops and tempo changes. Play With Transport: no trigger needed.
//  • A pad's sound (the window's Sound, Envelopes, LFOs and Routing tabs):
//    region, Resample / Stretch, Reverse, envelopes, filter, its own shared
//    modulation (scModulation) per voice.
//  • The Mixer tab: a channel per pad with Level, Pan, Mute, Solo, a meter,
//    and its effects (EQ, Compressor, Saturator, Echo, Reverb, Stereo image),
//    each a synth of its own that exists only while switched on.
//  • Outputs: Out (every pad, mixed, x Gain) and Pad <n> (each pad alone,
//    after its effects and Level).
//  • The pad list (left): click to edit, drag to reorder, drop a sample on a
//    row, right-click for more. A row shows what plays it (the lane's name
//    when a MetroBox or EuclideanBox is wired in).
//  • Synths: synthdefs/Defaults/SampleBox (SampleBoxPad_K_N_R|T, the
//    effects, SampleBoxStrip_N). Files: anything macOS can decode, decoded on
//    a worker thread; from the browser, Load..., or a Finder drop.
//  • Parameters: pad 1's have the plain names ("Cutoff"), pad n's end in
//    " n" ("Cutoff 3"). Right-click any control in the window to type a
//    value, publish it to the node, or modulate it. Moving a pad moves its
//    settings and sample; published controls stay with their position.
//

#pragma once

#include "../../../ofxOceanodeSuperColliderConfig.h"
#include "../../../scNode.h"
#include "../shared/scKeyboardPiano.h"
#include "../../../serverManager.h"
#include "../../../timing/scTransportSync.h"
#include "../../../samples/scSampleBrowser.h"
#include "../slicerBox/scSlicerBox.h"
#include "../../../ui/scEQEditor.h"
#include "../shared/scModulation.h"
#include "../shared/scAudioParam.h"
#include "../../../timing/scScheduledInputs.h"
#include "../../../ui/scTimelineMenu.h"
#include "../../../ui/scDropdowns.h"
#include "../shared/scSnapshots.h"
#include "../shared/scBoxMacroMorph.h"
#include "ofxSCGroup.h"
#include "ofxSCSynth.h"
#include "ofxSCBuffer.h"
#include "ofxSCBus.h"
#include <array>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

class scSampleBox : public scNode {
    scKeyboardPiano keyboardPiano;
public:
    // its window (SC Mixer Pro's "Go to origin synth window" opens and focuses it)
    std::string getWindowTitle() override { return "SampleBox " + ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return &showWindow; }
private:
    ofParameter<bool> embedSamples;
public:
    static constexpr int MaxChannels = 16;
    static constexpr int MaxVoices = 8;
    static constexpr int MaxPads = 16;
    static constexpr int ChokeGroups = 8;
    explicit scSampleBox(std::vector<serverManager*> servers);
    ~scSampleBox() override;

    void setup() override;
    void update(ofEventArgs&) override;
    void setBpm(float bpm) override;
    void draw(ofEventArgs&) override;
    void buildSynth(ofxSCServer*) override;
    void createSynth(ofxSCServer*) override;
    void free(ofxSCServer*) override;
    void setOutputBus(ofxSCServer*, int, int) override;
    int getOutputBusIndex(ofxSCServer*, int) override;
    void moveSynthBefore(ofxSCServer*, int) override;
    int getLastSynthID(ofxSCServer*) override;
    void setInputBus(ofxSCServer*, scNode*, int) override;
    void resetInputBusses(ofxSCServer*, int) override;
    std::string requiredSynthdefFolder() const override { return synthdefFolder; }
    void resendParametersForNRT() override;
    void presetSave(ofJson&) override;
    void loadBeforeConnections(ofJson&) override;
    void presetRecallBeforeSettingParameters(ofJson&) override;
    void presetRecallAfterSettingParameters(ofJson&) override;
    // MacroMixer: the pads saved in "sampleBoxPads" blend on the running synths
    // through the snapshot morph (applySnapshot) when the target has the same
    // pads and samples; otherwise MacroMixer replaces the node.
    MorphKeys morphKeys() const override;
    MorphHint morphHint(const ofxOceanodeAbstractParameter& p) const override;
    bool morphCheck(const ofJson& target, std::string& reason) const override;
    bool morphBegin(const ofJson& target, std::string& reason) override;
    void morphApply(float t, bool switched) override { macroMorph.apply(snaps, t, switched); }
    void morphEnd(bool completed) override { macroMorph.end(snaps, completed); }
#ifdef OFXOCEANODE_HAS_TIMELINE_INSTRUMENT
    // A kit: Trigger/Gate and Velocity, key C1 = channel 1 (the pads keep their own pitch)
    bool getTimelineInstrument(ofxOceanodeTimelineInstrument& out) override;
#endif

private:
    // Which synth a pad control goes to
    enum Fx : int { FX_EQ = 0, FX_COMP, FX_SAT, FX_ECHO, FX_REVERB, FX_IMAGE, FX_COUNT };
    enum Dest : int { DEST_ENGINE = 0, DEST_FX, DEST_STRIP = DEST_FX + FX_COUNT, DEST_NONE };

    struct Control {
        std::string key;         // escaped parameter name ("Cutoff_3")
        std::string base;        // the same, without the pad's suffix ("Cutoff")
        int pad = -1;            // -1: the node's
        int dest = DEST_NONE;
        std::string scName;
        ofParameter<float>* f = nullptr;
        ofParameter<int>* i = nullptr;
        ofParameter<bool>* b = nullptr;
        std::function<std::shared_ptr<ofxOceanodeAbstractParameter>()> add;
        std::function<std::string()> save;
        std::function<void(const std::string&)> load;
    };

    // What a pad plays: moves with the pad
    struct Sample {
        std::string path, playPath;      // as chosen (kept in presets) / what scsynth reads
        int channels = 1;
        float seconds = 0.0f, rate = 0.0f;
        std::vector<float> peaks;
        std::future<scSlicerBox::Audio> task;
        std::string decodingPath, pendingFile, error;
        std::map<ofxSCServer*, std::vector<ofxSCBuffer*>> buffers;
    };
    // A pad's objects on one server, in its group: the voices, the
    // modulation's sources and routes, the effects, the strip
    struct PadSC {
        ofxSCGroup* group = nullptr;
        ofxSCGroup* voices = nullptr;
        ofxSCGroup* sources = nullptr;
        ofxSCGroup* routes = nullptr;
        ofxSCGroup* fx = nullptr;
        ofxSCSynth* engine = nullptr;
        std::array<ofxSCSynth*, FX_COUNT> fxSynth{};
        ofxSCSynth* strip = nullptr;
        ofxSCBus* bus = nullptr;         // audio, MaxChannels: the pad's sound
        ofxSCBus* gateBus = nullptr;     // audio, 8: the voices' gates (for the envelopes)
        ofxSCBus* vis = nullptr;         // control, 8: playheads
        ofxSCBus* latest = nullptr;      // control, 1: the latest voice (the effects' modulation)
        ofxSCBus* meter = nullptr;       // control, 1: the strip's peak
        std::string engineDef;
        bool settlePending = false;      // on _M while the morph rests at R or T: swap once idle
        int idleReads = 0;               // consecutive all-silent playhead reads
        bool created = false;
    };
    struct Pad {
        int index = 0;
        std::string name;
        std::unique_ptr<Sample> sample = std::make_unique<Sample>();
        // Sound
        ofParameter<int> channel, choke;
        ofParameter<float> inPoint, outPoint, loopCrossfade, rate, stretch, warpWindow, transpose;
        ofParameter<bool> loop, poly, gateMode, reverse;
        ofParameter<int> voices, stretchMode;
        ofParameter<float> attack, release, pan;
        ofParameter<bool> filterOn;
        ofParameter<int> filterType;
        ofParameter<float> cutoff, resonance;
        // Tempo
        ofParameter<float> fitBeats;     // 0: off
        ofParameter<bool> sync, autoplay;
        float sentFitRate = -1.0f;
        bool autoPlaying = false;        // Play With Transport holds a note now
        bool loopRunning = false;        // a looping note was started (Play, Trigger/Gate) and not stopped
        bool synced() const { return sync.get() && fitBeats.get() > 0.0f; }
        // Mixer
        ofParameter<float> level;
        ofParameter<bool> mute, solo;
        std::array<ofParameter<bool>, FX_COUNT> fxOn;
        std::array<ofParameter<float>, 5> eqGain, eqFreq, eqShape;
        ofParameter<float> eqMix;
        ofParameter<float> compThreshold, compRatio, compAttack, compRelease, compMakeup;
        ofParameter<float> satDrive, satMix;
        ofParameter<float> echoTime, echoFeedback, echoTone, echoSpread, echoAmount;
        ofParameter<float> revSize, revDecay, revPredelay, revLowpass, revPosition, revSpread, revMix;
        ofParameter<float> width, diffuse, bassMono, bassFreq;
        scModulation mod;
        scEQEditor eqEditor;
        std::map<ofxSCServer*, PadSC> sc;
        ofEventListeners listeners;
        // play state
        int fireCount = 0, stopCount = 0;
        bool gateHigh = false;           // Trigger/Gate (Oceanode) on its channel
        bool auditionHeld = false;
        std::vector<float> playheads;
        float meter = 0.0f;
        std::string suffix() const { return index == 0 ? std::string() : " " + ofToString(index + 1); }
    };
    std::vector<std::unique_ptr<Pad>> pads;
    int selected = 0;

    std::vector<Control> controls;
    std::vector<serverManager*> servers;
    std::string synthdefFolder;          // where our SynthDefs are (requireSynthdefs)
    scSampleBrowser browser{"scSampleBox", {".wav", ".aif", ".aiff", ".flac"}};

    // ── The node's server objects ───────────────────────────────────────────
    struct NodeSC {
        ofxSCGroup* root = nullptr;      // every pad's group, in order
        ofxSCBus* choke = nullptr;       // control, 16: each pad's triggers (choke groups)
        bool created = false;
        // The pads wait while the server is still loading definitions (our
        // folder, asked for when the node was made: a preset load builds the
        // graph right after); update() creates them once it has synced
        bool padsPending = false;
        float pendingSince = 0.0f;
    };
    std::map<ofxSCServer*, NodeSC> nodeSC;
    std::map<ofxSCServer*, std::map<int, int>> outputBuses;   // 0 Out, n Pad n
    std::map<ofxSCServer*, int> inputBuses;                   // Trig/Gate Audio
    float bpm = 120.0f;
    float nextMeterRead = 0.0f;
    float nextSettleRead = 0.0f;
    ofEventListeners nodeListeners;
    ofEventListener dragListener;

    // ── The node's parameters ───────────────────────────────────────────────
    ofParameter<bool> showWindow;
    ofParameter<std::vector<float>> trigger, pitch;   // per channel
    std::shared_ptr<ofxOceanodeAbstractParameter> triggerHandle;
    scScheduledInputs noteInputs;    // the timeline's notes, timetagged
    ofParameter<float> gain;                                    // the mix's (and every Pad output's) level
    ofParameter<int> channels;                                  // N Chan
    scAudioParam pitchIn;   // Pitch audio cable: channel c for a pad on channel c

    // ── Pads ────────────────────────────────────────────────────────────────
    Pad* addPad();                           // at the end; nullptr when full
    void removePad(int k);
    void duplicatePad(int k);
    void movePad(int from, int to);
    void setupPad(Pad& p);                   // its parameters, listeners, modulation
    int padChannel(const Pad& p) const;      // 0-based
    std::string channelSource(int channel);  // what plays a channel ("Kick"), "" unknown
    std::string padLabel(const Pad& p) const;
    // a pad's settings as pad-relative keys (base -> value) and its modulation
    ofJson padState(int k) const;
    void applyPadState(int k, const ofJson& state);
    std::string relKey(const std::string& key) const;          // a pad control: "#base"
    std::string absKey(const std::string& rel, int pad) const;
    void rewriteModKeys(ofJson& modJson, bool toRelative, int pad) const;

    // ── Sample and synths ───────────────────────────────────────────────────
    void loadSample(Pad& p, const std::string& path);
    void applyDecoded(Pad& p, scSlicerBox::Audio&& audio);
    void ensureBuffers(Pad& p, ofxSCServer*);
    void releaseBuffers(Sample& s, ofxSCServer*);
    std::string engineDef(const Pad& p) const;
    void syncEngine(ofxSCServer*, Pad& p);   // the right definition (see engineDef), its values
    void settleEngines();                     // _M -> R / T on idle pads
    void createPad(ofxSCServer*, Pad& p);
    void destroyPad(ofxSCServer*, Pad& p, bool sendFree = true);
    void syncPad(Pad& p);                    // every server: synths that should exist, all values
    void syncPadServer(ofxSCServer*, Pad& p);
    void replaceEngine(ofxSCServer*, Pad& p);
    void syncFx(ofxSCServer*, Pad& p);       // create / free the effects after their On
    void sendEngine(ofxSCServer*, Pad& p, bool includeNotes=true);
    void sendFx(ofxSCServer*, Pad& p, int fx);
    void sendStrip(ofxSCServer*, Pad& p);
    void sendChannelInputs(Pad& p, bool pitchOnly=false);          // trig bus, pitch, gate/velocity level (after Channel or the inputs changed)
    void sendChokeMasks();
    void sendMutes();
    // Tempo: Fit and Sync
    float currentBpm() const;                // the transport's (else the node's setBpm)
    double beatsPerBar() const;
    float fitRateOf(const Pad& p) const;     // the speed that makes the region last Fit beats
    void sendTempo(Pad& p);                  // fitrate, fitbeats, sync
    void updateTempo();                      // per frame: fit rates, anchors, Play With Transport
    static float guessBeats(float seconds, double beatsPerBar);
    scTransportSync::Follower follower;
    int anchorIdCounter = 0;
    void sendTransportAnchors();
    void requestAnchor();
    void rebuildAll();                       // N Chan: every pad's synths again
    void fire(Pad& p);
    void stop(Pad& p);
    // A new engine (the graph rebuilt, a new definition) has no voices: the
    // loop that should be sounding starts again there (in phase when synced)
    void resumeNotes(ofxSCServer*, Pad& p);
    void requireSynthdefs();
    bool anySolo() const;
    float valueFor(const std::vector<float>& v, int channel, float fallback) const;

    // ── Controls, publishing ────────────────────────────────────────────────
    template<typename T> Control& registerPadControl(Pad& p, ofParameter<T>& param, const std::string& label,
                                                     T def, T lo, T hi, int dest, const char* scName);
    void registerModControl(Pad& p, ofAbstractParameter& param, std::function<std::shared_ptr<ofxOceanodeAbstractParameter>()> add,
                            std::function<std::string()> save, std::function<void(const std::string&)> load);
    Control* findControl(const std::string& key);
    std::map<std::string, std::shared_ptr<ofxOceanodeAbstractParameter>> published;
    scDropdowns dropdowns;   // the combos' names (a published combo is a dropdown)
    scSnapshots snaps;                    // the shared snapshots: the kit, library, morph, presets
    ofJson initialPad;                    // a fresh pad (the factory's base)
    ofJson appliedKit;                    // the pads a snapshot last loaded whole
    bool kitBlended = false;              // the last snapshot frame was between two kits
    scBoxMacroMorph macroMorph;           // a MacroMixer morph: the start, the arrival
    ofJson snapshotOf(const ofJson& saved) const;   // a saved patch as a snapshot
    template<typename T> void registerNodeControl(ofParameter<T>& param);
    ofJson captureSnapshot() const;
    void applySnapshot(const ofJson& a, const ofJson& b, float t, bool sw);
    std::vector<ofJson> factorySnapshots() const;
    std::vector<std::string> publishedKeys;
    bool publishedSeparator = false;
    bool restoring = false;
    scTimelineMenu timeline;              // "Add to Timeline" from the window
    void publish(const std::string&);
    void unpublish(const std::string&, bool force = false);
    bool isConnected(const std::string&) const;
    void drawPublishMenu(const std::string&);
    void drawPublishedUnderline(const std::string&) const;

    // ── Window ──────────────────────────────────────────────────────────────
    float browserWidth = 240.0f;
    float waveformHeight = 0.0f;
    float padListHeight = 0.0f;
    int regionDrag = -1;
    float regionDragOffset = 0.0f;
    bool windowHovered = false;
    bool pendingLoadDialog = false;
    std::string pendingDropFile;             // from the Finder, for the selected pad
    int pendingMoveFrom = -1, pendingMoveTo = -1;
    int pendingRemove = -1, pendingDuplicate = -1;
    int renaming = -1;
    int hoveredPad = -1;                     // the pad row (or waveform) under the mouse: a Finder drop goes there
    char renameBuf[64] = "";
    float labelW = 74.0f;
    void drawWindow();
    void drawPadList(float w, float h);
    void drawHeader(Pad& p, float w);
    void drawWaveform(Pad& p, float w);
    void drawTransport(Pad& p, float w);
    void drawSoundTab(Pad& p, float w);
    void drawMixerTab(float w);
    void drawStrip(Pad& p, float w, float h);
    void drawEffects(Pad& p, float w);
    void drawEqEditor(Pad& p, float w, float h);
    void sectionHeader(const char* title, float w, ofParameter<bool>* on = nullptr);
    void slider(ofParameter<float>& p, const char* label, float w, const char* fmt = "%.2f", bool logarithmic = false);
    void sliderInt(ofParameter<int>& p, const char* label, float w, const char* fmt = "%d");
    void combo(ofParameter<int>& p, const char* label, float w, const std::vector<std::string>& items);
    void toggle(ofParameter<bool>& p, const char* label);
    void beginColumn(const char* id);
    void endColumn(bool last = false);
};
