(
var scriptPath = thisProcess.nowExecutingPath.dirname +/+ "steamPipe.eel2";
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

        OceanodeParameterLag.ar(\attack, 56.535432, n, -20, 100, "vf", 0.01, true),
        OceanodeParameterLag.ar(\decay, 20.629920, n, -20, 100, "vf", 0.01, true),
        OceanodeParameterLag.ar(\sustain, 0.905512, n, 0, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\release, 40.472441, n, -20, 100, "vf", 0.01, true),
        OceanodeParameterLag.ar(\velocity_amount, 0.330709, n, 0, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\env_key_scaling, -0.503887, n, -1, 0, "vf", 0.01, true),

        OceanodeParameterLag.ar(\dc_noise, 0.826772, n, 0, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\noise_cut, 109.606297, n, 0, 120, "vf", 0.01, true),
        OceanodeParameterLag.ar(\noise_key_track, 0.330709, n, 0, 2, "vf", 0.01, true),
        OceanodeParameterLag.ar(\noise_velocity_track, 0.307087, n, 0, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\noise_resonance, 0, n, 0, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\noise_one_pole, 1, n, 0, 1, "vf", 0.01, true),

        OceanodeParameterLag.ar(\tune, 1.417322, n, -60, 60, "vf", 0.01, true),
        OceanodeParameterLag.ar(\fine, -0.748031, n, -1, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\sample_correction, -1.535433, n, -5, 0, "vf", 0.01, true),
        OceanodeParameterLag.ar(\tune_mw, -1.212598, n, -2, 2, "vf", 0.01, true),
        OceanodeParameterLag.ar(\vibrato_amount, 0.060000, n, 0, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\vibrato_frequency, 5.900000, n, 0, 12, "vf", 0.01, true),
        OceanodeParameterLag.ar(\polarity, 1, n, -1, 1, "vf", 0.01, true),

        OceanodeParameterLag.ar(\allpass_tune, -19.370079, n, -60, 60, "vf", 0.01, true),
        OceanodeParameterLag.ar(\allpass_fine, 0.007874, n, -1, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\allpass_sample_correction, 0, n, -5, 0, "vf", 0.01, true),
        OceanodeParameterLag.ar(\allpass_mw, 1.338583, n, -2, 2, "vf", 0.01, true),
        OceanodeParameterLag.ar(\allpass_on, 1, n, 0, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\diffusion, -0.448819, n, -1, 1, "vf", 0.01, true),

        OceanodeParameterLag.ar(\offset, 0.401575, n, -1, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\push, 0.818898, n, 0, 2, "vf", 0.01, true),
        OceanodeParameterLag.ar(\feedback_rt, -60.708661, n, -90, 30, "vf", 0.01, true),
        OceanodeParameterLag.ar(\feedback_key_track, 1.007874, n, 0, 2, "vf", 0.01, true),
        OceanodeParameterLag.ar(\release_damp, 10.393701, n, 0, 120, "vf", 0.01, true),
        OceanodeParameterLag.ar(\soft_hard, 0.795276, n, 0, 1, "vf", 0.01, true),
        OceanodeParameterLag.ar(\symmetry, 0, n, 0, 1, "vf", 0.01, true),

        OceanodeParameterLag.ar(\hp0, 0, n, 0, 120, "vf", 0.01, true),
        OceanodeParameterLag.ar(\hp1, 77.480314, n, 0, 120, "vf", 0.01, true),
        OceanodeParameterLag.ar(\hp_key_track, 1.007874, n, 0, 2, "vf", 0.01, true),
        OceanodeParameterLag.ar(\lp0, 120, n, 0, 120, "vf", 0.01, true),
        OceanodeParameterLag.ar(\lp1, 120, n, 0, 120, "vf", 0.01, true),
        OceanodeParameterLag.ar(\lp_key_track, 1.118110, n, -2, 2, "vf", 0.01, true),
        OceanodeParameterLag.ar(\gain, -18, n, -60, 12, "vf", 0.01, true)
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
