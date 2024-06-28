//
//  scNode.cpp
//  ofxOceanodeSuperCollider
//
//  Created by Eduard Frigola on 27/7/23.
//

#include "scNode.h"
#include "ofxSuperCollider.h"

scNode::scNode(std::string name) : ofxOceanodeNodeModel("SC " + name){
    
};

scNode::~scNode(){
    
};

void scNode::addInput(std::string name){
    inputs.emplace_back(name, nodePort());
    addParameter(inputs.back());
    listeners.push(inputs.back().newListener([this](nodePort &port){
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
    while(visitedNodeChilds[this].second.size() < inputs.size()){
        if(inputs[i]->getNodeRef() != nullptr &&
           std::find(parents.begin(), parents.end(), inputs[i]->getNodeRef()) == parents.end() &&
           inputs[i]->getNodeRef()->appendOrderedNodes(nodesList, visitedNodeChilds, parents)){
            visitedNodeChilds[this].first = (visitedNodeChilds[this].first+1) % inputs.size();
//            nodesList.push_back(this);
//            return true;
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


void scNode::getConnections(std::map<nodePort, vector<scNode*>> &connections){
    for(int i = 0; i < inputs.size(); i++){
        if(inputs[i]->getNodeRef() != nullptr)
            connections[inputs[i].get()].push_back(this);
    }
}


