(
~synthCreator.value("FilteredFeedbackDelay", {|n|
    var time, feedback, pitch, res, type, mix, sig;
    time = OceanodeParameter.ar(\time, 8, n, 0, 2000, "vf");
    feedback = OceanodeParameter.ar(\feedback, 0.874, n, -0.99, 0.99, "vf");
    pitch = OceanodeParameter.ar(\cutp, 82.2, n, 0, 127, "vf");
    res = OceanodeParameter.ar(\res, 0.14, n, 0, 0.98, "vf");
    type = OceanodeParameter.ar(\type, 0, n, 0, 2, "vi");
    mix = OceanodeParameter.ar(\mix, 1, n, 0, 1, "vf");
    sig = FilteredFeedbackDelay.ar(In.ar(OceanodeInput.kr(\in), n), time, feedback, pitch.midicps, res, type, mix);
    Out.ar(OceanodeOutput.kr(\out), sig);
}, description: "Four point Core microdelay with sample feedback SVF and parabolic crossfade", category: "Effect/Time/Delay");
)
