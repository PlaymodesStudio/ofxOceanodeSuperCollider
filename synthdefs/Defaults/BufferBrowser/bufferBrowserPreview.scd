// BufferBrowserPreview SynthDef
// One-shot stereo buffer playback for the file-browser audition.
// Frees itself when playback completes (doneAction: 2).
(
var d, defDir;

if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "/CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "/SuperCollider/CompiledSynthdefs";
};

defDir = d +/+ "/BufferBrowserPreview";
File.mkdir(d);
File.mkdir(defDir);

SynthDef(\BufferBrowserPreview, { |bufnum = 0, out = 0, gain = 0.7|
    var sig = PlayBuf.ar(2, bufnum, BufRateScale.kr(bufnum), 1, 0, doneAction: 2);
    Out.ar(out, sig * gain);
}).writeDefFile(defDir);

"BufferBrowserPreview written to: ".post; defDir.postln;
)
