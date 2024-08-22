//
//  scSynthdef.h
//  ofxOceanodeSuperCollider
//
//  Created by Eduard Frigola Bagué on 31/7/23.
//

#ifndef scSynthdef_h
#define scSynthdef_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"

class ofxSCSynth;
class ofxSCServer;
class ofxSCServer;

struct synthdefDesc{
    string filepath;
    string name;
    string type;
    map<string, map<string, string>> params;
    string description;
    string category;
    vector<std::pair<string, int>> variables;
};

class scSynthdef : public scNode {
public:
    scSynthdef(synthdefDesc description);
    ~scSynthdef(){
        freeAll();
    }
    
    void setup() override;
    
    
    void presetWillBeLoaded() override{
        //        isPresetLoading = true;
    }
    
    void activateConnections() override{
        //        isPresetLoading = false;
    }
    
    void presetHasLoaded() override{
        //        isPresetLoading = false;
    }
    
    void activate() override{
        //        synth->run(true);
    }
    
    void deactivate() override{
        //        synth->run(false);
    }
    
    void createSynth(ofxSCServer* server) override;
    void free(ofxSCServer* server) override;
    void freeAll();
    
    void setOutputBus(ofxSCServer* server, int index, int bus) override;
    void setInputBus(ofxSCServer* server, scNode* node, int bus) override;
    
    int getOutputBusIndex(ofxSCServer* server, int index) override;
    
    ofEvent<void> resendParams;
    
    static synthdefDesc readAndCreateSynthdef(string file);
    
private:
    
    string getSynthdefFilename();
    
    ofEventListeners listeners;
    
    std::map<ofxSCServer*, ofxSCSynth*> synths;
    
    synthdefDesc synthDescription;
    std::string synthdefName;
    string file;
    ofParameter<int> numChannels;
    int oldNumChannels;
    bool variableChanged;
    std::map<ofxSCServer*, std::map<int, int>> outputBuses;
    std::map<ofxSCServer*, std::map<scNode*, int>> inputBuses;
    
    vector<ofParameter<vector<int>>> buffers;
    
    ofParameter<bool> doNotDistributeInputs;
    ofParameter<bool> doNotDistributeOutputs;
};

#endif /* scSynthdef_h */
