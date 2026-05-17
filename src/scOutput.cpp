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
    stereomix = false;
    stereomixSize = 2;
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

    addParameter(delayMsParam.set("Delay Ms", 0.0f, 0.0f, 10000.0f));
    listeners.push(delayMsParam.newListener([this](float &ms){
        setDelay((int)ms);
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
        // DelayN \delay argument is in seconds; delay is stored in ms
        synth->set("delay", delay / 1000.0f);
    }
}

void scOutput::setStereoMix(bool _stereomix){
    stereomix = _stereomix;
    if(synth != nullptr){
        synth->set("stereomix", stereomix);
    }
}

void scOutput::setStereoMixSize(int _stereomixSize){
    stereomixSize = _stereomixSize;
    if(synth != nullptr){
        synth->set("stereomixsize", stereomixSize);
    }
}

void scOutput::activate(){
    if(synth != nullptr) synth->run(true);
}

void scOutput::deactivate(){
    if(synth != nullptr) synth->run(false);
}

void scOutput::buildSynth(ofxSCServer *server){
    if(server == outputServers[serverIndex]->getServer()){
        synth = new ofxSCSynth("output", server);
    }
}

void scOutput::createSynth(ofxSCServer* server){
    if(server == outputServers[serverIndex]->getServer()){
        synth->set("out", outputChannel);
        synth->set("levels", volume);
        synth->set("delay", delay / 1000.0f);   // ms → seconds for DelayN
        synth->set("stereomix", stereomix);
        synth->set("stereomixsize", stereomixSize);
        synth->set("in", inputBus[server]);
        synth->createAndRun(0, 1, getActive());
    }
}

void scOutput::moveSynthBefore(ofxSCServer* server, int nodeID){
    if(server == outputServers[serverIndex]->getServer()){
        synth->set("out", outputChannel);
        synth->set("levels", volume);
        synth->set("delay", delay / 1000.0f);   // ms → seconds for DelayN
        synth->set("stereomix", stereomix);
        synth->set("stereomixsize", stereomixSize);
        synth->set("in", inputBus[server]);
        synth->moveBefore(nodeID);
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
    inputBus[server] = bus;
    if(synth != nullptr && inputs[0]->getNodeRef() == node){
        synth->set("in", bus);
    }
}

void scOutput::resetInputBusses(ofxSCServer* server, int targetBus){
    inputBus[server] = 0;
    if(synth != nullptr){
        synth->set("in", targetBus);
    }
}

int scOutput::getLastSynthID(ofxSCServer* server){
    if(server == outputServers[serverIndex]->getServer()){
        return synth->nodeID;
    }
    return -1;
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
