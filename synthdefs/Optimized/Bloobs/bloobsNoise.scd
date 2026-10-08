(
// The four legacy 78-channel noise choices, with a variable channel count.
// Control-rate parameters avoid running audio-rate selector plumbing for
// every voice when BLOOBS is driving them from Oceanode control nodes.
~synthCreator.value("BloobsNoise", {|n|
    var density, type, levels, waves, signal;
    density = OceanodeParameter.kr(\density, 1, n, 0, 1, "vf");
    type = OceanodeParameterDropdown.kr(\type, 0, n, "White:Pink:Crackle:Dust");
    levels = OceanodeParameterLag.kr(\levels, 0, n, 0, 1, "vf", 1/30, true);

    waves = [
        WhiteNoise.ar(1 ! n, 0),
        PinkNoise.ar(1 ! n, 0),
        Crackle.ar(density * 2, 1, 0),
        Dust2.ar(density * 8, 1, 0) * 2
    ];
    signal = Select.ar(type, waves) * levels;
    Out.ar(OceanodeOutput.kr(\out), signal);
}, description: "BLOOBS four-choice noise source", category: "Optimized/Bloobs");
)
