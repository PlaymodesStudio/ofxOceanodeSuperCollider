// MetroBoxPlayer — plays the onset buffer written by the MetroBox node (scMetroBox).
//
// All generation (placement, durations, amplitudes, randomness) happens in
// C++. This synth only follows the transport and reads, for each layer, the
// buffer frame under the playhead.
//
// Positions are in ticks (48 per beat). Clock: the scTransportSync.h anchor
// contract (\anchorId \anchorHard \anchorPos \anchorFire \run \bpm), always on.
//
// Buffer: 2 channels (amplitude, duration in ticks), MAX_LAYERS regions of
// SLOTS * MAX_CYCLE_TICKS frames. Inside a layer's region, frame
// (absoluteTick mod (cycleTicks * SLOTS)) holds that tick's onset: amplitude 0
// means none. C++ sends, with every anchor, \base = the absolute tick at
// floor(anchorPos) modulo (cycleTicks * SLOTS) per layer, computed in 64-bit;
// here we only add the small number of ticks elapsed since the anchor.
//
// Outputs: \gatesout, every layer's gate (8 channels: layer n = channel n),
// and one mono bus per layer (\gateout1 .. \gateout8). A gate is the onset's
// amplitude for its duration (measured in ticks, so it follows tempo
// changes; a synth plays at that level), 0 for 3 ms when a layer is
// retriggered while still high (so the new edge is seen). Layers at or above
// \layers output silence: their ports are gone, and their bus controls may
// still point at a bus the graph now gives to another node.

(
var d, defDir;
var maxLayers = 8, slots = 4, maxCycleTicks = 16 * 48;
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "/CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "/SuperCollider/CompiledSynthdefs";
};
defDir = d +/+ "/MetroBox";
File.mkdir(d);
File.mkdir(defDir);

SynthDef(\MetroBoxPlayer, {
    var buf, mute, cycleTicks, base, numLayers;
    var anchorId, anchorHard, anchorPos, anchorFire, run, bpm;
    var ticksPerSec, anchorTrig, hardTrig, reset, pos, idx, top, fire, elapsed;
    var gates;

    buf     = \buf.kr(-1);
    mute    = \mute.kr(0 ! maxLayers);
    numLayers = \layers.kr(0);

    // Values read on the anchor's first sample are audio-rate controls (a .kr
    // would still hold the previous value on that sample).
    anchorId   = \anchorId.kr(0);
    anchorHard = \anchorHard.kr(0);
    anchorPos  = \anchorPos.ar(0);
    anchorFire = \anchorFire.ar(0);
    run        = \run.ar(0);
    bpm        = \bpm.kr(120);
    cycleTicks = \cycleTicks.ar(192 ! maxLayers);
    base       = \base.ar(0 ! maxLayers);

    ticksPerSec = bpm / 60 * 48;
    anchorTrig  = T2A.ar(Changed.kr(anchorId));
    hardTrig    = T2A.ar(Changed.kr(anchorHard));
    pos         = anchorPos + Sweep.ar(anchorTrig, ticksPerSec * run);
    idx         = pos.floor;
    // The highest tick fired is reset on hard anchors, and on any anchor that
    // moves the position back by more than 2 ticks: otherwise every lane
    // would stay silent until the playhead climbed back past it.
    reset       = (hardTrig + (anchorTrig * (idx < (Delay1.ar(idx) - 2)))) > 0;
    top         = RunningMax.ar(idx, reset);
    fire        = ((idx > Delay1.ar(top)) * (1 - reset)) + (hardTrig * anchorFire);
    elapsed     = idx - anchorPos.floor;        // ticks since the anchor (>= 0)

    gates = Array.newClear(maxLayers);

    maxLayers.do { |i|
        var period = cycleTicks[i].max(1) * slots;
        var local  = (base[i] + elapsed) % period;
        var frame  = (i * slots * maxCycleTicks) + local;
        var data   = BufRd.ar(2, buf, frame, 0, 1);
        var amp    = data[0];
        var on     = i < numLayers;
        var onset  = fire * (amp > 0) * (1 - mute[i]) * on;
        var dur    = Latch.ar(data[1], onset);
        var raw    = (Sweep.ar(onset, ticksPerSec * run) < dur) * (run > 0);
        // retriggered while high: low for 3 ms, so the edge is seen
        var dip    = Latch.ar(Delay1.ar(raw), onset) * (Sweep.ar(onset, 1) < 0.003);

        gates[i] = raw * (1 - dip) * Latch.ar(amp, onset) * on;
    };

    Out.ar(OceanodeOutput.kr(\gatesout), gates);
    maxLayers.do { |i|
        Out.ar(OceanodeOutput.kr(("gateout" ++ (i + 1)).asSymbol), gates[i]);
    };
}).writeDefFile(defDir);

"MetroBoxPlayer compiled".postln;
)

