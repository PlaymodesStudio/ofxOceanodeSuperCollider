// Drawbar organ: sine partials at the classic footages, a bit of key click and vibrato. Gated.
// Mangle: the drawbars array (9 values 0..1, send a vector), vibrato.
(
SynthDef(\organ, { |out = 0, freq = 220, gate = 1, vibrato = 0.004, click = 0.2, amp = 0.2, pan = 0,
    drawbars = #[0.8, 0.8, 1, 0, 0.6, 0, 0.3, 0, 0.2]|
    var footages = [0.5, 1.5, 1, 2, 3, 4, 5, 6, 8];
    var vib = SinOsc.kr(6.5, 0, vibrato, 1);
    var sig = (SinOsc.ar(freq * footages * vib) * drawbars).sum;
    var env = EnvGen.ar(Env.asr(0.005, 1, 0.05), gate, doneAction: 2);
    sig = sig + (HPF.ar(WhiteNoise.ar, 2000) * EnvGen.ar(Env.perc(0, 0.01)) * click);
    Out.ar(out, Pan2.ar(sig * env * amp, pan));
}, metadata: (specs: (freq: [30, 4000, \exp], vibrato: [0, 0.03], click: [0, 1], drawbars: [0, 1]))).add;
)
