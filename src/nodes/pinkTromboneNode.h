//
//  pinkTromboneNode.h
//  ofxOceanodeSuperCollider
//
//  Vocal-tract synthesiser (Pink Trombone by Neil Thapen, C++ port by cutelabnyc)
//  wrapped as an Oceanode scNode.
//
//  Audio path:
//    CoreAudio render callback
//      └─ Pink Trombone (Glottis + Tract, 2 passes/sample)
//           ├─ speaker output (local monitoring, off by default)
//           └─ SHM ring buffer (/OceanodeRadio_N)
//                └─ SuperCollider reads via RadioStation SynthDef
//                     └─ SC audio bus → any downstream SC node
//

#pragma once

#include "scNode.h"
#include "ofxSCSynth.h"

// ── SHM bridge (from ofxOceanodeOnlineRadio) ─────────────────────────────────
#if __has_include("OceanodeRadioSHM.h")
  #include "OceanodeRadioSHM.h"
  #define PT_HAS_SHM 1
#elif __has_include("../../ofxOceanodeOnlineRadio/src/OceanodeRadioSHM.h")
  #include "../../ofxOceanodeOnlineRadio/src/OceanodeRadioSHM.h"
  #define PT_HAS_SHM 1
#else
  #define PT_HAS_SHM 0
#endif

// ── Standard headers ─────────────────────────────────────────────────────────
#include <AudioToolbox/AudioToolbox.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <memory>
#include <map>
#include <vector>

// ── Pink Trombone engine ─────────────────────────────────────────────────────
#include "../external/pinkTrombone/Glottis.hpp"
#include "../external/pinkTrombone/Tract.hpp"
#include "../external/pinkTrombone/WhiteNoise.hpp"

// ─────────────────────────────────────────────────────────────────────────────
class pinkTromboneNode : public scNode {
public:
    pinkTromboneNode();
    ~pinkTromboneNode();

    void setup()  override;
    void update(ofEventArgs &args) override;

    // ── scNode graph-management interface ────────────────────────────────────
    void buildSynth        (ofxSCServer* server) override;
    void createSynth       (ofxSCServer* server) override;
    void moveSynthBefore   (ofxSCServer* server, int nodeID) override;
    void free              (ofxSCServer* server) override;
    void setOutputBus      (ofxSCServer* server, int index, int bus) override;
    void setInputBus       (ofxSCServer* server, scNode* node, int bus) override;
    void resetInputBusses  (ofxSCServer* server, int targetBus = 0) override;
    int  getOutputBusIndex (ofxSCServer* server, int index) override;
    int  getLastSynthID    (ofxSCServer* server) override;
    void activate()  override;
    void deactivate() override;

private:
    // ── Synthesis engine ──────────────────────────────────────────────────────
    std::unique_ptr<Glottis>    glottis;
    std::unique_ptr<Tract>      tract;
    std::unique_ptr<WhiteNoise> noiseGen;
    t_tractProps       tractProps;
    std::atomic<bool>  engineReady   {false};  // written main-thread, read audio-thread
    int                sampleInBlock {0};

    static constexpr int PT_SR         = 44100;
    static constexpr int PT_BLOCK_SIZE = 512;   // finishBlock() cadence (samples)

    // ── Atomic parameter mirrors (audio-thread safe) ──────────────────────────
    std::atomic<float> aFreq              {140.f};
    std::atomic<float> aTenseness         {0.6f};
    std::atomic<float> aTongueIndex       {12.9f};
    std::atomic<float> aTongueDiameter    {2.43f};
    std::atomic<float> aConstrictionIndex {-1.f};   // < 0 = no constriction
    std::atomic<float> aConstrictionDiam  {3.5f};
    std::atomic<float> aFricative         {0.f};
    std::atomic<float> aGain              {1.f};
    std::atomic<bool>  aSHMActive         {false};
    std::atomic<bool>  aMonitor           {false};  // true → also play through local device

    // ── OF Parameters ─────────────────────────────────────────────────────────
    ofParameter<float> frequency;           // Hz  [20 – 2000]
    ofParameter<float> tenseness;           // [0 – 1]
    ofParameter<float> tongueIndex;         // tract position [lower – upper bound]
    ofParameter<float> tongueDiameter;      // [0 – 3.5]
    ofParameter<float> constrictionIndex;   // < 0 → disabled
    ofParameter<float> constrictionDiam;    // [-2 – 3.5]
    ofParameter<float> fricativeIntensity;  // [0 – 1]
    ofParameter<float> gain;               // [0 – 2]
    ofParameter<bool>  monitor;            // local speaker monitoring (off = SC-only)
    ofParameter<int>   numChannels;        // 1 (mono) or 2 (stereo)

    // Per-server synth instances and assigned output buses (index → bus)
    std::map<ofxSCServer*, ofxSCSynth*>        synthInstances;
    std::map<ofxSCServer*, std::map<int, int>> outputBuses;

    std::string getSynthDefName() const;

    ofEventListeners listeners;

    // ── CoreAudio ─────────────────────────────────────────────────────────────
    AudioComponent         audioComponent {nullptr};
    AudioComponentInstance audioUnit      {nullptr};
    bool setupAudioUnit();
    void cleanupAudioUnit();

    static OSStatus audioRenderCallback(
        void*                        inRefCon,
        AudioUnitRenderActionFlags*  ioActionFlags,
        const AudioTimeStamp*        inTimeStamp,
        UInt32                       inBusNumber,
        UInt32                       inNumberFrames,
        AudioBufferList*             ioData);

#if PT_HAS_SHM
    // ── SHM bridge (same layout as radioStationVLC) ───────────────────────────
    void*  shmBase    {nullptr};
    int    shmFd      {-1};
    int    instanceId {0};
    inline static std::atomic<int> nextInstanceId {0};

    bool setupSHM();
    void cleanupSHM(bool withUnlink = false);
    void writeSHM(const float* interleavedBuf, uint32_t frames) noexcept;
#endif

    void initEngine();
    void cleanup();
};
