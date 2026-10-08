(
~synthCreator.value("RhythmicMute", {|n|
    var signal, phasor, reset, threshold, freq;
    reset = OceanodeParameter.kr(\reset, 0, n, 0, 1, "vi");
    threshold = OceanodeParameterLag.kr(\pw, 0.5, n, 0, 1, "vf", 1/30, true);
    freq = OceanodeParameterLag.kr(\hz, 1, n, 0, 440, "vf", 1/30, true);
    signal = In.ar(OceanodeInput.kr(\in), n);
    phasor = Phasor.ar(reset, freq / SampleRate.ir, 0, 1, 0);
    Out.ar(OceanodeOutput.kr(\out), signal * (phasor <= threshold));
}, description: "Per-channel rhythmic mute", category: "Compatibility/OceanAudio");
)
