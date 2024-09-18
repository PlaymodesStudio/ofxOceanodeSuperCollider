//
//  scInfo.h
//  Parallels
//
//  Created by Eduard Frigola Bagué on 03/01/2023.
//

#ifndef scInfo_h
#define scInfo_h

#include "ofxOceanodeNodeModel.h"

class scInfo : public ofxOceanodeNodeModel {
public:
    scInfo(vector<serverManager*> outputServers) : ofxOceanodeNodeModel("SC Info"){
        servers = outputServers;
        synth = nullptr;
        ampBus = nullptr;
        peakBus = nullptr;
    };
    ~scInfo(){
        if(synth != nullptr){
            synth->free();
            delete synth;
        }
        if(ampBus != nullptr){
            ampBus->free();
            delete ampBus;
        }
        if(peakBus != nullptr){
            peakBus->free();
            delete peakBus;
        }
    }
    
    void setup(){
        addParameter(showWindow.set("Show", false));
        
        addParameter(input.set("In", nullptr), ofxOceanodeParameterFlags_DisableOutConnection);
        addParameter(serverIndex.set("Server", 0, 0, servers.size()-1));
        addParameter(numChannels.set("N Chan", 1, 1, 100));
        
        listeners.push(input.newListener([this](scNode* node){
            if(node != nullptr){
                recreateSynth();
            }else{
                if(synth != nullptr){
                    synth->free();
                    delete synth;
                    synth = nullptr;
                }
                if(ampBus != nullptr){
                    ampBus->free();
                    delete ampBus;
                    ampBus = nullptr;
                }
                if(peakBus != nullptr){
                    peakBus->free();
                    delete peakBus;
                    peakBus = nullptr;
                }
            }
        }));
    
        listeners.push(serverIndex.newListener([this](int &i){
            if(input.get() != nullptr){
                recreateSynth();
            }
            serverGraphListener.unsubscribe();
            serverGraphListener = servers[serverIndex]->graphComputed.newListener([this](){
                recreateSynth();
            });
        }));
        
        listeners.push(numChannels.newListener([this](int &i){
            if(input.get() != nullptr){
                recreateSynth();
            }
        }));
        
        addParameter(lagTime.set("Lag Time", 0.2, 0, FLT_MAX));
        
        listeners.push(lagTime.newListener([this](float &f){
            if(synth != nullptr)
                synth->set("lagTime", f);
        }));
        
        addParameter(decay.set("Decay", 0.99, 0, 1));
        
        listeners.push(decay.newListener([this](float &f){
            if(synth != nullptr)
                synth->set("decay", f);
        }));
        
        addOutputParameter(amps.set("Amps", {0}, {0}, {1}));
        addOutputParameter(peaks.set("Peaks", {0}, {0}, {1}));
    }
    
    void update(ofEventArgs &args) override{
        if(synth != nullptr){
            amps = ampBus->readValues;
            peaks = peakBus->readValues;
            ampBus->requestValues();
            peakBus->requestValues();
        }
    }
    
    void draw(ofEventArgs &args) override{
        if(showWindow){
            string modCanvasID = canvasID == "Canvas" ? "" : (canvasID + "/");
            if(ImGui::Begin((modCanvasID + "SC Info " +
                ofToString(getNumIdentifier())).c_str(), (bool *)&showWindow.get())){
                auto size = ImGui::GetContentRegionAvail();
                if(synth != nullptr){
                    ImGui::PlotHistogram("", &ampBus->readValues[0], ampBus->readValues.size(), 0, NULL, 0, 1, size);
                    ImGui::SameLine(7);
                    vector<float> peakValues(0, size.x);
                    int eachChannelSize = size.x / peakBus->readValues.size();
                    for(int i = 0; i < peakBus->readValues.size(); i++){
                        vector<float> vals(eachChannelSize, peakBus->readValues[i]);
                        peakValues.insert(peakValues.begin() + ( i * eachChannelSize), vals.begin(), vals.end());
                    }
                    
                    
                    ImGui::PlotLines("", &peakValues[0], peakValues.size(), 0, NULL, 0, 1, size);
                    ampBus->requestValues();
                    peakBus->requestValues();
                }
            }
            ImGui::End();
        }
    }
    
    void recreateSynth(){
        int numChans = numChannels;
        if(synth != nullptr){
            synth->free();
            delete synth;
            synth = nullptr;
        }
        if(ampBus != nullptr){
            ampBus->free();
            delete ampBus;
            ampBus = nullptr;
        }
        if(peakBus != nullptr){
            peakBus->free();
            delete peakBus;
            peakBus = nullptr;
        }
        if(input.get() != nullptr){
            synth = new ofxSCSynth("info" + ofToString(numChans), servers[serverIndex]->getServer());
            synth->addToTail();
            ampBus = new ofxSCBus(RATE_CONTROL, numChans, servers[serverIndex]->getServer());
            peakBus = new ofxSCBus(RATE_CONTROL, numChans, servers[serverIndex]->getServer());
            
            synth->set("in", servers[serverIndex]->getOutputBusForNode(input.get()));
            synth->set("lagTime", lagTime);
            synth->set("decay", decay);
            synth->set("amp", ampBus->index);
            synth->set("peak", peakBus->index);
        }
    }
    
private:
    ofEventListeners listeners;
    ofEventListener serverGraphListener;
    
    ofParameter<float> lagTime;
    ofParameter<float> decay;
    ofParameter<bool> showWindow;
    
    ofParameter<scNode*> input;
    ofParameter<int> serverIndex;
    ofParameter<int> numChannels;
    
    ofParameter<vector<float>> amps;
    ofParameter<vector<float>> peaks;
    
    ofxSCBus* ampBus;
    ofxSCBus* peakBus;
    
    ofxSCSynth *synth;
    vector<serverManager*> servers;
};

#endif /* scInfo_h */
