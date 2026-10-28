// DrumKitBox's own effects, offline: a short blip (DKTrig -> DKBody ->
// DKVoice_2) into boxEchoDrift_2 (Pitch +7, feedback 0.6) and, in a second
// file, into boxGrainDelay_2. fx_test.py measures them.
// Usage: sclang fx_test.scd <DrumKitBox defs> <out dir>
(
var args = thisProcess.argv, defDir = args[0], outDir = args[1], render;
render = { |name, fx, cond|
    var score = [[0.0, [\d_loadDir, defDir]],
        [0.01, [\s_new, \DKTrig, 1000, 0, 0, \hitbus, 20, \vis, -1, \gatebus, 40, \velbus, 300, \rndbus, 301]],
        [0.01, [\s_new, \DKBody, 1001, 1, 0, \hitbus, 20, \out, 30, \modbus, 200, \bodyfreq, 600, \sweep, 0, \bodydecay, 0.05, \bodycurve, -6]],
        [0.01, [\s_new, \DKVoice_2, 1002, 1, 0, \raw, 30, \hitbus, 20, \out, 50, \modbus, 200, \keybus, -1]],
        [0.01, [\s_new, fx[0], 1003, 1, 0, \in, 50, \out, 0] ++ fx[1]],
        [0.1, [\n_set, 1000, \gate, 1, \fire, 1]], [0.15, [\n_set, 1000, \gate, 0]],
        [4.0, [\c_set, 0, 0]]];
    Score(score).recordNRT(nil, outDir +/+ (name ++ ".wav"), sampleRate: 48000, headerFormat: "WAV", sampleFormat: "float",
        options: ServerOptions.new.numOutputBusChannels_(2).sampleRate_(48000), duration: 4.0, action: { cond.unhang });
};
fork {
    var c = Condition();
    render.("echo", [\boxEchoDrift_2, [\delay, 0.4, \feed, 0.6, \pitch, 7, \mix, 0.5, \cutoff, 130, \pingpong, 0]], c); c.hang;
    render.("grain", [\boxGrainDelay_2, [\delay, 0.2, \density, 30, \size, 0.06, \feed, 0.4, \mix, 0.6]], c); c.hang;
    0.exit;
};
)
