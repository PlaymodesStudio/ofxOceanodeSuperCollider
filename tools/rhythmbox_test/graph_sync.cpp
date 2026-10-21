#include "scTransportSync.h"
#include <cassert>
#include <map>
#include <string>
#include <vector>
#include <iostream>
using namespace scTransportSync;
constexpr int RATE_AUDIO = 0;
struct ofxSCBus {
    static inline int next = 64, live = 0;
    int index = next++;
    ofxSCBus(int, int, ofxSCServer*) { ++live; }
    void free() {}
    ~ofxSCBus() { --live; }
};
struct ofxSCBuffer {
    static inline int next = 3, live = 0;
    int index = next++;
    ofxSCBuffer(int, int, ofxSCServer*) { ++live; }
    void alloc() {}
    void free() {}
    ~ofxSCBuffer() { --live; }
};
struct ofxSCSynth {
    bool created = true;
    std::map<std::string, double> values;
    bool isCreated() { return created; }
    void set(const std::string& key, double value) { values[key] = value; }
};
struct scSynthControlBatch {
    ofxSCSynth* s;
    scSynthControlBatch(ofxSCSynth* s, ofxSCServer*) : s(s) {}
    void set(const std::string& key, double value) { s->set(key, value); }
    void send() {}
};
struct Value { double value = 0; double get() const { return value; } };
struct scRhythmBox {
    static constexpr int MAX_TRACKS = 8;
    int numTracks = 2, reloads = 0, rebuilds = 0, creations = 0;
    Value beatOffsetP;
    Follower follower;
    std::map<ofxSCServer*, ofxSCBuffer*> silentBuffers;
    std::vector<std::map<ofxSCServer*, ofxSCBuffer*>> trackBufs = std::vector<std::map<ofxSCServer*, ofxSCBuffer*>>(MAX_TRACKS);
    std::map<ofxSCServer*, ofxSCBus*> mixBuses;
    std::map<ofxSCServer*, std::map<int,int>> outputBuses;
    std::map<ofxSCServer*, std::vector<ofxSCBus*>> privateBuses, stepBuses, gateBuses;
    std::map<ofxSCServer*, std::vector<ofxSCSynth*>> trackSynths;
    std::map<ofxSCServer*, std::vector<bool>> syncJoined;
    std::vector<StepClock> stepClocks = std::vector<StepClock>(MAX_TRACKS);
    std::vector<int> syncSpb = std::vector<int>(MAX_TRACKS, 4);
    std::vector<bool> needHard = std::vector<bool>(MAX_TRACKS, true);
    std::vector<double> hardCounters = std::vector<double>(MAX_TRACKS, 0);
    double syncBpm = 120, anchorIdCounter = 0;
    uint64_t lastAnchorUs = 0, lastAnchorTimetag = 0;
    void reloadSamplesForServer(ofxSCServer*) { ++reloads; }
    void freeServerSynths(ofxSCServer* srv) {
        ++rebuilds;
        for(auto* s : trackSynths[srv]) delete s;
        trackSynths[srv].clear();
    }
    void createTrackSynth(ofxSCServer* srv, int ti) {
        ++creations;
        trackSynths[srv][ti] = new ofxSCSynth;
    }
    int getBufnum(int, ofxSCServer*) const;
    void buildSynth(ofxSCServer*);
    void createSynth(ofxSCServer*);
    void free(ofxSCServer*);
    void setOutputBus(ofxSCServer*,int,int);
    int getOutputBusIndex(ofxSCServer*,int);
    std::vector<ofxSCServer*> activeGraphServers() const;
    void sendAnchor(const Anchor&);
};
#include "production_methods.h"
int main() {
    ofxSCServer serverA, serverB;
    scRhythmBox box;
    // UI/preset rebuild of an unconnected node must not create any voices.
    box.createSynth(&serverA);
    assert(box.creations == 0 && box.reloads == 0 && box.activeGraphServers().empty());
    box.buildSynth(&serverA);
    const auto mixBus = box.getOutputBusIndex(&serverA, scRhythmBox::MAX_TRACKS);
    assert(mixBus >= 64);
    assert(box.getBufnum(0, &serverA) > 0);
    assert(box.getBufnum(1, &serverA) == box.getBufnum(0, &serverA));
    assert(box.getBufnum(0, &serverB) == -1);
    box.buildSynth(&serverA);
    assert(box.getOutputBusIndex(&serverA, scRhythmBox::MAX_TRACKS) == mixBus && ofxSCBus::live == 1);
    box.createSynth(&serverA);
    assert(box.creations == 2);
    box.createSynth(&serverB);
    assert(box.creations == 2 && box.activeGraphServers().size() == 1);
    box.setOutputBus(&serverA,0,128);
    assert(box.getOutputBusIndex(&serverA,0) == 128);
    box.setOutputBus(&serverA,-1,0);
    box.setOutputBus(&serverA,scRhythmBox::MAX_TRACKS,0);
    assert(box.outputBuses[&serverA].size() == 1);
    box.createSynth(&serverA); // full rebuild replaces, rather than adds, voices
    assert(box.trackSynths[&serverA].size() == 2 && box.creations == 4);
    // Second graph server joins later: it must not retrigger the first server.
    box.buildSynth(&serverB); box.createSynth(&serverB);
    auto* late = box.trackSynths[&serverB][0]; late->created = false;
    Anchor a; a.playing = true; a.bpm = 120; a.discontinuity = true;
    box.sendAnchor(a);
    auto* ready = box.trackSynths[&serverA][0];
    assert(ready->values["anchorFire"] == 1 && ready->values["run"] == 1);
    const auto hard = ready->values["anchorHard"];
    a.discontinuity = false; a.beat = .25; box.sendAnchor(a);
    assert(ready->values["anchorHard"] == hard && ready->values["anchorFire"] == 0);
    a.beat = .5; box.sendAnchor(a);
    assert(ready->values["anchorHard"] == hard);
    late->created = true; a.beat = .75; box.sendAnchor(a);
    assert(late->values["anchorFire"] == 1 && late->values["anchorHard"] == hard);
    assert(ready->values["anchorFire"] == 0 && ready->values["anchorHard"] == hard);
    a.playing = false; a.discontinuity = true; a.beat = 0; box.sendAnchor(a);
    assert(ready->values["run"] == 0 && ready->values["anchorFire"] == 0);
    a.playing = true; a.bpm = 90; box.sendAnchor(a);
    assert(ready->values["anchorFire"] == 1 && ready->values["bpm"] == 90);
    // Geometry changes at a step boundary must fire that step once.
    a.discontinuity = false; box.needHard[0] = true; a.beat = 1; box.sendAnchor(a);
    assert(ready->values["anchorFire"] == 1);
    box.sendAnchor(a); assert(ready->values["anchorFire"] == 0);
    box.free(&serverA);
    assert(!box.trackSynths.count(&serverA) && !box.outputBuses.count(&serverA) && !box.mixBuses.count(&serverA));
    const auto creations = box.creations;
    box.createSynth(&serverA); assert(box.creations == creations);
    assert(box.activeGraphServers().size() == 1);
    box.free(&serverB); assert(ofxSCBus::live == 0 && ofxSCBuffer::live == 0 && box.activeGraphServers().empty());
    std::cout << "PASS: graph membership, stable mix bus, full rebuilds, disconnect cleanup, delayed server joins, hard/soft anchors, stop/start and tempo\n";
}
