#pragma once

#include "ofxOscMessage.h"

// Create paused, configure through the regular /n_set(n) path, then run.
// ofxSCSynth caches set() calls until /n_go, so explicitly flush them before
// resuming: DSP must never start on default buses or default array controls.
template<typename Synth, typename Server, typename Configure>
void scStartConfiguredSynth(Synth* synth, Server* server, int position, int group, Configure configure){
    synth->createAndRun(position, group, false);
    configure();
    synth->resendStoredArgs();
    // run() itself waits for /n_go on an unacknowledged node. Send explicitly
    // after configuration so capture-only NRT (which has no replies) runs too.
    ofxOscMessage resume;
    resume.setAddress("/n_run");
    resume.addIntArg(synth->nodeID);
    resume.addIntArg(1);
    server->sendMsg(resume);
}
