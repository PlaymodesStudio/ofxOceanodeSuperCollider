(
~synthCreator.value("CompensatedSVF", {|n|
    var pitch, res, type, sig;
    pitch = OceanodeParameter.ar(\pitch, 84, n, -60, 140, "vf");
    res = OceanodeParameter.ar(\res, 0, n, 0, 1, "vf");
    type = OceanodeParameter.ar(\type, 2, n, 0, 2, "vi");
    sig = CompensatedSVF.ar(In.ar(OceanodeInput.kr(\in), n), pitch.midicps, res, type);
    Out.ar(OceanodeOutput.kr(\out), sig);
}, description: "Reaktor Core compensated two pole state variable filter; type HP BP LP", category: "Effect/Filter");
)
