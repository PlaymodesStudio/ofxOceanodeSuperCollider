//
//  scNode.h
//  ofxOceanodeSuperCollider
//
//  Created by Eduard Frigola on 27/7/23.
//

#ifndef scNode_h
#define scNode_h

#include "ofxOceanodeNodeModel.h"

class scServer;
class ofxSCServer;

class scNode : public ofxOceanodeNodeModel {
public:
    scNode(std::string name);
    ~scNode();
    
    void addInputs(int numInputs);
    void addOutput();
    
    bool appendOrderedNodes(vector<scNode*> &nodesList, map<scNode*, std::pair<int, vector<int>>> &visitedNodeChilds, vector<scNode*> parents = {});
    
    void getConnections(std::map<scNode*, vector<scNode*>> &connections);
    
    virtual void createSynth(ofxSCServer* server){};
    virtual void free(ofxSCServer* server){};
    virtual void setOutputBus(ofxSCServer* server, int bus){};
    virtual void setInputBus(ofxSCServer* server, scNode* node, int bus){};
    
    ofEvent<int> createdSynth;
    
protected:
    ofEventListeners listeners;
    
    vector<ofParameter<scNode*>> inputs;
    ofParameter<scNode*> output;
    
    int ins;
    int outs;
    int bufs;
};

#endif /* scNode_h */
