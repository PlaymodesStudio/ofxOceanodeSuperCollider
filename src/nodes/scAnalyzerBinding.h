//
//  scAnalyzerBinding.h
//  ofxOceanodeSuperCollider
//
//  Analysis nodes (SC Info, A2k, PitchTracker, BeatTracker, FFT, FFT HD,
//  Spectrogram, Wavescope, Lissajous) run their synth outside the patch graph,
//  at the tail of the default group, and listen to graphComputed because a
//  rebuild can move their input to a different bus.
//
//  They used to answer every rebuild -- any cable change anywhere on the server
//  -- by freeing the synth and its buses and creating them again. The synth
//  survives a rebuild (only nodes in the graph are freed or moved), so all it
//  needs is its "in" control pointed at the new bus. This records what the
//  synth was built against, so the node can tell whether that is enough.
//
//  It is not enough after a teardown (preset load, NRT capture start / end)
//  or a server reboot: those clear the whole default group and reset the bus
//  allocators. serverManager counts those in getGraphGeneration(); a synth
//  built in an earlier generation no longer exists and must be recreated.
//

#ifndef scAnalyzerBinding_h
#define scAnalyzerBinding_h

#include "serverManager.h"
#include <cstdint>

class ofxSCServer;

struct scAnalyzerBinding {
    // Call once the synth has been created on manager's server with "in" = inBus.
    void bind(serverManager* manager, int inBus){
        server = manager != nullptr ? manager->getServer() : nullptr;
        generation = manager != nullptr ? manager->getGraphGeneration() : 0;
        bus = inBus;
        valid = manager != nullptr;
    }

    void clear(){
        valid = false;
        server = nullptr;
        bus = -1;
    }

    // True when the synth built earlier is still running on manager's server.
    bool canRebind(serverManager* manager) const {
        return valid && manager != nullptr
            && manager->getServer() == server
            && manager->getGraphGeneration() == generation;
    }

    int inBus() const { return bus; }
    void setInBus(int inBus){ bus = inBus; }

private:
    ofxSCServer* server = nullptr;
    uint64_t generation = 0;
    int bus = -1;
    bool valid = false;
};

#endif /* scAnalyzerBinding_h */
