// Formant "aah": a buzzy source through three vowel formants that drift between vowels. Gated.
// Mangle: vowel (0 a, 0.5 e, 1 i ... interpolated), breath, drift.
(
SynthDef(\choir, { |out = 0, freq = 220, gate = 1, vowel = 0, breath = 0.15, drift = 0.3, attack = 0.8, release = 2, amp = 0.25, pan = 0|
    var v = (vowel + LFNoise1.kr(0.2, drift * 0.2)).clip(0, 1);
    var f1 = v.linlin(0, 1, 800, 270), f2 = v.linlin(0, 1, 1150, 2300), f3 = v.linlin(0, 1, 2900, 3000);
    var src = Saw.ar(freq * LFNoise2.kr(4 ! 3, 0.004, 1)).sum / 3 + (PinkNoise.ar * breath);
    var sig = (BPF.ar(src, f1, 0.1) * 1.0) + (BPF.ar(src, f2, 0.08) * 0.6) + (BPF.ar(src, f3, 0.07) * 0.3);
    var env = EnvGen.ar(Env.adsr(attack, 0.3, 0.9, release), gate, doneAction: 2);
    Out.ar(out, Pan2.ar(sig * env * amp * 30, pan));
}, metadata: (specs: (freq: [60, 1000, \exp], vowel: [0, 1], breath: [0, 1], drift: [0, 1],
    attack: [0.01, 6, \exp], release: [0.1, 10, \exp]))).add;
)
