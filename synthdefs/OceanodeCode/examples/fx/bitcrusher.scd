// Bitcrusher: sample-rate reduction and bit-depth reduction, core UGens only. Persistent effect.
// Mangle: rate (Hz of the held samples), bits, mix.
(
SynthDef(\bitcrusher, { |in = 0, out = 0, rate = 8000, bits = 8, mix = 1|
    var dry = In.ar(in, 2);
    var steps = 2 ** bits;
    var wet = Latch.ar(dry, Impulse.ar(rate));
    wet = (wet * steps).round / steps;
    Out.ar(out, XFade2.ar(dry, wet, mix * 2 - 1));
}, metadata: (specs: (rate: [200, 44100, \exp], bits: [1, 16], mix: [0, 1]))).add;
)
