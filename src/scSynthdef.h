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
#include "scSchedulingCompat.h"
#include "scSynthdefCompiler.h"

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
        // Every scheduled-event handler this node registered points at members
        // that are about to go away.
        scScheduling::unregisterOwner(this);
        scSynthdefCompiler::get().cancel(this);
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
    
    void resendParametersForNRT() override { resendParams.notify(); }
    // The generic mixer SynthDefs are mixer points as much as the C++ one.
    bool isNRTStemPoint() const override {
        return synthdefName.rfind("PolyMixer", 0) == 0 || synthdefName.rfind("Mixer", 0) == 0;
    }
    // Like Mixer Pro, "Encapsulate All" on a mixer SynthDef only takes its audio inputs
    bool canEncapsulateSubgraphFrom(ofxOceanodeAbstractParameter& parameter) const override {
        return !isNRTStemPoint() || std::any_of(inputs.begin(), inputs.end(), [&](const ofParameter<nodePort>& input) {
            return input.getName() == parameter.getName();
        });
    }
    ofEvent<void> resendParams;
    ofEvent<std::pair<ofxSCServer*, int>> resetAudioRateBusAssignments;
    ofEvent<std::tuple<ofxSCServer*, scNode*, int>> setAudioRateBusAssignment;
    
    static synthdefDesc readAndCreateSynthdef(string file);
    
private:
    string getSynthdefFilename();
    // Replace every synth with one of getSynthdefFilename(), parameters and
    // bus wiring included, in place of the old one.
    void replaceSynths();
    // N Chan / a variable asks for a definition with no .scsyndef: keep the
    // current synths paused and ask scSynthdefCompiler for it.
    void waitForSynthdef(const std::string& defName);
    // Declined or failed: go back to what the synths were built with.
    void revertToBuiltSynthdef();
    // True when unknown (no metadata path): then the server decides, as before.
    bool isSynthdefCompiled(const std::string& defName) const;
    // inChannels and the input / output bus controls of the synth on server
    void resendBusesToSynth(ofxSCServer* server);
    
    ofEventListeners listeners;
    
    std::map<ofxSCServer*, ofxSCSynth*> synths;
    // Bumped whenever a synth object is created, replaced or deleted: the
    // per-parameter "last value sent" caches are only valid for the synths
    // they were filled for (a new synth can even reuse a freed address).
    uint64_t sendCacheEpoch = 0;
    
    synthdefDesc synthDescription;
    std::string synthdefName;
    string file;
    ofParameter<int> numChannels;
    int oldNumChannels;
    bool variableChanged;
    std::map<ofxSCServer*, std::map<int, int>> outputBuses;
    std::map<ofxSCServer*, std::map<scNode*, int>> inputBuses;
    std::map<ofxSCServer*, std::map<std::string, std::pair<int, int>>> audioRateState; // name -> (_sel, bus)
    
    ofParameter<bool> doNotDistributeInputs;
    ofParameter<bool> doNotDistributeOutputs;

    // What the synths actually run. While waitingForSynthdef the parameters
    // already ask for another definition, which is being compiled; the synths
    // stay paused, since they would read parameter vectors of the wrong size.
    std::string builtSynthdef;
    int builtNumChannels = 1;
    std::map<std::string, int> builtVariables;
    // Last value seen per variable, to tell a real change from a re-notify
    std::map<std::string, int> lastVariableValues;
    bool waitingForSynthdef = false;
};

#endif /* scSynthdef_h */
