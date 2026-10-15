// Acid bass, 303-style: saw/square into a resonant low-pass with an envelope. Gated.
// Mangle: cutoff, res, envAmt (the squelch), wave (0 saw .. 1 square), slide (portamento).
(
SynthDef(\acid, { |out = 0, freq = 55, gate = 1, wave = 0, cutoff = 300, res = 0.15, envAmt = 2500, decay = 0.25, slide = 0.04, amp = 0.35, pan = 0|
    var f = Lag.kr(freq, slide);
    var osc = XFade2.ar(Saw.ar(f), Pulse.ar(f, 0.5), wave * 2 - 1);
    var fenv = EnvGen.ar(Env.perc(0.002, decay, curve: -4));
    var env = EnvGen.ar(Env.adsr(0.002, 0.1, 0.8, 0.05), gate, doneAction: 2);
    var sig = RLPF.ar(osc, (cutoff + (fenv * envAmt)).clip(30, 18000), res);
    Out.ar(out, Pan2.ar((sig * 1.5).tanh * env * amp, pan));
}, metadata: (specs: (freq: [25, 400, \exp], wave: [0, 1], cutoff: [40, 4000, \exp], res: [0.03, 1, \exp],
    envAmt: [0, 8000], decay: [0.02, 2, \exp], slide: [0, 0.5]))).add;
)
