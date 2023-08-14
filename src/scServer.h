//
//  scServer.h
//  ofxOceanodeSuperCollider
//
//  Created by Eduard Frigola on 27/7/23.
//

#ifndef scServer_h
#define scServer_h

#include "ofxOceanodeNodeModel.h"
#include "ofxSCBus.h"

class ofxSCServer;
class ofxSCSynth;
class scNode;
class ofxOceanodeSuperColliderController;

class scServer : public ofxOceanodeNodeModel {
public:
    scServer(ofxSCServer *_server, ofxOceanodeSuperColliderController *_controller, int index);
    ~scServer();
    
    void setup() override;
    
    void setVolume(float volume);
    
    void setDelay(int delay);
    
    void presetHasLoaded() override;
    
private:
    void recomputeGraph();

    ofEventListeners listeners;
    
    ofParameter<int> outputChannel;
    ofParameter<scNode*> input;
    vector<scNode*> connectedNodes; //List of all nodes
    
    ofxSCSynth *synth;
    ofxSCServer *server;
    
    
    vector<scNode*> nodesList;
    std::map<scNode*, std::map<int, vector<ofxSCSynth*>>> synthMap;
    
    vector<ofxSCBus*> busses;
    ofxOceanodeSuperColliderController *controller;
};

#endif /* scServer_h */
