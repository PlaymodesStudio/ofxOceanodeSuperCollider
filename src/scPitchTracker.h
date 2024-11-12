#ifndef scPitchTracker_h
#define scPitchTracker_h

#include "ofxOceanodeNodeModel.h"
#include "serverManager.h"
#include "ofxSCSynth.h"
#include "ofxSCBus.h"
#include "scNode.h"

class scPitchTracker : public ofxOceanodeNodeModel {
public:
    scPitchTracker(vector<serverManager*> outputServers) : ofxOceanodeNodeModel("SC PitchTracker"){
        servers = outputServers;
        synth = nullptr;
        outBus = nullptr;
    };
    
    ~scPitchTracker(){
        if(synth != nullptr){
            synth->free();
            delete synth;
        }
        if(outBus != nullptr){
            outBus->free();
            delete outBus;
        }
    }
    
    void setup(){
        addParameter(input.set("In", nodePort()), ofxOceanodeParameterFlags_DisableOutConnection);
        addParameter(serverIndex.set("Server", 0, 0, servers.size()-1));
        addParameter(numChannels.set("N Chan", 1, 1, 100));
        addParameter(lagTime.set("Lag Time", 0.2, 0, FLT_MAX));
        addOutputParameter(pitchValues.set("Pitch", {0}, {20}, {2000}));
        
        listeners.push(input.newListener([this](nodePort &port){
            if(port.getNodeRef() != nullptr){
                recreateSynth();
            }else{
                if(synth != nullptr){
                    synth->free();
                    delete synth;
                    synth = nullptr;
                }
                if(outBus != nullptr){
                    outBus->free();
                    delete outBus;
                    outBus = nullptr;
                }
            }
        }));
    
        listeners.push(serverIndex.newListener([this](int &i){
            if(input->getNodeRef() != nullptr){
                recreateSynth();
            }
            serverGraphListener.unsubscribe();
            serverGraphListener = servers[serverIndex]->graphComputed.newListener([this](){
                recreateSynth();
            });
        }));
        
        listeners.push(numChannels.newListener([this](int &i){
            if(input->getNodeRef() != nullptr){
                recreateSynth();
            }
        }));
        
        listeners.push(lagTime.newListener([this](float &f){
            if(synth != nullptr){
                synth->set("lagTime", f);
            }
        }));
    }
    
    void update(ofEventArgs &args) override{
        if(synth != nullptr && outBus != nullptr){
            // Only take every other value (just the frequency, skip the hasFreq)
            vector<float> pitchOnly;
            for(int i = 0; i < outBus->readValues.size()/2; i++) {
                pitchOnly.push_back(outBus->readValues[i]);
            }
            pitchValues = pitchOnly;
            outBus->requestValues();
        }
    }
    
private:
    void recreateSynth(){
        if(synth != nullptr){
            synth->free();
            delete synth;
            synth = nullptr;
        }
        if(outBus != nullptr){
            outBus->free();
            delete outBus;
            outBus = nullptr;
        }
        
        if(input->getNodeRef() != nullptr){
            string synthName = "PitchTracker" + ofToString(numChannels);
            synth = new ofxSCSynth(synthName, servers[serverIndex]->getServer());
            synth->addToTail();
            outBus = new ofxSCBus(RATE_CONTROL, numChannels * 2, servers[serverIndex]->getServer());
            
            synth->set("in", input->getBusIndex(servers[serverIndex]->getServer()));
            synth->set("pitch", outBus->index);
            synth->set("lagTime", lagTime);
            synth->set("decay", decay);
        }
    }

    ofEventListeners listeners;
    ofEventListener serverGraphListener;
    
    ofParameter<nodePort> input;
    ofParameter<int> serverIndex;
    ofParameter<int> numChannels;
    ofParameter<float> lagTime;
    ofParameter<float> decay;
    ofParameter<vector<float>> pitchValues;
    
    ofxSCSynth *synth;
    ofxSCBus *outBus;
    vector<serverManager*> servers;
};

#endif /* scPitchTracker_h */
