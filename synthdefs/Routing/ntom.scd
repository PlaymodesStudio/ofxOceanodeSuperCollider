(
~synthCreator.value("NtoM", {|n, variables|
	var m, source, inPositions, outPositions, signal, weights, level, width;

	// Get number of output channels from variables
	m = variables[0];

	// Get input signal
	source = In.ar(OceanodeInput.kr(\in), n);
	source = source.asArray;
	level = OceanodeParameterLag.kr(\level, 1, m, 0, 1, "vf", 1/30, true);

	// Positions from 0 to 1 (a single channel sits in the middle)
	inPositions = if(n > 1) { (0..(n-1)) / (n-1) } { [0.5] };
	outPositions = if(m > 1) { (0..(m-1)) / (m-1) } { [0.5] };

	// Interpolation width is the coarser of the two grids: upmixing interpolates between
	// neighbouring inputs, downmixing spreads every input over its neighbouring outputs
	// (a narrower width dropped every input that was not next to an output).
	width = max(1 / max(n - 1, 1), 1 / max(m - 1, 1));

	// weights[in][out]: triangle kernel between channel positions
	weights = n.collect{|inIndex|
		m.collect{|outIndex|
			if((n == 1) or: { m == 1 }) { 1.0 } { max(0, 1.0 - (abs(outPositions[outIndex] - inPositions[inIndex]) / width)) }
		}
	};

	weights = if(n <= m) {
		// Upmix: every output interpolates its neighbouring inputs (weights per output sum to 1)
		m.collect{|o| var col = weights.collect(_[o]); col / max(col.sum, 0.000001) }.flop
	} {
		// Downmix: every input is panned over its neighbouring outputs at constant power, so a
		// sound keeps its level whichever input channel it is on
		weights.collect{|row| (row / max(row.sum, 0.000001)).sqrt }
	};

	signal = m.collect{|outIndex|
		Mix.ar(source * weights.collect(_[outIndex])) * level.asArray[outIndex]
	};

	// Output the result
	Out.ar(OceanodeOutput.kr(\out), signal);
}, description: "Channel count converter with interpolation", category: "Routing", variables: ["NumOut"], variableDimensions: [32]);
)
