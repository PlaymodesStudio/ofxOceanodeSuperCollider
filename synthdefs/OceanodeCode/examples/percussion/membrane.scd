// Struck membrane / tabla-ish drum: a noise strike rings a bank of resonators. One-shot.
// Mangle: freq, inharm (spreads the modes), ring (how long they sing), strike (brightness).
(
SynthDef(\membrane, { |out = 0, freq = 120, inharm = 1, ring = 0.8, strike = 3000, amp = 0.4, pan = 0|
    var modes = [1, 1.59, 2.14, 2.3, 2.65, 2.92] ** inharm;
    var exc = LPF.ar(WhiteNoise.ar, strike) * EnvGen.ar(Env.perc(0.0005, 0.006));
    var sig = Klank.ar(`[modes, [1, 0.6, 0.45, 0.35, 0.3, 0.2], ring * [1, 0.7, 0.5, 0.45, 0.35, 0.3]], exc, freq);
    DetectSilence.ar(sig, 0.0001, 0.1, doneAction: 2);
    Out.ar(out, Pan2.ar(sig * amp * 0.3, pan));
}, metadata: (specs: (freq: [40, 800, \exp], inharm: [0.5, 2], ring: [0.05, 4, \exp], strike: [300, 12000, \exp]))).add;
)
