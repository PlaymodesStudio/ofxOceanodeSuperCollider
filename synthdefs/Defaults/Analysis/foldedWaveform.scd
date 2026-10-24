// Compact scrolling scope for folded node displays, adapted from wavescope2.
// 64 control-rate taps, one audio channel, no trigger/stabilization graph.
// The client creates it only for visible displays and polls at 20 Hz.
// Defaults scripts are loaded by syncCompile.scd, whose argv[0] is the
// compilation mode ("script"), not an output path. Write beside this source
// so the sync installer can harvest CompiledSynthdefs, as with wavescope2.
var compiledDir, destination;
compiledDir = if(thisProcess.nowExecutingPath.notNil) {
    thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};
File.mkdir(compiledDir);
destination = compiledDir +/+ "foldedWaveform";
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
