#pragma once

#include "ofxSCServer.h"
#include <vector>

// /c_setn accepts multiple (bus, count, values) ranges. Keep live control
// updates together without changing the server's timetag/latency/NRT path.
class scControlBusBatch {
public:
    explicit scControlBusBatch(ofxSCServer* server) : server(server) { reset(); }
    void set(int bus, const std::vector<float>& values) {
        if(!server || bus < 0 || values.empty()) return;
        const size_t bytes = 12 + 5 * values.size();
        if(pending && estimatedBytes + bytes > 4096) send();
        message.addIntArg(bus);
        message.addIntArg(static_cast<int>(values.size()));
        for(float value : values) message.addFloatArg(value);
        estimatedBytes += bytes;
        pending = true;
    }
    void send() {
        if(!pending) return;
        server->sendMsg(message);
        reset();
    }
private:
    void reset() { message.clear();message.setAddress("/c_setn");estimatedBytes=24;pending=false; }
    ofxSCServer* server;
    ofxOscMessage message;
    size_t estimatedBytes=24;
    bool pending=false;
};
