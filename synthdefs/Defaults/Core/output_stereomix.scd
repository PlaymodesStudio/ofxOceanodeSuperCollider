(/// output generator
SynthDef.new(\output, {
	arg in = 0, out=0;
	var sig;
	sig = In.ar(in, 13);
	sig=Sanitize.ar(sig);   // before LeakDC: a NaN reaching LeakDC stayed in its state until the synth was recreated
	sig=LeakDC.ar(sig);
	sig = DelayN.ar(sig, 5, \delay.kr(0));
	// soft start: the output synth is recreated with every graph rebuild (preset load)
	Out.ar(out, sig.tanh * \levels.kr(0, 0.05) * EnvGen.kr(Env([0, 0, 1], [0.05, 0.3], [0, 2])));
}).writeDefFile(d);
)

//with integrated stereomix
(
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
)

//output with circular stereomix
(
SynthDef.new(\output, {
    arg in = 0, out = 0;
    var sig;
    var size = max(2, \stereomixsize.kr(13));
    var level = 1 * size.reciprocal.sqrt;
    var rightSegment = (size - 1) / 2;

    // Create circular panning positions array
    var positions = Array.fill(100, { |i|
        var idx = i + 1;  // Match original indexing
        Select.kr(
            i < size,
            [
                // For channels beyond size, use center (0)
                0,
                // For active channels, calculate circular position
                Select.kr(
                    (idx == 1).asInteger,  // Check for first channel (center)
                    [
                        Select.kr(
                            (idx <= (rightSegment + 1)).asInteger,
                            [
                                // Left side of circle
                                sin(2pi * (size - idx + 1) / (size - 1)) * -1,
                                // Right side of circle
                                sin(2pi * (idx - 1) / (size - 1))
                            ]
                        ),
                        0  // Center channel
                    ]
                )
            ]
        )
    });

    sig = In.ar(in, 100) * 0.5;
    sig = Select.ar(\stereomix.kr(0),
        [
            sig,
            (Pan2.ar(sig, positions) * level).flop.collect(Mix(_));
        ]
    );

    // Signal processing chain
    sig = Sanitize.ar(sig);   // before LeakDC: a NaN reaching LeakDC stayed in its state until the synth was recreated
    sig = LeakDC.ar(sig);
    sig = DelayN.ar(sig, 5, \delay.kr(0));

    // soft start: the output synth is recreated with every graph rebuild (preset load)
    Out.ar(out, sig.tanh * \levels.kr(0, 0.05) * EnvGen.kr(Env([0, 0, 1], [0.05, 0.3], [0, 2])));
}).writeDefFile(d);
)

d = thisProcess.nowExecutingPath.dirname;
