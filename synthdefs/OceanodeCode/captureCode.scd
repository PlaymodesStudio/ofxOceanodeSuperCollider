// Headless runner used by the SC Code node (scCodeNode):
//
//     sclang -l <conf with OceanodeCode/classes> captureCode.scd <outDir> <prefix> <block.scd>...
//
// Runs the pasted code block by block, with no server (OceanodeCodeSandbox.sc),
// and keeps every SynthDef it builds. Each one is renamed <prefix>_<name>,
// written to <outDir>/<prefix>_<name>.scsyndef and described in
// <outDir>/meta.json, with the errors of any block that failed. Exits with 0
// once meta.json is written.

var args = thisProcess.argv;
var outDir = args[0], prefix = args[1], blocks = args.copyRange(2, args.size - 1);
var errors = List.new;
var quote, num, describe, finish;

quote = { |value|
	"\"" ++ value.asString
		.replace("\\", "\\\\").replace("\"", "\\\"")
		.replace("\n", "\\n").replace("\t", "\\t").replace("\r", "") ++ "\""
};
num = { |value|
	value = value.asFloat;
	if(value.isNaN or: { value.abs == inf }) { "null" } { value.asString }
};

describe = { |def, name|
	var desc, specs, controls, ios;
	def.name = (prefix ++ "_" ++ name).asSymbol;
	desc = def.asSynthDesc(\oceanodeCode, true);
	def.writeDefFile(outDir);
	specs = def.metadata !? { def.metadata[\specs] };
	controls = (def.allControlNames ? []).collect { |control|
		var defaults = control.defaultValue.asArray;
		var spec = (specs !? { specs[control.name] }) ?? {
			// A standard spec found by name alone (\detune is in Hz, \rate
			// 0.125..8...) only when the default fits it
			var standard = control.name.asSpec;
			var value = defaults.first ? 0;
			if(standard.isKindOf(ControlSpec) and: {
				value == 0 or: { value.abs >= ((standard.maxval - standard.minval).abs * 0.001) }
			} and: { value.inclusivelyBetween(standard.minval.min(standard.maxval), standard.minval.max(standard.maxval)) }) {
				standard
			}
		};
		var text = "{\"name\": " ++ quote.(control.name)
			++ ", \"rate\": " ++ quote.(control.rate)
			++ ", \"default\": [" ++ defaults.collect(num).join(", ") ++ "]"
			++ ", \"lag\": " ++ num.(control.lag.asArray.first ? 0);
		spec = spec !? { spec.asSpec };
		if(spec.isKindOf(ControlSpec)) {
			text = text ++ ", \"spec\": {\"min\": " ++ num.(spec.minval)
				++ ", \"max\": " ++ num.(spec.maxval)
				++ ", \"warp\": " ++ quote.(spec.warp.class.name)
				++ ", \"step\": " ++ num.(spec.step)
				++ ", \"units\": " ++ quote.(spec.units ? "") ++ "}"
		};
		text ++ "}"
	};
	ios = { |list|
		list.collect { |io|
			var bus = io.startingChannel;
			"{\"type\": " ++ quote.(io.type.name)
				++ ", \"rate\": " ++ quote.(io.rate)
				++ ", \"channels\": " ++ io.numberOfChannels
				++ ", \"bus\": " ++ case
					{ bus.isKindOf(Symbol) } { quote.(bus) }
					{ bus.isNumber } { num.(bus) }
					{ "null" }
				++ "}"
		}.join(", ")
	};
	"{\"name\": " ++ quote.(name)
		++ ", \"defName\": " ++ quote.(def.name)
		++ ", \"canFreeSynth\": " ++ desc.canFreeSynth
		++ ", \"hasGate\": " ++ desc.hasGate
		++ ", \"functionPlay\": " ++ (def.metadata[\oceanodeFunctionPlay] == true)
		++ ", \"controls\": [" ++ controls.join(", ") ++ "]"
		++ ", \"inputs\": [" ++ ios.(desc.inputs) ++ "]"
		++ ", \"outputs\": [" ++ ios.(desc.outputs) ++ "]}"
};

finish = {
	var byName = IdentityDictionary.new, order = List.new, entries = List.new;
	// The last definition of a name wins, as on a server
	OceanodeCodeCapture.defs.do { |def|
		if(byName[def.name].isNil) { order.add(def.name) };
		byName[def.name] = def;
	};
	SynthDescLib(\oceanodeCode);
	order.do { |name|
		try {
			entries.add(describe.(byName[name], name));
		} { |error|
			errors.add("SynthDef %: %".format(name, error.errorString));
			error.reportError;
		};
	};
	File.use(outDir +/+ "meta.json", "w", { |file|
		file.write("{\"synthdefs\": [" ++ entries.join(",\n") ++ "],\n\"errors\": ["
			++ errors.collect(quote).join(", ") ++ "]}\n");
	});
	"CAPTURE_CODE_DONE % SynthDefs".format(entries.size).postln;
	0.exit;
};

// Nothing reaches a real server: port 9 is discard
Server.all.do { |server| server.addr = NetAddr("127.0.0.1", 9) };
OceanodeCodeCapture.start;

blocks.do { |path, i|
	var function = thisProcess.interpreter.compileFile(path);
	if(function.isNil) {
		errors.add("block % does not parse".format(i + 1));
	} {
		try {
			function.value;
		} { |error|
			errors.add("block %: %".format(i + 1, error.errorString));
			error.reportError;
		};
	};
};

// Time for anything deferred (waitForBoot, fork) to build its SynthDefs
SystemClock.sched(1.0, { finish.value; nil });
