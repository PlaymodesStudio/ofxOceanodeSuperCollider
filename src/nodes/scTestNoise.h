//
//  scTestNoise.h
//  ofxOceanodeSuperCollider
//
//  Minimal white-noise source node for output-routing diagnostics.
//  Uses the exact same buildSynth/createSynth pattern as scSynthdef.
//

#pragma once

#include "scNode.h"
#include "ofxSCSynth.h"
#include <map>

class scTestNoise : public scNode {
public:
    scTestNoise() : scNode("TestNoise") {}
    ~scTestNoise();

    void setup() override;

    // scNode interface
    void buildSynth(ofxSCServer* server)                     override;
    void createSynth(ofxSCServer* server)                    override;
    void free(ofxSCServer* server)                           override;
    void setOutputBus(ofxSCServer* server, int idx, int bus) override;
    int  getOutputBusIndex(ofxSCServer* server, int idx)     override;
    void moveSynthBefore(ofxSCServer* server, int nodeID)    override;
    int  getLastSynthID(ofxSCServer* server)                 override;

    // No audio input
    void setInputBus(ofxSCServer*, scNode*, int)  override {}
    void resetInputBusses(ofxSCServer*, int)       override {}

private:
    std::map<ofxSCServer*, ofxSCSynth*>            synths;
    std::map<ofxSCServer*, std::map<int, int>>     outputBuses;
};
