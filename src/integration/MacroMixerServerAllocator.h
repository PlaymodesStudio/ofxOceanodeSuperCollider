#pragma once
#include <vector>

class ofxSCServer;

// Chooses the SuperCollider server a MacroMixer transition starts its incoming
// audio on, so the server playing the origin never builds the destination's
// graph. A pure function of what it is given; the ScBus bridge gathers it.
// Rules, in order:
//   1. The origin keeps its server (it is never a candidate).
//   2. A server that plays no graph (only the mixer's silent bridge), the one
//      with its SynthDefs loaded first, then the lowest average CPU.
//   3. Otherwise the lowest average CPU among the busy ones.
//   4. Single (Audio Servers: Single, a source that shares a server-bound
//      resource) or no other server playing the mixer: nullptr, which keeps
//      both sides on the origin's server, as with one server.
namespace MacroMixerServerAllocator {
struct Candidate {
    ofxSCServer* server = nullptr;
    bool reachable = false; // An SC Output on it plays the mixer's audio bridge.
    bool busy = false;      // It runs nodes besides Outputs and mixer bridges.
    bool loading = false;   // It is still loading required SynthDefs.
    float cpu = 0;          // Average CPU, in percent.
};
struct Request {
    std::vector<Candidate> candidates;
    ofxSCServer* origin = nullptr;
    bool single = false;
};
// The server for the incoming side, or nullptr: the origin's.
ofxSCServer* choose(const Request& request);
}
