// Private MacroMixer audio bridges, 1..128 channels in one SynthDef file.
// Rebuild with: sclang -D /absolute/path/to/this/build.scd
(
var destination = (thisProcess.nowExecutingPath.dirname +/+ "../../data/Supercollider/Synthdefs/Defaults/macromixer").standardizePath;
var bytes = Int8Array.newFrom([83, 67, 103, 102, 0, 0, 0, 2, 0, 128]);
File.mkdir(destination);
(1..128).do { |channels|
    var definition = SynthDef(("oceanodeMacroMixer" ++ channels).asSymbol, {
        |inA=0, inB=0, out=0, gainA=1, gainB=0, smoothing=0.02|
        var a = In.ar(inA, channels), b = In.ar(inB, channels);
        var gains = Lag.kr([gainA, gainB], smoothing);
        Out.ar(out, (a * gains[0]) + (b * gains[1]));
    });
    bytes = bytes ++ definition.asBytes.copyRange(10, definition.asBytes.size - 1);
};
File.use(destination +/+ "macroMixer.scsyndef", "wb", { |file| file.write(bytes) });
("Wrote " ++ destination ++ "/macroMixer.scsyndef").postln;
0.exit;
)
