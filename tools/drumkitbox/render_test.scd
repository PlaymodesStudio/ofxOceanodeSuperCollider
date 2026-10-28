// DrumKitBox NRT check: renders one piece per file (DKTrig -> layers ->
// DKVoice_2) with three hits, for render_test.py to measure.
// Usage: sclang render_test.scd <compiled defs dir> <out dir> [MixerPro defs dir]
(
var args = thisProcess.argv, defDir = args[0], outDir = args[1], fxDir = args[2], pieces, server, chain;
server = Server(\nrt, NetAddr("127.0.0.1", 57999), ServerOptions.new.numOutputBusChannels_(2).sampleRate_(48000));
// hit bus 20..25, raw 30..31, modbus 200.. (zeros), all wired by hand
pieces = [
    [\kick, [[\DKBody, []], [\DKClick, [\clicklevel, -6]]], [\drive, 0]],
    [\snare, [[\DKBody, [\bodyfreq, 185, \sweep, 12, \bodydecay, 0.12, \bodyshape, 1, \m2level, 0.5]],
              [\DKNoise, [\noisefilter, 2, \noisecut, 1800, \noisedecay, 0.22, \noiselevel, -3, \noisewidth, 0.6]]], []],
    [\hat, [[\DKMetal, [\metaldecay, 0.06]], [\DKNoise, [\noisefilter, 2, \noisecut, 8000, \noisedecay, 0.05, \noiselevel, -12]]], []],
    [\hatdur, [[\DKMetal, [\metaldecay, 0.05]], [\DKNoise, [\noisefilter, 2, \noisecut, 8000, \noisedecay, 0.04, \noiselevel, -12]]], []],
    [\ohatmono, [[\DKMetal, [\metaldecay, 0.4, \metalcurve, -3]]], []],
    [\ohatpoly, [[\DKMetalP, [\metaldecay, 0.4, \metalcurve, -3]]], []],
    [\hatfilt, [[\DKNoise, [\noisefilter, 0, \noisecut, 18000, \noisedecay, 0.2, \noiselevel, -6]]], []],
    [\stutter, [[\DKBody, [\bodyfreq, 52, \sweep, 0, \bodydecay, 0.4, \bodycurve, -2]]], []],
    [\clap, [[\DKNoise, [\bursts, 4, \burstspace, 0.011, \noisefilter, 1, \noisecut, 1300, \noisereso, 0.3, \noisedecay, 0.25]]], []],
    [\tom, [[\DKBody, [\bodyfreq, 110, \sweep, 7, \bodydecay, 0.35, \m2level, 0.3]], [\DKClick, [\clicktype, 3, \clicktone, 2500, \clicklevel, -12]]], []],
    [\cowbell, [[\DKMetal, [\metalmode, 1, \metalfreq, 540, \inharm, 0.35, \metaltone, 2600, \metalhp, 400, \metaldecay, 0.25, \metalcolor, 0.6]]], []],
    [\zap, [[\DKBody, [\bodyfreq, 80, \sweep, 48, \sweeptime, 0.12, \sweepcurve, -2, \bodyshape, 2, \fmamt, 1.5, \bodydecay, 0.3]]],
        [\charon, 1, \drivetype, 2, \drive, 12, \bits, 8, \srate, 12000]]
];

// The whole chain of two pads, as scDrumKitBox wires it: a kick, and a clap
// through boxMicroSpace (MixerPro's) into its strip, ducked by the kick's key.
// Out 0-1: the mix; 2-3: the clap's own output. Hits: kick + clap at 0.1,
// the clap alone at 1.1.
chain = { |cond|
    var score = [[0.0, [\d_loadDir, defDir]], [0.0, [\d_loadDir, fxDir]],
        [0.01, [\s_new, \DKTrig, 1000, 1, 0, \hitbus, 20, \vis, -1, \gatebus, 90, \velbus, 300, \rndbus, 301]],
        [0.01, [\s_new, \DKBody, 1001, 1, 0, \hitbus, 20, \out, 30, \modbus, 200]],
        [0.01, [\s_new, \DKVoice_2, 1002, 1, 0, \raw, 30, \hitbus, 20, \out, 40, \modbus, 200, \keybus, 400]],
        [0.01, [\s_new, \DKStrip_2, 1003, 1, 0, \bus, 40, \mix, 0, \padout, -1, \meterbus, 310, \master, 1]],
        [0.01, [\s_new, \DKTrig, 1100, 1, 0, \hitbus, 50, \vis, -1, \gatebus, 91, \velbus, 302, \rndbus, 303]],
        [0.01, [\s_new, \DKNoise, 1101, 1, 0, \hitbus, 50, \out, 60, \modbus, 200, \bursts, 4, \noisecut, 1200, \noisedecay, 0.2]],
        [0.01, [\s_new, \DKVoice_2, 1102, 1, 0, \raw, 60, \hitbus, 50, \out, 70, \modbus, 200, \keybus, 401]],
        [0.01, [\s_new, \boxMicroSpace2, 1103, 1, 0, \in, 70, \out, 80, \mix, 0.4, \decay, 2.0]],
        [0.01, [\s_new, \DKStrip_2, 1104, 1, 0, \bus, 80, \mix, 0, \padout, 2, \meterbus, 311, \master, 1,
            \duckbus, 400, \duckamt, 1, \duckrel, 0.3]],
        [0.1, [\n_set, 1000, \gate, 1, \fire, 1], [\n_set, 1100, \gate, 1, \fire, 1]],
        [0.15, [\n_set, 1000, \gate, 0], [\n_set, 1100, \gate, 0]],
        [1.1, [\n_set, 1100, \gate, 1, \fire, 2]],
        [1.15, [\n_set, 1100, \gate, 0]],
        [3.0, [\c_set, 0, 0]]];
    Score(score).recordNRT(nil, outDir +/+ "chain.wav", sampleRate: 48000, headerFormat: "WAV", sampleFormat: "float",
        options: ServerOptions.new.numOutputBusChannels_(4).sampleRate_(48000), duration: 3.0,
        action: { "rendered chain".postln; cond.unhang });
};
fork {
pieces.do { |piece|
    var cond = Condition(), name = piece[0], layers = piece[1], voiceArgs = piece[2], score, path;
    score = [[0.0, [\d_loadDir, defDir]],
        [0.01, [\s_new, \DKTrig, 1000, 0, 0, \hitbus, 20, \vis, -1, \gatebus, 40, \velbus, 300, \rndbus, 301]]];
    layers.do { |l, i| score = score.add([0.01, [\s_new, l[0], 1001 + i, 1, 0, \hitbus, 20, \out, 30, \modbus, 200] ++ l[1]]) };
    score = score.add([0.01, [\s_new, \DKVoice_2, 1100, 1, 0, \raw, 30, \hitbus, 20, \out, 0, \modbus, 200, \keybus, -1] ++ voiceArgs]);
    // hits at 0.1 (full), 1.1 (0.5), 2.1 (full, pitched +7)
    score = score.add([0.1, [\n_set, 1000, \gate, 1, \fire, 1]]);
    score = score.add([0.15, [\n_set, 1000, \gate, 0]]);
    if(name.asString.beginsWith("ohat")) {
        score = score.add([0.13, [\n_set, 1000, \gate, 1, \fire, 10]]);
        score = score.add([0.135, [\n_set, 1000, \gate, 0]]);
    };
    score = score.add([1.1, [\n_set, 1000, \gate, 0.5, \fire, 2]]);
    score = score.add([1.15, [\n_set, 1000, \gate, 0]]);
    score = score.add([2.1, if(name == \stutter, { [\n_set, 1000, \stut, 60, \gate, 1, \fire, 3] }, { nil }) ?? if(name == \hatfilt, { [\n_set, 1000, \filt, -0.8, \gate, 1, \fire, 3] }, { nil }) ?? if(name == \hatdur, { [\n_set, 1000, \dur, 4, \gate, 1, \fire, 3] }, { [\n_set, 1000, \pitch, 67, \gate, 1, \fire, 3] })]);
    score = score.add([if(name == \stutter, 2.5, 2.15), [\n_set, 1000, \gate, 0]]);
    score = score.add([3.0, [\c_set, 0, 0]]);
    path = outDir +/+ (name ++ ".wav");
    Score(score).recordNRT(nil, path, sampleRate: 48000, headerFormat: "WAV", sampleFormat: "float",
        options: server.options, duration: 3.0, action: { ("rendered " ++ path).postln; cond.unhang });
    cond.hang;
};
if(fxDir.notNil) { var cond = Condition(); chain.(cond); cond.hang };
0.exit;
};
)
