(
~synthCreator.value("RandomBeatClock", {|n|
    var bpm, change, randomize, run, division, sig;
    bpm = OceanodeParameter.ar(\bpm, 100, n, 10, 400, "vf");
    change = OceanodeParameter.ar(\change, 8, n, 1, 32, "vf");
    randomize = OceanodeParameter.ar(\randomize, 1, n, 0, 1, "vi");
    run = OceanodeParameter.ar(\run, 1, n, 0, 1, "vi");
    division = OceanodeParameter.ar(\division, 1, n, 1, 16, "vf");
    // Random integer beat durations, with an optional fixed duration.
    // Reject the zero quantization bin by sampling the nonzero interval.
    sig = n.collect { |i|
        var divisor = (Dwhite(0.0, 1.0, inf) * Dseq([(change[i] - 0.5).max(0.5)], inf)) + 0.5;
        var beats = Dswitch1([Dseq([change[i]], inf), divisor.round(1).max(1)], Dseq([randomize[i]], inf));
        TDuty.ar((beats * Dseq([60 / (bpm[i].max(1) * division[i].max(1))], inf)).max(SampleDur.ir), 0, 1) * run[i]
    };
    Out.ar(OceanodeOutput.kr(\out), sig);
}, description: "Beat clock with random or fixed integer beat intervals", category: "Modulation/Trigger");
)
