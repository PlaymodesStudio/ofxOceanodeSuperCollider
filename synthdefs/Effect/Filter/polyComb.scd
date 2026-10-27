(
~synthCreator.value("PolyComb", {|n|
    var input, pitch, transpose, detune, glide, vibrate, vibrand, vibamt;
    var feedback, satlevel, send, mix, note, rate, vibrato, wet;

    // Standalone version of PolyCombBox's PCCombR resonator (OceanodeAnalog).
    // Pitch is absolute MIDI here; cables replace the box's internal note and
    // modulation buses through the standard Oceanode audio parameter inputs.
    pitch = OceanodeParameter.ar(\pitch, 60, n, 0, 135, "vf");
    transpose = OceanodeParameter.ar(\transpose, 0, n, -48, 48, "vf");
    detune = OceanodeParameterLag.ar(\detune, 0, n, 0, 1, "vf", 1/30, true);
    glide = OceanodeParameter.kr(\glide, 0, n, 0, 2, "vf");
    vibrate = OceanodeParameterLag.ar(\vibrate, 5, n, 0, 20, "vf", 0.03, true);
    vibrand = OceanodeParameterLag.ar(\vibrand, 0, n, 0, 1, "vf", 0.03, true);
    vibamt = OceanodeParameterLag.ar(\vibamt, 0, n, 0, 2, "vf", 0.03, true);
    feedback = OceanodeParameter.ar(\feedback, 0.7, n, -1.2, 1.2, "vf");
    satlevel = OceanodeParameterLag.ar(\satlevel, 12, n, 0, 48, "vf", 0.03, true);
    send = OceanodeParameterLag.ar(\send, 1, n, 0, 2, "vf", 1/30, true);
    mix = OceanodeParameter.ar(\mix, 1, n, 0, 1, "vf");

    input = In.ar(OceanodeInput.kr(\in), n);
    note = Lag.ar(pitch + transpose, glide.max(0.02)) + (({ Rand(-1, 1) } ! n) * detune);
    rate = vibrate * (1 + (({ Rand(-1, 1) } ! n) * vibrand.clip(0, 1)));
    vibrato = SinOsc.ar(rate, { Rand(0, 2pi) } ! n) * vibamt;
    // Mix belongs inside ReaktorComb, before its output saturator. Feedback
    // can exceed unity: the loop's parabolic saturator limits the resonance.
    wet = Sanitize.ar(ReaktorComb.ar(input * send,
        (note + vibrato).clip(0, 135).midicps.clip(20, 20000),
        feedback.clip(-1.2, 1.2), satlevel.clip(0, 48).dbamp, mix.clip(0, 1), 1));
    Out.ar(OceanodeOutput.kr(\out), wet);
}, description: "PolyCombBox comb resonator with bipolar feedback and parabolic loop saturation; satlevel in dB", category: "Effect/Filter");
)
