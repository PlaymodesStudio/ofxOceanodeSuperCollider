// bufferscopeSpread1_<points> for scBufferScope: <points> samples of a mono
// buffer, evenly spread over its whole length, written to a control bus.
// Rebuilt in October 2026 from the shipped binaries (the source was lost).
(
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};
File.mkdir(d);
File.mkdir(d +/+ "bufferscope");

[32, 64, 128, 256, 512, 1024, 2048, 4096].do({ arg points;
    SynthDef.new(("bufferscopeSpread1_" ++ points).asSymbol, {
        arg buf, out;
        var frames = BufFrames.kr(buf);
        var phases = Array.fill(points, { |i| (i / points) * frames });
        Out.kr(out, BufRd.kr(1, buf, phases, 1, 2));
    }).writeDefFile(d +/+ "bufferscope");
});
);
