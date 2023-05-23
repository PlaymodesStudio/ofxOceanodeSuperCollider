//
//  scOut.h
//  Parallels
//
//  Created by Eduard Frigola Bagué on 24/11/22.
//

#ifndef scOut_h
#define scOut_h

#include "ofxOceanodeNodeModel.h"
#include "ofxOceanodeSuperColliderController.h"
#include "scSynthdef.h"

class scOut : public ofxOceanodeNodeModel {
public:
    scOut(ofxSCServer *_server = ofxSCServer::local(), ofxOceanodeSuperColliderController *_controller = nullptr) : ofxOceanodeNodeModel("SC Output"){
        server = _server;
        controller = _controller;
        synth = nullptr;
        triggerOrderInNextCycle = false;
    };
    ~scOut(){
        if(synth != nullptr){
            synth->free();
            delete synth;
        }
        if(controller != nullptr){
            controller->removeOutput(this);
        }
    }
    
    void setup(){
        synth = new ofxSCSynth("output", server);
        synth->addToTail();
        
        ofParameter<std::pair<ofxSCBus*, scSynthdef*>> p;
        addParameter(p.set("In", std::make_pair(nullptr, nullptr)), ofxOceanodeParameterFlags_DisableOutConnection);
        listeners.push(p.newListener([this](std::pair<ofxSCBus*, scSynthdef*> &pair){
            if(pair.first != nullptr){
                synth->set("in", pair.first->index);
            }
        }));
        addParameter(outputChannel.set("Chan", 0, 0, INT_MAX));
        listeners.push(outputChannel.newListener([this](int &ch){
            synth->set("out", ch);
        }));
        
        if(controller != nullptr){
            controller->addOutput(this);
        }
    }
    
    void update(ofEventArgs &a) override{
        if(triggerOrderInNextCycle){
            auto p = getParameter<std::pair<ofxSCBus*, scSynthdef*>>("In");
            if(p->first != nullptr){
                p->second->triggerNodeOrder();
            }
        }
        triggerOrderInNextCycle = false;
    }
    
    void setVolume(float volume){
        synth->set("levels", volume);
    }
    
    void setDelay(int delay){
        synth->set("delay", delay);
    }
    
    void presetHasLoaded() override{
        triggerOrderInNextCycle = true;
    }
    
private:
    ofEventListeners listeners;
    
    ofParameter<int> outputChannel;
    
    std::string synthdefName;
    std::string params;
    int ins;
    int outs;
    
    ofxSCSynth *synth;
    ofxSCServer *server;
    ofxOceanodeSuperColliderController *controller;
    bool triggerOrderInNextCycle;
};

#endif /* scOut_h */
