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
class scNode;

class nodePort{
public:
    nodePort() = default;
    nodePort(int _index, scNode* _nodeRef) : index(_index), nodeRef(_nodeRef){};
    
    int getIndex() const {return index;};
    scNode* getNodeRef() const {return nodeRef;};
    
    int getBusIndex(ofxSCServer* server) const;
    
    // Define the less-than operator for nodePort
    bool operator<(const nodePort& other) const {
        if (index != other.index) {
            return index < other.index;
        }
        return nodeRef < other.nodeRef;
    }
private:
    int index = -1;
    scNode* nodeRef = nullptr;
};

class scNode : public ofxOceanodeNodeModel {
public:
    scNode(std::string name);
    ~scNode();
    
    void addInput(std::string name);
    void addOutput(std::string name);
    
    bool appendOrderedNodes(vector<scNode*> &nodesList, map<scNode*, std::pair<int, vector<int>>> &visitedNodeChilds, vector<scNode*> parents = {});
    
    void getConnections(std::map<nodePort, vector<scNode*>> &connections);
    
    int getNumOutputs(){return outputs.size();};
    
    virtual void createSynth(ofxSCServer* server){};
    virtual void free(ofxSCServer* server){};
    virtual void runSynth(ofxSCServer* server){};
    virtual void setOutputBus(ofxSCServer* server, int index, int bus){};
    virtual void setInputBus(ofxSCServer* server, scNode* node, int bus){};
    
    virtual int getOutputBusIndex(ofxSCServer* server, int index){return -1;};
    
    ofEvent<int> createdSynth;
    ofEvent<void> reassignAudioControls;
    
protected:
    ofEventListeners listeners;
    
    vector<ofParameter<nodePort>> inputs;
    vector<ofParameter<nodePort>> outputs;
    
    vector<std::shared_ptr<nodePort>> availableInputs;
    
    int ins;
    int outs;
    int bufs;
};

#endif /* scNode_h */
