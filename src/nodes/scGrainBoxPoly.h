//
//  scGrainBoxPoly.h
//  ofxOceanodeSuperCollider
//
//  Multichannel granular sampler with interactive waveform/envelope GUI.
//
//  Architecture:
//    • Inherits scNode — pure audio source (no In port).
//    • Playheads: tab per playhead in the window; each has a complete set of
//      grain/trigger/jitter/LFO/envelope/region settings and its own synth.
//    • numChannels controls both grain-voice count and PanAz speaker count.
//    • One GrainBox_K_N SynthDef per sample-channel count K (1..2) and channel
//      count N (1..16); compiled by SPECIAL_SYNTHDEFS/KR/scGrainBoxPoly.scd.
//    • Per SC server: one mono sample buffer per sample channel of every
//      loaded sample (playhead 1's sample is shared by every playhead in
//      "Shared" mode; a playhead in "Own" mode has its own), and per playhead
//      one synth + one envelope buffer.
//    • Per playhead channels/offset: its synth is GrainBox_K_Np writing to
//      output bus + offset (Channels 0 = follow N Chan).
//    • One output port (N-channel wide bus, managed by serverManager).
//
//  Custom GUI (floating ImGui window):
//    • File browser with audio preview (scSampleBrowser, shared with
//      scRhythmBox): click previews, double-click / Enter / drag onto the
//      waveform loads into the selected playhead's sample.
//    • Waveform display: shows loaded sample, per-channel grain highlights
//      (duration × amp intensity, color per channel), draggable inPoint/outPoint.
//    • Envelope editor: attack / release / shape parameters rendered as a
//      512-sample buffer uploaded to SC via /b_setn.
//

#pragma once
#include <array>

#include "ofxOceanodeNodeModel.h"
#include "ofxOceanodeShared.h"
#include "scNode.h"
#include "serverManager.h"
#include "ofxSuperCollider.h"
#include "ofxSCSynth.h"
#include "ofxSCBuffer.h"
#include "imgui.h"
#include "ofxOceanodeSuperColliderConfig.h"
#include "scTransportSync.h"
#include "scSampleBrowser.h"
#include "scEQEditor.h"
#include <vector>
#include <string>
#include <map>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>

class scGrainBoxPoly : public scNode {
public:
    // ── Constants ─────────────────────────────────────────────────────────────
    static constexpr int   MAX_CHANNELS        = 16;
    static constexpr int   MAX_SAMPLE_CHANNELS = 2;   // GrainBox_K_N exists for K = 1..2
    static constexpr int   ENV_BUFFER_SIZE     = 512;
    static constexpr int   WAVEFORM_BINS       = 4096;
    static constexpr int   MAX_PLAYHEADS       = 8;    // engines
    static constexpr int   MAX_VOICES          = 500;  // playheads per engine (largest GrainBoxPolyCtl_<cap>)
    static constexpr int   NUM_LFO             = 7;   // Pos Dur Pitch Amp Pan TrRt Cut
    static constexpr int   LFO_AMP             = 3;
    static constexpr int   LFO_TRRT            = 5;
    static constexpr int   LFO_CUT             = 6;   // output filter cutoff
    static const char* LFO_TARGET_NAMES[NUM_LFO];   // defined in .cpp

    // ── Constructor / Destructor ──────────────────────────────────────────────
    scGrainBoxPoly(vector<serverManager*> servers);
    ~scGrainBoxPoly();

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
    // Audio input ("In"): recorded by the live playheads' GrainBoxRec synths
    void setInputBus(ofxSCServer* s, scNode* node, int bus) override;
    void resetInputBusses(ofxSCServer* s, int targetBus)    override;

    // ── Preset serialization ──────────────────────────────────────────────────
    void presetSave(ofJson& j)                          override;
    void presetRecallBeforeSettingParameters(ofJson& j) override;
    void presetRecallAfterSettingParameters(ofJson& j)  override;
    void loadBeforeConnections(ofJson& j)               override;
    void presetHasLoaded()                              override;

private:
    // ── Servers ───────────────────────────────────────────────────────────────
    vector<serverManager*> allServers;

    // ── A loaded sample: waveform data + one mono SC buffer per sample channel
    //    per server ──────────────────────────────────────────────────────────
    struct SampleData {
        std::string        path;
        std::vector<float> peaks;             // WAVEFORM_BINS peak values 0..1
        std::vector<float> lows;              // Live only: signed min per bin (peaks = signed max)
        float              durationSecs = 0.0f;
        int                numChannels  = 1;  // from the WAV header, 1..MAX_SAMPLE_CHANNELS
        std::map<ofxSCServer*, std::vector<ofxSCBuffer*>> bufs;
    };
    SampleData mainSample;   // playhead 1's sample, shared by "Shared" playheads

