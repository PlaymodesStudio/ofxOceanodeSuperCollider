// slicerBox.scd — SynthDefs for the SlicerBox C++ node (src/nodes/boxSynths/slicerBox/scSlicerBox.*)
//
// One persistent synth plays every slice of one buffer: a voice per slice,
// each triggered by its own lane of the gate vector.
//
//   SlicerBoxPlayer<S>_<C>   S slice voices (16, 32, 64: the node picks the
//                            smallest cap >= its slice count), a buffer of C
//                            channels (1 or 2); always writes stereo to \out.
//
// Controls (arrays of S unless noted):
//   buf out level           buffer, output bus, level (scalars)
//   gate                    gate per slice (control rate)
//   gate_sel gate_ar        1 = also read the audio-rate gates gate_ar (an
//                           audio cable on the node's Gate inlet, or the
//                           sequencer below); they add to \gate
//   audition                gate per slice from the node's window
//   start end               slice bounds in buffer frames
//   rate                    playback rate (pitch and the buffer's own sample
//                           rate are applied here with BufRateScale)
//   rev                     1 = play the slice backwards
//   mode                    0 = one-shot (plays to its end on the rising
//                           edge), 1 = gated (stops when the gate falls)
//   vis                     control bus of S channels: each voice's progress
//                           through its slice, 0 when silent (for the window)
//
//   curveA curveD curveR     its segments' tension (SC curve values, -4 default)
//   atk dec sus rel          the amplitude envelope (ADSR, seconds), the same
//                            for every slice. One-shot: the release starts in
//                            time to end with the slice; gated: when the gate
//                            falls.
//
// A rising edge restarts its voice and its envelope. Each voice also fades out
// over the last 3 ms before its slice ends, so no slice clicks at its end or
// bleeds into the next one.
(
var d;
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} { d = Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs" };
File.mkdir(d);
d = d +/+ "SlicerBox";
File.mkdir(d);

[16, 32, 64].do { |cap|
    [1, 2].do { |channels|
        SynthDef(("SlicerBoxPlayer" ++ cap ++ "_" ++ channels).asSymbol, {
            var buf = \buf.kr(0), level = \level.kr(1, 0.05);
            // control-rate gates plus, when gate_sel is 1, the audio-rate ones
            // (an audio cable on the Gate inlet, or the sequencer)
            var gate = K2A.ar(\gate.kr(0 ! cap)) + (\gate_ar.ar(0 ! cap) * \gate_sel.kr(0));
            var audition = \audition.kr(0 ! cap);
            var start = \start.kr(0 ! cap), end = \end.kr(0 ! cap), rate = \rate.kr(1 ! cap);
            var rev = \rev.kr(0 ! cap), mode = \mode.kr(0 ! cap);
            var scale = BufRateScale.kr(buf)   // x Global Transpose (resampled)
                * (2 ** (((\gtrbus.kr(-1) >= 0) * In.kr(\gtrbus.kr(-1).max(0))) / 12));
            // amplitude envelope, the same for every slice (seconds)
            var atk = \atk.kr(0.002).max(0.001), dec = \dec.kr(0.1).max(0.001);
            var sus = \sus.kr(1).clip(0, 1), rel = \rel.kr(0.008).max(0.003);
            // each segment's tension (SC curve numbers: 0 linear, -4 the default)
            var curves = [\curveA.kr(-4), \curveD.kr(-4), \curveR.kr(-4)].collect(_.clip(-12, 12));
            var voices = cap.collect { |i|
                var g = ((gate[i] + K2A.ar(audition[i])) > 0);
                var trig = HPZ1.ar(g) > 0;                       // rising edge
                var speed = rate[i] * scale;                     // buffer frames per output sample
                var len = (end[i] - start[i]).max(1);
                var pos = Sweep.ar(trig, SampleRate.ir * speed); // frames played since the edge
                var armed = PulseCount.ar(trig) > 0;             // nothing before the first edge
                var playing = (pos < len) * armed;
                // one-shot: the release starts in time to end with the slice;
                // gated: when the gate falls
                var hold = Select.ar(mode[i], [(len - pos) > (rel * speed * SampleRate.ir), g]);
                var on = playing * hold;
                var frame = Select.ar(rev[i], [start[i] + pos, end[i] - 1 - pos]);
                var edge = ((len - pos) / (speed * SampleRate.ir * 0.003).max(1)).clip(0, 1);
                // a rising edge restarts the envelope (the gate dips for one sample)
                var env = EnvGen.ar(Env.adsr(atk, dec, sus, rel, 1, curves), on * (1 - trig));
                var sig = BufRd.ar(channels, buf, frame, 0, 4) * (env * edge);
                [sig, A2K.kr(playing * (env > 0.0005) * (pos / len).clip(0.001, 1))]
            };
            var mix = voices.collect(_[0]).sum;
            if(channels == 1) { mix = mix ! 2 };
            Out.kr(\vis.kr(0), voices.collect(_[1]));
            Out.ar(\out.kr(0), LeakDC.ar(mix) * level);
        }).writeDefFile(d);
    };
};

// SlicerBoxSeq: the node's step sequencer, before the player. Step j plays
// slice \slice[j] (-1: nothing) for \len[j] steps (times \gatescale): it
// writes that gate, sample accurately, into channel slice[j] of \out (64
// channels, mapped to the player's gate_ar). A note ends 2 ms early, so
// back-to-back notes on the same slice retrigger. \ext (an audio cable on the
// Gate inlet, -1 none) is passed on too. Steps per second = bpm / 60 *
// perBeat; \run's rising edge (and t_reset) restarts at step 1. \posbus
// gets the position in steps (-1 stopped), then the slice each step plays
// now (64; -1 rest), for the window.
// Random (\randamt 0..1): each time a step comes round, it plays a random
// slice (of \nslices) instead of its own with that chance. Drawn when the
// step's note ends (and at the start), so a note never changes mid-way.
SynthDef(\SlicerBoxSeq, {
    var n = 64, out = \out.kr(0), ext = \ext.kr(-1);
    var steps = \steps.kr(16).clip(1, n);
    var rate = \bpm.kr(120).max(1) / 60 * \perBeat.kr(4);
    var run = \run.kr(0) > 0.5;
    var restart = (HPZ1.kr(run) > 0) + \reset.tr(0);
    // an audio signal on the node's Reset inlet: each rising edge restarts,
    // sample accurately (-1: none)
    var resetBus = \resetbus.kr(-1);
    var resetAr = HPZ1.ar((In.ar(resetBus.max(0), 1) * (resetBus >= 0)) > 0.5) > 0;
    var pos = Phasor.ar((T2A.ar(restart > 0) + resetAr) > 0, rate * run * SampleDur.ir, 0, steps);
    var slices = \slice.kr(-1 ! n), lens = \len.kr(1 ! n);
    var gap = 0.002 * rate, scale = \gatescale.kr(1).max(0.01);
    var randAmt = \randamt.kr(0).clip(0, 1), nsl = \nslices.kr(1).max(1);
    var playing = n.collect { |j|
        var len = (lens[j] * scale).clip(0.02, n);
        var since = (pos - j).wrap(0, steps);
        var has = (slices[j] >= 0) * (j < steps);
        var on = (since < (len - gap).max(gap)) * has * run;
        // a new draw when this step's note ends: the next pass, never mid-note
        var trig = (HPZ1.kr(A2K.kr(on)) < 0) + Impulse.kr(0);
        var sl = Select.kr(TRand.kr(0, 1, trig) < randAmt, [slices[j], TIRand.kr(0, nsl - 1, trig)]);
        Out.ar(out + sl.clip(0, n - 1), on);
        Select.kr(has, [-1, sl])
    };
    Out.ar(out, In.ar(ext.max(0), n) * (ext >= 0));
    Out.kr(\posbus.kr(0), [Select.kr(run, [-1, A2K.kr(pos)])] ++ playing);
}).writeDefFile(d);

// SlicerBoxFilter: the stereo filter after the player (and the EQ). type 0
// low-pass, 1 high-pass, 2 band-pass; slope 0 = 12 dB/oct, 1 = 24 dB/oct;
// resonance 0..1 (0: Butterworth, no peak). \on crossfades to dry.
SynthDef(\SlicerBoxFilter, {
    var sig = In.ar(\in.kr(0), 2);
    var f = \cutoff.kr(2000, 0.05).clip(20, 20000).min(SampleRate.ir * 0.45);
    var res = (1 - \resonance.kr(0, 0.05).clip(0, 0.95)).max(0.05);
    var bw = res * 2;                                  // band-pass width, octaves
    var lp12 = BLowPass.ar(sig, f, res * 2.sqrt);
    var hp12 = BHiPass.ar(sig, f, res * 2.sqrt);
    var bp12 = BBandPass.ar(sig, f, bw);
    var lp24 = BLowPass.ar(BLowPass.ar(sig, f, 0.5412.reciprocal), f, res * 1.3066.reciprocal * 2.sqrt);
    var hp24 = BHiPass.ar(BHiPass.ar(sig, f, 0.5412.reciprocal), f, res * 1.3066.reciprocal * 2.sqrt);
    var bp24 = BBandPass.ar(BBandPass.ar(sig, f, bw), f, bw);
    var filters = [lp12, hp12, bp12, lp24, hp24, bp24];
    var types = NamedControl.kr(\typeweights, 0 ! 3).lag(0.03);
    var slopes = NamedControl.kr(\slopeweights, 0 ! 2).lag(0.03);
    var y = Select.ar(types.sum > 0, [
        Select.ar(\type.kr(0).clip(0, 2) + (3 * (\slope.kr(0) > 0.5)), filters),
        Mix(filters.collect { |signal, i| signal * types[i % 3] * slopes[i.div(3)] })]);
    Out.ar(\out.kr(0), LinXFade2.ar(sig, y, \on.kr(1, 0.03) * 2 - 1));
}).writeDefFile(d);
"SlicerBox SynthDefs written to %".format(d).postln;
)
