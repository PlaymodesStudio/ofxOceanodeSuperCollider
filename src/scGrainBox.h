//
//  scGrainBox.h
//  ofxOceanodeSuperCollider
//
//  Multichannel granular sampler with interactive waveform/envelope GUI.
//
//  Architecture:
//    • Inherits scNode — pure audio source (no In port).
//    • numChannels controls both grain-voice count and PanAz speaker count.
//    • One GrainBoxN SynthDef per channel count (N=1..16); compiled by
//      SPECIAL_SYNTHDEFS/KR/scGrainBox.scd.
//    • One synth + one sample buffer + one envelope buffer per SC server.
//    • One output port (N-channel wide bus, managed by serverManager).
//
//  Custom GUI (floating ImGui window):
//    • File browser with audio preview.
//    • Waveform display: shows loaded sample, per-channel grain highlights
//      (duration × amp intensity, color per channel), draggable inPoint/outPoint.
//    • Envelope editor: attack / release / shape parameters rendered as a
//      512-sample buffer uploaded to SC via /b_setn.
//

#pragma once

#include "ofxOceanodeNodeModel.h"
#include "ofxOceanodeShared.h"
#include "scNode.h"
#include "serverManager.h"
#include "ofxSuperCollider.h"
#include "ofxSCSynth.h"
#include "ofxSCBuffer.h"
#include "imgui.h"
#include <vector>
#include <string>
#include <map>
#include <deque>
#include <filesystem>

class scGrainBox : public scNode {
public:
    // ── Constants ─────────────────────────────────────────────────────────────
    static constexpr int   MAX_CHANNELS    = 16;
    static constexpr int   ENV_BUFFER_SIZE = 512;
    static constexpr int   WAVEFORM_BINS   = 4096;

    // ── Constructor / Destructor ──────────────────────────────────────────────
    scGrainBox(vector<serverManager*> servers);
    ~scGrainBox();

    // ── ofxOceanodeNodeModel overrides ────────────────────────────────────────
    void setup()              override;
    void update(ofEventArgs&) override;
    void draw(ofEventArgs&)   override;
    void activate()           override;
    void deactivate()         override;

    // ── scNode interface ──────────────────────────────────────────────────────
    void buildSynth(ofxSCServer* s)                     override;
    void createSynth(ofxSCServer* s)                    override;
    void free(ofxSCServer* s)                           override;
    void setOutputBus(ofxSCServer* s, int idx, int bus) override;
    int  getOutputBusIndex(ofxSCServer* s, int idx)     override;
    void moveSynthBefore(ofxSCServer* s, int nodeID)    override;
    int  getLastSynthID(ofxSCServer* s)                 override;
    // No audio inputs:
    void setInputBus(ofxSCServer*, scNode*, int)        override {}
    void resetInputBusses(ofxSCServer*, int)            override {}

    // ── Preset serialization ──────────────────────────────────────────────────
    void presetSave(ofJson& j)                          override;
    void presetRecallAfterSettingParameters(ofJson& j)  override;

private:
    // ── Servers ───────────────────────────────────────────────────────────────
    vector<serverManager*> allServers;

    // ── SC resources (per server) ─────────────────────────────────────────────
    std::map<ofxSCServer*, ofxSCSynth*>               synths;
    std::map<ofxSCServer*, std::vector<ofxSCBuffer*>> sampleBufs;  // one buf per sample channel
    std::map<ofxSCServer*, ofxSCBuffer*>              envBufs;
    std::map<ofxSCServer*, int>          outputBuses;
    std::map<ofxSCServer*, ofxSCBus*>    privateBuses;

    // ── Node GUI parameters ───────────────────────────────────────────────────
    ofParameter<bool>          showWindow;
    ofParameter<int>           numChannelsP;   // 1..MAX_CHANNELS

    // Trigger
    ofParameter<vector<int>>   triggerP;       // per-channel 0/1 impulse
    ofParameter<bool>          autoTrigP;      // global auto-trigger enable
    ofParameter<vector<float>> chanceP;        // trigger probability per channel (0..1)

    // Grain params
    ofParameter<vector<float>> ampP;           // grain amplitude (0..1)
    ofParameter<vector<float>> pitchP;         // pitch semitones (-48..48)
    ofParameter<bool>          dynamicDurP;    // dynamic duration toggle
    ofParameter<bool>          trigDurP;       // grain dur = inter-trigger period
    ofParameter<vector<float>> durationP;      // grain duration ms (1..10000)
    ofParameter<vector<float>> positionP;      // grain start position (0..1)
    ofParameter<vector<float>> panAzP;         // azimuth pan (0..2)

    // Jitter (always latched per spec)
    ofParameter<vector<float>> posJitP;        // position jitter (0..1)
    ofParameter<vector<float>> pitchJitP;      // pitch jitter (0..12 semitones)
    ofParameter<vector<float>> durJitP;        // duration jitter ms (0..2000)
    ofParameter<vector<float>> ampJitP;        // amplitude jitter (0..1)
    ofParameter<vector<float>> panAzJitP;      // pan azimuth jitter (0..2)
    ofParameter<vector<float>> levelsP;        // per-output-channel linear gain (0..1)

    // Region and envelope (live-interactive in GUI)
    ofParameter<float>         inPointP;       // accessible region start (0..1)
    ofParameter<float>         outPointP;      // accessible region end (0..1)
    ofParameter<float>         sampleMsP;      // output: (outpoint-inpoint)*bufDuration in ms
    ofParameter<float>         envAttackP;     // attack proportion (0..1)
    ofParameter<float>         envReleaseP;    // release proportion (0..1)
    ofParameter<float>         envTensionP;    // curve tension: -1=log (concave), 0=linear, 1=exp (convex)