    // ── Shared SC resources (per server) ──────────────────────────────────────
    std::map<ofxSCServer*, int>          outputBuses;
    std::map<ofxSCServer*, ofxSCBus*>    privateBuses;

    void reloadSampleBuffersForServer(SampleData& sd, ofxSCServer* srv);
    void releaseSampleBuffers(SampleData& sd, ofxSCServer* srv);
    static bool bufferIsLive(ofxSCServer* srv, ofxSCBuffer* b);   // false after a server reboot

    // ── Global node parameters ────────────────────────────────────────────────
    ofParameter<bool>          showWindow;
    ofParameter<int>           numChannelsP;   // 1..MAX_CHANNELS
    ofParameter<float>         sampleMsP;      // output: playhead 1 region length in ms

    // ── Playheads ─────────────────────────────────────────────────────────────
    // Playhead 1 (index 0) owns the original parameters with their original
    // names, so old presets load unchanged. Every extra playhead has a full,
    // independent set (names suffixed " 2", " 3"...) and its own synth per
    // server (same GrainBox_K_N def and sample buffers, own envelope buffer),
    // all writing (Out.ar adds) to the node's output bus.
    struct LfoGroup {
        ofParameter<vector<float>> shape;    // 0=sin 1=tri 2=saw 3=invSaw 4=randStep 5=randCurve
        ofParameter<vector<float>> speed;    // cycles per bar (1/128..128), or per region length (speedRel)
        ofParameter<bool>          speedRel; // Speed relative to the region's length (Smp) instead of the bar
        ofParameter<vector<float>> phase;    // initial phase / reset target (0..1)
        ofParameter<vector<float>> quant;    // quantization steps (0=off, 2..32)
        ofParameter<vector<float>> strength; // 0..1 (0..48 semitones for pitch, 0..2 for pan)
        ofParameter<vector<float>> pow;      // curve exponent 0.1..10 on the 0..1 shape (1 = off)
    };
    struct GrainHighlight {
        float position;   // absolute buffer position (0..1)
        float duration;   // seconds (used for lifetime and width)
        float amp;        // 0..1, used for highlight intensity
        int   channel;    // output channel (voice + the playhead's offset), 0-based
        int   playhead;   // playhead index, for color
        float lifeTime;   // remaining life in seconds
        float maxLife;    // total life = duration
    };
    struct Playhead {
        int         index = 0;
        std::string sfx;              // "" for playhead 1, " 2", " 3"...

        // Trigger
        ofParameter<vector<int>>   trigger;
        ofParameter<bool>          autoTrig;
        // "Interval": time between auto triggers in the engine's own unit
        // (intervalUnit, independent of the global Time Units: 0 Relative =
        // fraction of the region, 1 ms, 2 Beats = note values). Sent to the
        // SynthDef as grains per beat (divSend).
        ofParameter<vector<float>> autoTrigBeatDiv;
        ofParameter<int>           intervalUnit;
        int                        intervalUnitCur = 2;   // unit the values are in now
        ofParameter<bool>          monoTrig;    // no overlap: a grain starts once the previous one ended
        ofParameter<vector<float>> trigPhase;   // 0..1 of the interval, per playhead
        ofParameter<int>           maxGrains;   // overlap limit of the engine (0 = off)
        float grainLoad = 0.0f, grainThin = 1.0f;   // /grainLoad: grains sounding, thinning
        // /lfoState (30 x / s while the window is open): every LFO's phase and
        // 0..1 value as SC computes them, and a short history (random shapes)
        float lfoPhaseNow[NUM_LFO] = {0};
        float lfoValNow[NUM_LFO]   = {0};
        std::deque<std::pair<float, float>> lfoHist[NUM_LFO];   // (time, value)
        float lfoStateAt = -1.0f;
        ofParameter<vector<float>> chance;
        ofParameter<bool>          uniqueTrig;
        ofParameter<int>           syncGate;
        // Grain
        ofParameter<vector<float>> amp, pitch, duration, position, panAz;
        ofParameter<vector<float>> reverse;          // per-grain probability of playing backwards
        // Pitch to scale: 0 Off, 1 Chromatic ... 11 Custom (scaleMask bits, relative to root)
        ofParameter<int>           scaleType, scaleRoot, scaleMask;
        ofParameter<bool>          dynamicDur, trigDur;
        // Jitter
        ofParameter<bool>          uniqueJit;
        ofParameter<vector<float>> posJit, pitchJit, durJit, ampJit, panAzJit;
        // Region / envelope / levels
        ofParameter<float>         inPoint, outPoint;
        ofParameter<float>         envAttack, envRelease, envTension;
        ofParameter<int>           envShape;         // 0 Custom (attack/release/tension) .. 6 Rectangular
        ofParameter<vector<float>> levels;
        // Output channels: Channels 0 = follow the node's N Chan; Offset = first
        // output channel (clamped so offset + channels <= N Chan)
        ofParameter<int>           channels, chanOffset;
        // Mixing (global tab): phgain = 0 when muted, or when another playhead is soloed
        ofParameter<bool>          mute, solo;
        // Filter: 0 = Off, 1 LPF2, 2 BPF, 3 Notch, 4 HPF2 (output filter in GrainBox_K_N;
        // latchCut is kept only so older presets load; it no longer does anything)
        ofParameter<int>           filterType;
        ofParameter<float>         cutoff, filterQ;
        // Latch: Amp / Cut LFO (+ base + jitter) sampled at each grain's trigger
        ofParameter<bool>          latchAmp, latchCut;
        // LFOs ("Unique" = Link voices: every voice uses voice 1's value)
        ofParameter<bool>          uniqueLfo[NUM_LFO];
        LfoGroup                   lfo[NUM_LFO];

