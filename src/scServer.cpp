//
//  scServer.cpp
//  ofxOceanodeSuperCollider
//
//  Created by Eduard Frigola on 27/7/23.
//

#include "scServer.h"
#include "ofxOceanodeSuperCollider.h"
#include "ofxOceanodeSuperColliderController.h"
#include "ofxSuperCollider.h"
#include "scNode.h"

scServer::scServer(ofxSCServer *_server, ofxOceanodeSuperColliderController *_controller, int index, vector<string> _wavs) : ofxOceanodeNodeModel("SC Server " + ofToString(index)){
    server = _server;
    controller = _controller;
    synth = nullptr;
    wavs = _wavs;
};

scServer::~scServer(){
    if(synth != nullptr){
        synth->free();
        delete synth;
    }
    if(controller != nullptr){
        controller->removeServer(this);
    }
}

void scServer::setup(){
    synth = new ofxSCSynth("output", server);
    synth->addToTail();
    
    addParameter(input.set("In", nullptr), ofxOceanodeParameterFlags_DisableOutConnection);
    listeners.push(input.newListener([this](scNode* &node){
        recomputeGraph();
    }));
    
    addParameter(outputChannel.set("Chan", 0, 0, INT_MAX));
    listeners.push(outputChannel.newListener([this](int &ch){
        synth->set("out", ch);
    }));
    
    if(controller != nullptr){
        controller->addServer(this);
    }
    
    for(auto &w : wavs){
        buffers[w] = new ofxSCBuffer(0, 0, server);
        buffers[w]->readChannel(ofToDataPath("Supercollider/Samples/" + w, true), {0});
    }
}

void scServer::setVolume(float volume){
    synth->set("levels", volume);
}

void scServer::setDelay(int delay){
    synth->set("delay", delay);
}

void scServer::presetHasLoaded(){
    
}

void scServer::recomputeGraph(){
    if(input.get() == nullptr){
        for(auto node : nodesList) node->free(server);
        nodesList.clear();
        for(auto b : busses) b->free();
        busses.clear();
    }else{
        vector<scNode*> newNodesList;
        map<scNode*, std::pair<int, vector<int>>> nodeChilds;
        if(input.get() != nullptr)
            while(input.get()->appendOrderedNodes(newNodesList, nodeChilds));
        
        for(auto node : nodesList){
//            if(std::find(newNodesList.begin(), newNodesList.end(), node) == newNodesList.end()){
                node->free(server);
//            }else{
//                newNodesList.erase(std::remove(newNodesList.begin(), newNodesList.end(), node), newNodesList.end());
//            }
        }
        
        nodesList = newNodesList;
    //    ofLog() << " ----- Server " << getNumIdentifier() << " -----";
//        for(auto n : nodesList) ofLog() << n->getNumIdentifier();
    //    ofLog() << " -------------------";
        
        std::map<scNode*, vector<scNode*>> connections;
//        for(auto n : nodesList){
//            n->getConnections(connections);
//        }
        
//        for(auto n : nodesList){
//            n->createSynth(server);
//        }
        for (auto it = nodesList.rbegin(); it != nodesList.rend(); ++it) {
            (*it)->getConnections(connections);
            (*it)->createSynth(server);
        }
        
        for(auto b : busses) b->free();
        busses.clear();
        
        auto busref = busses.emplace_back(new ofxSCBus(RATE_AUDIO, MAX_NODE_CHANNELS, server)); //From server to first node
        input.get()->setOutputBus(server, busref->index);
        synth->set("in", busref->index);
        for(auto &c : connections){
            busref = busses.emplace_back(new ofxSCBus(RATE_AUDIO, MAX_NODE_CHANNELS, server));
            c.first->setOutputBus(server, busref->index);
            for(auto &dest : c.second){
                dest->setInputBus(server, c.first, busref->index);
    //            ofLog() << c.first->getNumIdentifier() << " ---- " << dest->getNumIdentifier();
            }
        }
    }
    
    
    
    
    
    int x = 0;
    x+1;
}
