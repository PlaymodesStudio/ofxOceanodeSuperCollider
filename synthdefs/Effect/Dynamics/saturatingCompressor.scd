(
// NI Core Compressor (Stereo Linked): Knee -> Follower -> Poly Sat 31 -> gain.
// The automatic makeup term, full-width knee, gain-dependent saturation ceiling,
// and dB follower differ materially from CompressorPro. Kept as one dynamics node.
~synthCreator.value("SaturatingCompressor", {|n|
    var input, threshold, ratio, knee, attack, release, drive, makeup, mix;
    var level, over, c, kneeout, envelope, gain, ceiling, x, saturated, wet;
    input = In.ar(OceanodeInput.kr(\in), n).asArray;
    threshold = OceanodeParameter.ar(\threshold, -38, 1, -60, 0, "vf");
    ratio = OceanodeParameter.ar(\ratio, 0.7416667, 1, 0, 1, "vf");
    knee = OceanodeParameter.ar(\knee, 27, 1, 0.001, 48, "vf").max(0.001);
    attack = OceanodeParameter.ar(\attack, 1, 1, 0.1, 10000, "vf");
    release = OceanodeParameter.ar(\release, 354.8134, 1, 1, 100000, "vf");
    drive = OceanodeParameter.ar(\drive, -6, 1, -6, 24, "vf");
    makeup = OceanodeParameter.ar(\makeup, 11.2, 1, -24, 24, "vf");
    mix = OceanodeParameter.ar(\mix, 1, 1, 0, 1, "vf");
    level = input.abs.reduce(\max).max(0.000001).ampdb;
    over = (level - threshold + knee).max(0) / knee;
    c = over.min(2) * 0.25;
    kneeout = (over - (4 * (c - c.squared))) * knee;
    envelope = AsymmetricFollower.ar(kneeout, attack.max(0.001) * 0.001, release.max(0.001) * 0.001);
    gain = ((ratio.neg * envelope) + (threshold * ratio * -0.666000009) + makeup).dbamp;
    ceiling = (envelope + threshold + drive).dbamp.max(0.000001);
    x = input.clip(ceiling * -1.5, ceiling * 1.5);
    saturated = x - (0.148149997 * x.cubed / ceiling.squared);
    wet = saturated * gain;
    Out.ar(OceanodeOutput.kr(\out), input + ((wet - input) * mix));
}, description: "Linked compressor with full-width soft knee, automatic makeup and polynomial saturation", category: "Effect/Dynamics");
)
