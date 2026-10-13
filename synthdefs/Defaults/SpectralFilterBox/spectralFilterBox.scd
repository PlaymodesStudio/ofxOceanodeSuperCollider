// spectralFilterBox.scd — SynthDefs for the SC Spectral Filter Box node
// (src/nodes/scSpectralFilterBox.*).
//
// A spectral filter drawn by a vector: per channel FFT -> PV_MaskFilter ->
// IFFT (sine windows, 50% overlap: unity gain), the dry signal delayed to line
// up with the wet. PV_MaskFilter is a custom UGen (ugens/OceanodeSpectral):
// its .scx and PV_MaskFilter.sc must be installed in the extensions to
// compile these and in the server's plugins to run them.
//
//   SpectralFilterBox_<fft>_<n>   fft = 2048 or 4096, n = channels (1..16)
//
// Controls:
//   in out                 buses
//   mask[256]              the gains (0..1, above 1 boosts) over log
//                          frequency lowfreq..highfreq: the node resamples and
//                          shapes the incoming vector (any size) to 256 values
//   transpose[n]           semitones: the mask moved up / down, per channel
//   lowfreq highfreq       the mask's frequency span (Hz)
//   wrap                   1: the mask repeats outside the span (scrolls round),
//                          0: its edge values hold
//   attack release hold    per-bin smoothing of the levels (s); hold freezes them
//   gain mix               makeup (linear), dry .. wet
//
// Latency: fft - blockSize samples (2048: 45 ms, 4096: 91 ms at 44.1 kHz).
//
// Output: <this folder>/CompiledSynthdefs/SpectralFilterBox/*.scsyndef, to be
// copied to the app's data/Supercollider/Synthdefs/Defaults/SpectralFilterBox/.

(
var d, maxChannels = 16, maskSize = 256, fftSizes = [2048, 4096];
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};
File.mkdir(d);
File.mkdir(d +/+ "SpectralFilterBox");

fftSizes.do { |fft|
    (1..maxChannels).do { |n|
        SynthDef(("SpectralFilterBox_" ++ fft ++ "_" ++ n).asSymbol, {
            var lag = { |name, default| NamedControl.kr(name, default, 1/30, fixedLag: true) };
            var mask = lag.(\mask, 1 ! maskSize);
            var transpose = lag.(\transpose, 0 ! n);
            var low = \lowfreq.kr(20), high = \highfreq.kr(20000), wrap = \wrap.kr(1);
            var attack = \attack.kr(0), release = \release.kr(0), hold = \hold.kr(0);
            var gain = lag.(\gain, 1), mix = lag.(\mix, 1);
            var sig = In.ar(\in.kr(0), n);
            var dry = DelayN.ar(sig, fft / 44100, (fft - BlockSize.ir) * SampleDur.ir);
            var wet = n.collect { |c|
                var chain = FFT(LocalBuf(fft), sig[c], 0.5, 0);
                chain = PV_MaskFilter(chain, mask, transpose[c], low, high, wrap, attack, release, hold);
                IFFT(chain, 0)
            };
            Out.ar(\out.kr(0), Sanitize.ar(dry + (((wet * gain) - dry) * mix)));
        }).writeDefFile(d +/+ "SpectralFilterBox");
    };
};
"SpectralFilterBox: % SynthDefs written to %".format(fftSizes.size * maxChannels, d +/+ "SpectralFilterBox").postln;
)
