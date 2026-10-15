// Metallic hi-hat from six detuned square waves (the 808 recipe). One-shot.
// Mangle: decay (closed 0.05 .. open 0.8), pitch (scales every partial), tone (HPF).
(
SynthDef(\hihat, { |out = 0, decay = 0.08, pitch = 1, tone = 7000, amp = 0.25, pan = 0|
    var ratios = [205.3, 304.4, 369.6, 522.7, 540, 800] * pitch;
    var metal = Pulse.ar(ratios).sum / 6;
    var sig = HPF.ar(BPF.ar(metal, 10000, 1), tone) * EnvGen.ar(Env.perc(0.001, decay, curve: -6), doneAction: 2);
    Out.ar(out, Pan2.ar(sig * amp * 24, pan));
}, metadata: (specs: (decay: [0.02, 1.2, \exp], pitch: [0.25, 4, \exp], tone: [2000, 14000, \exp]))).add;
)
