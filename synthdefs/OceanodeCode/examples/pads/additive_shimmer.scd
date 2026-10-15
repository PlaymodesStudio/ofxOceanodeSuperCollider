// Additive pad: 16 partials, each with its own slow tremolo, odd/even balance and tilt. Gated.
// Mangle: tilt (brightness), odd (0 = only even .. 1 = all), shimmer (tremolo depth), rate.
(
SynthDef(\additiveShimmer, { |out = 0, freq = 110, gate = 1, tilt = 1.2, odd = 0.7, shimmer = 0.5, rate = 0.3, attack = 2, release = 4, amp = 0.2|
    var n = (1..16);
    var amps = (n ** tilt.neg) * n.collect { |k| if(k.odd) { odd } { 1 } };
    var trem = LFNoise2.kr(rate ! 16).range(1 - shimmer, 1);
    var sig = Splay.ar(SinOsc.ar(freq * n, Rand(0, 2pi ! 16)) * amps * trem);
    var env = EnvGen.ar(Env.adsr(attack, 1, 0.8, release), gate, doneAction: 2);
    Out.ar(out, sig * env * amp * 8);
}, metadata: (specs: (freq: [30, 1000, \exp], tilt: [0, 3], odd: [0, 1], shimmer: [0, 1], rate: [0.01, 8, \exp],
    attack: [0.01, 10, \exp], release: [0.1, 15, \exp]))).add;
)
