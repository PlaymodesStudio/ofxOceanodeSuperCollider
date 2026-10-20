// recbuf<n> for scRecbuf: records n channels into a buffer while `record` is
// on (restarting at its start each time), overdubbing by `prelevel` when
// looping; the input is monitored on the n buses after it.
// Rebuilt in October 2026 from the shipped binaries (the source was lost).
(
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};
File.mkdir(d);
File.mkdir(d +/+ "recbuf");

(1..32).do({ arg n;
    SynthDef.new(\recbuf ++ n.asSymbol, {
        arg in, bufnum, record, reclevel = 1, loop, prelevel, monitorlevel;
        var sig = In.ar(in, n) * reclevel.lincurve(0, 1, 0, 1, 4);
        var amp = Amplitude.ar(sig);
        var frames = BufFrames.kr(bufnum);
        var trig = Trig.kr(record, 0.01);
        var monitor;
        Phasor.ar(trig, record, 0, frames);
        RecordBuf.ar(sig, bufnum, 0, record, Select.kr(loop, [0, prelevel]), 1, loop, trig);
        monitor = sig * monitorlevel.lincurve(0, 1, 0, 1, 4);
        amp.do { |a| Peak.ar(a, Impulse.kr(15)) };
        Out.ar(in + n, monitor);
    }).writeDefFile(d +/+ "recbuf");
});
);