        // Sample: playhead 1 always uses mainSample; others share it unless
        // ownSample (then `own`, loaded from the browser while its tab is active)
        bool        ownSample = false;
        SampleData  own;
        // Live input (any playhead): a rolling mono buffer recorded from the
        // node's input; `live` is its SampleData (peaks in age order, 0 = newest)
        bool        liveInput = false;
        SampleData  live;
        ofParameter<float> liveLength;              // buffer length: seconds, or beats (liveLenBeats)
        ofParameter<bool>  liveLenBeats;            // Live Length in beats (follows the tempo)
        ofParameter<bool>  grainless;               // Live: plain delay line per playhead (no grains)
        ofParameter<float> overlap;                 // TrigDur: grain length = Overlap x trigger period
        ofParameter<bool>  delayJump;               // Grainless: 0 Glide (tape slide), 1 Jump (crossfade)
        ofParameter<float> delayGlide;              // Grainless: glide / crossfade time (ms)
        ofParameter<bool>  freeze;                  // stop recording
        std::vector<float> liveRing;                // signed max per bin, buffer order
        std::vector<float> liveRingMin;             // signed min per bin
        float              liveHead = 0.0f;         // write position 0..1
        float              liveResizeAt = -1.0f;    // debounced re-allocation after a length change
        struct LiveSC {
            ofxSCBuffer*    buf      = nullptr;
            ofxSCBus*       phaseBus = nullptr;     // audio, 1 channel
            ofxSCSynth*     rec      = nullptr;
            ofEventListener recListener;
        };
        std::map<ofxSCServer*, LiveSC> liveSC;
        // CPU: paused (n_run 0) while muted / excluded by solo
        bool               isPaused = false;
        float              pauseAt  = -1.0f;

        // Playheads: simultaneous grain streams of this engine (1..MAX_VOICES),
        // independent of its speaker count. Playhead i reads index i of every
        // per-playhead vector (a scalar applies to all of them).
        ofParameter<int>           numPlayheads;

        // SC state. `synths` = the engine's CONTROL synth (GrainBoxPolyCtl):
        // it takes every control, as GrainBox_K_N did. Then one voice synth
        // per playhead (GrainBoxPolyV_K_N) and one output synth (GrainBoxPolyOut_N).
        std::map<ofxSCServer*, ofxSCSynth*>     synths;
        std::map<ofxSCServer*, std::vector<ofxSCSynth*>>      voices;
        std::map<ofxSCServer*, std::vector<std::unique_ptr<ofEventListener>>> voiceListeners;
        std::map<ofxSCServer*, ofxSCSynth*>     outSynths;
        struct EngineBuses {
            ofxSCBus* ctl = nullptr;   // control, ctlBusSize(): shared values + per-playhead values
            ofxSCBus* clk = nullptr;   // audio, 7: the engine clock
            ofxSCBus* mix = nullptr;   // audio, MAX_CHANNELS: the voices' grains
        };
        std::map<ofxSCServer*, EngineBuses>     ebus;
        std::map<ofxSCServer*, ofxSCBuffer*>    envBufs;
        std::map<ofxSCServer*, ofEventListener> grainListeners;
        std::vector<float> envData;
        bool               envNeedsUpdate = false;
        float              envResendAt    = -1.0f;  // re-send once after a fresh allocation
        ofEventListeners   listeners;

        // Robust pulses: SC sees ever-increasing counters (a rising edge per
        // pulse), so a 1-then-0 pair landing in one control block is not lost.
        std::vector<int> lastTrigger;
        std::vector<int> triggerCounts;
        int   syncGateCount = 0;
        bool  syncGateReset = false;
        bool  triggerReset  = false;   // window Trig button: back to 0 next frame
#if OFXOCEANODESC_HAS_TRANSPORT
        double hardCounter = 0.0;
        bool   needHard    = true;
#endif
    };
    std::vector<std::unique_ptr<Playhead>> playheads;
    int  activePlayhead = 0;       // GUI tab
    int  pendingTabSelect = -1;    // select this tab on the next frame
    Playhead& ph0() { return *playheads[0]; }

