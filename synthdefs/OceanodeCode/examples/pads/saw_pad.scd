// Gated pad: Gate on starts a voice, Gate off releases it (Env.adsr, doneAction: 2).
(
SynthDef(\sawPad, { |out = 0, freq = 220, gate = 1, amp = 0.3, cutoff = 1800, detune = 0.006, attack = 0.4, release = 2.5|
    var env = EnvGen.kr(Env.adsr(attack, 0.3, 0.8, release), gate, doneAction: 2);
    var sig = Saw.ar(freq * (1 + (detune * [-1, 0, 1]))).sum / 3;
    sig = RLPF.ar(sig, cutoff * (env * 0.5 + 0.5), 0.35);
    Out.ar(out, (sig * env * amp) ! 2);
}).add;
)
