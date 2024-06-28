//
//  scOutput.cpp
//  ofxOceanodeSupeCollider
//
//  Created by Eduard Frigola on 6/5/24.
//

#include "scOutput.h"
#include "serverManager.h"
#include "ofxSCSynth.h"

scOutput::scOutput(vector<serverManager*> _outputServers) : scNode("Output"){
    outputServers = _outputServers;
    synth = nullptr;
    volume = 0;
    delay = 0;
    serverIndex = -1;
}

scOutput::~scOutput(){
    if(serverIndex != -1)
    outputServers[serverIndex]->removeOutput(this);
}

void scOutput::setup(){
    scNode::addInput("In");
    listeners.push(inputs[0].newListener([this](nodePort &port){
        outputServers[serverIndex]->recomputeGraph();
    }));
    
    addParameter(serverIndex.set("Server", 0, 0, outputServers.size()-1));
    listeners.push(serverIndex.newListener([this](int &serverIdx){
        if(lastServerIndex != serverIdx){
            outputServers[lastServerIndex]->removeOutput(this);
            outputServers[serverIndex]->addOutput(this);
            outputServers[serverIndex]->recomputeGraph();
            lastServerIndex = serverIdx;
        }
    }));
    
    lastServerIndex = serverIndex;
    outputServers[serverIndex]->addOutput(this);
    
    addParameter(outputChannel.set("Chan", 0, 0, INT_MAX));
    listeners.push(outputChannel.newListener([this](int &ch){
        if(synth != nullptr){
            synth->set("out", ch);
        }
    }));
    
}

void scOutput::setVolume(float _volume){
    volume = _volume;
    if(synth != nullptr){
        synth->set("levels", volume);
    }
}

void scOutput::setDelay(int _delay){
    delay = _delay;
    if(synth != nullptr){
        synth->set("delay", delay);
    }
}

void scOutput::createSynth(ofxSCServer* server){
    if(server == outputServers[serverIndex]->getServer()){
        synth = new ofxSCSynth("output", server);
        synth->create();
        synth->set("out", outputChannel);
        synth->set("levels", volume);
        synth->set("delay", delay);
    }
}

void scOutput::free(ofxSCServer* server){
    if(synth != nullptr){
        synth->free();
        delete synth;
        synth = nullptr;
    }
}

void scOutput::setInputBus(ofxSCServer* server, scNode* node, int bus){
    if(synth != nullptr && inputs[0]->getNodeRef() == node){
        synth->set("in", bus);
    }
}

//void scOutput::presetWillBeLoaded(){
//    isLoadingPreset = true;
//    outputServers[serverIndex]->resetRecomputeGraphOnce();
//}
//
//void scOutput::presetHasLoaded(){
//    isLoadingPreset = false;
//    outputServers[serverIndex]->recomputeGraphOnce();
//}
