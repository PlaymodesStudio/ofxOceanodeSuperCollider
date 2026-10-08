(
~synthCreator.value("XPanAzLegacy", {|n, variables|
	var numSpeakers, source, x, width, rotation, frontPos, backPos, frontSignals, backSignals, signal;

	numSpeakers = variables[0];
	source = In.ar(OceanodeInput.kr(\in), n);
	x = OceanodeParameterLag.kr(\x, 0.5, n, 0, 1, "vf", 1/30, true) / 2;
	width = OceanodeParameterLag.kr(\width, 2, n, 1, 13, "vf", 1/30, true);
	rotation = 0.4188;

	frontPos = ((x * 2) - 1 + rotation).wrap(-1, 1);
	backPos = ((x * -2) + 1 + rotation).wrap(-1, 1);
	frontSignals = PanAz.ar(numSpeakers, source.asArray, frontPos, 1, width);
	backSignals = PanAz.ar(numSpeakers, source.asArray, backPos, 1, width);
	signal = Array.fill(numSpeakers, { |chan|
		var front = frontSignals.collect({ |sig| sig[chan] }).sum;
		var back = backSignals.collect({ |sig| sig[chan] }).sum;
		(front + back) * 0.5;
	});

	Out.ar(OceanodeOutput.kr(\out), signal);
}, description: "Legacy XPanAz 78-to-speaker mirror panner", category: "Spatial/Panning", variables: ["NumSpeakers"], variableDimensions: [32]);
)
