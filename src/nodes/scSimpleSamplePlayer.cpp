//
//  scSimpleSamplePlayer.cpp
//  ofxOceanodeSuperCollider
//

#include "scSimpleSamplePlayer.h"

scSimpleSamplePlayer::~scSimpleSamplePlayer() {
    listeners.unsubscribeAll();
    for(auto& [srv, s] : synths) { if(s) { s->free(); delete s; } }
    synths.clear();
}

void scSimpleSamplePlayer::setup() {
    addParameter(bufnum.set("Bufnum", 0, 0, 4095));
    addParameter(gain.set("Gain", 0.5f, 0.0f, 1.0f));
    addOutput("Out");

    listeners.push(bufnum.newListener([this](int& v) {
        for(auto& [srv, s] : synths)
            if(s) s->set("bufnum", v);
    }));
    listeners.push(gain.newListener([this](float& v) {
        for(auto& [srv, s] : synths)
            if(s) s->set("gain", v);
    }));
}

// ── scNode interface ──────────────────────────────────────────────────────────

void scSimpleSamplePlayer::buildSynth(ofxSCServer* server) {
    if(!server) return;
    if(synths.count(server) && synths[server]) {
        synths[server]->free();
        delete synths[server];
    }
    synths[server] = new ofxSCSynth("SimpleSamplePlayer", server);
    synths[server]->set("bufnum", bufnum.get());
    synths[server]->set("gain",   gain.get());
}

void scSimpleSamplePlayer::createSynth(ofxSCServer* server) {
    if(!server || !synths.count(server) || !synths[server]) return;
    synths[server]->createAndRun(0, 1, true);
}

void scSimpleSamplePlayer::free(ofxSCServer* server) {
    if(synths.count(server) && synths[server]) {
        synths[server]->free();
        delete synths[server];
        synths.erase(server);
    }
    outputBuses.erase(server);
}

void scSimpleSamplePlayer::setOutputBus(ofxSCServer* server, int idx, int bus) {
    outputBuses[server][idx] = bus;
    if(synths.count(server) && synths[server])
        synths[server]->set("out", bus);
}

int scSimpleSamplePlayer::getOutputBusIndex(ofxSCServer* server, int idx) {
    if(outputBuses.count(server) && outputBuses[server].count(idx))
        return outputBuses[server].at(idx);
    return -1;
}

void scSimpleSamplePlayer::moveSynthBefore(ofxSCServer* server, int nodeID) {
    if(synths.count(server) && synths[server])
        synths[server]->moveBefore(nodeID);
}

int scSimpleSamplePlayer::getLastSynthID(ofxSCServer* server) {
    if(synths.count(server) && synths[server])
        return synths[server]->nodeID;
    return -1;
}
