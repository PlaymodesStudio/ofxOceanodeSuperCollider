//
//  scDrumKitBox.h
//  ofxOceanodeSuperCollider
//
//  DrumKitBox: a kit of 1 to 16 synthesized drum pieces (Kick, Snare, Clap,
//  Hats, Tom, Rim, Cowbell, Cymbal, Perc, 808 Sub, FX, Sample), each a stack
//  of layers with its own voice, effects and mixer channel. It plays: the
//  sequencing comes from outside (Gate cables, a sequencer's Gates, the
//  timeline's Drum Grid or piano roll).
//
//  • Playing: Gate (values) and Gate Audio (cable) are multichannel, one
//    channel per piece: a piece plays the channel set in its "Channel"
//    (Auto: its row, so row 2 plays channel 2). A rising edge is a hit, its
//    level the velocity. Pitch is per channel too (MIDI, 60 = the piece's
//    own tuning): a timeline step at 62 plays the kick 2 semitones up.
//    Duration, per channel too, stretches a hit's decays (x2: a closed hat
//    rings like an open one), taken at each hit.
//    Pieces in the same Choke group cut each other (closed hat / open hat).
//  • A piece's sound is up to five layers, each a synth that exists only
//    while it is on (no CPU while off):
//      Body    the tonal drum: phase-reset oscillator (sine / triangle /
//              square), pitch sweep and bend, FM, a second mode, drive
//      Click   the transient: pulse, noise burst, blip, ping
//      Noise   noise (white / pink / brown), clap bursts, filter + envelope
//      Metal   six oscillators at the 808's ratios: square, ring, FM
//      Sample  a file, layered with the synthesis (start, length, tune...)
//    then its voice: filter with envelope, character (drive types, bit /
//    rate crush), transient shaper, Gated, velocity, pan / width.
//    Model loads a piece's starting point, Style one of its variations;
//    every layer stays editable (a kick can get noise, a hat a sample).
//    Each Model has its own library of saved sounds (the header's < > and
//    list, shared by every DrumKitBox: nodeSnapshots/DrumKitBox/Sounds/
//    <Model>.json); recalling one keeps the pad's channel and mixing.
//  • Modulation: the shared scModulation per piece (envelopes, LFOs,
//    routing) onto Pitch, Decay, Cutoff, Drive, Pan, Sweep...
//  • The Mixer tab: a channel per piece with Level, Pan, Mute, Solo, a meter,
//    ducking from another piece (side-chain pumping), and its effects (the
//    Box family's: EQ, Compressor, Saturator, Echo (+ pitch drift), Grains
//    (a granular delay), Reverb, Width), each a
//    synth that exists only while on (and until its tail has rung out).
//  • Outputs: Out (every piece, mixed, x Gain) and <n> (each piece alone).
//  • Synths: synthdefs/Defaults/DrumKitBox (DKTrig, DKBody, DKClick, DKNoise,
//    DKMetal, DKSample1|2, DKVoice_N, DKStrip_N), the effects from MixerPro,
//    the modulation from Modulation.
//  • Parameters: piece 1's have the plain names ("Body Decay"), piece n's
//    end in " n" ("Body Decay 3"). Right-click any control in the window to
//    type a value, publish it to the node, automate or modulate it.
//

#pragma once

#include "../../../ofxOceanodeSuperColliderConfig.h"
#include "../../../scNode.h"
#include "../shared/scKeyboardPiano.h"
#include "../../../serverManager.h"
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
#include <vector>

class scDrumKitBox : public scNode {
    scKeyboardPiano keyboardPiano;
public:
    std::string getWindowTitle() override { return "DrumKitBox " + ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return &showWindow; }

    static constexpr int MaxChannels = 16;
    static constexpr int MaxPads = 16;
    static constexpr int ChokeGroups = 8;
    explicit scDrumKitBox(std::vector<serverManager*> servers);
    ~scDrumKitBox() override;

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
    // MacroMixer: the pieces saved in "drumKitBoxPads" blend on the running
    // synths through the snapshot morph (applySnapshot) when the target has the
    // same pads and samples; otherwise MacroMixer replaces the node.
    MorphKeys morphKeys() const override;
    MorphHint morphHint(const ofxOceanodeAbstractParameter& p) const override;
    bool morphCheck(const ofJson& target, std::string& reason) const override;
    bool morphBegin(const ofJson& target, std::string& reason) override;
    void morphApply(float t, bool switched) override { macroMorph.apply(snaps, t, switched); }
    void morphEnd(bool completed) override { macroMorph.end(snaps, completed); }
#ifdef OFXOCEANODE_HAS_TIMELINE_INSTRUMENT
    // A kit: Gate (level = velocity) and Pitch per channel, row n = channel n,
    // each row named after its piece
    bool getTimelineInstrument(ofxOceanodeTimelineInstrument& out) override;
#endif

