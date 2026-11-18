//
//  scNode.h
//  ofxOceanodeSuperCollider
//
//  Created by Eduard Frigola on 27/7/23.
//

#ifndef scNode_h
#define scNode_h

#include "ofxOceanodeNodeModel.h"
#include <map>
#include <set>
#include <cstdint>

class scServer;
class ofxSCServer;
class scNode;

class nodePort{
public:
    nodePort() = default;
    nodePort(int _index, scNode* _nodeRef, std::shared_ptr<void> _owner = {}) : index(_index), nodeRef(_nodeRef), owner(std::move(_owner)){};
    
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
    // Resource ports keep their private audio processor alive across
    // ordinary Router/Macro copies. Ordinary SC ports remain non-owning.
    std::shared_ptr<void> owner;
};

class scNode : public ofxOceanodeNodeModel {
public:
    scNode(std::string name);
    ~scNode();
    
    void addInput(std::string name);
    void addOutput(std::string name);
    
    virtual bool appendOrderedNodes(vector<scNode*> &nodesList, map<scNode*, std::pair<int, vector<int>>> &visitedNodeChilds, vector<scNode*> parents = {});
    
    virtual void getConnections(std::map<nodePort, vector<scNode*>> &connections);

    // Paused nodes stay in the graph (and retain their synths and buses), but
    // consumers read their first audio input instead of their processed output.
    virtual bool isAudioEnabled() const { return true; }
    virtual void applyAudioEnabled(ofxSCServer* server) {}
    virtual void setBypassedOutputBus(ofxSCServer* server, int index, int bus) {}

    // ── Graph suspension (SC Mixer Pro: "Mute Pauses Sources") ──────────────
    // serverManager works out which nodes nobody can hear -- everything they
    // produce ends in a silenced mixer strip -- and pauses them (/n_run 0) to
    // save CPU. See serverManager::updateGraphSuspension().
    //
    // A node that is heard through more than what the graph's cables show
    // (control data read by the patch: a VU, an envelope follower): it and
    // everything upstream of it are never suspended.
    virtual bool keepsGraphAlive() const { return false; }
    // Whether this node, being heard itself, does anything with the audio of
    // `source` (one of its inputs) on `server`. A mixer answers false for a
    // port that only reaches silenced strips.
    virtual bool needsInputAudio(ofxSCServer* server, const nodePort& source) { return true; }
    // Pause / resume every synth of this node on `server`. The node keeps the
    // flag: whatever it creates, replaces or runs later on that server stays
    // paused until it is called with false. Must tolerate missing synths.
    // Default: the node cannot pause and simply keeps running unheard (its
    // own upstream can still be suspended).
    virtual void applyGraphSuspended(ofxSCServer* server, bool suspended) {}

    // A port whose bus is asked for outside a graph build is being read by
    // something the graph does not know (an analyzer, a recorder, a buffer
    // writer...): its node must keep running. Recorded by nodePort::
    // getBusIndex(); serverManager forgets them before each graphComputed,
    // which those readers answer by asking again.
    static bool isExternallyTapped(ofxSCServer* server, scNode* node);
    static void clearExternalTaps(ofxSCServer* server);
    static uint64_t getExternalTapEpoch(){ return externalTapEpoch; }
    // Asking for a bus without reading it (a display, the node's own wiring)
    struct ScopedNoTap {
        ScopedNoTap(){ ++tapSuppress; }
        ~ScopedNoTap(){ --tapSuppress; }
    };

    // "Mute When Stopped" (inspector): the node's outputs fade to silence
    // while the transport it follows is stopped. serverManager puts a gate
    // synth after the node for it, so every SC node has it without any code
    // of its own, and the node keeps running underneath.
    bool getMuteWhenStopped() const { return muteWhenStopped.get(); }
    bool isMutedByTransport() const;
    static nodePort resolveAudioSource(nodePort port);

