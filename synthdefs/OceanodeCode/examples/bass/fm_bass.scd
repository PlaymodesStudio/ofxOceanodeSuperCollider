// Punchy FM bass: modulation index falls fast, so the attack barks and the tail is round. Gated.
// Mangle: index, ratio (try 0.5, 1, 2, 3.5), punch (how fast the index falls).
(
SynthDef(\fmBass, { |out = 0, freq = 55, gate = 1, ratio = 1, index = 4, punch = 0.12, amp = 0.4, pan = 0|
    var iEnv = EnvGen.ar(Env.perc(0, punch, 1, -4)) * index + 0.3;
    var sig = SinOsc.ar(freq + (SinOsc.ar(freq * ratio) * freq * iEnv));
    var env = EnvGen.ar(Env.adsr(0.003, 0.2, 0.7, 0.15), gate, doneAction: 2);
    Out.ar(out, Pan2.ar(sig * env * amp, pan));
}, metadata: (specs: (freq: [25, 400, \exp], ratio: [0.25, 8, \exp], index: [0, 15], punch: [0.01, 1, \exp]))).add;
)
