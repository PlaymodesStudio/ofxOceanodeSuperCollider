//
//  scServer.h
//  ofxOceanodeSuperCollider
//
//  Created by Eduard Frigola on 27/7/23.
//

#ifndef scServer_h
#define scServer_h

#include "ofxOceanodeNodeModel.h"

class scNode;
class serverManager;

class scServer : public ofxOceanodeNodeModel {
public:
    scServer(vector<serverManager*> outputServers);
    ~scServer();
    
    void setup() override;
    
    void setVolume(float volume);
    
    void setDelay(int delay);
    
    void presetHasLoaded() override;
    
private:
    ofEventListeners listeners;
    
    ofParameter<int> outputChannel;
    ofParameter<int> serverIndex;
    ofParameter<scNode*> input;
    
    vector<serverManager*> outputServers;
};

#endif /* scServer_h */
