// Comb resonator bank tuned to a chord: anything you feed in rings at those pitches. Persistent effect.
// Mangle: root, the chord array (ratios, send a vector), decay, mix.
(
SynthDef(\resonator, { |in = 0, out = 0, root = 110, decay = 2, mix = 0.5, chord = #[1, 1.5, 2, 2.5]|
    var dry = In.ar(in, 2);
    var wet = Mix(CombC.ar(dry, 0.05, (root * chord).reciprocal, decay)) * 0.15;
    Out.ar(out, XFade2.ar(dry, LeakDC.ar(wet).tanh, mix * 2 - 1));
}, metadata: (specs: (root: [30, 1000, \exp], decay: [0.05, 15, \exp], mix: [0, 1], chord: [0.5, 8]))).add;
)
