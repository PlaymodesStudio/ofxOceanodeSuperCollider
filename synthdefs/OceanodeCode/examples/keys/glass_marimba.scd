// Mallet: decaying modes of a bar (marimba ratios), harder hits are brighter. One-shot.
// Mangle: hardness, ratio set via "bar" (0 marimba .. 1 glassy), decay.
(
SynthDef(\glassMarimba, { |out = 0, freq = 440, hardness = 0.5, bar = 0, decay = 1.5, amp = 0.4, pan = 0|
    var ratios = [1, 3.93, 9.54, 16.4] * (1 - bar) + ([1, 2.76, 5.4, 8.93] * bar);
    var exc = LPF.ar(WhiteNoise.ar, hardness.linexp(0, 1, 800, 14000)) * EnvGen.ar(Env.perc(0, 0.002));
    var sig = Klank.ar(`[ratios, [1, 0.5, 0.25, 0.12], decay * [1, 0.4, 0.2, 0.1]], exc, freq);
    DetectSilence.ar(sig, 0.0001, 0.1, doneAction: 2);
    Out.ar(out, Pan2.ar(sig * amp, pan));
}, metadata: (specs: (freq: [60, 3000, \exp], hardness: [0, 1], bar: [0, 1], decay: [0.1, 6, \exp]))).add;
)
