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
class ofxSCBus;

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
    
    void activate() override;
    
    void deactivate() override;
    
    void buildSynth(ofxSCServer* server) override;
    void createSynth(ofxSCServer* server) override;
    void moveSynthBefore(ofxSCServer* server, int nodeID) override;
    void free(ofxSCServer* server) override;
    void freeAll();
    
    void setOutputBus(ofxSCServer* server, int index, int bus) override;
    void setInputBus(ofxSCServer* server, scNode* node, int bus) override;
    void resetInputBusses(ofxSCServer* server, int targetBus = 0) override;
    
    int getOutputBusIndex(ofxSCServer* server, int index) override;
    
    int getLastSynthID(ofxSCServer* server) override;
    
    ofEvent<void> resendParams;
    ofEvent<std::pair<ofxSCServer*, int>> resetAudioRateBusAssignments;
    ofEvent<std::tuple<ofxSCServer*, scNode*, int>> setAudioRateBusAssignment;
    
    static synthdefDesc readAndCreateSynthdef(string file);
    
private:
    string getSynthdefFilename();
    string getCompensationSynthdefFilename();
	string findNextAvailableSynthdef();
    bool supportsSerialIterations() const;
    bool usesIterationCompensation() const;
    int getEffectiveIterations() const;
    void buildSynthChain(ofxSCServer* server);
    void replaceSynthChain(ofxSCServer* server);
    void createSynthChain(ofxSCServer* server);
    void createSerialResources(ofxSCServer* server);
    void configureSynthRouting(ofxSCServer* server, ofxSCSynth* synth, int iterationIndex);
    void configureCompensationRouting(ofxSCServer* server);
    void freeSynthChain(ofxSCServer* server);
    void freeSerialSynths(ofxSCServer* server);
    void freeCompensationSynth(ofxSCServer* server);
    void freeSerialBuses(ofxSCServer* server);
    template<typename Function>
    void forEachSynth(Function function){
        for(auto &synthServer : synths){
            forEachSynth(synthServer.first, function);
        }
    }
    template<typename Function>
    void forEachSynth(ofxSCServer* server, Function function){
        if(synths.count(server) != 0 && synths[server] != nullptr){
            function(synths[server]);
        }
        if(serialSynths.count(server) != 0){
            for(auto synth : serialSynths[server]){
                if(synth != nullptr) function(synth);
            }
        }
    }
    
    ofEventListeners listeners;
    
    std::map<ofxSCServer*, ofxSCSynth*> synths;
    std::map<ofxSCServer*, std::vector<ofxSCSynth*>> serialSynths;
    std::map<ofxSCServer*, ofxSCSynth*> compensationSynths;
    std::map<ofxSCServer*, std::vector<ofxSCBus*>> serialBuses;
    
    synthdefDesc synthDescription;
    std::string synthdefName;
    string file;
    ofParameter<int> numChannels;
    ofParameter<int> iterations;
    int oldNumChannels;
    int oldIterations;
    bool variableChanged;
    std::map<ofxSCServer*, std::map<int, int>> outputBuses;
    std::map<ofxSCServer*, std::map<scNode*, int>> inputBuses;
    std::map<ofxSCServer*, int> defaultInputBuses;
    
    ofParameter<bool> doNotDistributeInputs;
    ofParameter<bool> doNotDistributeOutputs;
	bool synthdefExists(string filename);
};

#endif /* scSynthdef_h */
