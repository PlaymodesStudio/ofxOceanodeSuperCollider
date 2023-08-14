//
//  scNode.cpp
//  ofxOceanodeSuperCollider
//
//  Created by Eduard Frigola on 27/7/23.
//

#include "scNode.h"
#include "scServer.h"
#include "ofxSuperCollider.h"

scNode::scNode(std::string name) : ofxOceanodeNodeModel("SC " + name){
    
};

scNode::~scNode(){
    
};

void scNode::addInputs(int numInputs){
    inputs.resize(numInputs);
    for(int i = 0; i < inputs.size(); i++){
        string paramName = "In";
        if(i > 0) paramName += ofToString(i+1);
        addParameter(inputs[i].set(paramName, nullptr), ofxOceanodeParameterFlags_DisableOutConnection);
        listeners.push(inputs[i].newListener([this, i](scNode* node){
            output = output;
        }));
    }
}

void scNode::addOutput(){
    addOutputParameter(output.set("Out", this));
}

//DIY Implementation of BFS https://www.geeksforgeeks.org/breadth-first-search-or-bfs-for-a-graph/
bool scNode::appendOrderedNodes(vector<scNode*> &nodesList, map<scNode*, std::pair<int, vector<int>>> &visitedNodeChilds, vector<scNode*> parents){
    if(visitedNodeChilds.count(this) == 0){
        visitedNodeChilds[this] = std::make_pair(0, vector<int>(0, 0));
    }
    parents.push_back(this);
    int i = visitedNodeChilds[this].first;
    while(visitedNodeChilds[this].second.size() < inputs.size()){
        if(inputs[i].get() != nullptr &&
           std::find(parents.begin(), parents.end(), inputs[i].get()) == parents.end() &&
           inputs[i].get()->appendOrderedNodes(nodesList, visitedNodeChilds, parents)){
            visitedNodeChilds[this].first = (visitedNodeChilds[this].first+1) % inputs.size();
            return true;
        }else if(std::find(visitedNodeChilds[this].second.begin(), visitedNodeChilds[this].second.end(), i) == visitedNodeChilds[this].second.end()){
            visitedNodeChilds[this].second.push_back(i);
        }
        i = (i+1) % inputs.size();
    }
    if(std::find(nodesList.begin(), nodesList.end(), this) == nodesList.end()){
        nodesList.push_back(this);
        return true;
    }
    return false;
}


void scNode::getConnections(std::map<scNode*, vector<scNode*>> &connections){
    for(int i = 0; i < inputs.size(); i++){
        if(inputs[i].get() != nullptr)
            connections[inputs[i].get()].push_back(this);
    }
}


