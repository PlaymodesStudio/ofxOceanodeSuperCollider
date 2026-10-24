//
//  scTestNoise.cpp
//  ofxOceanodeSuperCollider
//

#include "scTestNoise.h"

scTestNoise::~scTestNoise() {
    for(auto& [srv, s] : synths) { if(s) { s->free(); delete s; } }
    synths.clear();
}

void scTestNoise::setup() {
    addOutput("Out");
}

// ── scNode interface ──────────────────────────────────────────────────────────

void scTestNoise::buildSynth(ofxSCServer* server) {
    if(!server) return;
    // Create wrapper only — no createAndRun yet.
    // setOutputBus() is called next; it stores "out" while created=false
    // so the bus is baked into /s_new when createSynth() fires.
    if(synths.count(server) && synths[server]) {
        synths[server]->free();
        delete synths[server];
    }
    synths[server] = new ofxSCSynth("TestNoise", server);
}

void scTestNoise::createSynth(ofxSCServer* server) {
    if(!server || !synths.count(server) || !synths[server]) return;
    synths[server]->createAndRun(0, 1, true);
}

void scTestNoise::free(ofxSCServer* server) {
    if(synths.count(server) && synths[server]) {
        synths[server]->free();
        delete synths[server];
        synths.erase(server);
    }
    outputBuses.erase(server);
}

void scTestNoise::setOutputBus(ofxSCServer* server, int idx, int bus) {
    outputBuses[server][idx] = bus;
    if(synths.count(server) && synths[server])
        synths[server]->set("out", bus);
}

int scTestNoise::getOutputBusIndex(ofxSCServer* server, int idx) {
    if(outputBuses.count(server) && outputBuses[server].count(idx))
        return outputBuses[server].at(idx);
    return -1;
}

void scTestNoise::moveSynthBefore(ofxSCServer* server, int nodeID) {
    if(synths.count(server) && synths[server])
        synths[server]->moveBefore(nodeID);
}

int scTestNoise::getLastSynthID(ofxSCServer* server) {
    if(synths.count(server) && synths[server])
        return synths[server]->nodeID;
    return -1;
}
