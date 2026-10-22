(
~synthCreator.value("EventADSR", {|n|
    var trigger, gate, attack, decay, sustain, release, peak, sig;
    trigger = OceanodeParameter.ar(\trigger, 0, n, 0, 1, "vi");
    gate = OceanodeParameter.ar(\gate, 1, n, 0, 1, "vi");
    attack = OceanodeParameter.ar(\attack, 1, n, 0.01, 100000, "vf");
    decay = OceanodeParameter.ar(\decay, 100, n, 0.01, 100000, "vf");
    sustain = OceanodeParameter.ar(\sustain, 0, n, 0, 1, "vf");
    release = OceanodeParameter.ar(\release, 1, n, 0.01, 100000, "vf");
    peak = OceanodeParameter.ar(\peak, 1, n, 0, 1, "vf");
    sig = EventADSRKernel.ar(trigger, gate, attack.max(0.01), decay.max(0.01), sustain, release.max(0.01), peak);
    Out.ar(OceanodeOutput.kr(\out), sig);
}, description: "Event-retriggered ADSR; linear attack and exponential time constants in milliseconds", category: "Modulation/Envelope");
)
