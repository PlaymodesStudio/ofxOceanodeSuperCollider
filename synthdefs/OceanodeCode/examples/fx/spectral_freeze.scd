// Spectral freeze: hold the current spectrum while freeze > 0.5, smeared by blur. Persistent effect.
// Mangle: freeze (gate it from a sequencer), blur, mix.
(
SynthDef(\spectralFreeze, { |in = 0, out = 0, freeze = 0, blur = 0.3, mix = 1|
    var dry = In.ar(in, 2);
    var wet = dry.collect { |ch|
        var chain = FFT(LocalBuf(2048), ch);
        chain = PV_MagFreeze(chain, freeze > 0.5);
        chain = PV_MagSmear(chain, (blur * 30).round);
        IFFT(chain)
    };
    Out.ar(out, XFade2.ar(dry, wet, mix * 2 - 1));
}, metadata: (specs: (freeze: [0, 1], blur: [0, 1], mix: [0, 1]))).add;
)
