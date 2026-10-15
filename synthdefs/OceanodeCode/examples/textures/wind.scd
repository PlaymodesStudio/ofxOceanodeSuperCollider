// Wind: noise through a resonant band-pass that gusts around. Persistent.
// Mangle: center, gust (how far it moves), speed, whistle (resonance).
(
SynthDef(\wind, { |out = 0, center = 700, gust = 0.7, speed = 0.15, whistle = 0.3, amp = 0.3|
    var move = LFNoise2.kr(speed ! 2).exprange(1 - (gust * 0.8), 1 + (gust * 3));
    var sig = BPF.ar(PinkNoise.ar ! 2, (center * move).clip(40, 16000), whistle.linexp(0, 1, 1, 0.02));
    sig = sig * LFNoise2.kr(speed * 2 ! 2).range(0.3, 1);
    Out.ar(out, sig * amp * 9);
}, metadata: (specs: (center: [80, 6000, \exp], gust: [0, 1], speed: [0.01, 4, \exp], whistle: [0, 1]))).add;
)
