// Hand clap: a few fast noise bursts then a tail. One-shot.
// Mangle: spread (time between bursts), tail, color.
(
SynthDef(\clap, { |out = 0, spread = 0.011, tail = 0.25, color = 1400, amp = 0.5, pan = 0|
    var bursts = EnvGen.ar(Env([0, 1, 0, 1, 0, 1, 0, 0.9, 0], [0, spread, 0, spread, 0, spread, 0, tail], -6), doneAction: 2);
    var sig = BPF.ar(WhiteNoise.ar, color * [1, 2.1], 0.6).sum * bursts;
    Out.ar(out, Pan2.ar((sig * amp * 2).tanh, pan));
}, metadata: (specs: (spread: [0.003, 0.04, \exp], tail: [0.05, 1.5, \exp], color: [300, 6000, \exp]))).add;
)
