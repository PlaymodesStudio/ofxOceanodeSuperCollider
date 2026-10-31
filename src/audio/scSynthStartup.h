#pragma once

// Configure first, then create: every set() / mapan() made before /s_new rides
// on it as a creation argument (scalars, arrays and bus mappings alike), so
// the synth runs from its very first block with its real buses and controls.
//
// It used to be created paused, configured through /n_set, and resumed by a
// bundle stamped a fixed 10 ms ahead. scsynth's clock runs ahead of the wall
// clock by its hardware buffer plus the device's output latency, so on most
// devices that resume arrived "late": executed on arrival, logged on every
// voice, envelope and LFO a Box started. Nothing here is timed any more.
//
// configure() may route neighbours relative to this synth: it has its id
// before configure() runs. A resendStoredArgs() inside configure() (a routing
// helper flushing a synth that awaits /n_go) leaves this one alone, since its
// stored controls are the coming /s_new's arguments (ofxSCSynth).
//
// The /s_new must go out as a BUNDLE (createAndRun), never as a lone message
// (create): during a graph rebuild the server runs with its 200 ms latency,
// which stamps a lone message 200 ms ahead while a bundle's contents run on
// arrival. A Box's routing helpers flush other parts' stored controls with
// bundles right after; against a /s_new delayed 200 ms those landed on a node
// that did not exist yet and were lost (PolyComb after a preset load or an
// encapsulation: oscillator levels, insert routing, the resonator).
template<typename Synth, typename Server, typename Configure>
void scStartConfiguredSynth(Synth* synth, Server* server, int position, int group, Configure configure){
    (void)server;
    synth->ensureNodeID();
    configure();
    synth->createAndRun(position, group, true);
}
