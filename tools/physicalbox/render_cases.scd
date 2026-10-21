// Renders the PhysicalBox Phase 0 voice offline (NRT), one WAV per case.
// Run headless:
//   /Applications/SuperCollider.app/Contents/MacOS/sclang render_cases.scd [outDir] [caseFilter|all] [script.eel2]
// then analyse with analyze.py. Needs DynGen in the user extensions.
(
var here = PathName(thisProcess.nowExecutingPath).pathOnly;
var args = thisProcess.argv;
var outDir = (args[0] ? "/tmp/physicalbox-render").standardizePath;
var filter = args[1];
var synth, cases, run, rate = 48000;
var def;

~pbScriptPath = args[2];
(here +/+ "../../synthdefs/WIP/PhysicalBox/physicalbox_core.scd").standardizePath.load;
def = ~pbDef;
File.mkdir(outDir);
// PB_ENGINE=ugen: the Phase 1 C++ PhysicalVoice instead of the DynGen script
synth = if("PB_ENGINE".getenv == "ugen") {
	SynthDef(\pbTest, {
		var pairs = ~pbSpecs.collect({|sp|
			[sp[0], switch(sp[2], \step, { NamedControl.ar(sp[0], sp[1]) }, \trig, { NamedControl.tr(sp[0], 0) },
				{ NamedControl.kr(sp[0], sp[1]) })]
		}).flatten;
		Out.ar(0, PhysicalVoice.ar(WhiteNoise.ar(0.5), pairs));
	})
} { SynthDef(\pbTest, { Out.ar(0, ~pbVoice.(WhiteNoise.ar(0.5))) }) };

// [name, seconds, params, gate events [[time, value], ...]]
cases = [
	["strike_string", 3, (excite: 0, listen: 0, freq: 220, adecay: 3), [[0.1, 1], [0.3, 0]]],
	["strike_string_low", 3, (excite: 0, listen: 0, freq: 55, adecay: 4), [[0.1, 1], [0.3, 0]]],
	["strike_string_high", 2, (excite: 0, listen: 0, freq: 1760, adecay: 1.5), [[0.1, 1], [0.3, 0]]],
	["strike_stiff_high", 2, (excite: 0, listen: 0, freq: 880, adecay: 2, astiff: 0.6), [[0.1, 1], [0.3, 0]]],
	["strike_bar", 3, (excite: 1, listen: 1, freq: 220, bshape: 1, bdecay: 2), [[0.1, 1], [0.3, 0]]],
	["strike_bell", 4, (excite: 1, listen: 1, freq: 220, bshape: 3, material: 1, bdecay: 3), [[0.1, 1], [0.3, 0]]],
	["strike_wood", 2, (excite: 1, listen: 1, freq: 330, bshape: 1, material: 0, bdecay: 2), [[0.1, 1], [0.3, 0]]],
	["strike_membrane_tension", 3, (excite: 1, listen: 1, freq: 110, bshape: 5, material: 0.4, tension: 1), [[0.1, 1], [0.3, 0]]],
	["gong_plain", 5, (excite: 1, listen: 1, freq: 90, bshape: 4, material: 1, bdecay: 4, hardness: 0.8, mass: 0.8), [[0.1, 1], [0.3, 0]]],
	["gong_cascade", 5, (excite: 1, listen: 1, freq: 90, bshape: 4, material: 1, bdecay: 4, cascade: 0.8, hardness: 0.8, mass: 0.8), [[0.1, 1], [0.3, 0]]],
	["sitar_rattle", 3, (excite: 0, listen: 0, freq: 147, adecay: 4, rattle: 0.6, rattleth: 0.15), [[0.1, 1], [0.3, 0]]],
	["bow_string", 3, (strike: 0, bow: 1, excite: 0, listen: 0, freq: 220, adecay: 2), [[0.1, 0.8], [2.2, 0]]],
	["bow_string_press", 3, (strike: 0, bow: 1, excite: 0, listen: 0, freq: 220, adecay: 2, bowpress: 1.0), [[0.1, 0.8], [2.2, 0]]],
	["bow_glass", 3, (strike: 0, bow: 1, excite: 1, listen: 1, freq: 440, bshape: 2, material: 0.5, bdecay: 3, pos: 0.5, bowpress: 0.2, bowspeed: 0.7), [[0.1, 0.8], [2.2, 0]]],
	["bow_bar", 3, (strike: 0, bow: 1, excite: 1, listen: 1, freq: 220, bshape: 1, material: 1), [[0.1, 0.8], [2.2, 0]]],
	["flute", 3, (strike: 0, blow: 1, blowtype: 0, abore: 0.5, abright: 0.25, excite: 0, listen: 0, freq: 440, adecay: 0.3, breath: 0.6, pos: 0.02), [[0.1, 0.8], [2.2, 0]]],
	["flute_low", 3, (strike: 0, blow: 1, blowtype: 0, abore: 0.5, abright: 0.25, excite: 0, listen: 0, freq: 262, adecay: 0.3, breath: 0.6, pos: 0.02), [[0.1, 0.8], [2.2, 0]]],
	["clarinet", 3, (strike: 0, blow: 1, blowtype: 1, abore: 0.5, pos: 0.02, excite: 0, listen: 0, freq: 220, adecay: 0.5), [[0.1, 0.8], [2.2, 0]]],
	["blown_bell", 3, (strike: 0, blow: 1, blowtype: 0, excite: 1, listen: 1, freq: 220, bshape: 3, material: 1), [[0.1, 0.8], [2.2, 0]]],
	["scrape_metal", 3, (strike: 0, scrape: 1, excite: 1, listen: 1, freq: 180, bshape: 4, material: 1, chatter: 0.5), [[0.1, 0.8], [2.2, 0]]],
	["coupled_mutual", 4, (excite: 0, listen: 0.5, freq: 220, btune: 0.15, bshape: 0, couple: 0.4, mutual: 1, adecay: 4, bdecay: 4), [[0.1, 1], [0.3, 0]]],
	["coupled_listen_b", 4, (excite: 0, listen: 1, freq: 220, btune: 0.15, bshape: 0, couple: 0.4, mutual: 1, adecay: 4, bdecay: 4), [[0.1, 1], [0.3, 0]]],
	["drone_hold", 4, (strike: 0, bow: 0.6, blow: 0.4, hold: 1, excite: 0.6, listen: 0.6, freq: 110, bshape: 2, drift: 1, freeze: 0.5), []],
	["ext_resonator", 3, (strike: 0, ext: 0.05, excite: 1, listen: 1, freq: 220, bshape: 3, material: 1), []]
];
// PB_CASES=<file>: render the cases array written in that file instead.
"PB_CASES".getenv !? {|f| if(f.size > 0) { cases = File.readAllString(f).interpret } };
if(filter.notNil and: { filter != "all" }) { cases = cases.select({|c| c[0].contains(filter) }) };

run = {|i|
	if(i >= cases.size) {
		File.use(outDir +/+ "cases.txt", "w", {|f|
			cases.do({|c| f.write(c[0] ++ " " ++ c[1] ++ " " ++ c[3].collect({|e| e[0] }).join(",") ++ "\n") })
		});
		"DONE".postln;
		0.exit;
	} {
		var c = cases[i], score, events;
		events = [[0.0, def.sendMsg, [\d_recv, synth.asBytes]],
			[0.001, [\s_new, \pbTest, 1000, 0, 0] ++ c[2].asPairs]];
		c[3].do({|e| events = events.add([e[0], [\n_set, 1000, \gate, e[1]]]) });
		events = events.add([c[1], [\c_set, 0, 0]]);
		score = Score(events);
		score.recordNRT(nil, outDir +/+ c[0] ++ ".wav", nil, rate, "WAV", "float",
			ServerOptions.new.numOutputBusChannels_(2).numInputBusChannels_(0).memSize_(65536),
			action: { ("rendered " ++ c[0]).postln; run.(i + 1) });
	};
};
run.(0);
)
