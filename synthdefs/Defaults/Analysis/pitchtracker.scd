// pitchTracker<n> for scPitchTracker: Tartini pitch and confidence of each
// input channel, written to two control buses of n channels.
// Rebuilt in October 2026 from the shipped binaries (the source was lost).
(
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};
File.mkdir(d);
File.mkdir(d +/+ "pitchTracker");

(1..24).do({ arg n;
    SynthDef.new(\pitchTracker ++ n.asSymbol, {
        arg in, freq, hasFreq;
        var pitch = Tartini.kr(In.ar(in, n), 0.93, 2048, 0, 1024, 0.5);
        if(n == 1) { pitch = [pitch] };
        Out.kr(freq, pitch.collect(_[0]));
        Out.kr(hasFreq, pitch.collect(_[1]));
    }).writeDefFile(d +/+ "pitchTracker");
});
);
