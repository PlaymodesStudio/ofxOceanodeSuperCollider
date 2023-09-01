//
//  scServer.cpp
//  ofxOceanodeSuperCollider
//
//  Created by Eduard Frigola on 27/7/23.
//

#include "scServer.h"
#include "scNode.h"
#include "serverManager.h"

scServer::scServer(vector<serverManager*> _outputServers) : ofxOceanodeNodeModel("SC Server"){
    outputServers = _outputServers;
};

scServer::~scServer(){
//    if(controller != nullptr){
//        controller->removeServer(this);
//    }
}

void scServer::setup(){
    addParameter(input.set("In", nullptr), ofxOceanodeParameterFlags_DisableOutConnection);
    listeners.push(input.newListener([this](scNode* &node){
        outputServers[serverIndex]->recomputeGraph(input.get());
    }));
    
    addParameter(serverIndex.set("Server", 0, 0, outputServers.size()-1));
    
    addParameter(outputChannel.set("Chan", 0, 0, INT_MAX));
    listeners.push(outputChannel.newListener([this](int &ch){
        outputServers[serverIndex]->setOutputChannel(ch);
    }));
    
//    if(controller != nullptr){
//        controller->addServer(this);
//    }
}

void scServer::presetHasLoaded(){
    
}
