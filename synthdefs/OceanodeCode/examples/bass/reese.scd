// Reese bass: detuned saws beating against each other, low-passed. Gated.
// Mangle: detune (beating speed), cutoff, sub (sine an octave down).
(
SynthDef(\reese, { |out = 0, freq = 55, gate = 1, detune = 0.012, cutoff = 900, sub = 0.5, amp = 0.3, pan = 0|
    var saws = Saw.ar(freq * [1 - detune, 1, 1 + detune]).sum / 3;
    var sig = LPF.ar(saws, cutoff) + (SinOsc.ar(freq * 0.5) * sub);
    var env = EnvGen.ar(Env.adsr(0.01, 0.2, 0.9, 0.2), gate, doneAction: 2);
    Out.ar(out, Pan2.ar(sig.tanh * env * amp * 2, pan));
}, metadata: (specs: (freq: [25, 400, \exp], detune: [0, 0.05], cutoff: [60, 8000, \exp], sub: [0, 1]))).add;
)
