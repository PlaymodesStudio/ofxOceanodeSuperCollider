// Tape delay: wobbling delay time, saturation and darkening on every repeat. Persistent effect.
// Mangle: time, feedback (above 0.9 it runs away, tanh keeps it bounded), wobble, tone.
(
SynthDef(\tapeDelay, { |in = 0, out = 0, time = 0.375, feedback = 0.55, wobble = 0.003, tone = 3000, mix = 0.4|
    var dry = In.ar(in, 2);
    var fb = LocalIn.ar(2);
    var t = (time + LFNoise2.kr(0.5 ! 2, wobble) + SinOsc.kr(0.3, 0, wobble * 0.5)).clip(0.01, 2);
    var wet = DelayC.ar(dry + fb, 2, t - ControlDur.ir);
    wet = LPF.ar(HPF.ar((wet * 1.2).tanh, 80), tone);
    LocalOut.ar(wet * feedback);
    Out.ar(out, XFade2.ar(dry, wet, mix * 2 - 1));
}, metadata: (specs: (time: [0.02, 2, \exp], feedback: [0, 1.1], wobble: [0, 0.02], tone: [500, 12000, \exp], mix: [0, 1]))).add;
)
