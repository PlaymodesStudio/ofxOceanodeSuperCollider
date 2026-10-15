// DX7-style electric piano: two FM pairs, the second one a tine. Gated: Gate on/off holds and releases.
// Mangle: index (bark), tine (bell amount), release.
(
SynthDef(\fmEpiano, { |out = 0, freq = 261.6, gate = 1, index = 2.5, tine = 0.3, decay = 2.5, release = 0.4, amp = 0.3, pan = 0|
    var env = EnvGen.ar(Env.adsr(0.002, decay, 0.2, release, curve: -4), gate, doneAction: 2);
    var modEnv = EnvGen.ar(Env.perc(0.001, decay * 0.4));
    var body = SinOsc.ar(freq + (SinOsc.ar(freq) * freq * index * modEnv));
    var bell = SinOsc.ar(freq * 2 + (SinOsc.ar(freq * 14) * freq * 3 * EnvGen.ar(Env.perc(0, 0.2)))) * tine;
    Out.ar(out, Pan2.ar((body + bell) * env * amp, pan));
}, metadata: (specs: (freq: [30, 4000, \exp], index: [0, 10], tine: [0, 1], decay: [0.1, 8, \exp], release: [0.01, 4, \exp]))).add;
)
