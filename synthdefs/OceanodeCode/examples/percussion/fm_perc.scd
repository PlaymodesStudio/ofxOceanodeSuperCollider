// FM percussion: tom, cowbell, woodblock, gong... depending on ratio and index. One-shot.
// Mangle: ratio (non-integer = metallic), index, the two decays.
(
SynthDef(\fmPerc, { |out = 0, freq = 160, ratio = 1.41, index = 6, decay = 0.4, modDecay = 0.15, amp = 0.4, pan = 0|
    var modEnv = EnvGen.ar(Env.perc(0, modDecay, curve: -4));
    var mod = SinOsc.ar(freq * ratio) * freq * index * modEnv;
    var sig = SinOsc.ar(freq + mod) * EnvGen.ar(Env.perc(0.001, decay, curve: -4), doneAction: 2);
    Out.ar(out, Pan2.ar(sig * amp, pan));
}, metadata: (specs: (freq: [30, 2000, \exp], ratio: [0.25, 8, \exp], index: [0, 20], decay: [0.02, 3, \exp], modDecay: [0.005, 2, \exp]))).add;
)
