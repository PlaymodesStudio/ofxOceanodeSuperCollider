// Vinyl crackle and dust: sparse clicks with random color, plus a soft hiss. Persistent.
// Mangle: density, hiss, color.
(
SynthDef(\crackle, { |out = 0, density = 8, hiss = 0.05, color = 3000, amp = 0.4|
    var clicks = Dust2.ar(density ! 2) * TExpRand.ar(0.1, 1, Dust.ar(density ! 2));
    var sig = BPF.ar(clicks, color, 1.5) * 4 + (LPF.ar(PinkNoise.ar ! 2, 6000) * hiss);
    Out.ar(out, sig * amp);
}, metadata: (specs: (density: [0.5, 200, \exp], hiss: [0, 0.5], color: [300, 12000, \exp]))).add;
)
