(
// Two cascaded BPFs per channel, matching the old bandpass78 graph.
~synthCreator.value("BloobsBandpass", {|n|
    var input, pitch, rq, signal;
    input = In.ar(OceanodeInput.kr(\in), n);
    pitch = OceanodeParameterLag.kr(\pitch, 128, n, 0, 130, "vf", 0.05, true).midicps;
    rq = OceanodeParameter.kr(\q, 1, n, 0, 1, "vf");
    signal = BPF.ar(input, pitch, rq);
    signal = BPF.ar(signal, pitch, rq);
    Out.ar(OceanodeOutput.kr(\out), signal);
}, description: "BLOOBS cascaded 78-voice bandpass", category: "Optimized/Bloobs");
)
