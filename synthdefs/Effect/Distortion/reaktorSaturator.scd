(
~synthCreator.value("ReaktorSaturator", {|n|
    var input = In.ar(OceanodeInput.kr(\in), n);
    // NI mono::AudioSaturator: parabolic saturation, unity slope at zero,
    // zero slope at +/-4, output limited to +/-2.
    var x = input.clip(-4, 4);
    Out.ar(OceanodeOutput.kr(\out), x * (1 - (x.abs * 0.125)));
}, description: "Reaktor parabolic saturator with a ceiling of plus or minus 2", category: "Effect/Distortion");
)
