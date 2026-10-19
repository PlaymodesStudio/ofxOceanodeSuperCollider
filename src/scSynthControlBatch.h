#pragma once

#include "ofxSCSynth.h"
#include <string>

// One parameter update often changes a whole effect's controls. For a live
// synth, /n_set accepts all those pairs in one message. Before /n_go, keep
// using set() so arguments are retained/replayed by ofxSCSynth as usual.
// sendMsg retains the server's latency, scoped timetag and NRT handling.
class scSynthControlBatch {
public:
    scSynthControlBatch(ofxSCSynth* synth, ofxSCServer* server) : synth(synth), server(server) {
        message.setAddress("/n_set");
        if(synth) message.addIntArg(synth->nodeID);
    }

    void set(const std::string& name, float value) {
        if(!synth) return;
        if(!server || !synth->isCreated()) { synth->set(name, value); return; }
        message.addStringArg(name);
        message.addFloatArg(value);
        pending = true;
    }

    void set(const std::string& name, int value) {
        if(!synth) return;
        if(!server || !synth->isCreated()) { synth->set(name, value); return; }
        message.addStringArg(name);
        message.addIntArg(value);
        pending = true;
    }

    void send() {
        if(!pending) return;
        server->sendMsg(message);
        pending = false;
    }

private:
    ofxSCSynth* synth;
    ofxSCServer* server;
    ofxOscMessage message;
    bool pending = false;
};
