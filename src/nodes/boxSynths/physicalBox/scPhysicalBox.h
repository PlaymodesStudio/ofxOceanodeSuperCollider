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
#include "../../../ui/scEQEditor.h"
#include "../../../ui/scDropdowns.h"
#include "../shared/scPulseGate.h"
#include "../../../ui/scSectionHelp.h"
#include "scPhysicalIcons.h"
#include <array>
#include <map>
#include <memory>

// PhysicalBox: a physical-modelling synth. Each voice is one PhysicalVoice
// (ugens/OceanodePhysical): strike / bow / blow / scrape / external exciters
// into a waveguide (string <> tube) and a modal body (bar .. membrane),
// coupled, with rattle / tension / cascade and two pickups. Around the voices
// the shared modular parts: ModGate / Mod sources / routes (per voice), the
// stereo-pickup spreader, the effects (MixerPro inserts + MicroSpace's Space).
class scPhysicalBox : public scNode {
    scKeyboardPiano keyboardPiano;
public:
    explicit scPhysicalBox(std::vector<serverManager*> servers);
    ~scPhysicalBox();
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
    int getOutputNumChannels(int) override { return channels.get(); }
    void setInputBus(ofxSCServer*, scNode*, int) override;
    void resetInputBusses(ofxSCServer*, int) override;
    void resendParametersForNRT() override;
    void presetSave(ofJson&) override;
    void loadBeforeConnections(ofJson&) override;
    void presetRecallAfterSettingParameters(ofJson&) override;
    std::string getWindowTitle() override { return "PhysicalBox " + ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return &show; }
#ifdef OFXOCEANODE_HAS_TIMELINE_INSTRUMENT
    // Gate / Pitch, one note per voice (timeline piano rolls); the gate's level is the velocity
    bool getTimelineInstrument(ofxOceanodeTimelineInstrument&) override;
#endif
private:
    // server-side modulation targets (per voice); Pan is the spreader's
    enum Target { Pitch, Material, Excite, Position, Hardness, BowPressure, BowSpeed, Breath, ScrapeSpeed,
                  Brightness, Decay, Shape, Couple, Cascade, Amp, Pan, NUM_TARGETS };
    struct SoundControl {
        ofParameter<float> p;
        std::string sc, group, fmt = "%.2f";
        bool discrete = false, log = false, voiceInput = true;   // voiceInput: a PhysicalVoice input
    };
    struct Control {
        std::string key;
        ofAbstractParameter* param = nullptr;
        std::function<std::shared_ptr<ofxOceanodeAbstractParameter>()> add;
        std::function<ofJson()> save;
        std::function<void(const ofJson&)> load;
        bool discrete = false;
    };
    enum Stage { StSat, StEq, StEcho, StRvb, StSpace, NumStages };
    struct Fx { ofxSCSynth* synth = nullptr; ofxSCBus* in = nullptr; bool running = false; float offAt = -1; };
    // one group per part, in running order; the effects after the spreader
    enum Slot { SlGate, SlSrc, SlRoute, SlVoice, SlSpread, SlFx, NumSlots = SlFx + NumStages };
    struct State {
        ofxSCGroup* root = nullptr;
        std::array<ofxSCGroup*, NumSlots> groups{};
        std::array<Fx, NumStages> fx{};
        ofxSCSynth* gate = nullptr;
        ofxSCSynth* spread = nullptr;
        std::array<ofxSCSynth*, 16> voices{};
        std::array<std::map<std::string, float>, 16> sent;   // per voice: only changes are sent
        ofxSCBus* gates = nullptr;
        ofxSCBus* audio = nullptr;      // two channels (pickups) per voice, into the spreader
        ofxSCBus* silent = nullptr;     // nobody writes it: the Ext input when nothing is wired
        ofxSCBus* vis = nullptr;
        int out = -1, in = -1, inChannels = 1, builtVoices = 0, builtChannels = 0;
        bool created = false;
        int droneSent = -1;
    };
    static constexpr int VIS_VOICE = 4;                // per voice: level, gate, note, -
    static constexpr int VIS_PAN = 16 * VIS_VOICE;     // the spreader's 16 positions
    static constexpr int VIS_PULSE = VIS_PAN + 16;     // per voice: event count, last length, last velocity
    static constexpr int VIS_SIZE = VIS_PULSE + scPulseGate::VIS_SIZE;

