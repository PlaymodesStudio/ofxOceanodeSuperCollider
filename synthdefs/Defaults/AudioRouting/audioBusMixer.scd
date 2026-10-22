// Core audio bus mixer: sum/crossfade two buses with optional gain smoothing.
(
var destination, bytes;
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};
destination = d +/+ "AudioRouting";
File.mkdir(d);
bytes = Int8Array.newFrom([83, 67, 103, 102, 0, 0, 0, 2, 0, 128]);
File.mkdir(destination);
(1..128).do { |channels|
    var definition = SynthDef(("oceanodeAudioBusMixer" ++ channels).asSymbol, {
        |inA=0, inB=0, out=0, gainA=1, gainB=0, smoothing=0.02|
        var a = In.ar(inA, channels), b = In.ar(inB, channels);
        var gains = Lag.kr([gainA, gainB], smoothing);
        Out.ar(out, (a * gains[0]) + (b * gains[1]));
    });
    bytes = bytes ++ definition.asBytes.copyRange(10, definition.asBytes.size - 1);
};
File.use(destination +/+ "audioBusMixer.scsyndef", "wb", { |file| file.write(bytes) });
("Wrote " ++ destination ++ "/audioBusMixer.scsyndef").postln;
)
