// Info<n> for scInfo: amplitude, peak and value of n audio channels, written
// to three control buses.
(
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};
File.mkdir(d);
File.mkdir(d +/+ "Info");

(1..100).do({ arg n;
    SynthDef.new(\Info ++ n.asSymbol, {
        arg in, amp, peak, value, lagTime = 0.2, decay = 0.99;
        var sig = In.ar(in, n);
        Out.kr(amp, Lag.kr(Amplitude.kr(sig), lagTime));
        Out.kr(peak, PeakFollower.kr(sig, decay));
        Out.kr(value, A2K.kr(sig));
    }).writeDefFile(d +/+ "Info");
});
);
