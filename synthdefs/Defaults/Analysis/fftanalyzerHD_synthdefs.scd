// fftanalyzerHD SynthDef generator
// Run this ONCE in SuperCollider to compile the binary SynthDef files.
//
// FFT size  : 4096 → 2048 real bins (~10.8 Hz/bin at 44100 Hz)
// Bands out : 1080 log-spaced, max-aggregated, normalized to ~1.0 at 0 dBFS
// All 1080 bands map to ≥1 real FFT bin — no interpolation needed.
//
// Follows the same proven pattern as fftanalyzer.scd (128 bands) but scaled up.
// Delete old fftanalyzerHD/*.scsyndef files before running.

var d, fftDir;

if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};

fftDir = d +/+ "fftanalyzerHD";
File.mkdir(d);
File.mkdir(fftDir);

"Saving fftanalyzerHD SynthDefs to: %".format(fftDir).postln;

~fftSizeHD  = 4096;
~numBandsHD = 1080;
~sr         = 44100.0;
~nyquist    = ~sr / 2.0;
~freqMinHD  = 20.0;
~freqMaxHD  = ~nyquist;
~numFftBins = ~fftSizeHD / 2;    // 2048 usable bins
~normFactor = ~fftSizeHD / 2.0;  // normalize: 0 dBFS ≈ 1.0

// Pre-compute log-spaced bin ranges at build time (same as fftanalyzer.scd)
~logRatioHD = log(~freqMaxHD / ~freqMinHD);
~bandBinRangesHD = ~numBandsHD.collect({|k|
    var loFreq = ~freqMinHD * exp(~logRatioHD * k       / ~numBandsHD);
    var hiFreq = ~freqMinHD * exp(~logRatioHD * (k + 1) / ~numBandsHD);
    var loBin  = max(0,              (loFreq * ~fftSizeHD / ~sr).floor.asInteger);
    var hiBin  = min(~numFftBins-1,  (hiFreq * ~fftSizeHD / ~sr).ceil.asInteger);
    if(hiBin < loBin) { hiBin = loBin };
    [loBin, hiBin]
});

[1, 2, 3, 4, 6, 8, 12, 16, 24, 32].do({|numChannels|
    SynthDef("fftanalyzerHD" ++ numChannels, {
        arg in = 0, fftbus = 0;
        var sig, mixed, buf, chain, windowStarts, mags;

        sig = In.ar(in, numChannels);

        mixed = if(numChannels == 1,
            { sig },
            { Mix.ar(sig) / numChannels.sqrt }
        );

        buf   = LocalBuf(~fftSizeHD);
        chain = FFT(buf, mixed, hop: 0.5, wintype: 1);
        windowStarts = chain > -1;

        // For each log band: max magnitude across all real FFT bins in range, normalized
        mags = ~bandBinRangesHD.collect({|range|
            var loBin  = range[0];
            var hiBin  = range[1];
            var binMags = (loBin..hiBin).collect({|b|
                Demand.kr(windowStarts, 0, Unpack1FFT(chain, ~fftSizeHD, b, 0))
            });
            binMags.reduce(\max) / ~normFactor
        });

        Out.kr(fftbus, mags);

    }).writeDefFile(fftDir);

    ("  Saved fftanalyzerHD" ++ numChannels ++ ".scsyndef").postln;
});

"Done! Recompile the app so it picks up the new 1080-band SynthDefs.".postln;
