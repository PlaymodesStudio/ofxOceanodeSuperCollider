// Karplus-Strong string (Pluck UGen: no LocalIn block delay, so it stays in tune). One-shot.
// Mangle: decay, damp (0 = bright steel, 1 = nylon), pick (exciter brightness).
(
SynthDef(\pluckKS, { |out = 0, freq = 220, decay = 3, damp = 0.4, pick = 6000, amp = 0.4, pan = 0|
    var exc = LPF.ar(WhiteNoise.ar, pick);
    var sig = Pluck.ar(exc, Impulse.ar(0), 0.05, freq.reciprocal, decay, damp);
    DetectSilence.ar(sig, 0.0001, 0.1, doneAction: 2);
    Out.ar(out, Pan2.ar(LeakDC.ar(sig) * amp * 1.8, pan));
}, metadata: (specs: (freq: [30, 2000, \exp], decay: [0.1, 15, \exp], damp: [0, 0.99], pick: [500, 16000, \exp]))).add;
)
