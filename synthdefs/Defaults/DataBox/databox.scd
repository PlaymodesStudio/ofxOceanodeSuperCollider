// DataBox: one playhead of the SC DataBox node (src/nodes/scDataBox.*).
//
// Any file is decoded in C++ into mono float buffers (one per data channel).
// The node creates one DataBoxVoice per output channel; each loops a window
// of its buffer around a position, so moving the position scans the file.
//
//   out     output bus; the voice writes to out + chan
//   buf     mono buffer of decoded data
//   posBus  control bus; the voice writes its read position (0..1) to posBus + chan
//   pos     window start, 0..1 of the buffer (lagged by `lag`, which scrubs)
//   win     window length, 0..1 of the buffer
//   rate    playback rate (negative plays backwards)
//   fade    fade at the window edges, as a fraction of the window
//   dc      1: remove DC offset (raw data is rarely centred)
//
// Output: <this folder>/CompiledSynthdefs/DataBox/DataBoxVoice.scsyndef,
// to be copied to the app's data/Supercollider/Synthdefs/Defaults/DataBox/.

(
var d;
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};
File.mkdir(d);
File.mkdir(d +/+ "DataBox");

SynthDef(\DataBoxVoice, {
    |out = 0, chan = 0, buf = 0, posBus = 0,
     pos = 0, win = 0.05, rate = 1, level = 0.5, lag = 0.05, fade = 0.02, dc = 1|
    var frames, winFrames, start, phase, index, edge, sig;

    frames = BufFrames.kr(buf).max(2);
    winFrames = (win.clip(0.0001, 1) * frames).max(2);
    start = Lag.kr(pos.clip(0, 1), lag) * frames;
    phase = Phasor.ar(0, rate * BufRateScale.kr(buf), 0, winFrames);
    index = (start + phase).wrap(0, frames);

    // Click-free loop point: fade in and out at the window edges
    edge = (min(phase, winFrames - phase) / (fade.max(0.0001) * winFrames)).clip(0, 1);

    sig = BufRd.ar(1, buf, index, 1, 4);
    sig = Select.ar(dc.clip(0, 1), [sig, LeakDC.ar(sig)]);
    sig = Sanitize.ar(sig) * edge * Lag.kr(level, 0.02);

    Out.ar(out + chan, sig);
    Out.kr(posBus + chan, A2K.kr(index / frames));
}).writeDefFile(d +/+ "DataBox");

"DataBoxVoice written".postln;
)
