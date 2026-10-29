// BeatRepeat Pro, offline: a kick on every beat (120 BPM, onsets 30 samples
// into a control block, like a sample-accurate drum Box) into BeatRepeatPro2,
// Go sent on a beat.  Channels 0-1 are the node's output, channel 2 the raw
// input.  render_test.py measures each case.
// Usage: sclang render_test.scd <BeatRepeatPro defs> <out dir>
(
var args = thisProcess.argv, defDir = args[0], outDir = args[1], testDefs, render, offset = 30 / 48000;
testDefs = outDir +/+ "testdefs";
File.mkdir(testDefs);
SynthDef(\BRTestKick, { |out = 10, raw = 2, tone = 0|
    var trig = DelayN.ar(Impulse.ar(2), 0.01, 30 / 48000);
    // Each beat has its own level, so a repeat is told apart from the dry.
    var amp = Demand.ar(trig, 0, Dseq([1, 0.5, 0.8, 0.3], inf));
    var kick = SinOsc.ar(70, pi / 2) * EnvGen.ar(Env.perc(0, 0.25, curve: -6), trig) * amp;
    var sig = kick + (SinOsc.ar(440) * tone);
    Out.ar(out, [sig, sig]);
    Out.ar(raw, sig);
}).writeDefFile(testDefs);
render = { |name, params, cond, goAt = 1.0, offAt|
    var score = [[0.0, [\d_loadDir, defDir]], [0.0, [\d_loadDir, testDefs]],
        [0.0, [\s_new, \BRTestKick, 1000, 0, 0, \out, 10, \raw, 2, \tone, params.at(\tone) ? 0]],
        [0.0, [\s_new, \BeatRepeatPro2, 1001, 3, 1000, \in, 10, \out, 0, \bpm, 120, \vis, 0,
            \limiter, 0] ++ params.reject({ |v, k| k == \tone }).getPairs],
        [goAt + offset, [\n_set, 1001, \go, 1]],
        [(offAt ? 3.9) + offset, [\n_set, 1001, \go, offAt.notNil.if(0, 1)]],
        [4.0, [\c_set, 0, 0]]];
    Score(score).recordNRT(nil, outDir +/+ (name ++ ".wav"), sampleRate: 48000, headerFormat: "WAV", sampleFormat: "float",
        options: ServerOptions.new.numOutputBusChannels_(3).sampleRate_(48000), duration: 4.0, action: { cond.unhang });
};
fork {
    var c = Condition();
    render.("repeat", (division: 4), c); c.hang;
    render.("gate", (division: 4, gate: 0.5, tone: 0.1), c); c.hang;
    render.("gaterepeat", (division: 4, gate: 0.5), c); c.hang;
    render.("auto", (division: 8, autoreset: 1, autoresetmode: 1, autobeats: 1), c); c.hang;
    render.("autoloops", (division: 8, autoreset: 1, autoresetmode: 0, autoloops: 2), c); c.hang;
    render.("bypass", (division: 4, mix: 0), c); c.hang;
    render.("release", (division: 4), c, 1.0, 2.6); c.hang;
    render.("pitch", (pitchmode: 1, repeatpitch: 60, pitchstep: 1, numpitchsteps: 4), c); c.hang;
    render.("reverse", (division: 8, reverse: 1), c); c.hang;
    render.("rate", (division: 4, rate: 2, adaptlength: 1, pitchstep: -3, numpitchsteps: 3,
        autoreset: 1, autoloops: 5), c); c.hang;
    0.exit;
};
)
