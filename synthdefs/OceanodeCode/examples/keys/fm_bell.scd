// One-shot FM bell: it frees itself, so the node plays it as voices.
// Each rising Gate lane rings one bell with that lane's values.
(
SynthDef(\fmBell, { |out = 0, freq = 440, amp = 0.4, index = 3, ratio = 1.4, decay = 3, pan = 0|
    var env = EnvGen.ar(Env.perc(0.002, decay, curve: -6), doneAction: 2);
    var mod = SinOsc.ar(freq * ratio) * freq * index * env;
    var sig = SinOsc.ar(freq + mod) * env * amp;
    Out.ar(out, Pan2.ar(sig, pan));
}, metadata: (specs: (index: [0, 12], ratio: [0.25, 8, \exp], decay: [0.1, 10, \exp]))).add;
)
