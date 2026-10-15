// Ring modulator with an LFO on the carrier: from tremolo (low freq) to metallic (high). Persistent effect.
// Mangle: freq, lfoDepth, lfoRate, mix.
(
SynthDef(\ringMod, { |in = 0, out = 0, freq = 440, lfoDepth = 0, lfoRate = 0.2, mix = 1|
    var dry = In.ar(in, 2);
    var carrier = SinOsc.ar(freq * (1 + SinOsc.kr(lfoRate, [0, 0.5pi], lfoDepth)));
    Out.ar(out, XFade2.ar(dry, dry * carrier, mix * 2 - 1));
}, metadata: (specs: (freq: [0.5, 4000, \exp], lfoDepth: [0, 1], lfoRate: [0.01, 20, \exp], mix: [0, 1]))).add;
)
