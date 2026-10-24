#pragma once

#include "scNode.h"
#include "scKeyboardPiano.h"
#include "serverManager.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "ofxSCGroup.h"
#include "scAudioParam.h"
#include "scScheduledInputs.h"
#include "scTimelineMenu.h"
#include "scModulation.h"
#include "scSnapshots.h"
#include "scEQEditor.h"
#include "scDropdowns.h"
#include "scDX7Afx.h"
#include <array>
#include <map>
#include <memory>

// DX7-Supercollider-based instrument with the shared Box editor, modulation and effects.
class scFMBox : public scNode {
    scKeyboardPiano keyboardPiano;
public:
    explicit scFMBox(std::vector<serverManager*> servers);
    ~scFMBox();
    void setup() override;
    void update(ofEventArgs&) override;
    void draw(ofEventArgs&) override;
    void setBpm(float bpm) override;
    void activate() override;
    void deactivate() override;
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
    std::string getWindowTitle() override { return "fmBox " + ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return &show; }
#ifdef OFXOCEANODE_HAS_TIMELINE_INSTRUMENT
    // Gate / Pitch / Velocity, one note per voice (timeline piano rolls)
    bool getTimelineInstrument(ofxOceanodeTimelineInstrument&) override;
#endif
private:
    enum Target { Pitch, Scale, Feedback, Amp, Pan, Ratio = 5, Detune = 11, Level = 17, NUM_TARGETS = 23 };
    struct SoundControl {
        ofParameter<float> p;
        std::string sc, group;
    };
    struct Control {
        std::string key;
        ofAbstractParameter* param = nullptr;
        std::function<std::shared_ptr<ofxOceanodeAbstractParameter>()> add;
        std::function<ofJson()> save;
        std::function<void(const ofJson&)> load;
        bool discrete = false;
    };
    enum Stage {StSat,StEq,StEcho,StRvb,NumStages};
    struct Fx {ofxSCSynth* synth=nullptr;ofxSCBus* in=nullptr;bool running=false;float offAt=-1;};
    // one group per part, in running order; the effects after the spreader
    enum Slot { SlGate, SlNative, SlSrc, SlRoute, SlVoice, SlSpread, SlFx, NumSlots = SlFx + NumStages };
    struct State {
        ofxSCGroup* root = nullptr;
        std::array<ofxSCGroup*, NumSlots> groups{};
        std::array<Fx,NumStages> fx{};
        ofxSCSynth* gate = nullptr;
        ofxSCSynth* native = nullptr;
        ofxSCBus* nativeBus = nullptr;
        ofxSCSynth* spread = nullptr;
        std::array<ofxSCSynth*, 16> voices{};
        std::array<std::map<std::string,float>,16> sent;   // per voice: only changes are sent
        ofxSCBus* gates = nullptr;
        ofxSCBus* audio = nullptr;      // one channel per voice, into the spreader
        ofxSCBus* vis = nullptr;
        int out = -1, builtVoices = 0, builtChannels = 0;
        bool created = false;
        int droneSent = -1;
    };
    ofParameter<bool> eqOn;
    ofParameter<float> eqGain[5], eqFreq[5], eqShape[5];
    scEQEditor eqEditor;
    ofParameter<bool> echoOn, echoBeats;
    ofParameter<int> echoFilter;
    ofParameter<float> echoDelay, echoBeatVal, echoFeed, echoCutoff, echoResonance, echoPingPong, echoMix;
    ofParameter<bool> rvbOn;
    ofParameter<float> rvbMix, rvbDecay, rvbSize, rvbPredelay, rvbLate, rvbSpread, rvbLowpass,
                       rvbHiDamp, rvbLoDamp, rvbModRate, rvbSpin, rvbWander;

    ofParameter<bool> satOn;
    ofParameter<float> satDrive,satBias,satTone,satOutput,satMix;
    float labelW=70;
    void drawNativeEnvelope(int,float);
    void drawSoundTab(float);
    void drawHeader();
    bool droning() const;
    void sliderInt(ofParameter<int>&,const char*,float);
    void combo(ofParameter<int>&,const char*,float);   // its names: dropdowns
    ofParameter<float>& soundParam(const std::string&);
    void drawFxTab(float);
    void drawPanTab(float);
    std::vector<float> panPositions() const;
    void sliderVec(ofParameter<std::vector<float>>&,const char*,float,const char* = "%.2f",bool=false);
    ofParameter<int> spreadMode,spreadLayout;
    ofParameter<float> spreadCenter;
    ofParameter<std::vector<float>> panP;
    int panDragVoice=-1;
    void sectionHeader(const char*,float,ofParameter<bool>* =nullptr,ImU32=0);
    void slider(ofParameter<float>&,const char*,float,const char* = "%.2f",bool=false);
    void toggle(ofParameter<bool>&,const char*);
    void updateStages(ofxSCServer*,State&);
    void routeChain(State&);
    float stageOn(int) const;
    std::string stageDef(int) const;
    float stageTail(int) const;
    float echoDelaySec() const;
    void sendStage(int,ofxSCSynth*,ofxSCServer*);
    void sendStageAll(int);
    std::vector<serverManager*> servers;
    std::map<ofxSCServer*, State> states;
    ofParameter<bool> show;
    ofParameter<void> resetLFOs;
    ofParameter<int> voices, channels, gateMode;
    bool gateTouched=false;
    ofParameter<float> master;
    ofParameter<std::vector<float>> gate, pitch, velocity;
    scAudioParam gateIn, pitchIn, velIn;
    scScheduledInputs scheduled;
    scModulation mod;
    scSnapshots snaps;
    scDropdowns dropdowns;     // the combos' names (a published combo is a dropdown)
    scTimelineMenu timeline;
    std::vector<std::unique_ptr<SoundControl>> sound;
    std::vector<Control> controls;
    std::map<std::string, std::shared_ptr<ofxOceanodeAbstractParameter>> published;
    std::vector<std::string> publishedKeys;
    bool separator = false, restoring = false, structureDirty = false;
    int placeBefore = -1;      // rebuild: the old root the new one goes before
    float bpm = 120, nextRead = 0;
    int draggingPoint = -1, draggingOp = -1;
    std::vector<float> live;
    const ofJson* modSource = nullptr;
    ofJson initial;
    template<class T> void reg(ofParameter<T>&, std::function<void()>);
    void publish(const std::string&);
    void unpublish(const std::string&);
    bool connected(const std::string&) const;
    void menu(const std::string&);
    void controlMenuItems(const std::string&);
    void mark(const std::string&);
    void sendNotes(ofxSCServer*, State&);
    void sendSound(ofxSCServer*, State&);
    void sendAll();
    void refreshOutputs() { for(auto& output : outputs) output = output; }
    void release(ofxSCServer*, State&);
    void rebuild();
    void requireSynthdefs();
    ofJson capture() const;
    void apply(const ofJson&, const ofJson&, float, bool);
    std::vector<ofJson> factory() const;
    void drawWindow();
    void drawMatrix(float);
    void setAlgorithm(int);
    ofJson dx7Snapshot(const scDX7::Patch&, int) const;
    bool loadAFX(const std::string&, bool recallFirst=true);
    void recallAFX(int);
    void drawPresets(float);
    void drawDX7Global(float);
    std::vector<scDX7::Patch> afxBank;
    std::vector<scDX7::PresetName> afxNames;
    std::vector<scDX7::PresetBank> afxBanks;
    int afxBankSelection=0;
    std::string afxPath, afxMessage;
    ofParameter<int> afxPreset;
    char afxSearch[64]{};
};
