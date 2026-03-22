//
//  scConvolution.h
//  ofxOceanodeSuperCollider
//
//  Partitioned convolution reverb.  Two IR modes:
//
//    Synthetic (default) — gaussian noise + time-varying one-pole LPF
//    (darkens over time) + exponential decay envelope.  Controlled by
//    RT60, Brightness, Absorption.
//
//    File — any WAV/AIFF loaded as a mono IR.
//
//  State machine per server (4 states):
//    kLoadingIR    → allocate + send load/alloc; for file: b_query at 600 ms,
//                    transition at 800 ms; for synth: launch scconvIRGen at
//                    300 ms, transition immediately.
//    kPreparingSpec→ on entry: allocate exact spec buffer, start PreparePartConv
//                    once gen synth has finished writing (timer >= rt60 for synth,
//                    timer >= 0 for file).  Transition 1.5 s after PreparePartConv.
//    kReady        → kAddAction_replace with new spec buffer as init-arg.
//
//  genSynth is tracked per-server so it can be freed *before* its target
//  irBuffer is freed/reallocated — prevents old synth from writing into a
//  reused buffer index and causing periodic noise bursts.
//

#ifndef scConvolution_h
#define scConvolution_h

#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanodeNodeModel.h"
#include "scNode.h"
#include "ofxSCSynth.h"
#include "ofxSCBuffer.h"
#include "ofxOscMessage.h"
#include <map>
#include <string>

class ofxSCServer;
class ofxSCSynth;
class ofxSCBuffer;

class scConvolution : public scNode {
public:
    // fftsize in the SynthDef: partition = 1024 samples ≈ 23 ms at 44.1 kHz
    static constexpr int kFftSize = 2048;

    scConvolution();
    ~scConvolution();

    void setup()                    override;
    void update(ofEventArgs& args)  override;
    void activate()                 override;
    void deactivate()               override;

    // scNode interface
    void buildSynth(ofxSCServer* server)              override;
    void createSynth(ofxSCServer* server)             override;
    void free(ofxSCServer* server)                    override;
    void moveSynthBefore(ofxSCServer* server, int id) override;

    void setInputBus(ofxSCServer* server, scNode* node, int bus)  override;
    void setOutputBus(ofxSCServer* server, int index, int bus)    override;
    void resetInputBusses(ofxSCServer* server, int targetBus = 0) override;

    int  getOutputBusIndex(ofxSCServer* server, int index) override;
    int  getLastSynthID(ofxSCServer* server)               override;

    ofEvent<void> resendParams;
    int           oldNumChannels = 0;

private:
    enum IRState { kIdle, kLoadingIR, kPreparingSpec, kReady };

    struct ServerState {
        ofxSCBuffer* irBuffer          = nullptr;
        ofxSCBuffer* activeSpecBuffer  = nullptr;
        ofxSCBuffer* pendingSpecBuffer = nullptr;
        ofxSCSynth*  genSynth          = nullptr;  // synthetic IR generator (tracked to prevent buffer reuse corruption)
        IRState      irState           = kIdle;
        float        timer             = 0.0f;
        bool         fileMode          = false;
        bool         prepConvSent      = false;
    };

    std::map<ofxSCServer*, ofxSCSynth*>             synthInstances;
    std::map<ofxSCServer*, std::map<scNode*, int>>  inputBuses;
    std::map<ofxSCServer*, std::map<int, int>>      outputBuses;
    std::map<ofxSCServer*, ServerState>             serverStates;

    ofParameter<int>         numChannels;
    ofParameter<float>       wet;
    ofParameter<float>       level;
    ofParameter<float>       rt60;
    ofParameter<float>       brightness;
    ofParameter<float>       absorpCurve;
    ofParameter<std::string> irFilePath;   // inspector + preset; not in main panel

    std::string getSynthDefName()                                   const;
    void        startSyntheticIR();
    void        startSyntheticIRForServer(ofxSCServer* srv);
    void        loadImpulseResponse(const std::string& path);
    void        cancelLoad(ofxSCServer* srv);
    void        allocExactSpecBuffer(ofxSCServer* srv, int irFrames);
    void        preparePartConv(ofxSCServer* server);
    void        sendParamsToSynth(ofxSCServer* server);
    void        swapAndReplaceWithNewSpec(ofxSCServer* srv);
    void        openFileDialog();
    void        drawStatusWidget();

    ofEventListeners listeners;
};

#endif /* scConvolution_h */
