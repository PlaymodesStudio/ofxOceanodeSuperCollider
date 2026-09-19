(
var here = thisProcess.nowExecutingPath.dirname;
var repository = (here +/+ "../..").standardizePath;
var scriptPath = repository +/+ "example/bin/data/Supercollider/DynGenDefs/SteamPipe.eel2";
var dynDef = DynGenDef(\steamPipeCore, File.readAllString(scriptPath));
var parameterNames = [
    \pitch, \gate, \velocity, \mod_wheel, \bend,
    \attack, \decay, \sustain, \release, \velocity_amount, \env_key_scaling,
    \dc_noise, \noise_cut, \noise_key_track, \noise_velocity_track,
    \noise_resonance, \noise_one_pole,
    \tune, \fine, \sample_correction, \tune_mw,
    \vibrato_amount, \vibrato_frequency, \polarity,
    \allpass_tune, \allpass_fine, \allpass_sample_correction, \allpass_mw,
    \allpass_on, \diffusion, \offset, \push,
    \feedback_rt, \feedback_key_track, \release_damp,
    \soft_hard, \symmetry,
    \hp0, \hp1, \hp_key_track, \lp0, \lp1, \lp_key_track, \gain
];

~synthCreator.value("SteamPipe", {|n|
    var controls, sig;

    controls = [
        OceanodeParameterLag.ar(\pitch, 60, n, 0, 127, "vf", 0.01, true),
        OceanodeParameter.ar(\gate, 0, n, 0, 1, "vf"),
        OceanodeParameter.ar(\velocity, 1, n, 0, 1, "vf"),
        OceanodeParameterLag.ar(\mod_wheel, 0, n, 0, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\bend, 0, n, -24, 24, "vf", 0.01, true),

        OceanodeParameterLag.ar(\attack, 25, n, -20, 100, "vf", 0.01, true),
        OceanodeParameterLag.ar(\decay, 20, n, -20, 100, "vf", 0.01, true),
        OceanodeParameterLag.ar(\sustain, 0.9, n, 0, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\release, 20, n, -20, 100, "vf", 0.01, true),
        OceanodeParameterLag.ar(\velocity_amount, 0.33, n, 0, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\env_key_scaling, -0.5, n, -1, 0, "vf", 0.01, true),

        OceanodeParameterLag.ar(\dc_noise, 0.66, n, 0, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\noise_cut, 80, n, 0, 120, "vf", 0.01, true),
        OceanodeParameterLag.ar(\noise_key_track, 0.33, n, 0, 2, "vf", 0.01, true),
        OceanodeParameterLag.ar(\noise_velocity_track, 0.3, n, 0, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\noise_resonance, 0, n, 0, 0.995, "vf", 0.01, true),
        OceanodeParameterLag.ar(\noise_one_pole, 1, n, 0, 1, "vf", 0.01, true),

        OceanodeParameterLag.ar(\tune, 1, n, -60, 60, "vf", 0.01, true),
        OceanodeParameterLag.ar(\fine, -0.12, n, -1, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\sample_correction, -1.5, n, -5, 0, "vf", 0.01, true),
        OceanodeParameterLag.ar(\tune_mw, -1.2, n, -2, 2, "vf", 0.01, true),
        OceanodeParameterLag.ar(\vibrato_amount, 0.16, n, 0, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\vibrato_frequency, 2.4, n, 0, 12, "vf", 0.01, true),
        OceanodeParameterLag.ar(\polarity, 1, n, -1, 1, "vf", 0.01, true),

        OceanodeParameterLag.ar(\allpass_tune, 0, n, -60, 60, "vf", 0.01, true),
        OceanodeParameterLag.ar(\allpass_fine, 0, n, -1, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\allpass_sample_correction, 0, n, -5, 0, "vf", 0.01, true),
        OceanodeParameterLag.ar(\allpass_mw, 0, n, -2, 2, "vf", 0.01, true),
        OceanodeParameterLag.ar(\allpass_on, 0, n, 0, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\diffusion, 0, n, -1, 1, "vf", 0.01, true),

        OceanodeParameterLag.ar(\offset, 0.4, n, -1, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\push, 1.56, n, 0, 2, "vf", 0.01, true),
        OceanodeParameterLag.ar(\feedback_rt, -60, n, -90, 30, "vf", 0.01, true),
        OceanodeParameterLag.ar(\feedback_key_track, 1, n, 0, 2, "vf", 0.01, true),
        OceanodeParameterLag.ar(\release_damp, 6, n, 0, 120, "vf", 0.01, true),
        OceanodeParameterLag.ar(\soft_hard, 0, n, 0, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\symmetry, 0.33, n, 0, 1, "vf", 0.01, true),

        OceanodeParameterLag.ar(\hp0, 12, n, 0, 120, "vf", 0.01, true),
        OceanodeParameterLag.ar(\hp1, 66, n, 0, 120, "vf", 0.01, true),
        OceanodeParameterLag.ar(\hp_key_track, 1, n, 0, 2, "vf", 0.01, true),
        OceanodeParameterLag.ar(\lp0, 66, n, 0, 120, "vf", 0.01, true),
        OceanodeParameterLag.ar(\lp1, 96, n, 0, 120, "vf", 0.01, true),
        OceanodeParameterLag.ar(\lp_key_track, 0.77, n, -2, 2, "vf", 0.01, true),
        OceanodeParameterLag.ar(\gain, 0, n, -60, 12, "vf", 0.01, true)
    ];

    sig = n.collect({ |channel|
        var pairs = Array.new(parameterNames.size * 2);
        parameterNames.do({ |name, index|
            pairs = pairs.add(name).add(controls[index][channel]);
        });
        DynGen.ar(1, dynDef, params: pairs, sync: 0.0);
    });

    Out.ar(OceanodeOutput.kr(\out), sig);

}, description: "Dry sample-accurate reconstruction of SteamPipe without reverb or post-delay", category: "Source/PhysicalModelling");
)
