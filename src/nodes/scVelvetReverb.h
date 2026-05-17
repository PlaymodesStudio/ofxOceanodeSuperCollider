//
//  scVelvetReverb.h
//  ofxOceanodeSuperCollider
//
//  scConvolution + post-reverb LPF/HPF + feedback.
//  File loading removed; IR generated in C++ (velvet noise).
//  Architecture is otherwise identical to scConvolution.
//

#ifndef scVelvetReverb_h
#define scVelvetReverb_h

#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanodeNodeModel.h"
#include "scNode.h"
#include "ofxSCSynth.h"
#include "ofxSCBuffer.h"
#include "ofxOscMessage.h"
#include <map>
#include <string>
#include <vector>
#include <random>

class ofxSCServer;
class ofxSCSynth;
class ofxSCBuffer;

class scVelvetReverb : public scNode {
public:
    static constexpr int kFftSize = 2048;

    scVelvetReverb();
    ~scVelvetReverb();

    void setup()                    override;
    void update(ofEventArgs& args)  override;
    void activate()                 override;
    void deactivate()               override;

    // scNode interface — identical to scConvolution
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

    // Identical to scConvolution::ServerState minus genSynth and fileMode
    struct ServerState {
        ofxSCBuffer* irBuffer          = nullptr;
        ofxSCBuffer* activeSpecBuffer  = nullptr;
        ofxSCBuffer* pendingSpecBuffer = nullptr;
        IRState      irState           = kIdle;
        float        timer             = 0.0f;
        bool         prepConvSent      = false;
    };

    std::map<ofxSCServer*, ofxSCSynth*>            synthInstances;
    std::map<ofxSCServer*, std::map<scNode*, int>> inputBuses;
    std::map<ofxSCServer*, std::map<int, int>>     outputBuses;
    std::map<ofxSCServer*, ServerState>            serverStates;

    ofParameter<int>   numChannels;
    ofParameter<float> wet, level;
    ofParameter<float> rt60, brightness, damping, diffusion;
    // Added over scConvolution:
    ofParameter<float> lpf, hpf, fbAmt;

    ofEventListeners listeners;

    std::string        getSynthDefName()                              const;
    void               startSyntheticIR();
    void               startSyntheticIRForServer(ofxSCServer* srv);
    void               cancelLoad(ofxSCServer* srv);
    void               allocExactSpecBuffer(ofxSCServer* srv, int irFrames);
    void               preparePartConv(ofxSCServer* server);
    void               sendParamsToSynth(ofxSCServer* server);
    void               swapAndReplaceWithNewSpec(ofxSCServer* srv);
    void               drawStatusWidget();

    // C++ velvet noise IR generation (replaces scconvIRGen synth)
    std::vector<float> generateIR(int frames, float brt, float dmp,
                                   float diff, float sr, unsigned int seed);
    void               sendIRData(ofxSCServer* srv, ofxSCBuffer* buf,
                                   const std::vector<float>& data);
};

#endif /* scVelvetReverb_h */