    void createPlayheadParameters(Playhead& ph);
    void setupPlayheadListeners(Playhead& ph);
    void allocEnvBuffers(Playhead& ph);
    void ensureEnvBuffer(Playhead& ph, ofxSCServer* srv);    // alloc + data atomically
    // NRT render: the capture can only recreate the envelope buffers EMPTY
    // (their data is generated here, not read from a file), which would make
    // every grain silent. Re-send each one with its data at score time zero,
    // and push every playhead's parameters (scNode contract).
public:
    void resendParametersForNRT() override;
private:
    void freePlayheadSC(Playhead& ph);
    bool addPlayhead();                 // appends one; false at MAX_PLAYHEADS
    bool removeLastPlayhead();          // never removes playhead 1
    void setNumPlayheads(int n);
    void configureSynth(Playhead& ph, ofxSCSynth* s, ofxSCServer* srv);
    void createPlayheadSynth(Playhead& ph, ofxSCServer* srv); // runtime add
    void replaceSynths(ofxSCServer* srv);                     // def variant / bus changed
    void refreshPlayheadSynth(Playhead& ph, ofxSCServer* srv); // one playhead
    void refreshPlayheadSynths(Playhead& ph);                 // on every server
    ofxSCSynth* earliestSynth(ofxSCServer* srv, int belowIndex);
    std::vector<float> expandF(const vector<float>& v, int n) const;  // exactly n values
    std::vector<int>   expandI(const vector<int>& v, int n) const;
    std::vector<int>   triggerCountsFor(Playhead& ph);
    int  phChannels(const Playhead& ph) const;   // effective speaker count
    int  phVoices(const Playhead& ph) const;     // playheads (= vector size sent)
    int  ctlCap(const Playhead& ph) const;       // control synth size: 8 / 32 / 128 / 500 playheads
    int  ctlBusSize(const Playhead& ph) const;   // 80 + 15 * ctlCap
    void replaceCtl(Playhead& ph, ofxSCServer* srv);   // another capacity: new bus + control synth
    std::string voiceDefFor(const Playhead& ph) const;
    std::string outDefFor(const Playhead& ph) const;
    void ensureEngineBuses(Playhead& ph, ofxSCServer* srv);
    void freeEngineBuses(Playhead& ph, ofxSCServer* srv);
    void buildEngineObjects(Playhead& ph, ofxSCServer* srv);       // voices + out (not running)
    void runEngineChain(Playhead& ph, ofxSCServer* srv, bool run); // voices + out after the ctl
    void freeEngineVoices(Playhead& ph, ofxSCServer* srv);         // voices + out
    ofxSCSynth* newVoiceSynth(Playhead& ph, ofxSCServer* srv, int i);
    void syncVoiceCount(Playhead& ph, ofxSCServer* srv);           // Playheads changed (running)
    ofxSCSynth* engineLast(Playhead& ph, ofxSCServer* srv);        // its latest synth in SC order
    void forEngineSynth(Playhead& ph, const std::function<void(ofxSCSynth*)>& f);
    int  phOffset(const Playhead& ph) const;     // effective first output channel
    int  outBusFor(const Playhead& ph, ofxSCServer* srv) const;
    SampleData&       sampleOf(Playhead& ph);
    const SampleData& sampleOf(const Playhead& ph) const;
    std::string defNameFor(const Playhead& ph) const;
    void setBufnums(Playhead& ph, ofxSCSynth* s, ofxSCServer* srv);
    void setOwnSample(Playhead& ph, bool own);

    // ── Live input ────────────────────────────────────────────────────────────
    std::map<ofxSCServer*, int> inputBuses;          // the "In" port's bus per server
    std::map<ofxSCServer*, std::deque<ofxSCSynth*>> recOrder;   // recorders, front = earliest
    void setLiveInput(Playhead& ph, bool on);
    void ensureLiveResources(Playhead& ph, ofxSCServer* srv);   // buffer + phase bus
    void createLiveRecorder(Playhead& ph, ofxSCServer* srv);    // before the earliest synth
    void freeLiveRecorder(Playhead& ph, ofxSCServer* srv);
    void freeLiveResources(Playhead& ph, ofxSCServer* srv);     // recorder + buffer + bus
    void resizeLiveBuffers(Playhead& ph);
    void setLiveArgs(Playhead& ph, ofxSCSynth* s);
    void setRecArgs(Playhead& ph, ofxSCSynth* rec, ofxSCServer* srv);
    ofxSCSynth* earliestRecorder(ofxSCServer* srv);
    void updateLivePeaks(Playhead& ph);             // ring (buffer order) -> age order

