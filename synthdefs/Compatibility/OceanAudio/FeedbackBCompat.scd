(
~synthCreator.value("FeedbackBCompat", {|n|
    var snd, env, delaytime, grainSize, pitchShift;
    env = OceanodeParameterLag.kr(\ingain, 1, 1, 0, 1, "vf", 1/30, true);
    delaytime = OceanodeParameterLag.kr(\delaytime, 0.05, 8, 0.01, 0.5, "vf", 1/30, true);
    grainSize = OceanodeParameterLag.kr(\grainsize, 0.1, 2, 0.01, 1, "vf", 1/30, true);
    pitchShift = OceanodeParameterLag.kr(\pitchshift, 0.5, 2, 0.01, 4, "vf", 1/30, true);
    snd = In.ar(OceanodeInput.kr(\in), 1) * env;
    snd = snd + LocalIn.ar(1);
    snd = DelayC.ar(snd, 0.2, delaytime);
    snd = Splay.ar(snd);
    snd = snd + PitchShift.ar(snd, grainSize, pitchShift);
    snd = LeakDC.ar(snd);
    snd = snd.tanh * env;
    LocalOut.ar(snd.sum);
    Out.ar(OceanodeOutput.kr(\out), snd * -15.dbamp);
}, description: "Original eight-tap stereo feedback effect", category: "Compatibility/OceanAudio");
)
