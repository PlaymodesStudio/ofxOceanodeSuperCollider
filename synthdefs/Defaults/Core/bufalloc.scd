(
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};
File.mkdir(d);
File.mkdir(d +/+ "bufalloc");

// Hybrid bufalloc: supports both fragment and circular modes
SynthDef(\bufalloc, {
    arg in = 0, buf = 0, record = 0, loop = 0;
    var sig = In.ar(in, 1);
    // run stops writes without changing the buffer; a new Record rising edge
    // starts at frame zero. Circular mode wraps only while recording.
    RecordBuf.ar(sig, buf, recLevel: 1, preLevel: 0,
        run: record, loop: loop > 0, trigger: record);
}).writeDefFile(d +/+ "bufalloc");

"✓ hybrid bufalloc SynthDef written".postln;
)
