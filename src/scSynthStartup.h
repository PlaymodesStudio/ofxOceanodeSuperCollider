#pragma once

#include "ofxOscMessage.h"
#include <cstdint>

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
    // scsynth ignores /n_run 1 for a node paused before its first block (its
    // first calc then leaves it paused for good), so the resume goes a few
    // blocks after the creation: in real time, in a scoped instant and in NRT.
    constexpr double resumeDelay = 0.01;
    ofxOscMessage resume;
    resume.setAddress("/n_run");
    resume.addIntArg(synth->nodeID);
    resume.addIntArg(1);
    const uint64_t scoped = Server::getScopedTimetag();
    typename Server::ScopedNRTOffset nrt(resumeDelay);
    server->sendMsgAt(resume, scoped > 1 ? scoped + (uint64_t)(resumeDelay * 4294967296.0)
                                         : Server::timetagNow(resumeDelay));
}
