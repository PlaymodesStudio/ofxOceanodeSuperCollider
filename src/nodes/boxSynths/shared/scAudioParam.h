//
//  scAudioParam.h
//  ofxOceanodeSuperCollider
//
//  An Oceanode parameter (Gate, Pitch, Velocity...) that also takes an audio
//  cable: an SC node's output, read sample accurately by the synth (which maps
//  the bus and switches to it). Oceanode values and the cable are both fine;
//  the node decides what the synth reads.
//
//  The graph tells a node only which node an input comes from, not which of
//  its outputs: several of these on one node (Gate and Pitch from one
//  sequencer) are told apart by the bus of their source port.
//
//    gateIn.attach(addParameter(gate), availableInputs, [this]{ refreshOutputs(); });
//    setInputBus(server, node, bus):   for each: p.take(server, node, bus)
//    resetInputBusses(server):         for each: p.reset(server)
//    in the synth: p.bus(server) >= 0 ? mapan("x_ar", bus, n) + "x_sel" 1 : "x_sel" 0
//

#pragma once

#include "../../../scNode.h"
#include <algorithm>
#include <functional>
#include <map>
#include <memory>

struct scAudioParam {
    std::shared_ptr<nodePort> port = std::make_shared<nodePort>();
    std::shared_ptr<ofxOceanodeAbstractParameter> handle;
    std::map<ofxSCServer*, int> buses;

    // refresh: the node re-sends its outputs (the graph is rebuilt with the new cable)
    void attach(std::shared_ptr<ofxOceanodeAbstractParameter> h, std::vector<std::shared_ptr<nodePort>>& available,
                std::function<void()> refresh) {
        handle = h;
        auto p = port;
        h->addReceiveFunc<nodePort>([p, refresh](const nodePort& np){ *p = np; if(refresh) refresh(); });
        h->addDisconnectFunc([p, refresh](){
            if(p->getNodeRef() == nullptr) return;
            *p = nodePort();
            if(refresh) refresh();
        });
        available.push_back(port);
    }
    // the parameter left the node (unpublished): no cable any more
    void detach(std::vector<std::shared_ptr<nodePort>>& available) {
        available.erase(std::remove(available.begin(), available.end(), port), available.end());
        *port = nodePort();
        handle.reset();
        for(auto& [server, b] : buses) b = -1;
    }
    // the graph wires `bus` from `node`: is it this one's cable?
    bool take(ofxSCServer* server, scNode* node, int bus) {
        if(!node || port->getNodeRef() != node || port->getBusIndex(server) != bus) return false;
        buses[server] = bus;
        return true;
    }
    void reset(ofxSCServer* server) { buses[server] = -1; }
    int bus(ofxSCServer* server) const {
        auto it = buses.find(server);
        return it == buses.end() ? -1 : it->second;
    }
    bool cabled() const { return port->getNodeRef() != nullptr; }
    // the cable's channels that carry values (-1: no cable, or the source does not say)
    int channels() const { return cabled() ? port->getNodeRef()->getOutputNumChannels(port->getIndex()) : -1; }
    // a (control) connection, or a cable
    bool connected() const { return cabled() || (handle && handle->hasInConnection()); }
};
