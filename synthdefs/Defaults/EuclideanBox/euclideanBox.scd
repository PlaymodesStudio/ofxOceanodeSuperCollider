// EuclideanBoxPlayer<V> — plays the onset buffer written by the EuclideanBox
// node (scEuclideanBox.h), V voices per track (V = 1..16).
//
// As MetroBoxPlayer (the same clock: scTransportSync anchors, positions in
// ticks, 48 per beat; \base per track from C++), with 4 buffer channels per
// tick: amplitude (0: no onset), duration (ticks), pitch (MIDI), voice (0..V-1,
// -1: every voice). Up to 8 tracks, each a region of SLOTS chunks.
//
// Outputs per track, V channels each (\trigout1.., \gateout1.., \pitchout1..):
// trig = a one-sample pulse on the voice the onset goes to (1, or its
// amplitude with \trigAmp); gate = the onset's level for its duration (in
// ticks: follows tempo changes; the synths play at that level), 0 for 3 ms when
// a voice is retriggered while still high (so its envelopes start again);
// pitch held until the voice's next onset. Tracks at or above \layers output silence.

(
var d, defDir;
var maxLayers = 8, slots = 4, maxCycleTicks = 16 * 48;
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "/CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "/SuperCollider/CompiledSynthdefs";
};
defDir = d +/+ "/EuclideanBox";
File.mkdir(d);
File.mkdir(defDir);

(1..16).do { |nv|
    SynthDef(("EuclideanBoxPlayer" ++ nv).asSymbol, {
        var buf, trigAmp, mute, cycleTicks, base, numLayers;
        var anchorId, anchorHard, anchorPos, anchorFire, run, bpm;
        var ticksPerSec, anchorTrig, hardTrig, reset, pos, idx, top, fire, elapsed;

        buf     = \buf.kr(-1);
        trigAmp = \trigAmp.kr(0);
        mute    = \mute.kr(0 ! maxLayers);
        numLayers = \layers.kr(0);
        anchorId   = \anchorId.kr(0);
        anchorHard = \anchorHard.kr(0);
        anchorPos  = \anchorPos.ar(0);
        anchorFire = \anchorFire.ar(0);
        run        = \run.ar(0);
        bpm        = \bpm.kr(120);
        cycleTicks = \cycleTicks.ar(384 ! maxLayers);
        base       = \base.ar(0 ! maxLayers);

        ticksPerSec = bpm / 60 * 48;
        anchorTrig  = T2A.ar(Changed.kr(anchorId));
        hardTrig    = T2A.ar(Changed.kr(anchorHard));
        pos         = anchorPos + Sweep.ar(anchorTrig, ticksPerSec * run);
        idx         = pos.floor;
        reset       = (hardTrig + (anchorTrig * (idx < (Delay1.ar(idx) - 2)))) > 0;
        top         = RunningMax.ar(idx, reset);
        fire        = ((idx > Delay1.ar(top)) * (1 - reset)) + (hardTrig * anchorFire);
        elapsed     = idx - anchorPos.floor;

        maxLayers.do { |i|
            var period = cycleTicks[i].max(1) * slots;
            var local  = (base[i] + elapsed) % period;
            var frame  = (i * slots * maxCycleTicks) + local;
            var data   = BufRd.ar(4, buf, frame, 0, 1);
            var on     = i < numLayers;
            var onset  = fire * (data[0] > 0) * (1 - mute[i]) * on;
            var voices = nv.collect { |v|
                var hit = onset * ((((data[3] - v).abs < 0.5) + (data[3] < -0.5)) > 0);
                var dur = Latch.ar(data[1], hit);
                var amp = Latch.ar(data[0], hit);
                var pitch = Latch.ar(data[2], hit);
                var raw = (Sweep.ar(hit, ticksPerSec * run) < dur) * (run > 0);
                // retriggered while high: low for 3 ms, so the edge is seen
                var dip = Latch.ar(Delay1.ar(raw), hit) * (Sweep.ar(hit, 1) < 0.003);
                // the gate carries the hit's level (a gate rising to 0.6 plays at 0.6)
                [hit * (1 + (trigAmp * (amp - 1))), raw * (1 - dip) * on * amp, pitch]
            };
            Out.ar(OceanodeOutput.kr(("trigout" ++ (i + 1)).asSymbol), voices.collect(_[0]));
            Out.ar(OceanodeOutput.kr(("gateout" ++ (i + 1)).asSymbol), voices.collect(_[1]));
            Out.ar(OceanodeOutput.kr(("pitchout" ++ (i + 1)).asSymbol), voices.collect(_[2]));
        };
    }).writeDefFile(defDir);
};
"EuclideanBoxPlayer compiled".postln;
)