    // effects
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
    ofParameter<float> satDrive, satBias, satTone, satOutput, satMix;
    ofParameter<bool> spaceOn, spaceFreeze;
    ofParameter<float> spaceMix, spaceTime, spaceDamp, spaceDiff, spaceLowCut;

    // Gate Mode Pulse: the automatic gates (scPulseGate)
    scPulseGate pulse;
    bool pulsing() const { return gateMode.get() == 3; }
    void startPulse(ofxSCServer*, State&);
    // Panning: where each voice (its two pickups) sits on the speakers
    ofParameter<int> placement, speakerLayout;
    ofParameter<float> spreadCenter;
    ofParameter<std::vector<float>> panP;
    int panDragVoice = -1;
    std::vector<float> panPositions() const;
    void drawPanTab(float);
    void sliderVec(ofParameter<std::vector<float>>&, const char*, float, const char* = "%.2f");

    float labelW = 70;
    void drawSoundTab(float);
    void drawHeader();
    void drawDisplay(float);
    void drawPad(ImVec2, float);
    void drawObject(ImVec2, ImVec2);
    void drawSurface(ImVec2 a, float side, int kind, float presence);
    void drawMeters(ImVec2, ImVec2);
    bool droning() const;
    void sliderInt(ofParameter<int>&, const char*, float);
    void combo(ofParameter<int>&, const char*, float);
    void soundSlider(const std::string& sc, const char* label, float w);
    void soundGroup(const std::string& group, float w);
    ofParameter<float>& soundParam(const std::string&);
    SoundControl* sound(const std::string&);
    void drawFxTab(float);
    // icon: physIcons::Icon, or BODY_ICON for the body's current model
    static constexpr int BODY_ICON = 1000;
    void sectionHeader(const char*, float, ofParameter<bool>* = nullptr, ImU32 = 0, const char* help = nullptr, int icon = -1);
    void drawBodyPicture(float);
    void slider(ofParameter<float>&, const char*, float, const char* = "%.2f", bool = false);
    void toggle(ofParameter<bool>&, const char*);
    void updateStages(ofxSCServer*, State&);
    void routeChain(State&);
    float stageOn(int) const;
    std::string stageDef(int) const;
    float stageTail(int) const;
    float echoDelaySec() const;
    void sendStage(int, ofxSCSynth*, ofxSCServer*);
    void sendStageAll(int);

    std::vector<serverManager*> servers;
    std::map<ofxSCServer*, State> states;
    ofParameter<bool> show;
    ofParameter<void> resetLFOs;
    ofParameter<int> voices, channels, gateMode;
    bool gateTouched = false;
    ofParameter<float> master;
    ofParameter<std::vector<float>> gate, pitch;
    scAudioParam gateIn, pitchIn;
    scScheduledInputs scheduled;
    scModulation mod;
    scSnapshots snaps;
    scDropdowns dropdowns;
    scTimelineMenu timeline;
    std::vector<std::unique_ptr<SoundControl>> soundControls;
    std::vector<Control> controls;
    std::map<std::string, std::shared_ptr<ofxOceanodeAbstractParameter>> published;
    std::vector<std::string> publishedKeys;
    bool separator = false, restoring = false, structureDirty = false;
    int placeBefore = -1;      // rebuild: the old root the new one goes before
    float bpm = 120, nextRead = 0;
    std::vector<float> live;
    const ofJson* modSource = nullptr;
    ofJson initial;
    template<class T> void reg(ofParameter<T>&, std::function<void()>);
    void addSound(const std::string& name, const std::string& sc, float def, float lo, float hi,
                  const std::string& group, const char* fmt = "%.2f", bool log = false, bool discrete = false);
    void publish(const std::string&);
    void unpublish(const std::string&);
    bool connected(const std::string&) const;
    void menu(const std::string&);
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
};