    // ── What a piece is made of ─────────────────────────────────────────────
    enum Layer : int { L_BODY = 0, L_CLICK, L_NOISE, L_METAL, L_SAMPLE, L_COUNT };
    enum Fx : int { FX_EQ = 0, FX_COMP, FX_SAT, FX_ECHO, FX_GRAIN, FX_REVERB, FX_WIDTH, FX_COUNT };
    // Which synths a control goes to
    enum Dest : int { DEST_NONE = 0, DEST_TRIG, DEST_SOUND, DEST_LAYER_ON, DEST_STRIP, DEST_FX, DEST_FX_ON = DEST_FX + FX_COUNT };
    enum Model : int { M_KICK = 0, M_SNARE, M_CLAP, M_CLOSED_HAT, M_OPEN_HAT, M_TOM, M_RIM, M_COWBELL, M_CYMBAL,
                       M_PERC, M_SUB, M_FX, M_SAMPLE, M_COUNT };
    // A control of a piece, as the window draws it and the synths read it
    struct Spec {
        const char* section;     // where the window draws it ("body", "noise", "mix"...)
        const char* label;       // its name without the piece's suffix
        const char* sc;          // the synth's control ("" none), also its key in templates
        char type;               // 'f' 'i' 'b'
        float def, lo, hi;
        int dest;
        const char* fmt;         // the slider's format
        bool log;                // logarithmic slider
        const char* tip;
    };
    static const std::vector<Spec>& specs();

private:
    std::string synthdefFolder;

    struct Control {
        std::string key;         // escaped parameter name ("Body_Decay_3")
        std::string base;        // the same, without the piece's suffix ("Body_Decay")
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

    // The Sample layer's file: moves with the piece
    struct Sample {
        std::string path, playPath;
        int channels = 1;
        float seconds = 0.0f, rate = 0.0f;
        std::vector<float> peaks;
        std::future<scSlicerBox::Audio> task;
        std::string decodingPath, pendingFile, error;
        std::map<ofxSCServer*, std::vector<ofxSCBuffer*>> buffers;
    };
    // An effect of a piece on one server: its synth reads \in (its own bus)
    // and writes \out (the next stage's bus, or the strip's)
    struct Stage {
        ofxSCSynth* synth = nullptr;
        ofxSCBus* in = nullptr;
        bool running = false;     // in the chain (created on the server)
        float offAt = -1.0f;      // switched off: freed once its tail has rung out
        std::map<std::string, std::vector<float>> sent;
    };
    // A piece's objects on one server, in its group, in this order
    struct PadSC {
        ofxSCGroup* group = nullptr;
        ofxSCSynth* trig = nullptr;
        ofxSCGroup* sources = nullptr;   // the modulation's
        ofxSCGroup* routes = nullptr;
        ofxSCGroup* layers = nullptr;
        std::array<ofxSCSynth*, L_COUNT> layer{};
        std::array<std::string, L_COUNT> layerName{};   // each layer's definition (mono / poly, sample channels)
        ofxSCSynth* voice = nullptr;
        ofxSCGroup* fx = nullptr;
        std::array<Stage, FX_COUNT> stage{};
        ofxSCSynth* strip = nullptr;
        ofxSCBus* hits = nullptr;        // audio, 10 (DKTrig's)
        ofxSCBus* raw = nullptr;         // audio, 2: the layers' sum
        ofxSCBus* gateBus = nullptr;     // audio, 1: the modulation's envelopes follow it
        ofxSCBus* bus = nullptr;         // audio, N: the strip's input
        ofxSCBus* vis = nullptr;         // control, 1: hits so far
        ofxSCBus* meter = nullptr;       // control, 1: the strip's peak
        bool created = false;
    };
    struct Pad {
        int index = 0;
        std::string name;
        std::unique_ptr<Sample> sample = std::make_unique<Sample>();
        // every control of the piece, by its synth name (or label for those without)
        std::map<std::string, ofParameter<float>> f;
        std::map<std::string, ofParameter<int>> i;
        std::map<std::string, ofParameter<bool>> b;
        std::array<ofParameter<bool>*, L_COUNT> layerOn{};
        std::array<ofParameter<bool>*, FX_COUNT> fxOn{};
        scModulation mod;
        scEQEditor eqEditor;
        std::map<ofxSCServer*, PadSC> sc;
        ofEventListeners listeners;
        int fireCount = 0;
        bool gateHigh = false;
        bool auditionHeld = false;
        float meter = 0.0f;
        float hits = -1.0f;              // the last hit count read
        float flash = 0.0f;              // the window's hit light (1 at a hit, fading)
        std::string sound;               // the model library sound it was last set from ("": none)
        std::string suffix() const { return index == 0 ? std::string() : " " + ofToString(index + 1); }
        ofParameter<float>& F(const char* k) { return f.at(k); }
        ofParameter<int>& I(const char* k) { return i.at(k); }
        ofParameter<bool>& B(const char* k) { return b.at(k); }
    };
    std::vector<std::unique_ptr<Pad>> pads;
    int selected = 0;
    std::vector<Control> controls;
    std::vector<serverManager*> servers;
    scSampleBrowser browser{"scDrumKitBox", {".wav", ".aif", ".aiff", ".flac"}};

