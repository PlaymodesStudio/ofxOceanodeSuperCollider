// DrumKitBox demo: four bars at 128 BPM through the real synths (DKTrig ->
// layers -> DKVoice_2 -> DKStrip_2), with the factory sounds' settings
// (scDrumKitBoxData.cpp: Kick "Techno Rumble", Clap "Big Room" + reverb,
// Closed / Open Hat "909" choked together, Perc "Wood", 808 Sub with a
// pitch per step). The clap and hats duck from the kick.
// Usage: sclang demo_beat.scd <DrumKitBox defs> <MixerPro defs> <out.wav>
(
var args = thisProcess.argv, defDir = args[0], fxDir = args[1], outPath = args[2];
var bpm = 128, step = 60 / bpm / 4, bars = 4, t0 = 0.05, score, pads, id = 2000, bus = 20, kbus = 500;
var add = { |msg| score = score.add(msg) };
score = [[0.0, [\d_loadDir, defDir]], [0.0, [\d_loadDir, fxDir]]];
// one pad: [name, layers [[def, args]], voice args, strip args, fx [def, args] or nil, choke slot, mask]
pads = [
    [\kick, [[\DKBody, [\bodyfreq, 47, \sweep, 28, \sweeptime, 0.045, \sweepcurve, -5, \bodydecay, 0.7, \bodycurve, -3, \bodydrive, 0.35, \bodyshape, 0.1]],
             [\DKClick, [\clicklevel, -12, \clicktone, 2500, \clickdecay, 0.004]],
             [\DKNoise, [\noisecolor, 2, \noisefilter, 0, \noisecut, 180, \noisereso, 0.4, \noisedecay, 0.6, \noisecurve, -3, \noiselevel, -14]]],
        [\charon, 1, \drivetype, 0, \drive, 8], [\level, 0.9], nil],
    [\clap, [[\DKNoise, [\noisefilter, 1, \noisecut, 1500, \noisereso, 0.35, \bursts, 5, \burstspace, 0.014, \noisedecay, 0.5,
                         \noisecurve, -5, \noisewidth, 1, \noisefenv, 0.5, \noisefdecay, 0.03]]],
        [], [\level, 0.6, \duckamt, 0.4], [\boxMicroSpace2, [\mix, 0.3, \decay, 2.2, \size, 30, \predelay, 15, \lowpass, 9000, \position, 0.8, \spread, 0.15]]],
    [\chat, [[\DKMetal, [\metalfreq, 410, \metaltone, 10000, \metalreso, 0.3, \metalhp, 7000, \metaldecay, 0.05, \metalcurve, -6, \metalcolor, 0.2, \metallevel, -8]],
             [\DKNoise, [\noisefilter, 2, \noisecut, 7500, \noisedecay, 0.06, \noiselevel, -3, \noisewidth, 0.3]]],
        [\pan, -0.2], [\level, 0.45, \duckamt, 0.5], nil],
    [\ohat, [[\DKMetal, [\metalfreq, 410, \metaltone, 10000, \metalreso, 0.3, \metalhp, 7000, \metaldecay, 0.4, \metalcurve, -3, \metalcolor, 0.2, \metallevel, -7]],
             [\DKNoise, [\noisefilter, 2, \noisecut, 7500, \noisedecay, 0.35, \noiselevel, -4, \noisewidth, 0.3]]],
        [\pan, 0.2], [\level, 0.4, \duckamt, 0.6], nil],
    [\perc, [[\DKBody, [\bodyfreq, 750, \fmamt, 0.8, \fmratio, 3.4, \fmdecay, 0.05, \bodydecay, 0.06, \sweep, 3, \sweeptime, 0.01]],
             [\DKClick, [\clicktype, 3, \clicktone, 2200, \clicklevel, -10]]],
        [\pan, 0.5], [\level, 0.35], [\mixerProEcho2, [\delay, 60 / bpm * 0.75, \feed, 0.45, \cutoff, 96, \resonance, 0.1, \pingpong, 1, \mix, 0.35]]],
    [\sub, [[\DKBody, [\bodyfreq, 41.2, \sweep, 12, \sweeptime, 0.02, \bodydecay, 0.5, \bodycurve, -1.5, \bodydrive, 0.4]]],
        [], [\level, 0.55, \duckamt, 0.7, \duckrel, 0.12], nil]
];
// the graph, pad by pad: trig, layers, voice, (effect), strip
pads.do { |pad, k|
    var hit = bus, raw = bus + 10, pb = bus + 12, fxb = bus + 14, trig = id;
    var mask = 0 ! 16;
    if(pad[0] == \chat) { mask[3] = 1 };
    if(pad[0] == \ohat) { mask[2] = 1 };
    add.([0.01, [\s_new, \DKTrig, id, 1, 0, \hitbus, hit, \vis, -1, \gatebus, 900 + k, \velbus, 600 + k, \rndbus, 620 + k,
        \chokeblock, 700, \chokeslot, k] ++ [\chokemask, $[] ++ mask ++ [$]]]);
    id = id + 1;
    pad[1].do { |l| add.([0.01, [\s_new, l[0], id, 1, 0, \hitbus, hit, \out, raw, \modbus, 800] ++ l[1]]); id = id + 1 };
    add.([0.01, [\s_new, \DKVoice_2, id, 1, 0, \raw, raw, \hitbus, hit, \out, if(pad[4].notNil, fxb, pb), \modbus, 800,
        \keybus, kbus + k] ++ pad[2]]);
    id = id + 1;
    if(pad[4].notNil) { add.([0.01, [\s_new, pad[4][0], id, 1, 0, \in, fxb, \out, pb] ++ pad[4][1]]); id = id + 1 };
    add.([0.01, [\s_new, \DKStrip_2, id, 1, 0, \bus, pb, \mix, 0, \padout, -1, \meterbus, 650 + k, \master, 0.8,
        \duckbus, if(k == 0, -1, kbus)] ++ pad[3]]);
    id = id + 1;
    pad = pad.add(trig);
    pads[k] = pad;
    bus = bus + 17;
};
// the pattern: 16 steps a bar (x: hit, digits: velocity / 9)
(
    kick: "9...9...9...9...",
    clap: "....9.......9...",
    chat: "7.5.7.5.7.5.7569",
    ohat: "..9...9...9...9.",
    perc: "...6..5....7.4..",
    sub:  "..8...8..8....8."
).keysValuesDo { |name, pat|
    var pad = pads.detect { |p| p[0] == name }, trig = pad.last;
    var subNotes = [36, 36, 39, 31];
    bars.do { |bar|
        pat.do { |c, s|
            if(c != $.) {
                var t = t0 + ((bar * 16 + s) * step), vel = c.digit / 9;
                var msg = [\n_set, trig, \gate, vel, \fire, bar * 16 + s + 1];
                if(name == \sub) { msg = msg ++ [\pitch, subNotes[bar] + 24] };
                add.([t, msg]);
                add.([t + (step * 0.5), [\n_set, trig, \gate, 0]]);
            };
        };
    };
};
score = score.add([t0 + (bars * 16 * step) + 2.5, [\c_set, 0, 0]]);
score = score.sort { |a, b| a[0] <= b[0] };
fork {
    var cond = Condition();
    Score(score).recordNRT(nil, outPath, sampleRate: 48000, headerFormat: "WAV", sampleFormat: "int24",
        options: ServerOptions.new.numOutputBusChannels_(2).sampleRate_(48000), duration: t0 + (bars * 16 * step) + 2.5,
        action: { ("rendered " ++ outPath).postln; cond.unhang });
    cond.hang;
    0.exit;
};
)
