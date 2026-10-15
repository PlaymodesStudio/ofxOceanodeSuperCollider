// Effect: reads the In port, writes the Out port, never frees itself,
// so it runs as one persistent synth.
(
SynthDef(\pingPong, { |in = 0, out = 0, time = 0.3, feedback = 0.5, mix = 0.35, damp = 4000|
    var dry = In.ar(in, 2);
    var fb = LocalIn.ar(2);
    var wet = DelayC.ar(dry + LPF.ar(fb.reverse * feedback, damp), 2, time);
    LocalOut.ar(wet);
    Out.ar(out, XFade2.ar(dry, wet, mix * 2 - 1));
}, metadata: (specs: (time: [0.01, 2, \exp], feedback: [0, 0.95], mix: [0, 1], damp: [200, 16000, \exp]))).add;
)
