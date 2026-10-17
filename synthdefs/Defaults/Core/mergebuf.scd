// Merge mono input buffers into an N-channel output buffer.
// Choose mergeBufN to match the output buffer's channel count. Input buffer
// controls are inBufs0 .. inBufs(N-1); each input buffer must be mono.
(
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};
File.mkdir(d);
File.mkdir(d +/+ "mergeBuf");

(1..24).do { |nChans|
    SynthDef(("mergeBuf" ++ nChans).asSymbol, {
        arg outBuf = 0;
        var phase, inputs, sig;

        phase = Phasor.ar(0, 1, 0, BufFrames.kr(outBuf));
        inputs = Array.fill(nChans, { |i|
            ("inBufs" ++ i).asSymbol.kr(0)
        });
        sig = inputs.collect { |buf| BufRd.ar(1, buf, phase, loop: 1) };
        BufWr.ar(sig, outBuf, phase, loop: 1);
    }).writeDefFile(d +/+ "mergeBuf");
};
)
