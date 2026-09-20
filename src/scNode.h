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
    
    bool operator==(const nodePort& other) const {
        return (index == other.index && nodeRef == other.nodeRef);
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
    
    virtual bool appendOrderedNodes(vector<scNode*> &nodesList, map<scNode*, std::pair<int, vector<int>>> &visitedNodeChilds, vector<scNode*> parents = {});
    
    void getConnections(std::map<nodePort, vector<scNode*>> &connections);
    
    int getNumOutputs(){return outputs.size();};
    
    virtual void buildSynth(ofxSCServer* server){};

    // A node whose inputs are each worth rendering on their own: a mixer
    // point. Every connected input of one becomes a stem in an NRT render.
    virtual bool isNRTStemPoint() const { return false; };

    // Push every parameter's current value, whether or not it changed. A
    // score needs the patch's complete state at time zero: a parameter nobody
    // has touched is never sent, so the render would run on the SynthDef's own
    // default until the first change -- a trigger left at its default fires a
    // note at the top of the render that was never played.
    virtual void resendParametersForNRT(){};

    // Called once while the realtime server is still reachable, immediately
    // before an NRT capture switches the server to capture-only. A node whose
    // state lives inside the server (a VST plugin's program, say) uses this to
    // pull that state back, because once capture starts nothing is transmitted
    // and no reply can ever arrive.
    virtual void prepareForNRTCapture(){};
    // True while that pull is still in flight.
    virtual bool isNRTCapturePreparationPending() const { return false; };
    virtual void createSynth(ofxSCServer* server){};
    virtual void moveSynthBefore(ofxSCServer* server, int nodeID){};
    virtual void free(ofxSCServer* server){};
    virtual void runSynth(ofxSCServer* server){};
    virtual void setOutputBus(ofxSCServer* server, int index, int bus){};
    virtual void setInputBus(ofxSCServer* server, scNode* node, int bus){};
    virtual void resetInputBusses(ofxSCServer* server, int targetBus = 0){};
    
    virtual int getOutputBusIndex(ofxSCServer* server, int index){return -1;};
    
    virtual int getLastSynthID(ofxSCServer* server){return -1;};
    
    ofEvent<int> createdSynth;
    ofEvent<void> destroyedNode;
    
	void removeInput(int index);
    void removeOutput(int index);
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
