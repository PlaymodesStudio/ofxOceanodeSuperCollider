//
//  scInfo.h
//  Parallels
//
//  Created by Eduard Frigola Bagué on 03/01/2023.
//

#ifndef scInfo_h
#define scInfo_h

#include "ofxOceanodeSuperColliderConfig.h"
#include "ofxOceanodeNodeModel.h"
#include "scNode.h"

class scInfo : public ofxOceanodeNodeModel {
public:
    scInfo(vector<serverManager*> outputServers) : ofxOceanodeNodeModel("SC Info"){
        servers = outputServers;
        synth = nullptr;
        ampBus = nullptr;
        peakBus = nullptr;
        valueBus = nullptr;
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
        if(valueBus != nullptr){
            valueBus->free();
            delete valueBus;
        }
    }
    
    void setup(){
        addParameter(showWindow.set("Show", false));
        
        addParameter(input.set("In", nodePort()), ofxOceanodeParameterFlags_DisableOutConnection);
        addParameter(serverIndex.set("Server", 0, 0, servers.size()-1));
        addParameter(numChannels.set("N Chan", 1, 1, MAX_NODE_CHANNELS));
        
        listeners.push(input.newListener([this](nodePort &port){
            if(port.getNodeRef() != nullptr){
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
                if(valueBus != nullptr){
                    valueBus->free();
                    delete valueBus;
                    valueBus = nullptr;
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
        addOutputParameter(values.set("Values", {0}, {-FLT_MAX}, {FLT_MAX}));
    }
    
    void update(ofEventArgs &args) override{
        if(synth != nullptr){
            amps = ampBus->readValues;
            peaks = peakBus->readValues;
            values = valueBus->readValues;
            ampBus->requestValues();
            peakBus->requestValues();
            valueBus->requestValues();
        }
    }
    
    void draw(ofEventArgs &args) override{
        if(showWindow){
            string modCanvasID = canvasID == "Canvas" ? "" : (canvasID + "/");
            if(ImGui::Begin((modCanvasID + "SC Info " +
                ofToString(getNumIdentifier())).c_str(), (bool *)&showWindow.get())){
                auto size = ImGui::GetContentRegionAvail();
                size.x = std::max(1.0f, size.x);
                size.y = std::max(1.0f, size.y);
                if(synth != nullptr && ampBus != nullptr && peakBus != nullptr){
                    bool drewAmpPlot = false;
                    if(!ampBus->readValues.empty()){
                        ImGui::PlotHistogram("##scinfo_amp", ampBus->readValues.data(), (int)ampBus->readValues.size(), 0, NULL, 0, 1, size);
                        drewAmpPlot = true;
                    }
                    if(!peakBus->readValues.empty()){
                        if(drewAmpPlot) ImGui::SameLine(7);
                        vector<float> peakValues;
                        int eachChannelSize = std::max(1, (int)size.x / (int)peakBus->readValues.size());
                        peakValues.reserve(eachChannelSize * peakBus->readValues.size());
                        for(int i = 0; i < peakBus->readValues.size(); i++){
                            vector<float> vals(eachChannelSize, peakBus->readValues[i]);
                            peakValues.insert(peakValues.end(), vals.begin(), vals.end());
                        }
                        if(!peakValues.empty()){
                            ImGui::PlotLines("##scinfo_peak", peakValues.data(), (int)peakValues.size(), 0, NULL, 0, 1, size);
                        }
                    }
                    ampBus->requestValues();
                    peakBus->requestValues();
                }
            }
            ImGui::End();
        }
    }
    
    void activate() override {
        if(synth) synth->run(true);
    }

    void deactivate() override {
        if(synth) synth->run(false);
    }

    void recreateSynth(){
        int numChans = numChannels;
        if(numChannels < 1 || numChannels > MAX_NODE_CHANNELS) return;
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
        if(valueBus != nullptr){
            valueBus->free();
            delete valueBus;
        }
        if(input->getNodeRef() != nullptr){
            synth = new ofxSCSynth("Info" + ofToString(numChans), servers[serverIndex]->getServer());
            synth->createAndRun(1, 1, getActive()); //addToTail
            ampBus = new ofxSCBus(RATE_CONTROL, numChans, servers[serverIndex]->getServer());
            peakBus = new ofxSCBus(RATE_CONTROL, numChans, servers[serverIndex]->getServer());
            valueBus = new ofxSCBus(RATE_CONTROL, numChans, servers[serverIndex]->getServer());
            
            synth->set("in", input->getBusIndex(servers[serverIndex]->getServer()));
            synth->set("lagTime", lagTime);
            synth->set("decay", decay);
            synth->set("amp", ampBus->index);
            synth->set("peak", peakBus->index);
            synth->set("value", valueBus->index);
        }
    }
    
private:
    ofEventListeners listeners;
    ofEventListener serverGraphListener;
    
    ofParameter<float> lagTime;
    ofParameter<float> decay;
    ofParameter<bool> showWindow;
    
    ofParameter<nodePort> input;
    ofParameter<int> serverIndex;
    ofParameter<int> numChannels;
    
    ofParameter<vector<float>> amps;
    ofParameter<vector<float>> peaks;
    ofParameter<vector<float>> values;
    
    ofxSCBus* ampBus;
    ofxSCBus* peakBus;
    ofxSCBus* valueBus;
    
    ofxSCSynth *synth;
    vector<serverManager*> servers;
};

#endif /* scInfo_h */
