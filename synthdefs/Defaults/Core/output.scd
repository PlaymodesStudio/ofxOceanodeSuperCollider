// The output synth every server runs (scOutput, serverManager): 128 channels,
// optional stereo mix of the first `stereomixsize` channels, soft start.
// Older and alternative versions: WIP/Core/outputVariants.scd.
(
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};
File.mkdir(d);

SynthDef.new(\output, {
    arg in = 0, out=0;
    var sig;
    var size = max(2, \stereomixsize.kr(2));
    var level = 1 * size.reciprocal.sqrt;
    var positions = Array.fill(128, { |i|
        Select.kr(
            i < size,
            [0, LinLin.kr(i, 0, size-1, -1, 1)]
        )
    });
    // Mask to silence channels >= size
    var mask = Array.fill(128, { |i|
        Select.kr(i < size, [0, 1])
    });
    sig = In.ar(in, 128);
    sig = sig * mask;  // silence unused channels
    sig = Select.ar(\stereomix.kr(0),
        [
            sig,
            (Pan2.ar(sig, positions) * level).flop.collect(Mix(_));
        ]
    );
    sig = Sanitize.ar(sig);   // before LeakDC: a NaN reaching LeakDC stayed in its state until the synth was recreated
    sig = LeakDC.ar(sig);
    sig = DelayN.ar(sig, 5, \delay.kr(0));
    // soft start: the output synth is recreated with every graph rebuild (preset load)
    Out.ar(out, sig.tanh * \levels.kr(0, 0.05) * EnvGen.kr(Env([0, 0, 1], [0.05, 0.3], [0, 2])));
}).writeDefFile(d);
);