    // The server whose graph serverManager::recomputeGraph() is building while
    // it calls appendOrderedNodes() and getConnections(); nullptr otherwise.
    // Lets a node that spans several servers (SC Mixer Pro) take part in each
    // server's graph with only the inputs that belong to it.
    static ofxSCServer* getGraphTraversalServer(){ return graphTraversalServer; }
    // The source port serverManager is wiring while it calls setInputBus();
    // an empty port otherwise (then nodes match inputs by source node only)
    static const nodePort& getGraphInputPort(){ return graphInputPort; }
    struct ScopedInputPort {
        explicit ScopedInputPort(const nodePort& port) : previous(graphInputPort) { graphInputPort = port; }
        ~ScopedInputPort(){ graphInputPort = previous; }
        nodePort previous;
    };
    struct ScopedGraphTraversal {
        explicit ScopedGraphTraversal(ofxSCServer* server) : previous(graphTraversalServer) { graphTraversalServer = server; }
        ~ScopedGraphTraversal(){ graphTraversalServer = previous; }
        ofxSCServer* previous;
    };
    
    int getNumOutputs(){return outputs.size();};
    
    virtual void buildSynth(ofxSCServer* server){};
    virtual std::string requiredSynthdefFolder() const { return {}; }
    virtual bool isMacroMixerBridge() const { return false; }
    virtual void graphWillRebuild(ofxSCServer* server) {};
    virtual void graphDidRebuild(ofxSCServer* server,bool success) {};

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
    // How many of an output's channels carry values (a sequencer's voices; its
    // bus is wider). -1: not known, the reader assumes as many as it needs.
    virtual int getOutputNumChannels(int index){return -1;};

    // What a channel of an output carries, in a few words ("Kick" for a
    // sequencer's lane): shown by the node it is wired into. "" = nothing to say.
    virtual std::string getOutputChannelName(int outputIndex, int channel){return "";};

    // The node wired into this node's input called `name` (nullptr: none)
    scNode* getInputNode(const std::string& name) const;

    // A node with its own (dockable) window: the title it gives ImGui::Begin,
    // exactly, and its Show switch. "" / nullptr: no window. Lets another node
    // open and focus it (SC Mixer Pro: "Go to origin synth window").
    virtual std::string getWindowTitle(){return "";};
    virtual ofParameter<bool>* getWindowToggle(){return nullptr;};
    
    virtual int getLastSynthID(ofxSCServer* server){return -1;};
    
    ofEvent<int> createdSynth;
    ofEvent<void> destroyedNode;
    
	void removeInput(int index);
    void removeOutput(int index);
protected:
    // While true, an input change does not re-send the outputs (no graph
    // rebuild per connection while a node rewires many inputs at once)
    bool suppressInputRefresh = false;

    // For applyGraphSuspended(): the servers this node is suspended on. A
    // node that can pause ANDs !isGraphSuspended(server) into every run state
    // it sends for its top-level synths / root group.
    std::set<ofxSCServer*> graphSuspendedServers;
    bool isGraphSuspended(ofxSCServer* server) const { return graphSuspendedServers.count(server) > 0; }
    void setGraphSuspendedFlag(ofxSCServer* server, bool suspended){
        if(suspended) graphSuspendedServers.insert(server);
        else graphSuspendedServers.erase(server);
    }

    static ofxSCServer* graphTraversalServer;
    static nodePort graphInputPort;
    friend class nodePort;
    static std::map<ofxSCServer*, std::set<scNode*>> externalTaps;
    static uint64_t externalTapEpoch;
    static int tapSuppress;
    ofEventListeners listeners;
    ofParameter<bool> muteWhenStopped;
    
    vector<ofParameter<nodePort>> inputs;
    vector<ofParameter<nodePort>> outputs;
    
    vector<std::shared_ptr<nodePort>> availableInputs;
    
    int ins;
    int outs;
    int bufs;
};

#endif /* scNode_h */
