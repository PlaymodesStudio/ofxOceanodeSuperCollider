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
// Outputs, mono, one bus per layer and kind (\trigout1 .. \trigout8, \gateout1 ..,
// \ampout1 ..): trig = 1-sample impulse, value 1 or the amplitude with \trigAmp;
// gate = high for the onset's duration, measured in ticks so it follows tempo
// changes; amp = amplitude held until the next onset. Layers at or above
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
    var buf, trigAmp, mute, cycleTicks, base, numLayers;
    var anchorId, anchorHard, anchorPos, anchorFire, run, bpm;
    var ticksPerSec, anchorTrig, hardTrig, reset, pos, idx, top, fire, elapsed;
    var trigs, gates, amps;

    buf     = \buf.kr(-1);
    trigAmp = \trigAmp.kr(0);
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

    trigs = Array.newClear(maxLayers);
    gates = Array.newClear(maxLayers);
    amps  = Array.newClear(maxLayers);

    maxLayers.do { |i|
        var period = cycleTicks[i].max(1) * slots;
        var local  = (base[i] + elapsed) % period;
        var frame  = (i * slots * maxCycleTicks) + local;
        var data   = BufRd.ar(2, buf, frame, 0, 1);
        var amp    = data[0];
        var on     = i < numLayers;
        var onset  = fire * (amp > 0) * (1 - mute[i]) * on;
        var dur    = Latch.ar(data[1], onset);

        trigs[i] = onset * (1 + (trigAmp * (amp - 1)));
        amps[i]  = Latch.ar(amp, onset) * on;
        gates[i] = (Sweep.ar(onset, ticksPerSec * run) < dur) * (run > 0) * on;
    };

    maxLayers.do { |i|
        Out.ar(OceanodeOutput.kr(("trigout" ++ (i + 1)).asSymbol), trigs[i]);
        Out.ar(OceanodeOutput.kr(("gateout" ++ (i + 1)).asSymbol), gates[i]);
        Out.ar(OceanodeOutput.kr(("ampout" ++ (i + 1)).asSymbol), amps[i]);
    };
}).writeDefFile(defDir);

"MetroBoxPlayer compiled".postln;
)