    // ── Scale ─────────────────────────────────────────────────────────────────
    int  scaleMaskFor(const Playhead& ph) const;    // 12 bits, relative to the root
    void setScaleArgs(Playhead& ph, ofxSCSynth* s);

    // ── CPU ───────────────────────────────────────────────────────────────────
    bool visOn = true;                  // /grainTrig replies wanted (window open)
    bool windowDrawn = false;           // set by the window's draw, read by update
    void sendVis();
    void updatePausing();

    // ── Waveform display ──────────────────────────────────────────────────────
    void loadWaveformData(SampleData& sd, const std::string& path);

    float waveZoom   = 1.0f;   // 1=full sample visible; 64=1/64th
    float waveScroll = 0.0f;   // 0=left edge, 1=right edge
    float ctrlContentH = 0.0f;   // controls row: tallest column's content (last frame), px
    float ctrlMeasure  = 0.0f;
    float waveH      = 0.0f;   // waveform panel height in screen px (0=init on first draw)

    // ── Envelope buffer ───────────────────────────────────────────────────────
    void computeEnvData(Playhead& ph);
    void uploadEnvBuffers(Playhead& ph);

    // ── Grain highlight system ────────────────────────────────────────────────
    std::deque<GrainHighlight> grainHighlights;

    // ── Global controls (global tab; publishable, not in the node by default) ─
    ofParameter<float> transposeP, volumeP, speedP;
    // Dry/Wet (node parameter): 0 = only the input, 1 = only the engines (equal power)
    ofParameter<float> dryWetP;
    std::map<ofxSCServer*, ofxSCSynth*> drySynths;   // GrainBoxPolyDry_N, after engine 1
    float dryGain() const;
    float wetGain() const;
    void  createDry(ofxSCServer* srv);
    void  freeDry(ofxSCServer* srv);
    // Global offsets added (in C++, unclipped: the SynthDef clips pos / dur
    // after jitter + LFO; chance is a threshold) to every playhead's values
    ofParameter<float> gPosP, gDurP, gChanceP;
    std::vector<float> withOffset(const vector<float>& v, int n, float off) const;
    void sendOffsetParams(Playhead& ph);   // every time-dependent value (see Time units)

    // ── Time units (global): the time-dependent parameters (Position, Duration,
    //    In/Out, PosJit, DurJit, LFO Pos/Dur strength, global Position/Duration
    //    offsets) are STORED in the current unit; they are converted to the
    //    SynthDef's relative values when sent. 0 Relative (as always), 1 ms, 2 Beats.
    ofParameter<int> timeUnitsP;
    int   timeUnitsCur  = 0;       // unit the stored values are in
    bool  loadingPreset = false;   // no conversion while a preset sets the values
    float liveGap(const Playhead& ph) const;   // Live grain safety gap (0 when Grainless)
    float liveLenSec(const Playhead& ph) const;   // Live Length in seconds
    float lastTimeBpm   = -1.0f;
    bool  timeDirty     = false;   // lengths changed: ranges + re-send (update)
    double unitToSec(double u) const;
    double secToUnit(double s) const;
    float lengthSec(const Playhead& ph) const;   // sample, or Live Len
    float inRelOf(const Playhead& ph) const;     // In / Out as the SynthDef's 0..1
    float outRelOf(const Playhead& ph) const;
    float posScaleSec(const Playhead& ph) const; // seconds per unit of raw position
    float posOffsetSec(const Playhead& ph) const;// Live: delay of raw position 0
    float durScaleSec(const Playhead& ph) const; // seconds per unit of duration
    std::vector<float> posSend(const Playhead& ph, int n) const;
    std::vector<float> durSend(const Playhead& ph, int n) const;
    // Trigger interval (current unit) -> seconds / grains per beat (SynthDef)
    float intervalSec(const Playhead& ph, float v) const;
    std::vector<float> divSend(const Playhead& ph, int n) const;
    void  sendDiv(Playhead& ph);
    // LFO speed as SC wants it (cycles per bar): Smp mode -> per region length
    double lfoSpeedBar(const Playhead& ph, int t) const;
    void   sendLfoSpeeds(Playhead& ph);
    float defaultInterval(int unit) const;   // 1/4 region, 500 ms or 1 beat
    double intervalUnitToSec(const Playhead& ph, int unit, double v) const;
    double intervalSecToUnit(const Playhead& ph, int unit, double sec) const;
    void  setIntervalRange(Playhead& ph);
    std::vector<float> posAmtSend(const Playhead& ph, const vector<float>& v, int n) const;
    std::vector<float> durAmtSend(const Playhead& ph, const vector<float>& v, int n) const;
    std::vector<float> lfoStrSend(const Playhead& ph, int t, int n) const;
    void convertTimeUnits(int from, int to);
    void updateTimeRanges();
    const char* timeFmt() const;   // slider format of the current unit
    float relToDisplay(const Playhead& ph, float rel) const;   // waveform x of a relative position
    float displayToRel(const Playhead& ph, float x) const;
    void sendMixParams();                          // transpose/volume/speed/phgain → every synth
    float phGainFor(const Playhead& ph) const;     // mute / solo
    void  setMixArgs(Playhead& ph, ofxSCSynth* s);
    void  setFilterArgs(Playhead& ph, ofxSCSynth* s);
    static bool synthIsFiltered(ofxSCSynth* s);

