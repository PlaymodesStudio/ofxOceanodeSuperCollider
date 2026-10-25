#pragma once

#include "ofxSCSynth.h"
#include <map>
#include <string>
#include <vector>

// One parameter update often changes a whole effect's controls. For a live
// synth, /n_setn accepts scalar and vector controls in one message. Before
// /n_go, use set() so ofxSCSynth retains/replays creation arguments as usual.
// sendMsg retains the server's latency, scoped timetag and NRT handling.
class scSynthControlBatch {
public:
    using Cache = std::map<std::string, std::vector<float>>;
    // An opt-in cache belongs to one synth's lifetime. Clear it on recreation
    // or an explicit state resend; omitted caches preserve the ordinary path.
    scSynthControlBatch(ofxSCSynth* synth, ofxSCServer* server, Cache* cache = nullptr)
    : synth(synth), server(server), cache(cache) {
        resetMessage();
    }

    void set(const std::string& name, float value) {
        if(!synth) return;
        if(cache && !changed(name, {value})) return;
        if(!server || !synth->isCreated()) { synth->set(name, value); return; }
        reserveControl(name, 1);
        message.addStringArg(name);
        message.addIntArg(1);
        message.addFloatArg(value);
        pending = true;
    }

    void set(const std::string& name, double value) { set(name, static_cast<float>(value)); }

    void set(const std::string& name, int value) {
        if(!synth) return;
        if(cache && !changed(name, {static_cast<float>(value)})) return;
        if(!server || !synth->isCreated()) { synth->set(name, value); return; }
        reserveControl(name, 1);
        message.addStringArg(name);
        message.addIntArg(1);
        message.addIntArg(value);
        pending = true;
    }

    void set(const std::string& name, const std::vector<float>& values) {
        if(!synth || values.empty()) return;
        if(cache && !changed(name, values)) return;
        if(!server || !synth->isCreated()) { synth->set(name, values); return; }
        reserveControl(name, values.size());
        message.addStringArg(name);
        message.addIntArg(static_cast<int>(values.size()));
        for(float value : values) message.addFloatArg(value);
        pending = true;
    }

    void send() {
        if(!pending) return;
        server->sendMsg(message);
        pending = false;
        resetMessage();
    }

private:
    void resetMessage() {
        message.clear();
        message.setAddress("/n_setn");
        if(synth) message.addIntArg(synth->nodeID);
        estimatedBytes = 32;
    }
    void reserveControl(const std::string& name, size_t count) {
        // Keep batches bounded for high-partial-count instruments. Preserve a
        // single vector intact; splitting a control across messages changes its
        // semantics. Existing individual vectors may exceed this soft budget.
        const size_t bytes = ((name.size() + 4) & ~size_t(3)) + 12 + count * 5;
        if(pending && estimatedBytes + bytes > 4096) send();
        estimatedBytes += bytes;
    }
    bool changed(const std::string& name, const std::vector<float>& values) {
        if(!cache) return true;
        const auto previous = cache->find(name);
        if(previous != cache->end() && previous->second == values) return false;
        (*cache)[name] = values;
        return true;
    }
    ofxSCSynth* synth;
    ofxSCServer* server;
    Cache* cache;
    ofxOscMessage message;
    bool pending = false;
    size_t estimatedBytes = 32;
};
