(
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "/CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "/SuperCollider/CompiledSynthdefs";
};
File.mkdir(d ++ "/" ++ "recbufhold");

(1..16).do { |nCh|
    var name = ("recbufhold" ++ nCh).asSymbol;
    SynthDef(name, {
        arg in, buf, record = 0;
        var sig = InFeedback.ar(in, nCh);
        // RecordBuf stops writing when run is zero and stops at the end of
        // the buffer. A rising record value starts a fresh capture at frame 0.
        RecordBuf.ar(sig, buf, recLevel: 1, preLevel: 0,
            run: record, loop: 0, trigger: record);

    }).writeDefFile(d ++ "/recbufhold");
    name.postln;
};

"✓ recbufhold SynthDefs escrits (simple version)".postln;
)
