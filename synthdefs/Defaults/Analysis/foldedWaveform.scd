// Compact scrolling scope for folded node displays, adapted from wavescope2.
// 64 control-rate taps, one audio channel, no trigger/stabilization graph.
// The client creates it only for visible displays and polls at 20 Hz.
var destination = thisProcess.argv[0];
if(destination.isNil) {
    destination = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs/foldedWaveform";
};
File.mkdir(destination);
SynthDef(\oceanodeFoldedWaveform, {
    arg in = 0, out = 0;
    var points = 64, window = 0.05;
    var frequency = (points - 1) / window;
    var length = points + BlockSize.ir + 2;
    var history = LocalBuf(length, 1).clear;
    var phase = Phasor.ar(0, frequency / SampleRate.ir, 0, length);
    var signal = In.ar(in, 1);
    var readPhase;
    // Sanitize only the visualization path, without altering routed audio.
    signal = Select.ar(CheckBadValues.ar(signal, post: 0).min(1), [signal, DC.ar(0)]);
    BufWr.ar(signal, history, phase.floor, loop: 1);
    // Account for the block written by BufWr before control-rate readers run.
    readPhase = (A2K.kr(phase) + ((BlockSize.ir - 1) * frequency / SampleRate.ir)).floor;
    Out.kr(out, Array.fill(points, { |i|
        BufRd.kr(1, history, readPhase - (points - 1) + i + length,
            loop: 1, interpolation: 1);
    }));
}).writeDefFile(destination);
