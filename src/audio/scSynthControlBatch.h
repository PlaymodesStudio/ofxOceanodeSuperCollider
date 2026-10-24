#pragma once

#include "ofxSCSynth.h"
#include <string>
#include <vector>

// One parameter update often changes a whole effect's controls. For a live
// synth, /n_setn accepts scalar and vector controls in one message. Before
// /n_go, use set() so ofxSCSynth retains/replays creation arguments as usual.
// sendMsg retains the server's latency, scoped timetag and NRT handling.
class scSynthControlBatch {
public:
    scSynthControlBatch(ofxSCSynth* synth, ofxSCServer* server) : synth(synth), server(server) {
        message.setAddress("/n_setn");
        if(synth) message.addIntArg(synth->nodeID);
    }

    void set(const std::string& name, float value) {
        if(!synth) return;
        if(!server || !synth->isCreated()) { synth->set(name, value); return; }
        message.addStringArg(name);
        message.addIntArg(1);
        message.addFloatArg(value);
        pending = true;
    }

    void set(const std::string& name, double value) { set(name, static_cast<float>(value)); }

    void set(const std::string& name, int value) {
        if(!synth) return;
        if(!server || !synth->isCreated()) { synth->set(name, value); return; }
        message.addStringArg(name);
        message.addIntArg(1);
        message.addIntArg(value);
        pending = true;
    }

    void set(const std::string& name, const std::vector<float>& values) {
        if(!synth || values.empty()) return;
        if(!server || !synth->isCreated()) { synth->set(name, values); return; }
        message.addStringArg(name);
        message.addIntArg(static_cast<int>(values.size()));
        for(float value : values) message.addFloatArg(value);
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
