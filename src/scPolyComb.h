#pragma once

#include "ofxOceanodeNodeModel.h"
#include "ofxOceanodeShared.h"
#include "scNode.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "baseIndexer.h"
#include "imgui.h"
#include <vector>
#include <array>
#include <map>
#include <numeric>
#include <algorithm>
#include <random>

class scPolyComb : public scNode {
public:
    static constexpr int MAX_VOICES       = 16;
    static constexpr int NUM_LFOS         = 4;
    static constexpr int NUM_MOD_SOURCES  = 8;
    static constexpr int NUM_MOD_TARGETS  = 8;

    scPolyComb();
    ~scPolyComb();

    void setup()              override;
    void update(ofEventArgs&) override;
    void draw(ofEventArgs&)   override;
    void setBpm(float bpm)    override;
    void activate()           override;
    void deactivate()         override;

    void buildSynth(ofxSCServer* s)                     override;
    void createSynth(ofxSCServer* s)                    override;
    void free(ofxSCServer* s)                           override;
    void moveSynthBefore(ofxSCServer* s, int nodeID)    override;
    int  getLastSynthID(ofxSCServer* s)                 override;
    void setOutputBus(ofxSCServer* s, int idx, int bus) override;
    void setInputBus(ofxSCServer*, scNode*, int)        override {}
    void resetInputBusses(ofxSCServer*, int)            override {}

    void presetSave(ofJson& j)                         override;
    void presetRecallAfterSettingParameters(ofJson& j) override;

private:
    // ── SC resources ─────────────────────────────────────────────────────────
    std::map<ofxSCServer*, ofxSCSynth*>                         synthInstances;
    std::map<ofxSCServer*, std::map<int,int>>                   outputBuses;
    std::map<ofxSCServer*, std::array<ofxSCBus*, NUM_LFOS>>     lfoBuses;

    // ── Core ─────────────────────────────────────────────────────────────────
    ofParameter<int>           nVoices;
    ofParameter<float>         masterLevel;
    ofParameter<void>          resetLFOs;

    // ── Oscillators ──────────────────────────────────────────────────────────
    ofParameter<vector<float>> triLevel, sawLevel, pulseLevel, sineLevel;
    ofParameter<vector<float>> whiteNoiseLevel, pinkNoiseLevel, dustLevel;
    ofParameter<vector<float>> superSawLevel, superSawDetune, impulseLevel;
    ofParameter<vector<float>> pulseWidth, oscAmp;

    // ── Pitch ─────────────────────────────────────────────────────────────────
    ofParameter<vector<float>> pitch, pitchMod, pitchModRange, pitchDetune, pitchTranspose;
    ofParameter<bool>          pitchScramble;
    ofParameter<vector<float>> vibFreq, vibAmp;

    // ── FM ───────────────────────────────────────────────────────────────────
    ofParameter<vector<float>> fmRatio, fmAmp, fmMod;

    // ── Filter ───────────────────────────────────────────────────────────────
    ofParameter<vector<float>> filterType, cutoffMin, cutoffMax, cutoffMod, resonance;

    // ── Comb ─────────────────────────────────────────────────────────────────
    ofParameter<vector<float>> combPitch, combPitchMod, combPMRange, combDetune, combTranspose, combFeed, combMix;

    // ── Ring Mod ─────────────────────────────────────────────────────────────
    ofParameter<vector<float>> ringModPitch, ringModDetune, ringModMix;

    // ── Post Filter ──────────────────────────────────────────────────────────
    ofParameter<vector<float>> postFType, postCutMin, postCutMax, postCutMod, postRes;

    // ── Tremolo ──────────────────────────────────────────────────────────────
    ofParameter<vector<float>> minTremHz, maxTremHz, tremHzMod, tremAmp;

    // ── Sine Shaper ──────────────────────────────────────────────────────────
    ofParameter<vector<float>> ssLevel, ssMix;

