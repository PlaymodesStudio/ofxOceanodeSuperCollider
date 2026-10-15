// 808-style kick. One-shot: every rising Gate lane fires one.
// Mangle: sweep (pitch drop depth), punch (click), drive (tanh saturation).
(
SynthDef(\kick808, { |out = 0, freq = 48, decay = 0.6, sweep = 3, sweepTime = 0.05, punch = 0.3, drive = 1.5, amp = 0.6, pan = 0|
    var pitch = freq * (1 + (sweep * EnvGen.ar(Env.perc(0, sweepTime, curve: -6))));
    var body = SinOsc.ar(pitch, 0.5pi) * EnvGen.ar(Env.perc(0.001, decay, curve: -4), doneAction: 2);
    var click = HPF.ar(WhiteNoise.ar, 3000) * EnvGen.ar(Env.perc(0, 0.004)) * punch;
    var sig = ((body + click) * drive).tanh * amp;
    Out.ar(out, Pan2.ar(sig, pan));
}, metadata: (specs: (freq: [25, 120, \exp], decay: [0.05, 3, \exp], sweep: [0, 10], sweepTime: [0.005, 0.3, \exp],
    punch: [0, 1], drive: [1, 10, \exp]))).add;
)
