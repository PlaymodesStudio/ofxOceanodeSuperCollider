// fftanalyzer.scd - 128-band log-frequency FFT spectrum analyzer
//
// Uses a 4096-point FFT (2048 usable bins, bin width ≈ 10.8 Hz).
// Computes 128 log-spaced display bands at SynthDef BUILD TIME.
// Each display band takes the max magnitude across all FFT bins in its range,
// so a pure sine always shows as a single clean spike.
// Output is normalized: 0 dBFS ≈ 1.0.

var d, fftDir;

if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "/CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "/SuperCollider/CompiledSynthdefs";
};

fftDir = d +/+ "/fftanalyzer";
File.mkdir(d);
File.mkdir(fftDir);

// All build-time constants at top level (no wrapping parens needed)
~fftSize    = 4096;
~numBands   = 128;
~sr         = 44100.0;
~nyquist    = ~sr / 2.0;
~freqMin    = 20.0;
~freqMax    = ~nyquist;
~numFftBins = ~fftSize / 2;    // 2048 usable bins
~normFactor = ~fftSize / 2.0;  // normalize magnitudes

// Pre-compute log-spaced bin ranges for each display band.
// Band k: lo = freqMin * R^(k/numBands), hi = freqMin * R^((k+1)/numBands)
// FFT bin b covers frequency b * sr / fftSize
~logRatio = log(~freqMax / ~freqMin);
~bandBinRanges = ~numBands.collect({|k|
    var loFreq = ~freqMin * exp(~logRatio * k / ~numBands);
    var hiFreq = ~freqMin * exp(~logRatio * (k + 1) / ~numBands);
    var loBin  = max(0,              (loFreq * ~fftSize / ~sr).floor.asInteger);
    var hiBin  = min(~numFftBins-1,  (hiFreq * ~fftSize / ~sr).ceil.asInteger);
    if(hiBin < loBin) { hiBin = loBin };
    [loBin, hiBin]
});

[1, 2, 3, 4, 6, 8, 12, 16, 24, 32].do({|numChannels|
    var synthName = ("fftanalyzer" ++ numChannels).asSymbol;

    SynthDef(synthName, {
        arg in=0, fftbus=0;
        var sig, mixed, fftBuf, chain, windowStarts, mags;

        sig = In.ar(in, numChannels);

        // Mix to mono with RMS normalization for analysis
        mixed = if(numChannels == 1,
            { sig },
            { Mix.ar(sig) / numChannels.sqrt }
        );

        // 4096-point FFT, Hann window, hop = 0.5
        fftBuf = LocalBuf(~fftSize);
        chain  = FFT(fftBuf, mixed, hop: 0.5, wintype: 1);

        // Fire once per FFT frame
        windowStarts = chain > -1;

        // For each log-spaced display band, take the max magnitude
        // across all FFT bins in that band's frequency range.
        mags = ~bandBinRanges.collect({|range|
            var loBin  = range[0];
            var hiBin  = range[1];
            var binMags = (loBin..hiBin).collect({|b|
                Demand.kr(windowStarts, 0, Unpack1FFT(chain, ~fftSize, b, 0))
            });
            binMags.reduce(\max) / ~normFactor
        });

        Out.kr(fftbus, mags);

    }).writeDefFile(fftDir);

    ("Created: " ++ synthName).postln;
});

"FFT Analyzer SynthDefs created successfully.".postln;
