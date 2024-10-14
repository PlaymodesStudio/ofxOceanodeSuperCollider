//
//  scOldSynthdef.h
//  ofxOceanodeSuperCollider
//
//  Created by Eduard Frigola Bagué on 31/7/23.
//

#ifndef scOldSynthdef_h
#define scOldSynthdef_h

#include "ofxOceanodeNodeModel.h"
#include "scNode.h"

class ofxSCSynth;
class ofxSCServer;
class ofxSCServer;

struct oldSynthdefDesc{
    string filepath;
    string name;
    string type;
    int numInputs;
    int numBuffers;
    int numChannels;
    map<string, map<string, string>> params;
};

class scOldSynthdef : public scNode {
public:
    scOldSynthdef(oldSynthdefDesc description);
    ~scOldSynthdef(){
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

    static oldSynthdefDesc readAndCreateSynthdef(string file);

private:
    ofEventListeners listeners;

    std::map<ofxSCServer*, ofxSCSynth*> synths;

    oldSynthdefDesc description;
    std::string synthdefName;
    string file;
    ofParameter<int> numChannels;
    std::map<ofxSCServer*, int> outputBus;
    std::map<ofxSCServer*, std::map<scNode*, int>> inputBuses;

    vector<ofParameter<vector<int>>> buffers;
};

#endif /* scOldSynthdef_h */
