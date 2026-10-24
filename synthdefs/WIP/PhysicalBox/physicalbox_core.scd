// PhysicalBox Phase 0: loads physicalbox.eel2 for DynGen.
//
// DynGen 0.6.1 crashes scsynth when a script registers native _parameters
// (see src/nodes/generators/scDynGenPolyNode.cpp), so the readable script is rewritten
// before it is sent: every _name becomes in(k) and its @param line is dropped.
// in(0) stays the external audio input. Defines:
//   ~pbSpecs   [[name, default, type], ...] from the @param lines
//   ~pbDef     the DynGenDef (send it to a booted server, or use its sendMsg)
//   ~pbVoice   {|ext| ...} builds the voice inside a SynthDef from NamedControls
(
var path = ~pbScriptPath ? (PathName(thisProcess.nowExecutingPath).pathOnly +/+ "physicalbox.eel2");
var code = File.readAllString(path);
var specs = [];

code.split($\n).do({|line|
	var m = line.findRegexp("^@param ([a-z0-9]+): ([-0-9.]+)(, type=([a-z]+))?");
	if(m.size > 0) {
		var type = if(m.size > 4 and: { m[4][1].size > 0 }) { m[4][1] } { "lin" };
		specs = specs.add([m[1][1].asSymbol, m[2][1].asFloat, type.asSymbol]);
	};
});

code = code.split($\n).reject({|line| line.beginsWith("@param") }).join("\n");
specs.collect({|s, i| [s[0], i + 1] })
	.sort({|a, b| a[0].asString.size > b[0].asString.size })
	.do({|pair| code = code.replace("_" ++ pair[0], "in(" ++ pair[1] ++ ")") });

~pbSpecs = specs;
~pbDef = DynGenDef(\physicalBox, code);
~pbVoice = {|ext|
	var inputs = ~pbSpecs.collect({|s|
		switch(s[2],
			\trig, { T2A.ar(NamedControl.tr(s[0], 0)) },
			// Audio-rate controls jump without a ramp (gates, toggles).
			\step, { NamedControl.ar(s[0], s[1]) },
			{ K2A.ar(NamedControl.kr(s[0], s[1])) })
	});
	DynGen.ar(2, ~pbDef, inputs: [ext ? DC.ar(0)] ++ inputs, sync: 1);
};
)