    // ── Global FX chain: EQ → Echo → Reverb (the user's SynthDefs) ────────────
    // All off: playheads write straight to the output bus (no extra synths).
    // Otherwise playheads write to a private bus and the enabled stages chain
    // private buses → output bus, right after playhead 1's synth.
    enum FxStage { FX_EQ = 0, FX_ECHO = 1, FX_REVERB = 2, FX_COUNT = 3 };
    ofParameter<bool>  fxOnP[FX_COUNT];
    ofParameter<float> eqGainP[5], eqMixP;
    ofParameter<float> eqFreqP[5];    // Hz (graphiceqN defaults 80 / 250 / 1k / 4k / 12k)
    ofParameter<float> eqShapeP[5];   // slope for bands 1 and 5 (shelves), Q for 2..4
    scEQEditor eqEditor;              // curve editor shared with scGraphicEQ
    bool       eqCurveDirty = true;
    void       drawEqSection(float w, float h);
    ofParameter<float> echoDelayP, echoFeedP, echoCutoffP, echoMixP;
    // Echo delay in beats (note values, follows the tempo) instead of seconds
    ofParameter<bool>  echoBeatsP;
    ofParameter<float> echoBeatValP;   // beats
    float echoDelaySec() const;        // what the Echo synth gets (max 2 s)
    ofParameter<int>   echoFilterP;      // echo.scd \filtertype: 0 LowPass 1 HighPass 2 BandPass 3 PeakEQ
    ofParameter<float> echoResonanceP;   // echo.scd \resonance
    ofParameter<float> revSizeP, revDecayP, revPredelayP, revLowpassP, revMixP;
    ofParameter<float> revPositionP, revSpreadP;   // early/late balance, stereo spread
    struct FxState {
        std::vector<int>       plan;                 // enabled + loaded stages, in order
        ofxSCSynth*            synths[FX_COUNT] = {nullptr, nullptr, nullptr};
        std::vector<ofxSCBus*> buses;                // input bus of each planned stage
    };
    std::map<ofxSCServer*, FxState> fx;
    bool fxPending = false;                              // a stage waits for its defs
    std::string fxDefName(int stage) const;
    bool fxStageReady(int stage, ofxSCServer* srv) const;
    void requireFxSynthdefs();                          // serverManager loads their folders
    std::vector<int> computeFxPlan(ofxSCServer* srv) const;
    void planFx(ofxSCServer* srv);                       // plan + buses (no synths)
    void createFxSynths(ofxSCServer* srv);               // after playhead 1's synth
    void freeFxSynths(ofxSCServer* srv);
    void freeFx(ofxSCServer* srv);                       // synths + buses + plan
    void rebuildFx(ofxSCServer* srv);                    // runtime: re-plan, re-route, re-create
    void rebuildFxAll();
    void sendFxParams(int stage, ofxSCSynth* s);
    void sendFxParamsAll();
    int  fxOutBus(ofxSCServer* srv, int planPos) const;
    std::vector<std::string> fxParamKeys;                // for the publish registry

    // ── File browser + audio preview (shared with scRhythmBox) ────────────────
    scSampleBrowser browser{"scGrainBoxPoly", {".wav", ".aif", ".aiff", ".flac"}};
    float           browserW = 220.0f;   // unscaled (x zoom)

    // ── GUI ───────────────────────────────────────────────────────────────────
    void drawGrainBoxWindow();
    void drawBrowser(float w, float h);
    void drawPlayheadTabs();
    bool globalTab = false;           // the Global tab is selected
    bool pendingGlobalSelect = false;
    bool tabsInitialized = false;
    void drawGlobalPanel(float w);

    // ── Snapshots: 16 slots of every editor parameter (not samples / live
    //    buffer length); click recall, Shift-click store, right-click clear.
    //    "Snapshot" (1..16, publishable) recalls from a connection. ──
    std::array<ofJson, 16> snapSlots;
    std::array<bool, 16>   snapUsed{};
    int  activeSnap   = -1;
    bool snapRecalling = false;
    ofParameter<int> snapshotP;

