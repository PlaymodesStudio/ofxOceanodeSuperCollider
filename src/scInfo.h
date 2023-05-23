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
    scInfo(ofxSCServer *_server = ofxSCServer::local()) : ofxOceanodeNodeModel("SC Info"){
        server = _server;
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
        
        ofParameter<std::pair<ofxSCBus*, scSynthdef*>> p;
        addParameter(p.set("In", std::make_pair(nullptr, nullptr)), ofxOceanodeParameterFlags_DisableOutConnection);
        listeners.push(p.newListener([this](std::pair<ofxSCBus*, scSynthdef*> &pair){
            if(pair.first != nullptr){
                int numChans = pair.first->channels;
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
                synth = new ofxSCSynth("info" + ofToString(numChans), server);
                synth->addToTail();
                ampBus = new ofxSCBus(RATE_CONTROL, numChans, server);
                peakBus = new ofxSCBus(RATE_CONTROL, numChans, server);
                
                synth->set("in", pair.first->index);
                synth->set("lagTime", lagTime);
                synth->set("decay", decay);
                synth->set("amp", ampBus->index);
                synth->set("peak", peakBus->index);
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
//                    ampBus->requestValues();
//                    peakBus->requestValues();
                }
            }
            ImGui::End();
        }
    }
    
private:
    ofEventListeners listeners;
    
    ofParameter<float> lagTime;
    ofParameter<float> decay;
    ofParameter<bool> showWindow;
    
    ofParameter<vector<float>> amps;
    ofParameter<vector<float>> peaks;
    
    ofxSCBus* ampBus;
    ofxSCBus* peakBus;
    
    ofxSCSynth *synth;
    ofxSCServer *server;
};

#endif /* scInfo_h */
