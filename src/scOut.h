//
//  scOut.h
//  Parallels
//
//  Created by Eduard Frigola Bagué on 24/11/22.
//

#ifndef scOut_h
#define scOut_h

#include "ofxOceanodeNodeModel.h"

class scOut : public ofxOceanodeNodeModel {
public:
    scOut(ofxSCServer *_server = ofxSCServer::local()) : ofxOceanodeNodeModel("SC Output"){
        server = _server;
        synth = nullptr;
    };
    ~scOut(){
        if(synth != nullptr){
            synth->free();
            delete synth;
        }
    }
    
    void setup(){
        synth = new ofxSCSynth("output", server);
        synth->addToTail();
        
        ofParameter<std::pair<ofxSCBus*, ofxSCSynth*>> p;
        addParameter(p.set("In", std::make_pair(nullptr, nullptr)), ofxOceanodeParameterFlags_DisableOutConnection);
        listeners.push(p.newListener([this](std::pair<ofxSCBus*, ofxSCSynth*> &pair){
            if(pair.first != nullptr){
                synth->set("in", pair.first->index);
            }
        }));
        addParameter(outputChannel.set("Chan", 0, 0, INT_MAX));
        listeners.push(outputChannel.newListener([this](int &ch){
            synth->set("out", ch);
        }));
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
};

#endif /* scOut_h */