    struct NodeSC {
        ofxSCGroup* root = nullptr;
        ofxSCBus* choke = nullptr;       // control, 16: each piece's hits (choke groups)
        ofxSCBus* keys = nullptr;        // control, 16: each piece's level (ducking)
        bool created = false;
        bool padsPending = false;        // the definitions still loading (see createSynth)
        float pendingSince = 0.0f;
    };
    std::map<ofxSCServer*, NodeSC> nodeSC;
    std::map<ofxSCServer*, std::map<int, int>> outputBuses;   // 0 Out, n piece n
    std::map<ofxSCServer*, int> inputBuses;                   // Gate Audio
    float bpm = 120.0f;
    float nextMeterRead = 0.0f;
    ofEventListeners nodeListeners;
    ofEventListener dragListener;

    // ── The node's parameters ───────────────────────────────────────────────
    ofParameter<bool> showWindow, embedSamples;
    ofParameter<std::vector<float>> gate, pitch;   // per channel
    // per channel: each hit's decays x this (1: as set), latched at the hit
    ofParameter<std::vector<float>> duration;
    // per channel: each hit's LP / HP (-1 low-pass closed .. 0 open .. +1 high-pass closed)
    ofParameter<std::vector<float>> filter;
    // per channel: each hit's stutter, a retrigger rate as a MIDI note while its gate is open (0: off)
    ofParameter<std::vector<float>> stutter;
    std::shared_ptr<ofxOceanodeAbstractParameter> gateHandle, durationHandle, filterHandle, stutterHandle;
    scScheduledInputs noteInputs;
    ofParameter<float> gain;
    ofParameter<int> channels;                     // N Chan
    scAudioParam pitchIn;

    // ── Pieces ──────────────────────────────────────────────────────────────
    Pad* addPad(int model = M_KICK, bool applyModel = true);
    void removePad(int k);
    void duplicatePad(int k);
    void movePad(int from, int to);
    void setupPad(Pad& p);
    void applyModel(Pad& p, int model, int style);   // the piece's sound from Model / Style
    static const std::vector<std::string>& modelNames();
    static std::vector<std::string> styleNames(int model);
    int padChannel(const Pad& p) const;
    std::string channelSource(int channel);
    std::string padLabel(const Pad& p) const;
    ofJson padState(int k) const;
    void applyPadState(int k, const ofJson& state);
    std::string relKey(const std::string& key) const;
    std::string absKey(const std::string& rel, int pad) const;
    void rewriteModKeys(ofJson& modJson, bool toRelative, int pad) const;

    // ── The model libraries: saved pad sounds, one list per Model, shared by
    //    every DrumKitBox (data/nodeSnapshots/DrumKitBox/Sounds/<Model>.json) ──
    struct Sound { std::string name; ofJson pad; };
    std::map<int, std::vector<Sound>> library;      // loaded on first use
    std::vector<Sound>& sounds(int model);
    std::string libraryPath(int model) const;
    void saveLibrary(int model);
    void storeSound(Pad& p, const std::string& name);   // the pad's sound, under its model
    void recallSound(Pad& p, const Sound& s);           // its sound (not its channel or mixing)
    void deleteSound(int model, const std::string& name);
    void stepSound(Pad& p, int dir);                    // < >: through the styles, then the saved sounds
    char soundNameBuf[64] = "";
    bool openStorePopup = false;

    // ── Sample layer ────────────────────────────────────────────────────────
    void loadSample(Pad& p, const std::string& path);
    void applyDecoded(Pad& p, scSlicerBox::Audio&& audio);
    void ensureBuffers(Pad& p, ofxSCServer*);
    void releaseBuffers(Sample& s, ofxSCServer*);

