(
~synthCreator.value("Campaneitor", {|n, variables|
	var numSpeakers, level, width, spread, position, center, orientation;
	var source, rows, columns, folded, pos, signal;

	numSpeakers = variables[0];
	level = OceanodeParameterLag.kr(\level, 1, 1, 0, 1, "vf", 1/30, true);
	width = OceanodeParameterLag.kr(\width, 2, 1, 0, 13, "vf", 1/30, true);
	spread = OceanodeParameterLag.kr(\spread, 1, 1, 0, 1, "vf", 1/30, true);
	position = OceanodeParameterLag.kr(\position, 0.5, 1, 0, 1, "vf", 1/30, true);
	center = OceanodeParameterLag.kr(\center, 0.5, 1, 0, 1, "vf", 1/30, true);
	orientation = OceanodeParameterLag.kr(\orientation, 0.5, 1, 0, 1, "vf", 1/30, true);

	source = In.ar(OceanodeInput.kr(\in), n);
	columns = numSpeakers * 2;
	rows = max(1, (n / columns).asInteger);
	folded = Mix.ar(source.asArray.reshape(rows, columns));

	pos = if(numSpeakers == 13) {
		[0.0] ++ [0, 5.5 / 12].resamp1(12) ++ [6.5 / 12, 1].resamp1(12) ++ [1.0]
	} {
		[0.0] ++ [0, 1].resamp1(columns - 1)
	};

	signal = PanX.ar(numSpeakers, folded, pos, level * folded.size.reciprocal.sqrt, width)
		.flop.collect(Mix(_));

	// Read otherwise legacy-visible controls so presets keep their UI state.
	signal = signal + ((spread + position + center + orientation) * 0);

	Out.ar(OceanodeOutput.kr(\out), signal);
}, description: "Legacy Campaneitor channel folding and PanX spatializer", category: "Spatial/Panning", variables: ["NumSpeakers"], variableDimensions: [32]);
)