    // ── Morph between snapshots ──
    //   A/B: Morph (0..1) blends snapshot A into snapshot B (publishable, so a
    //   connection can drive it). Glide: a recall (Alt-click, or every recall
    //   with "Glide" on) moves from the current state to the slot over Morph
    //   Time (beats or seconds), with a curve. Continuous values glide (pitch
    //   in semitones; frequencies / speeds / intervals in log), switches /
    //   shapes / units jump at the switch point (start, middle, end).
    ofParameter<float> morphP, morphTimeP;
    ofParameter<int>   morphAP, morphBP, morphCurveP, morphSwitchP;
    ofParameter<bool>  morphBeatsP, morphGlideP;
    bool   morphRun = false;
    float  morphT0 = 0.0f, morphDur = 1.0f, morphProgress = 0.0f;
    int    morphTarget = -1;
    ofJson morphFrom, morphTo;
    void applyMorph(const ofJson& a, const ofJson& b, float t);
    void startGlide(int slot);
    void applyAB();
    float morphCurve(float u) const;
    void drawMorphPanel(float w);
    bool snapshotExcluded(const std::string& key) const;
    ofJson captureSnapshot() const;
    void applySnapshot(const ofJson& snap);
    void storeSnapshot(int slot);
    void recallSnapshot(int slot);
    void drawSnapshotMatrix(float w);
    // Snapshot library: the 16 slots live in shared files
    // (data/nodeSnapshots/GrainBoxPoly/snapshot_<n>.json), saved on every
    // change and read at start, so every project / preset / node sees them
    // (another node's change is picked up within a second). Named banks are
    // files in its banks/ folder. Each slot can have a "name".
    bool snapLoadDialog = false;        // "From file..." (run in update, outside ImGui)
    std::string snapBankMsg;
    char snapBankName[64] = "my bank";
    char snapRenameBuf[48] = "";
    std::filesystem::file_time_type snapLibTime{};
    float snapLibCheckAt = 0.0f;
    std::string snapLibPath() const;
    std::string snapBankDir() const;   // .../banks
    std::string snapSetDir() const;    // data/nodeSnapshots/GrainBoxPoly
    std::filesystem::file_time_type snapSetTime() const;
    void saveSnapshotLibrary();
    void loadSnapshotLibrary();
    void saveSnapshotBank(const std::string& path);
    bool loadSnapshotBank(const std::string& path);
    std::string snapName(int k) const;
    void drawGlobalWaveform(ImDrawList* dl, ImVec2 pos, float w, float h);
    void drawFxPanel(float w);
    void drawPlayheadHeader(Playhead& ph, float w);   // sample mode + channels row
    void drawWaveformPanel(ImDrawList* dl, ImVec2 pos, float w, float h);
    void drawEnvelopePanel(Playhead& ph, float w, float h);   // curve + sliders in one cell
    void drawControlsPanel(Playhead& ph, float w, float h);
    void drawModRow(Playhead& ph, float w, float h);
    // Live LFO state over the preview: a cursor + dot (periodic shapes), or
    // the real recent output (random shapes). False: no live data (preview only).
    bool drawLFOLive(ImDrawList* dl, ImVec2 pos, float w, float h, Playhead& ph, int t,
                     int shape, float phaseOff, float speed, bool overlayOnly);
    float lfoCtrlBottom = 0.0f;   // LFO cell: controls' bottom (last frame), for the row height
    void drawLFOPreview(ImDrawList* dl, ImVec2 pos, float w, float h,
                        int shape, float phase, float quant, float strength, float barDiv,
                        int playhead, float powExp = 1.0f);

    // In/out point drag state
    enum class WavDrag { None, InPoint, OutPoint };
    WavDrag wavDragMode = WavDrag::None;
    int     envDragHandle = -1;

    // ── Helpers ───────────────────────────────────────────────────────────────
    void        loadSample(const std::string& path);             // playhead 1's (shared) sample
    void        loadSampleInto(SampleData& sd, const std::string& path);
    void        loadSampleForPlayhead(Playhead& ph, const std::string& path);  // browser → tab
    void        sendAllParams(Playhead& ph);   // push all params to the playhead's synths
    void        validateAndReshapeVectorParams();  // ensure vector params match numChannels

    int                oldNumChannels = 1;
    ofEventListeners   nodeListeners;

    // ── BPM ───────────────────────────────────────────────────────────────────
    float currentBpm = 120.0f;
    void  setBpm(float bpm) override;
    float effectiveBpm() const;   // node BPM, or the transport's in sync mode

