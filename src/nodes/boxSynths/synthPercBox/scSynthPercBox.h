#pragma once

#include "../../../scNode.h"
#include "../../../serverManager.h"
#include "../shared/scAudioParam.h"
#include "../shared/scSnapshots.h"
#include "../../../timing/scScheduledInputs.h"
#include "scSynthPercBoxData.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "ofxSCGroup.h"
#include <array>
#include <map>
#include <string>
#include <vector>

// SynthPercBox: the Reaktor ensemble Bits.ens (Santi Vilanova, 2008) as a Box. Six percussion
// generators (K1, STOMPS, NOIX, CLIX, CRAX, QUAD), each with 16 pads: a pad is a whole sound of
// its generator. Click a pad: it plays; Shift+click: the generator's current sound is stored in it.
// The window shows every generator at once, one row each (pads, then its controls).
//  • Playing: Gate / Pitch (values or audio cables, 8 channels).
//      Input Kit (default): Gate channel 1..6 plays generator 1..6, its Pitch picks the pad
//        (60 = pad 1, 61 = pad 2...): the timeline's Drum Grid (a row per generator, a pad per hit).
//      Input Notes: Reaktor's key ranges (K1 0-13, STOMPS 14-27, NOIX 28-41, CLIX 43-56,
//        CRAX 59-72, QUAD 73-86), a key per pad, for a MIDI keyboard.
//    The pad is taken on the server at the gate's rising edge (sample accurate, cables too).
//    Gate level = velocity.
//  • Editing a control changes the generator's live sound (the pad stays as stored) until the
//    next hit; Shift+click a pad to keep it. Pads, names and the live sounds are saved with the patch
//    and in the snapshot bar (the whole kit).
//  • Synths: synthdefs/Defaults/SynthPercBox (SPIn, SPK1, SPStomps, SPNoix, SPClix, SPCrax, SPQuad,
//    SPSpace1/2). Factory pads: the ensemble's snapshots (tools/synthpercbox/make_factory.py).
class scSynthPercBox : public scNode {
public:
    static constexpr int GENS = 6, PADS = 16, ROWS = 17, LIVE = 16, NV = 8;
    explicit scSynthPercBox(std::vector<serverManager*> servers);
    ~scSynthPercBox();
    void setup() override;
    void update(ofEventArgs&) override;
    void draw(ofEventArgs&) override;
    void setBpm(float bpm) override;
    void activate() override;
    void deactivate() override;
    void applyGraphSuspended(ofxSCServer* server, bool suspended) override;
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
    void presetRecallAfterSettingParameters(ofJson&) override;
    std::string getWindowTitle() override { return "SynthPercBox " + ofToString(getNumIdentifier()); }
    ofParameter<bool>* getWindowToggle() override { return &show; }
#ifdef OFXOCEANODE_HAS_TIMELINE_INSTRUMENT
    bool getTimelineInstrument(ofxOceanodeTimelineInstrument&) override;
#endif

private:
    // a generator: its pads (rows 0..15) and its live sound (row 16), as the synth's \p_<key> arrays
    struct Gen {
        const synthPercData::Generator* data = nullptr;
        std::array<std::vector<float>, ROWS> rows;
        std::array<std::string, PADS> names;
        std::array<bool, PADS> stored{};      // holds a sound someone made (factory or Shift+click)
        int shown = 0;                        // the pad the controls show (LIVE: the edited sound)
        bool mute = false;
        float flash[PADS] = {};               // when each pad last played (the window lights it)
        int held = -1;                        // the pad the mouse holds down
        int param(const std::string& key) const;
        float value(int k) const { return rows[shown][k]; }
    };
    struct State {
        ofxSCGroup* root = nullptr;
        ofxSCSynth* in = nullptr;
        std::array<ofxSCSynth*, GENS> gen{};
        std::array<ofxSCSynth*, GENS> space{};
        ofxSCBus* inBus = nullptr;                 // audio 16: SPIn's gates + pitches
        std::array<ofxSCBus*, GENS> dry{};         // audio: a reverb generator's signal into its Space
        std::array<ofxSCBus*, GENS> pbus{};        // control 9: its Space settings
        ofxSCBus* rowBus = nullptr;                // control 6: the row each generator plays
        std::array<int, GENS> lastRow{};
        int out = -1;
        bool created = false;
        float nextRead = 0;
        std::map<std::string, std::vector<float>> inSent;
    };
    static int spaceChannels(int g) { return g == 2 || g == 3 ? 1 : g == 5 ? 2 : 0; }   // NOIX, CLIX: mono; QUAD: stereo

    std::vector<serverManager*> servers;
    std::map<ofxSCServer*, State> states;
    std::array<Gen, GENS> gens;
    ofParameter<bool> show;
    ofParameter<int> inputMode;
    ofParameter<float> gainDb;
    ofParameter<std::vector<float>> gate, pitch;
    scAudioParam gateIn, pitchIn;
    scScheduledInputs scheduled;
    scSnapshots snaps;
    float bpm = 120;

    void requireSynthdefs();
    void resetToFactory(int g);
    void release(ofxSCServer*, State&);
    // sends
    void sendIn(ofxSCServer*, State&);
    void sendGen(ofxSCServer*, State&, int g, bool table);
    void sendParam(int g, int k);
    void sendAllGens(bool table);
    float gainLinear() const;
    // editing
    void edit(int g, int k, float v);
    void storePad(int g, int pad);
    void press(int g, int pad);
    void releasePress(int g);
    void randomize(int g);
    // snapshots / presets
    ofJson capture() const;
    void load(const ofJson& j);
    // window
    void drawWindow();
    void drawRow(int g, float w);
    bool knob(const char* id, float& v, float lo, float hi, float step, const char* label, const char* tip, float defValue);
    bool envelope(int g, const std::vector<int>& ks, float w, float h);
};
