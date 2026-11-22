#pragma once

#include "../../../scNode.h"
#include "../shared/scKeyboardPiano.h"
#include "../../../serverManager.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "ofxSCGroup.h"
#include "../shared/scAudioParam.h"
#include "../../../timing/scScheduledInputs.h"
#include "../../../ui/scTimelineMenu.h"
#include "../shared/scModulation.h"
#include "../shared/scSnapshots.h"
#include "../shared/scBoxMacroMorph.h"
#include "../../../ui/scEQEditor.h"
#include "../../../ui/scDropdowns.h"
#include <array>
#include <map>
#include <memory>
#include <random>

// AnimaBox: the Reaktor ensemble AnimaMundi (Santi Vilanova) as a Box synth.
// A Body (oscillators or the live input, an ADSR on the note) excites up to 120
// bands, each a pair of 4-pole band-pass filters tuned to a degree of the Scale
// (an outer node: ChordBox, SC Pitch...) and placed on n speakers. The bands'
// energy and resonance move with the shared LFOs (bands = their voices), routed
// in the Routing matrix. Then Reaktor's Post Filter, and the effects (Delay and
// MetaVerb from the ensemble, plus the Box ones), each switchable.
// Specs of the decoded ensemble: tools/animabox/reaktor/spec_*.md.
class scAnimaBox : public scNode {
    scKeyboardPiano keyboardPiano;
public:
    static constexpr int MAX_BANDS = 120, CHUNK = 16, MAX_CHUNKS = (MAX_BANDS + CHUNK - 1) / CHUNK;
    explicit scAnimaBox(std::vector<serverManager*> servers);
    ~scAnimaBox();
    void setup() override;
    void update(ofEventArgs&) override;
    void draw(ofEventArgs&) override;
    void setBpm(float bpm) override;
    void activate() override;
    void deactivate() override;
    void applyGraphSuspended(ofxSCServer* server, bool suspended) override;
    bool keepsGraphAlive() const override { return mod.hasLfoOutputs(); }
    void buildSynth(ofxSCServer*) override;
    void createSynth(ofxSCServer*) override;
    void free(ofxSCServer*) override;
    void moveSynthBefore(ofxSCServer*, int) override;
    int getLastSynthID(ofxSCServer*) override;
    void setOutputBus(ofxSCServer*, int, int) override;
    int getOutputBusIndex(ofxSCServer*, int) override;
    void setInputBus(ofxSCServer*, scNode*, int) override;
    void resetInputBusses(ofxSCServer*, int) override;
    void resendParametersForNRT() override;
    void presetSave(ofJson&) override;
    void loadBeforeConnections(ofJson&) override;
    void presetRecallAfterSettingParameters(ofJson&) override;
    MorphKeys morphKeys() const override;
    MorphHint morphHint(const ofxOceanodeAbstractParameter& p) const override;
    bool morphCheck(const ofJson& target, std::string& reason) const override;
    bool morphBegin(const ofJson& target, std::string& reason) override;
    void morphApply(float t, bool switched) override { macroMorph.apply(snaps, t, switched); }
    void morphEnd(bool completed) override { macroMorph.end(snaps, completed); }
    std::string getWindowTitle() override { return "AnimaBox " + ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return &show; }
#ifdef OFXOCEANODE_HAS_TIMELINE_INSTRUMENT
    bool getTimelineInstrument(ofxOceanodeTimelineInstrument&) override;
#endif
private:
    // modulation targets: one value per band (bands are the modulation's voices)
    enum Target { Energy, Reso, Pitch, NUM_TARGETS };
    struct SoundControl {
        ofParameter<float> p;
        std::string group, help;
        bool discrete = false;
    };
    struct Control {
        std::string key;
        ofAbstractParameter* param = nullptr;
        std::function<std::shared_ptr<ofxOceanodeAbstractParameter>()> add;
        std::function<ofJson()> save;
        std::function<void(const ofJson&)> load;
        bool discrete = false;
    };
    enum Stage { StSat, StEq, StDelay, StVerb, StRvb, NumStages };
    struct Fx { ofxSCSynth* synth = nullptr; ofxSCBus* in = nullptr; bool running = false; float offAt = -1; };
    enum Slot { SlBody, SlSrc, SlRoute, SlBank, SlPost, SlFx, NumSlots = SlFx + NumStages };
    struct State {
        ofxSCGroup* root = nullptr;
        std::array<ofxSCGroup*, NumSlots> groups{};
        std::array<Fx, NumStages> fx{};
        ofxSCSynth* body = nullptr;
        ofxSCSynth* post = nullptr;
        std::array<ofxSCSynth*, MAX_CHUNKS> bank{};
        std::array<std::map<std::string, std::vector<float>>, MAX_CHUNKS> sent;   // per chunk: only changes
        std::map<std::string, float> bodySent, postSent;
        ofxSCBus* gates = nullptr;      // audio, 128: the note gate fanned to every band (envelopes)
        ofxSCBus* exc = nullptr;        // audio, 2: the Body
        ofxSCBus* bankOut = nullptr;    // audio, channels
        ofxSCBus* vis = nullptr;        // control: band levels (128), Body (4)
        int out = -1, builtBands = 0, builtChannels = 0, liveBus = -1, liveChannels = 1;
        bool created = false;
    };
    static constexpr int VIS_BODY = 128, VIS_SIZE = 132;

