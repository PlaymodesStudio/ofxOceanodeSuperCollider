(
~synthCreator.value("FeedbackNode", {|n|
    var input, gain;

    gain = OceanodeParameterLag.ar(\gain, 1, n, 0, 1, "vf", 1/30, true);
    input = InFeedback.ar(OceanodeInput.kr(\in), n);

    Out.ar(OceanodeOutput.kr(\out), input * gain);
}, description: "Explicit block-delayed bridge for modular feedback loops between SynthDefs", category: "Utilities/Feedback");
)
