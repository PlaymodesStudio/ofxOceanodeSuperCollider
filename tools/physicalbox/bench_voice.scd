// CPU of PhysicalVoice: 16 voices for 20 s offline per case, wall time printed.
//   sclang bench_voice.scd [filter]
(
var filter = thisProcess.argv[0], run, cases, rate = 48000, secs = 20, voices = ("PB_VOICES".getenv ? "16").asInteger;
cases = [
	// [name, params, pitch moves every block?]
	["bar", [\bfamily, 1, \bshape, 0, \excite, 1, \listen, 1], false],
	["bar_movingpitch", [\bfamily, 1, \bshape, 0, \excite, 1, \listen, 1], true],
	["bar_split", [\bfamily, 1, \bshape, 0, \excite, 1, \listen, 1, \split, 3], false],
	["bar_split_moving", [\bfamily, 1, \bshape, 0, \excite, 1, \listen, 1, \split, 3], true],
	["membrane", [\bfamily, 4, \bshape, 1, \excite, 1, \listen, 1], false],
	["membrane_moving", [\bfamily, 4, \bshape, 1, \excite, 1, \listen, 1], true],
	["morph_moving", [\bfamily, 1, \bshape, 0.6, \excite, 1, \listen, 1], true],
	["tube", [\bfamily, 1, \bshape, 0.75, \excite, 1, \listen, 1, \radiator, 1, \tremrate, 5], false],
	["box", [\bfamily, 1, \bshape, 0, \excite, 1, \listen, 1, \radiator, 3], false],
	["flute", [\strike, 0, \blow, 1, \abore, 1, \air, 1, \breath, 0.9, \jetratio, 0.5, \excite, 0, \listen, 0], false],
	["string_only", [\excite, 0, \listen, 0], false]
];
if(filter.notNil) { cases = cases.select { |c| c[0].contains(filter) } };
SynthDef(\pbBench, {
	var names = PhysicalVoice.names;
	var sig = voices.collect { |i|
		var moving = \moving.kr(0);
		var freq = 110 * (1.07 ** i) * (1 + (moving * SinOsc.kr(3 + (i * 0.1), 0, 0.01)));
		var pairs = names.collect { |n|
			[n, switch(n, \gate, { \gate.ar(0.8) }, \strk, { 0 }, \freq, { freq },
				{ NamedControl.kr(n, PhysicalVoice.defaults[names.indexOf(n)]) })]
		}.flatten;
		PhysicalVoice.ar(0, pairs).sum
	}.sum;
	Out.ar(0, sig * 0.05 ! 2);
}).store;
run = { |i|
	if(i < cases.size) {
		var c = cases[i], t0 = Main.elapsedTime;
		Score([[0.0, [\s_new, \pbBench, 1000, 0, 0, \moving, c[2].binaryValue] ++ c[1]], [secs, [\c_set, 0, 0]]]).recordNRT(nil, "/tmp/pbbench.wav",
			sampleRate: rate, headerFormat: "WAV", sampleFormat: "float", options: ServerOptions.new.numOutputBusChannels_(2).memSize_(262144),
			duration: secs, action: {
				var t = Main.elapsedTime - t0;
				"%: % s for % s of % voices -> % %% of one core per voice".format(c[0].padRight(18), t.round(0.01), secs, voices, (t / secs / voices * 100).round(0.01)).postln;
				run.(i + 1) });
	} { 0.exit };
};
run.(0);
)
