// Granular sine cloud. Persistent (never frees itself): one synth, always on.
// Mangle: density, freq + scatter (pitch spread in octaves), grainDur.
(
SynthDef(\grainCloud, { |out = 0, density = 30, freq = 600, scatter = 1, grainDur = 0.08, pan = 0.8, amp = 0.2|
    var trig = Dust.ar(density);
    var f = freq * (2 ** TRand.ar(scatter.neg, scatter, trig));
    var sig = GrainSin.ar(2, trig, grainDur, f, TRand.ar(pan.neg, pan, trig));
    Out.ar(out, sig * amp);
}, metadata: (specs: (density: [1, 400, \exp], freq: [60, 6000, \exp], scatter: [0, 3], grainDur: [0.005, 0.5, \exp], pan: [0, 1]))).add;
)
