// ArpeggiatorBoxPlayer — plays the notes the ArpeggiatorBox node
// (scArpeggiatorBox.h) writes into its buffer, sample accurately.
//
// Buffer: 80 channels = 20 slots (16 voices, then 4 bass voices) x
// [level, duration, pitch, offset]; level 0: no note.
//   frames 0..1535    clocked: one frame per tick (48 per beat), 4 chunks of 8 beats
//   frames 1536..1791 trigger mode: one frame per external trigger (ring of 256)
//
// \clock 1 Transport: MetroBox's clock (scTransportSync anchors, \base);
//        2 Internal: its own tick counter at \freeBpm (Phasor, wraps at
//          393216 ticks, a multiple of the buffer);
//        0 Trigger: every rising edge of \trig (Oceanode value), \trig_ar
//          (an audio cable, \trig_sel 1) or \t_bang steps one frame; the
//          step's length is the time since the previous trigger.
// Durations and offsets: ticks (clocked) or steps (trigger mode). A note
// starts `offset` after its frame (strum, swing, a pulse between ticks),
// then its gate stays high for `duration`, at its level; a voice
// retriggered while high goes low for 3 ms first (envelopes restart).
// Pitch is held from each note's start.
//
// \status (control bus, 2 channels): the trigger count and the Internal
// clock's tick, so the node writes the frames about to play.

(
var d, defDir;
var slots = 20, voices = 16, ch = 4, clocked = 4 * 384, rows = 256, freeWrap = 1536 * 256;
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "/CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "/SuperCollider/CompiledSynthdefs";
};
defDir = d +/+ "/ArpeggiatorBox";
File.mkdir(d);
File.mkdir(defDir);

SynthDef(\ArpeggiatorBoxPlayer, {
    var buf = \buf.kr(-1), clock = \clock.kr(1), trigAmp = \trigAmp.kr(0);
    var isTrig = clock < 0.5, isFree = clock > 1.5;
    // transport
    var anchorId = \anchorId.kr(0), anchorHard = \anchorHard.kr(0);
    var anchorPos = \anchorPos.ar(0), anchorFire = \anchorFire.ar(0), run = \run.ar(0), bpm = \bpm.kr(120), base = \base.ar(0);
    var tps = bpm / 60 * 48;
    var anchorTrig = T2A.ar(Changed.kr(anchorId)), hardTrig = T2A.ar(Changed.kr(anchorHard));
    var pos = anchorPos + Sweep.ar(anchorTrig, tps * run);
    var idx = pos.floor;
    var reset = (hardTrig + (anchorTrig * (idx < (Delay1.ar(idx) - 2)))) > 0;
    var top = RunningMax.ar(idx, reset);
    var fireT = ((idx > Delay1.ar(top)) * (1 - reset)) + (hardTrig * anchorFire);
    var frameT = (base + idx - anchorPos.floor) % clocked;
    // internal clock
    var freeRate = \freeBpm.kr(120) / 60 * 48 * isFree;
    var freeReset = T2A.ar(Changed.kr(\freeReset.kr(0)));
    var posF = Phasor.ar(freeReset, freeRate * SampleDur.ir, 0, freeWrap);
    var idxF = posF.floor;
    // every new tick, and tick 0 at the start and on a reset
    var fireF = ((HPZ1.ar(idxF).abs > 0) + freeReset + Impulse.ar(0)) > 0 * isFree;
    var frameF = idxF % clocked;
    // trigger mode
    var kv = \trig.kr(0) > 0, edgeK = T2A.ar((kv - Delay1.kr(kv)) > 0);
    var av = \trig_ar.ar(0) > 0, edgeA = (av - Delay1.ar(av)) > 0;
    var sel = \trig_sel.kr(0);
    var tIn = ((edgeK * (1 - sel)) + (edgeA * sel) + T2A.ar(\t_bang.tr(0))) > 0 * isTrig;
    var cnt = PulseCount.ar(tIn, T2A.ar(Changed.kr(\countReset.kr(0))));
    var interval = Latch.ar(Timer.ar(tIn), tIn);
    // the first trigger has no previous one: its length is unknown until the next
    var period = Latch.ar(interval, tIn * (cnt > 1) * (interval < 2) * (interval > 0.001));
    var frameS = clocked + ((cnt - 1).max(0) % rows);
    // the clock in use
    var fire = Select.ar(isTrig, [Select.ar(isFree, [fireT, fireF]), tIn]);
    var frame = Select.ar(isTrig, [Select.ar(isFree, [frameT, frameF]), frameS]);
    var rate = Select.ar(isTrig, [Select.ar(isFree, [tps * run, K2A.ar(freeRate)]), Select.ar(period > 0, [DC.ar(4), period.max(0.001).reciprocal])]);
    var running = Select.ar(isTrig, [Select.ar(isFree, [run > 0, DC.ar(1)]), DC.ar(1)]);
    var data = BufRd.ar(slots * ch, buf, frame, 0, 1);
    var outs = slots.collect { |j|
        var amp = data[j * ch], dur = data[j * ch + 1], pitch = data[j * ch + 2], off = data[j * ch + 3];
        var hit = fire * (amp > 0);
        var aL = Latch.ar(amp, hit), dL = Latch.ar(dur, hit), pL = Latch.ar(pitch, hit), oL = Latch.ar(off, hit);
        // the note starts `offset` after its frame
        var sw = Sweep.ar(hit, rate);
        var prev = Select.ar(hit, [Delay1.ar(sw), DC.ar(-1)]);
        var start = (sw >= oL) * (prev < oL) * (aL > 0);
        var since = Sweep.ar(start, rate);
        var nd = Latch.ar(dL, start), na = Latch.ar(aL, start), np = Latch.ar(pL, start);
        var raw = (since < nd) * (nd > 0) * running;
        var dip = Latch.ar(Delay1.ar(raw), start) * (Sweep.ar(start, 1) < 0.003);
        [start * (1 + (trigAmp * (na - 1))), raw * (1 - dip) * na, np]
    };
    Out.ar(OceanodeOutput.kr(\trigout), outs[0..(voices - 1)].collect(_[0]));
    Out.ar(OceanodeOutput.kr(\gateout), outs[0..(voices - 1)].collect(_[1]));
    Out.ar(OceanodeOutput.kr(\pitchout), outs[0..(voices - 1)].collect(_[2]));
    Out.ar(OceanodeOutput.kr(\btrigout), outs[voices..].collect(_[0]));
    Out.ar(OceanodeOutput.kr(\bgateout), outs[voices..].collect(_[1]));
    Out.ar(OceanodeOutput.kr(\bpitchout), outs[voices..].collect(_[2]));
    // no bus yet (-1): adds zeros to bus 0 instead (Out sums: harmless)
    Out.kr(\status.kr(-1).max(0), [A2K.kr(cnt), A2K.kr(posF)] * (\status.kr(-1) >= 0));
}).writeDefFile(defDir);
"ArpeggiatorBoxPlayer compiled".postln;
)
