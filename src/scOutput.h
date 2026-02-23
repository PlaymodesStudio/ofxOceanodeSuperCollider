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
    
    void setStereoMix(bool stereomix);
    void setStereoMixSize(int stereomixSize);
    
    void activate() override;
    void deactivate() override;

    void buildSynth(ofxSCServer* server) override;
    void createSynth(ofxSCServer* server) override;
    void moveSynthBefore(ofxSCServer* server, int nodeID) override;
    void free(ofxSCServer* server) override;
    
    bool isInputConnected(){return inputs[0]->getNodeRef() != nullptr;}
    scNode* getInputNode(){return inputs[0]->getNodeRef();}
    
    void setInputBus(ofxSCServer* server, scNode* node, int bus) override;
    void resetInputBusses(ofxSCServer* server, int targetBus = 0) override;
    
    int getLastSynthID(ofxSCServer* server) override;
    
//    void presetRecallBeforeSettingParameters(ofJson &json) override;
//
//    void presetHasLoaded() override;
    
private:
    ofEventListeners listeners;
    
    ofParameter<int> outputChannel;
    ofParameter<int> serverIndex;
    
    float volume;
    int delay;
    bool stereomix;
    int stereomixSize;
    
    int lastServerIndex;
    
    vector<serverManager*> outputServers;
    std::map<ofxSCServer*, int> inputBus;
    
    ofxSCSynth* synth;
//    bool isLoadingPreset;
};

#endif /* scOutput_hpp */
