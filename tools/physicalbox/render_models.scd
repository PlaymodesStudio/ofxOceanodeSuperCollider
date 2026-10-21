// Renders PhysicalVoice (the C++ UGen) offline, one WAV per case: every body
// model struck, the doublets (Split / Beat) and the materials.
//   /Applications/SuperCollider.app/Contents/MacOS/sclang render_models.scd [outDir] [filter]
// then: python3 analyze_models.py [outDir]
// Needs OceanodePhysical (.sc + .scx) in the user extensions.
(
var args = thisProcess.argv;
var outDir = (args[0] ? "/tmp/physicalbox-models").standardizePath;
var filter = args[1];
var rate = 48000, cases, run;
var models = [
	// [name, family, shape, pitch Hz, material]
	["harmonic", 0, 0, 220, 0.5],
	["freebar", 1, 0, 440, 1], ["xylophone", 1, 0.25, 440, 0], ["gamelan", 1, 0.5, 330, 1],
	["marimba", 1, 0.75, 220, 0], ["kalimba", 1, 1, 440, 1],
	["wineglass", 2, 0, 660, 0.5], ["bowl", 2, 0.25, 300, 1], ["handbell", 2, 0.5, 520, 1],
	["churchbell", 2, 0.75, 260, 1], ["gong", 2, 1, 70, 1],
	["handpan", 3, 0, 220, 1], ["steelpan", 3, 1, 330, 1],
	["tabla", 4, 0, 220, 0.4], ["timpani", 4, 0.5, 100, 0.4], ["membrane", 4, 1, 120, 0.4],
	["plate", 5, 0, 200, 1], ["woodblock", 5, 1, 600, 0],
	["morph_bar_marimba", 1, 0.6, 220, 0], ["bowl_wood", 2, 0.25, 300, 0]
];
cases = models.collect { |m|
	[m[0], 4, [\bfamily, m[1], \bshape, m[2], \freq, m[3], \material, m[4], \excite, 1, \listen, 1, \bdecay, 3]]
} ++ [
	["split_handpan", 6, [\bfamily, 3, \bshape, 0, \freq, 220, \material, 1, \excite, 1, \listen, 1, \bdecay, 4, \split, 8]],
	["beat_gamelan", 6, [\bfamily, 1, \bshape, 0.5, \freq, 330, \material, 1, \excite, 1, \listen, 1, \bdecay, 4, \beat, 6]],
	["split_zero_ref", 4, [\bfamily, 2, \bshape, 0, \freq, 660, \material, 0.5, \excite, 1, \listen, 1, \bdecay, 3]],
	["split_tiny", 4, [\bfamily, 2, \bshape, 0, \freq, 660, \material, 0.5, \excite, 1, \listen, 1, \bdecay, 3, \split, 0.001]],
	["bowed_bowl", 6, [\bfamily, 2, \bshape, 0.25, \freq, 300, \material, 1, \excite, 1, \listen, 1, \strike, 0, \bow, 1, \bowpress, 0.3, \bowspeed, 0.6, \atk, 0.3, \split, 3]],
	["bowed_glass_old", 6, [\bfamily, 2, \bshape, 0, \freq, 300, \material, 0.5, \excite, 1, \listen, 1, \strike, 0, \bow, 1, \bowpress, 0.2, \bowspeed, 0.7, \atk, 0.3, \pos, 0.5]],
	["bowed_bowl_pos", 6, [\bfamily, 2, \bshape, 0.25, \freq, 300, \material, 1, \excite, 1, \listen, 1, \strike, 0, \bow, 1, \bowpress, 0.2, \bowspeed, 0.7, \atk, 0.3, \pos, 0.5]],
	["bowed_bar_metal", 6, [\bfamily, 1, \bshape, 0, \freq, 300, \material, 1, \excite, 1, \listen, 1, \strike, 0, \bow, 1]],
	["highnote_gong", 3, [\bfamily, 2, \bshape, 1, \freq, 3000, \material, 1, \excite, 1, \listen, 1, \bdecay, 3, \split, 20, \beat, 20]]
];
if(filter.notNil and: { filter != "all" }) { cases = cases.select { |c| c[0].contains(filter) } };
File.mkdir(outDir);
SynthDef(\pbModel, {
	var names = PhysicalVoice.names;
	var pairs = names.collect { |n|
		[n, switch(n, \gate, { \gate.ar(0) }, \strk, { 0 }, { NamedControl.kr(n, PhysicalVoice.defaults[names.indexOf(n)]) })]
	}.flatten;
	Out.ar(0, PhysicalVoice.ar(0, pairs));
}).store;
run = { |i|
	if(i < cases.size) {
		var c = cases[i], score, path = outDir +/+ c[0] ++ ".wav";
		score = Score([
			[0.0, [\s_new, \pbModel, 1000, 0, 0] ++ c[2]],
			[0.1, [\n_set, 1000, \gate, 0.8]],
			[c[1] - 0.5, [\n_set, 1000, \gate, 0]],
			[c[1], [\c_set, 0, 0]]
		]);
		score.recordNRT(nil, path, sampleRate: rate, headerFormat: "WAV", sampleFormat: "float",
			options: ServerOptions.new.numOutputBusChannels_(2).memSize_(65536),
			duration: c[1], action: { ("rendered " ++ c[0]).postln; run.(i + 1) });
	} { "done".postln; 0.exit };
};
run.(0);
)
