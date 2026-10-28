// Global Pitch glide (scGlobalPitch.h, serverManager): Oceanode writes Global
// Pitch's values into a hidden block of control buses (\in); this synth, at
// the head of the root group, copies them into the block the pitched synths
// read (\out), gliding the transpose and the 24 scale offsets over \glide
// seconds. The on/off flags, detune, wrap and split pass straight through, so
// a Shift, Scale or Transpose change slides every sounding note to its new
// pitch while a note's own changes still jump. 8 blocks of 31 values each.
(
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};
File.mkdir(d);

SynthDef(\GlobalPitchGlide, {
    var blockSize = 31, blocks = 8;
    var values = In.kr(\in.kr(0), blockSize * blocks);
    var glide = \glide.kr(0);
    var glided = values.collect { |value, i|
        var field = i % blockSize;
        // transpose (0), scale offsets (2-13), low-scale offsets (18-29)
        if((field == 0) or: { field.inclusivelyBetween(2, 13) } or: { field.inclusivelyBetween(18, 29) })
            { Lag.kr(value, glide) } { value }
    };
    ReplaceOut.kr(\out.kr(0), glided);
}).writeDefFile(d);
)
