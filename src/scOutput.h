//
//  scOutput.hpp
//  ofxOceanodeSuperCollider
//
//  Created by Eduard Frigola on 6/5/24.
//

#ifndef scOutput_hpp
#define scOutput_hpp

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"

class serverManager;
class ofxSCSynth;

class scOutput : public scNode {
public:
    scOutput(vector<serverManager*> outputServers);
    ~scOutput();
    
    void setup() override;
    
    void setVolume(float volume);
    
    void setDelay(int delay);
    
    void createSynth(ofxSCServer* server) override;
    void free(ofxSCServer* server) override;
    
    bool isInputConnected(){return inputs[0].get() != nullptr;}
    scNode* getInputNode(){return inputs[0].get();}
    
    void setInputBus(ofxSCServer* server, scNode* node, int bus) override;
    
//    void presetRecallBeforeSettingParameters(ofJson &json) override;
//
//    void presetHasLoaded() override;
    
private:
    ofEventListeners listeners;
    
    ofParameter<int> outputChannel;
    ofParameter<int> serverIndex;
    
    float volume;
    int delay;
    
    int lastServerIndex;
    
    vector<serverManager*> outputServers;
    
    ofxSCSynth* synth;
//    bool isLoadingPreset;
};

#endif /* scOutput_hpp */
