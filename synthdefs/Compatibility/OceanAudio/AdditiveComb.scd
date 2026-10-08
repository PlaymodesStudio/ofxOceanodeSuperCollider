(
~synthCreator.value("AdditiveComb", {|n|
    var numPartials = 256;
    var freq, inharmonicity, warpSpectrum, combOffset, combDensity, combPeak, combSkew;
    var ratios, freqs, amps, normalizedFreqs, warpedFreqs, phase, warpedPhase, sig;
    var modFreqs, modPhases, lfos;
    var nyquist, fade, limiter;

    // Base parameters
    freq = OceanodeParameterLag.kr(\pitch, 36, 1, 0, 130, "vf", 1/30, true).midicps;
    inharmonicity = OceanodeParameterLag.kr(\inharmonicity, 0, 1, 0, 0.1, "vf", 1/30, true);
    warpSpectrum = OceanodeParameterLag.kr(\warpspec, 0.5, 1, 0, 1, "vf", 1/30, true);
    combOffset = OceanodeParameterLag.kr(\comboffset, 0, 1, 0, 1, "vf", 1/30, true);
    combDensity = OceanodeParameterLag.kr(\combdensity, 0, 1, 0, 1, "vf", 1/30, true);
    combPeak = OceanodeParameterLag.kr(\combpeak, 5, 1, 1, 10, "vf", 1/30, true);
    combSkew = OceanodeParameterLag.kr(\combskew, 0.5, 1, 0.01, 0.99, "vf", 1/30, true);

    // Generate harmonic series
    ratios = (1..numPartials);
    freqs = freq * ratios * (1 + (inharmonicity * ratios * ratios)).sqrt;

    // Frequency warping
    normalizedFreqs = (freqs / freqs[numPartials - 1]).clip(0, 1);
    warpedFreqs = Select.kr(
        normalizedFreqs > (1 - warpSpectrum),
        [
            0.5 * normalizedFreqs / (1 - warpSpectrum),
            0.5 * (1 + ((normalizedFreqs - (1 - warpSpectrum)) / warpSpectrum))
        ]
    );
    freqs = warpedFreqs * freqs[numPartials - 1];

    // Initialize amplitudes and apply spectral tilt
    amps = 1!numPartials;
    amps = amps * (ratios.log2 * OceanodeParameterLag.kr(\tiltperoctavedb, -3, 1, -12, -3, "vf", 1/30, true)).dbamp;

    // Apply comb filtering
    phase = freqs.log2 - freq.log2;
    phase = (phase * combDensity - combOffset).wrap(0, 1);
    warpedPhase = Select.kr(
        phase > combSkew,
        [
            0.5 * phase / combSkew,
            0.5 * (1 + ((phase - combSkew) / (1 - combSkew)))
        ]
    );
    amps = amps * (
        exp(combPeak.abs * (cos(warpedPhase * 2pi) - 1)) *
        (0.5 * (1 + cos(warpedPhase * 2pi)))
    );

    // Anti-aliasing limiter
    nyquist = SampleRate.ir / 2 - 2000;
    fade = nyquist - 1000;
    limiter = 1 - ((freqs.clip(fade, nyquist) - fade) * 0.001);
    amps = amps * limiter;

    // Generate and sum oscillators
    sig = SinOsc.ar(
        freq: freqs,
        phase: {Rand(0, 2pi)}!numPartials,
        mul: amps
    );

    // Sum odd/even partials with stereo spread
    sig = sig[0,2..].sum + ([-1,1] * sig[1,3..].sum);

    // Output processing
    sig = sig * -15.dbamp;
    sig = sig * OceanodeParameterLag.kr(\amp, -5, 1, -25, -5, "vf", 1/30, true).dbamp;
    sig = Limiter.ar(sig);
    sig = LeakDC.ar(sig);

    Out.ar(OceanodeOutput.kr(\out), sig);
}, description: "Original stereo additive comb synthesizer", category: "Compatibility/OceanAudio");
)
