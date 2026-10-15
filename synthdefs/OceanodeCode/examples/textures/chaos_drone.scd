// Chaotic drone: oscillators cross-modulating each other's frequency. Persistent.
// Mangle: freq, chaos (cross-mod depth - from calm beating to noise), cutoff.
(
SynthDef(\chaosDrone, { |out = 0, freq = 55, chaos = 0.3, cutoff = 1500, amp = 0.15|
    var fb = LocalIn.ar(2);
    var a = SinOsc.ar(freq * (1 + (fb[1] * chaos)));
    var b = SinOsc.ar(freq * 1.503 * (1 + (fb[0] * chaos)));
    var sig = LPF.ar([a + (b * 0.5), b + (a * 0.5)], cutoff);
    LocalOut.ar([a, b]);
    Out.ar(out, LeakDC.ar(sig).tanh * amp);
}, metadata: (specs: (freq: [20, 500, \exp], chaos: [0, 3], cutoff: [100, 12000, \exp]))).add;
)