    // ── Synths ──────────────────────────────────────────────────────────────
    void createPad(ofxSCServer*, Pad& p);
    void destroyPad(ofxSCServer*, Pad& p, bool sendFree = true);
    void syncPad(Pad& p);
    void syncPadServer(ofxSCServer*, Pad& p);
    void syncLayers(ofxSCServer*, Pad& p);          // create / free the layers after their On
    void syncStages(ofxSCServer*, Pad& p);          // create / switch off the effects after their On
    void routeChain(ofxSCServer*, Pad& p);          // voice -> running effects -> strip
    float stageTail(const Pad& p, int fx) const;
    std::string stageDef(int fx) const;
    std::string layerDef(const Pad& p, int layer) const;
    void sendTrig(ofxSCServer*, Pad& p, bool includeNotes = true);
    void sendSound(ofxSCServer*, Pad& p, ofxSCSynth* s, int layer);   // a layer (-1: the voice)
    void sendStage(ofxSCServer*, Pad& p, int fx);
    void sendStrip(ofxSCServer*, Pad& p);
    void sendChannelInputs(Pad& p, bool pitchOnly = false);
    void sendChokeMasks();
    void sendMutes();
    void sendDucks();
    void rebuildAll();
    void fire(Pad& p);
    void requireSynthdefs();
    bool anySolo() const;
    float valueFor(const std::vector<float>& v, int channel, float fallback) const;
    float echoSeconds(const Pad& p) const;
    float beatsToSeconds(float beats) const;

    // ── Controls, publishing ────────────────────────────────────────────────
    template<typename T> Control& registerPadControl(Pad& p, ofParameter<T>& param, const Spec& spec);
    void registerModControl(Pad& p, ofAbstractParameter& param, std::function<std::shared_ptr<ofxOceanodeAbstractParameter>()> add,
                            std::function<std::string()> save, std::function<void(const std::string&)> load);
    Control* findControl(const std::string& key);
    std::map<std::string, std::shared_ptr<ofxOceanodeAbstractParameter>> published;
    scDropdowns dropdowns;
    scSnapshots snaps;
    ofJson initialPad;
    ofJson appliedKit;
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
    scTimelineMenu timeline;
    void publish(const std::string&);
    void unpublish(const std::string&, bool force = false);
    bool isConnected(const std::string&) const;
    void drawPublishMenu(const std::string&);
    void drawPublishedUnderline(const std::string&) const;

    // ── Window ──────────────────────────────────────────────────────────────
    float browserWidth = 230.0f;
    float envViewHeight = 0.0f;
    bool windowHovered = false;
    bool pendingLoadDialog = false;
    std::string pendingDropFile;
    int pendingMoveFrom = -1, pendingMoveTo = -1;
    int pendingRemove = -1, pendingDuplicate = -1, pendingAdd = -1;
    int renaming = -1;
    int hoveredPad = -1;
    int envDrag = -1;                 // the envelope view's handle being dragged
    char renameBuf[64] = "";
    float labelW = 74.0f;
    void drawWindow();
    void drawPadList(float w, float h);
    void drawHeader(Pad& p, float w);
    void drawSoundBrowser(Pad& p);
    void drawEnvelopeView(Pad& p, float w);
    void drawLayersTab(Pad& p, float w);
    void drawVoiceTab(Pad& p, float w);
    void drawMixerTab(float w);
    void drawStrip(Pad& p, float w, float h);
    void drawEffects(Pad& p, float w);
    void drawEqEditor(Pad& p, float w, float h);
    void drawSampleWave(Pad& p, float w, float h);
    void drawSection(Pad& p, const char* section, float w);   // every spec of a section, in order
    void drawSpec(Pad& p, const Spec& s, float w);
    void sectionHeader(const char* title, float w, ofParameter<bool>* on = nullptr, ImU32 accent = 0);
    void slider(ofParameter<float>& p, const char* label, float w, const char* fmt = "%.2f", bool logarithmic = false, const char* tip = nullptr);
    void sliderInt(ofParameter<int>& p, const char* label, float w, const char* fmt = "%d", const char* tip = nullptr);
    void combo(ofParameter<int>& p, const char* label, float w, const std::vector<std::string>& items, const char* tip = nullptr);
    void toggle(ofParameter<bool>& p, const char* label, const char* tip = nullptr);
    void beginColumn(const char* id);
    void endColumn(bool last = false);
    void playButton(Pad& p, const char* id, ImVec2 size);
};