    // ── LFOs: per-voice audio params ─────────────────────────────────────────
    std::array<ofParameter<vector<float>>, NUM_LFOS> lfoFreq;
    std::array<ofParameter<vector<float>>, NUM_LFOS> lfoFreqDetune;
    std::array<ofParameter<float>, NUM_LFOS>         lfoInitPhase;
    std::array<ofParameter<vector<float>>, NUM_LFOS> lfoPhOff;   // internal — computed, sent to SC
    std::array<ofParameter<vector<float>>, NUM_LFOS> lfoRound;
    std::array<ofParameter<vector<float>>, NUM_LFOS> lfoSkew;
    std::array<ofParameter<vector<float>>, NUM_LFOS> lfoPulseWidth;
    std::array<ofParameter<bool>, NUM_LFOS>          lfoPulseCentered;
    std::array<ofParameter<vector<float>>, NUM_LFOS> lfoPow;
    std::array<ofParameter<vector<float>>, NUM_LFOS> lfoBiPow;
    std::array<ofParameter<vector<float>>, NUM_LFOS> lfoSampleHold;
    std::array<ofParameter<vector<float>>, NUM_LFOS> extLFO;

    // ── LFO outputs (live KR data → Oceanode output ports) ──────────────────
    std::array<ofParameter<vector<float>>, NUM_LFOS> lfoOut;
    std::array<vector<float>, NUM_LFOS>              lfoDisplayData;  // mirror for GUI

    // ── LFO phase indexers (one baseIndexer per LFO) ─────────────────────────
    std::array<baseIndexer, NUM_LFOS>        lfoIndexers;
    std::array<ofParameter<float>, NUM_LFOS> lfoNWaves;
    std::array<ofParameter<bool>,  NUM_LFOS> lfoNorm;
    std::array<ofParameter<float>, NUM_LFOS> lfoInvertP;
    std::array<ofParameter<int>,   NUM_LFOS> lfoSymP;
    std::array<ofParameter<float>, NUM_LFOS> lfoShuffleP;
    std::array<ofParameter<float>, NUM_LFOS> lfoRandP;
    std::array<ofParameter<float>, NUM_LFOS> lfoOffsetP;
    std::array<ofParameter<int>,   NUM_LFOS> lfoQuantP;
    std::array<ofParameter<float>, NUM_LFOS> lfoCombP;
    std::array<ofParameter<int>,   NUM_LFOS> lfoModuloP;

    // ── ModMatrix ────────────────────────────────────────────────────────────
    ofParameter<vector<float>> modMatrix;

    // ── GUI state ─────────────────────────────────────────────────────────────
    ofParameter<bool>  showWindow;
    ofParameter<float> windowWidth, windowHeight;

    // ── GUI draw methods ──────────────────────────────────────────────────────
    void drawPolyCombWindow();
    void drawPitchSection(float w);
    void drawOscSection(float w);
    void drawFMSection(float w);
    void drawFilterSection(float w, bool isPost);
    void drawCombSection(float w);
    void drawRingModSection(float w);
    void drawTremoloSection(float w);
    void drawSineShaperSection(float w);
    void drawLFOBank(float w, float h);
    void drawModMatrixGrid(float w, float h);
    void drawLFOIndexerControls(int li);

    // ── Helpers ───────────────────────────────────────────────────────────────
    string   getSynthDefName() const;
    void     sendFloatParam(const string& name, vector<float>& v);
    void     sendScalarParam(const string& name, float value);
    void     sendAllParams(ofxSCSynth* s);
    void     sendLFOBusIndices(ofxSCServer* server, ofxSCSynth* s);
    vector<float> convertBeatDurationsToHz(const vector<float>& beatValues) const;
    vector<float> convertBeatDivisionsToHz(const vector<float>& beatValues) const;
    vector<float> getProcessedPitchValues() const;
    void     updatePitchScrambleOrder();
    vector<float> computePreviewLFOValues(int li) const;
    void     resizeVoiceParams(int n);
    void     rebuildSynths();
    void     updateLFOPhaseOffsets();
    void     allocateLFOBuses(ofxSCServer* server);
    void     freeLFOBuses(ofxSCServer* server);

    // ── Listeners ─────────────────────────────────────────────────────────────
    ofEventListeners listeners;
    int oldNVoices = 0;
    float currentBpm = 120.0f;
    float lfoPreviewResetTime = 0.0f;
    int lfoResetCounter = 0;
    vector<int> pitchScrambleOrder;

    // ── Mod matrix drag state ─────────────────────────────────────────────────
    int activeMatrixCell = -1;
};
