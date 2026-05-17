//
//  scSimpleSamplePlayer.h
//  ofxOceanodeSuperCollider
//
//  Minimal looping PlayBuf node for bus-routing and buffer diagnostics.
//  Accepts bufnum and gain as node parameters; audio output follows
//  the same buildSynth/setOutputBus/createSynth pattern as scTestNoise.
//

#pragma once

#include "scNode.h"
#include "ofxSCSynth.h"
#include <map>

class scSimpleSamplePlayer : public scNode {
public:
    scSimpleSamplePlayer() : scNode("SimpleSamplePlayer") {}
    ~scSimpleSamplePlayer();

    void setup() override;

    // scNode interface
    void buildSynth(ofxSCServer* server)                     override;
    void createSynth(ofxSCServer* server)                    override;
    void free(ofxSCServer* server)                           override;
    void setOutputBus(ofxSCServer* server, int idx, int bus) override;
    int  getOutputBusIndex(ofxSCServer* server, int idx)     override;
    void moveSynthBefore(ofxSCServer* server, int nodeID)    override;
    int  getLastSynthID(ofxSCServer* server)                 override;

    void setInputBus(ofxSCServer*, scNode*, int)  override {}
    void resetInputBusses(ofxSCServer*, int)       override {}

private:
    ofParameter<int>   bufnum;
    ofParameter<float> gain;

    std::map<ofxSCServer*, ofxSCSynth*>        synths;
    std::map<ofxSCServer*, std::map<int, int>> outputBuses;

    ofEventListeners listeners;
};