    std::vector<serverManager*> servers;
    std::map<ofxSCServer*, State> states;
    ofParameter<bool> show;
    ofParameter<void> resetLFOs;
    ofParameter<int> bands, channels;
    ofParameter<bool> drone;
    ofParameter<float> master;
    ofParameter<std::vector<float>> gate, pitch, scale;
    scAudioParam gateIn, pitchIn;
    std::shared_ptr<ofxOceanodeAbstractParameter> scaleHandle;
    bool scaleConnected() const { return scaleHandle && scaleHandle->hasInConnection(); }
    scScheduledInputs scheduled;
    scModulation mod;
    scSnapshots snaps;
    scBoxMacroMorph macroMorph;
    scDropdowns dropdowns;
    scTimelineMenu timeline;
    std::vector<std::unique_ptr<SoundControl>> sound;
    std::map<std::string, SoundControl*> soundByName;
    std::vector<Control> controls;
    std::map<std::string, std::shared_ptr<ofxOceanodeAbstractParameter>> published;
    std::vector<std::string> publishedKeys;
    // effects (the Box ones: Saturation, EQ, Reverb; the ensemble's: Delay, MetaVerb)
    ofParameter<bool> satOn, eqOn, delayOn, verbOn, rvbOn;
    ofParameter<float> eqGain[5], eqFreq[5], eqShape[5];
    scEQEditor eqEditor;
    ofParameter<float> satDrive, satBias, satTone, satOutput, satMix;
    ofParameter<float> rvbMix, rvbDecay, rvbSize, rvbPredelay, rvbLate, rvbSpread, rvbLowpass, rvbHiDamp, rvbLoDamp, rvbModRate, rvbSpin, rvbWander;

    // the bands' pitches and everything static per band (computed here, sent on change)
    struct BandState {
        std::array<float, MAX_BANDS> pitch{}, idx{}, idxNorm{}, detune{}, vibRate{}, panA{}, panB{}, damp{};
        std::array<bool, MAX_BANDS> sounding{};
    } bandState;
    std::mt19937 rng{12345};
    std::array<float, MAX_BANDS> detuneDraw{}, vibDraw{}, idxRandDraw{}, octDraw[3]{};
    float transRand = 0, lastNote = 60, lastGateLevel = 0;
    double clockBeats = 0; long long lastStep = -1;
    float bpm = 120, nextRead = 0;
    bool separator = false, restoring = false, structureDirty = false, bandsDirty = true, soundDirty = false, stagesDirty = false;
    int placeBefore = -1;
    std::vector<float> live;
    ofJson initial;
    float labelW = 70;

    template<class T> void reg(ofParameter<T>&, std::function<void()>);
    SoundControl& add(const std::string& name, float def, float lo, float hi, const std::string& group,
                      std::function<void()> send, std::vector<std::string> options = {}, const std::string& help = "");
    float v(const std::string& name) const;
    ofParameter<float>& P(const std::string& name);
    void publish(const std::string&);
    void unpublish(const std::string&);
    bool connected(const std::string&) const;
    void menu(const std::string&);
    void mark(const std::string&);
    // pitch model (spec_pitch.md)
    void redrawDetune();
    void redrawIndexRandom();
    void computeBands();
    float pitchIndex(int voice, int count) const;
    // server
    void sendBody(ofxSCServer*, State&);
    void sendBank(ofxSCServer*, State&);
    void sendPost(ofxSCServer*, State&);
    void sendAll() { soundDirty = true; }
    void flushSound();
    void refreshOutputs() { for(auto& output : outputs) output = output; }
    void release(ofxSCServer*, State&);
    void rebuild();
    void requireSynthdefs();
    void updateStages(ofxSCServer*, State&);
    void routeChain(State&);
    float stageOn(int) const;
    std::string stageDef(int) const;
    float stageTail(int) const;
    void sendStage(int, ofxSCSynth*, ofxSCServer*);
    // snapshots
    ofJson capture() const;
    void apply(const ofJson&, const ofJson&, float, bool);
    std::vector<ofJson> factory() const;
    ofJson snapshotOf(const ofJson& saved) const;
    // window
    void drawWindow();
    void drawHeader();
    void drawSoundTab(float w);
    void drawBandsTab(float w);
    void drawFxTab(float w);
    void drawBandView(float w, float h);
    void drawIndexView(float w, float h);
    void sectionHeader(const char*, float, ofParameter<bool>* = nullptr, ImU32 = 0);
    void slider(ofParameter<float>&, const char*, float, const char* = "%.2f", bool = false);
    void sliderInt(ofParameter<int>&, const char*, float);
    void toggle(ofParameter<bool>&, const char*);
    void control(const std::string& name, float w, const char* label = nullptr, const char* fmt = "%.2f", bool log = false);
    void scaleEditor(float w);
};
