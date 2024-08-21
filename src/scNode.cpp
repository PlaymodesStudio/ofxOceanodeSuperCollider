//
//  scNode.cpp
//  ofxOceanodeSuperCollider
//
//  Created by Eduard Frigola on 27/7/23.
//

#include "scNode.h"
#include "ofxSuperCollider.h"

int nodePort::getBusIndex(ofxSCServer* server) const{
    if(nodeRef == nullptr) return -1;
    return nodeRef->getOutputBusIndex(server, index);
}

scNode::scNode(std::string name) : ofxOceanodeNodeModel("SC " + name){
    
};

scNode::~scNode(){
    
};

void scNode::addInput(std::string name){
    inputs.emplace_back(name, nodePort());
    addParameter(inputs.back());
    auto availableInput = availableInputs.emplace_back(std::make_shared<nodePort>(inputs.back().get()));
    listeners.push(inputs.back().newListener([this, availableInput](nodePort &port){
        *availableInput = port;
        for(auto &output : outputs) output = output;
    }));
}

void scNode::addOutput(std::string name){
    outputs.emplace_back(name, nodePort(outputs.size(), this));
    addOutputParameter(outputs.back());
}

//DIY Implementation of BFS https://www.geeksforgeeks.org/breadth-first-search-or-bfs-for-a-graph/
bool scNode::appendOrderedNodes(vector<scNode*> &nodesList, map<scNode*, std::pair<int, vector<int>>> &visitedNodeChilds, vector<scNode*> parents){
    if(visitedNodeChilds.count(this) == 0){
        visitedNodeChilds[this] = std::make_pair(0, vector<int>(0, 0));
    }
    parents.push_back(this);
    int i = visitedNodeChilds[this].first;
    while(visitedNodeChilds[this].second.size() < availableInputs.size()){
        if(availableInputs[i]->getNodeRef() != nullptr &&
           std::find(parents.begin(), parents.end(), availableInputs[i]->getNodeRef()) == parents.end() &&
           availableInputs[i]->getNodeRef()->appendOrderedNodes(nodesList, visitedNodeChilds, parents)){
            visitedNodeChilds[this].first = (visitedNodeChilds[this].first+1) % availableInputs.size();
//            nodesList.push_back(this);
//            return true;
        }else if(std::find(visitedNodeChilds[this].second.begin(), visitedNodeChilds[this].second.end(), i) == visitedNodeChilds[this].second.end()){
            visitedNodeChilds[this].second.push_back(i);
        }
        i = (i+1) % availableInputs.size();
    }
    if(std::find(nodesList.begin(), nodesList.end(), this) == nodesList.end()){
        nodesList.push_back(this);
        return true;
    }
    return false;
}


void scNode::getConnections(std::map<nodePort, vector<scNode*>> &connections){
    for(int i = 0; i < availableInputs.size(); i++){
        if(availableInputs[i]->getNodeRef() != nullptr)
            connections[*availableInputs[i]].push_back(this);
    }
}