    // ── Publish to Node (same mechanism as polyphonicArpeggiatorGUI) ─────────
    // Only a minimal set of playhead-1 parameters is always in the node
    // (Show, N Chan, Trigger, AutoTrig, Amp, Pitch, Duration, Position, Levels,
    // SampleMs). Every other window control can be published at runtime with
    // its right-click menu. Keys are the parameters' escaped names, which are
    // also the names old presets stored their values and connections under.
    struct EditorPublishAction {
        std::string key;
        std::function<bool()> publish;
        std::function<bool()> unpublish;
        std::function<bool()> isPublished;
        std::function<bool()> isAvailableInNode;
        std::function<void(ofJson&)> saveValue;          // value -> json[key]
        std::function<void(const ofJson&)> loadValue;    // json[key] -> value
        // Right-click "Value" field: current value as text / apply typed text
        // (clamped to the range; vectors: one value for all voices, or a
        // comma-separated list)
        std::function<std::string()> valueText;
        std::function<void(const std::string&)> applyText;
        bool isVector = false;
        bool legacyNodeParameter = false;  // was a node parameter before publishing existed
        // Morph: set the value between two stored ones (a, b, position 0..1,
        // past the switch point, step instead of glide)
        std::function<void(const ofJson&, const ofJson&, float, bool, bool)> morph;
        int         unitClass = 0;   // 1: in the global Time Units, 2: in its engine's Interval unit
        std::string unitKey;         // class 2: that engine's IntervalUnit key
        int  playhead = 0;
    };
    std::vector<EditorPublishAction> publishableEditorParameters;
    std::vector<std::string> publishedEditorParameterKeys;
    std::map<std::string, std::shared_ptr<ofxOceanodeAbstractParameter>> publishedEditorParameterHandles;
    bool publishedEditorSeparatorAdded = false;
    std::vector<std::string> legacyAutoPublishedKeys;   // see loadBeforeConnections

    void initializePublishableEditorParameters();
    bool isEditorParameterPublished(const std::string& key) const;
    const EditorPublishAction* findPublishableEditorParameter(const std::string& key) const;
    bool publishEditorParameterToNode(const std::string& key);
    bool unpublishEditorParameterFromNode(const std::string& key);
    void syncPublishedEditorParameters(const std::vector<std::string>& keys);
    void drawPublishedLabelUnderline(const std::string& key,
                                     const char* label,
                                     float frameWidth = -1.0f,
                                     bool checkbox = false) const;
    void drawPublishedCurrentItemUnderline(const std::string& key) const;
    // focusValue: focus the typed-value field when the menu opens; title
    // replaces its "Value" label (EQ band menus show three of them)
    void drawNodePublishMenuItems(const std::string& key, bool focusValue = true,
                                  const char* title = nullptr);
    void drawNodePublishContextMenu(const std::string& key,
                                    const char* label = nullptr,
                                    float frameWidth = -1.0f,
                                    bool checkbox = false);
    bool parameterHasConnection(const std::string& key) const;
    // A node input (published / node parameter) with an incoming connection
    bool hasInputConnection(const std::string& name);
    std::vector<ofParameter<vector<float>>*> perPlayheadVectors(Playhead& ph);
    std::map<std::string, bool> vecWasConnected;   // collapse a vector when its connection goes

    template<typename ParameterType>
    bool publishEditorParameterToNode(const std::string& key,
                                      ofParameter<ParameterType>& parameter,
                                      ofxOceanodeParameterFlags flags = 0) {
        if(getParameterGroup().contains(parameter.getEscapedName())) return false;
        if(!publishedEditorSeparatorAdded) {
            addSeparator("Published", ofColor(200));
            publishedEditorSeparatorAdded = true;
        }
        publishedEditorParameterHandles[key] = addParameter(parameter, flags);
        if(std::find(publishedEditorParameterKeys.begin(), publishedEditorParameterKeys.end(), key) == publishedEditorParameterKeys.end()) {
            publishedEditorParameterKeys.push_back(key);
        }
        return true;
    }

#if OFXOCEANODESC_HAS_TRANSPORT
    // ── Sync To Transport ─────────────────────────────────────────────────────
    ofParameter<bool>  syncToTransportP;
    ofParameter<float> beatOffsetP;
    scTransportSync::Follower  follower;
    scTransportSync::StepClock stepClock;   // unit = beats, grid block = 32 beats
    double   anchorIdCounter = 0.0;
    uint64_t lastAnchorUs    = 0;
    float    syncBpm         = 120.0f;
    void handleSyncChanged(bool on);
    void updateTransportSync();
    void sendAnchor(const scTransportSync::Anchor& a);
    void setSyncHoldArgs(Playhead& ph, ofxSCSynth* s);
    bool isSyncing() const { return syncToTransportP.get(); }
#else
    bool isSyncing() const { return false; }
#endif
};
