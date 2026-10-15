// Supersaw: seven detuned saws spread across the stereo field. Gated.
// Mangle: detune, spread, cutoff, attack / release for swells.
(
SynthDef(\supersaw, { |out = 0, freq = 220, gate = 1, detune = 0.15, spread = 1, cutoff = 3000, attack = 0.5, release = 2, amp = 0.15|
    var offsets = [-0.11, -0.063, -0.019, 0, 0.019, 0.063, 0.11] * detune;
    var saws = Saw.ar(freq * (1 + offsets), LFNoise1.kr(0.3 ! 7).range(0.6, 1));
    var sig = LPF.ar(Splay.ar(saws, spread), cutoff);
    var env = EnvGen.ar(Env.adsr(attack, 0.5, 0.8, release), gate, doneAction: 2);
    Out.ar(out, sig * env * amp * 4);
}, metadata: (specs: (freq: [30, 2000, \exp], detune: [0, 1], spread: [0, 1], cutoff: [200, 16000, \exp],
    attack: [0.001, 8, \exp], release: [0.05, 12, \exp]))).add;
)
