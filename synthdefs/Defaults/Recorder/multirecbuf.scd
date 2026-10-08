(
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "/CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "/SuperCollider/CompiledSynthdefs";
};
File.mkdir(d ++ "/" ++ "multirecbuf");

// multirecbuf: identical to recbufhold but uses InFeedback.ar
// instead of In.ar, so execution order in SC's node tree
// does not matter — always reads previous block's bus data.
// This allows multiple recorder synths to tap the same
// source buses reliably, regardless of when they were created.

(1..24).do { |nCh|
    var name = ("multirecbuf" ++ nCh).asSymbol;
    SynthDef(name, {
        arg in, buf, record = 0;
        var sig = InFeedback.ar(in, nCh);
        var bufFrames = BufFrames.kr(buf);
        var phase = Phasor.ar(
            trig: record,
            rate: 1,
            start: 0,
            end: bufFrames,
            resetPos: 0
        );
        var recordGate = record * (phase < (bufFrames - 1));
        BufWr.ar(sig * recordGate, buf, phase, loop: 0);
    }).writeDefFile(d ++ "/multirecbuf");
    name.postln;
};

"✓ multirecbuf SynthDefs escrits".postln;
)
