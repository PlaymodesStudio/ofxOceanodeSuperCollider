(
~synthCreator.value("CongruentialRandom", {|n|
    var clock, seed, reset, mode, sig;
    clock = OceanodeParameter.ar(\trigger, 0, n, 0, 1, "vi");
    seed = OceanodeParameter.ar(\seed, 0, n, -1000000, 1000000, "vf");
    reset = OceanodeParameter.ar(\reset, 0, n, 0, 1, "vi");
    mode = OceanodeParameterDropdown.kr(\mode, 0, n, "Triggered:Sample:Seeded Sample");
    sig = CongruentialRandomKernel.ar(clock, seed, reset, mode);
    Out.ar(OceanodeOutput.kr(\out), sig);
}, description: "Bipolar 32-bit linear congruential random; triggered, sample-rate, or continuously seeded", category: "Modulation/Random");
)
