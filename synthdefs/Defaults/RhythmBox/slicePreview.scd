// SlicePreview SynthDef
// Plays a normalised slice [startPos, endPos] of a buffer, then frees itself.
// startPos / endPos are normalised positions (0.0 = buffer start, 1.0 = buffer end).
(
var d, defDir;

if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "/CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "/SuperCollider/CompiledSynthdefs";
};

defDir = d +/+ "/SlicePreview";
File.mkdir(d);
File.mkdir(defDir);

SynthDef(\SlicePreview, { |bufnum = 0, startPos = 0.0, endPos = 1.0, out = 0, gain = 0.7|
    var numFrames  = BufFrames.kr(bufnum).max(2);
    var rate       = BufRateScale.kr(bufnum);
    var startFrame = startPos * numFrames;
    var sliceFrames = ((endPos - startPos) * numFrames).max(1);
    var duration   = sliceFrames / (rate * SampleRate.ir).max(0.001);
    var env        = EnvGen.kr(Env([1, 1, 0], [duration, 0.005]), doneAction: 2);
    var sig        = PlayBuf.ar(2, bufnum, rate, 1, startFrame, loop: 0, doneAction: 0);
    Out.ar(out, sig * gain * env);
}).writeDefFile(defDir);

"SlicePreview written to: ".post; defDir.postln;
)
