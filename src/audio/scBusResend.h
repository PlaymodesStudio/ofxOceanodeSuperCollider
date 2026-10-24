//
//  scBusResend.h
//  ofxOceanodeSuperCollider
//
//  On a graph rebuild, serverManager calls moveSynthBefore() on every node
//  whose synth already exists. Those synths keep every parameter they were
//  sent; only their bus wiring can have changed. Many nodes used to answer
//  with resendParams.notify(), which re-sent every parameter, to the synths
//  of every server, for each cable change anywhere in the patch. Nodes whose
//  parameters always reach the synth as they change now resend just this.
//

#ifndef scBusResend_h
#define scBusResend_h

#include <map>
#include "ofxSCSynth.h"

class scNode;
class ofxSCServer;

// "in" = the (first) connected input's bus and "out" = output 0's bus, as the
// nodes' own full resend does.
inline void scResendInOutBuses(ofxSCSynth* synth, ofxSCServer* server,
                               std::map<ofxSCServer*, std::map<scNode*, int>>& inputBuses,
                               std::map<ofxSCServer*, std::map<int, int>>& outputBuses){
    if(synth == nullptr) return;
    auto in = inputBuses.find(server);
    if(in != inputBuses.end() && !in->second.empty())
        synth->set("in", in->second.begin()->second);
    auto out = outputBuses.find(server);
    if(out != outputBuses.end()){
        auto bus = out->second.find(0);
        if(bus != out->second.end()) synth->set("out", bus->second);
    }
}

#endif /* scBusResend_h */
