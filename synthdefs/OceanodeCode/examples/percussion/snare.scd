// Snare: tuned body + filtered noise. One-shot.
// Mangle: tone (body pitch), snappy (noise amount), noiseDecay vs bodyDecay.
(
SynthDef(\snare, { |out = 0, tone = 185, bodyDecay = 0.12, noiseDecay = 0.22, snappy = 0.7, color = 4500, amp = 0.4, pan = 0|
    var body = SinOsc.ar(tone * [1, 1.62]).sum * 0.5 * EnvGen.ar(Env.perc(0.001, bodyDecay));
    var noise = BPF.ar(WhiteNoise.ar, color, 0.9) * 2 * EnvGen.ar(Env.perc(0.001, noiseDecay, curve: -5));
    var sig = (body * (1 - snappy) + (noise * snappy)) * amp;
    DetectSilence.ar(sig, 0.0001, 0.05, doneAction: 2);
    Out.ar(out, Pan2.ar(sig.tanh, pan));
}, metadata: (specs: (tone: [80, 400, \exp], bodyDecay: [0.02, 0.6, \exp], noiseDecay: [0.02, 1, \exp],
    snappy: [0, 1], color: [800, 12000, \exp]))).add;
)