    // ── Waveform display ──────────────────────────────────────────────────────
    std::string        samplePath;
    std::vector<float> waveformPeaks;   // WAVEFORM_BINS peak values 0..1
    void loadWaveformData(const std::string& path);

    float bufferDurationSecs = 0.0f;  // total loaded sample duration in seconds
    int   numSampleChannels  = 1;     // detected from WAV header; drives synthdef variant
    int   oldNumSampleChannels = 1;   // change detection
    float waveZoom   = 1.0f;   // 1=full sample visible; 64=1/64th
    float waveScroll = 0.0f;   // 0=left edge, 1=right edge
    float waveH      = 0.0f;   // waveform panel height in screen px (0=init on first draw)

    // ── Envelope buffer ───────────────────────────────────────────────────────
    std::vector<float> envData;        // ENV_BUFFER_SIZE float samples
    bool               envNeedsUpdate = false;
    void computeEnvData();
    void uploadEnvBuffers();

    // ── Grain highlight system ────────────────────────────────────────────────
    // Grain highlights represent active grains in the waveform display.
    // Each highlight spans [position, position + duration] in the waveform
    // and fades over its duration. Color is per-channel hue.
    struct GrainHighlight {
        float position;   // normalised within accessible region (0..1 → inPoint..outPoint)
        float duration;   // seconds (used for lifetime and width)
        float amp;        // 0..1, used for highlight intensity
        int   channel;    // 0-based, for color
        float lifeTime;   // remaining life in seconds
        float maxLife;    // total life = duration
    };
    std::deque<GrainHighlight> grainHighlights;
    // SC sends /grainTrig via SendReply.ar — per-server listener subscriptions
    std::map<ofxSCServer*, ofEventListener> grainFeedbackListeners;

    // ── File browser ──────────────────────────────────────────────────────────
    struct BrowseEntry { bool isDir; std::string name, fullPath; };
    std::string              browseDir;
    std::vector<BrowseEntry> browseEntries;
    int                      browserSel = -1;
    float                    browserW   = 220.0f;
    void refreshBrowse(const std::string& dir);

    // ── Audio preview ─────────────────────────────────────────────────────────
    ofxSCServer* previewServer = nullptr;
    ofxSCBuffer* previewBuf    = nullptr;
    ofxSCSynth*  previewSynth  = nullptr;
    void triggerPreview(const std::string& path);
    void stopPreview();

    // ── GUI ───────────────────────────────────────────────────────────────────
    void drawGrainBoxWindow();
    void drawBrowser(float w, float h);
    void drawWaveformPanel(ImDrawList* dl, ImVec2 pos, float w, float h);
    void drawEnvelopePanel(float w, float h);   // curve + sliders in one cell
    void drawControlsPanel(float w, float h);

    // In/out point drag state
    enum class WavDrag { None, InPoint, OutPoint };
    WavDrag wavDragMode = WavDrag::None;

    // ── Helpers ───────────────────────────────────────────────────────────────
    std::string getSynthdefName() const;
    void        loadSample(const std::string& path);
    void        sendAllParams();   // push all params to all running synths
    void        validateAndReshapeVectorParams();  // ensure vector params match numChannels

    int                oldNumChannels = 1;
    ofEventListeners   nodeListeners;

    // ── BPM ───────────────────────────────────────────────────────────────────
    float currentBpm = 120.0f;
    void  setBpm(float bpm) override;

    // ── AutoTrig mode ─────────────────────────────────────────────────────────
    ofParameter<vector<float>> autoTrigBeatDivP; // beat divisions (0.125..64)
    ofParameter<bool>          uniqueTrigP;    // trigger/chance: all voices share same random
    ofParameter<bool>          uniqueJitP;     // jitter: all voices share same random
    ofParameter<bool>          uniqueLfoTargetP[6];  // per-target LFO unique: one per Pos/Dur/Pitch/Amp/Pan/TrRt
    ofParameter<int>           syncGateP;      // 0/1 pulse resets all LFOs & BPM phasors
    bool                       syncGateReset = false; // flag to clear syncGate next frame

    // ── Modulation LFOs ───────────────────────────────────────────────────────
    // 5 targets: 0=Pos 1=Dur 2=Pitch 3=Amp 4=Pan
    static constexpr int NUM_LFO = 6;
    static const char* LFO_TARGET_NAMES[6];   // defined in .cpp
    struct LfoGroup {
        ofParameter<vector<float>> shape;    // 0=sin 1=tri 2=saw 3=invSaw 4=randStep 5=randCurve
        ofParameter<vector<float>> speed;    // beat divisions (0.25..32)
        ofParameter<vector<float>> phase;    // initial phase / reset target (0..1)
        ofParameter<vector<float>> quant;    // quantization steps (0=off, 2..32)
        ofParameter<vector<float>> strength; // 0..1 (0..48 semitones for pitch, 0..2 for pan)
    };
    LfoGroup lfoGroups[6];

    // ── Modulation GUI helpers ────────────────────────────────────────────────
    void drawModRow(float w, float h);
    void drawLFOPreview(ImDrawList* dl, ImVec2 pos, float w, float h,
                        int shape, float phase, float quant, float strength, float barDiv);
};
