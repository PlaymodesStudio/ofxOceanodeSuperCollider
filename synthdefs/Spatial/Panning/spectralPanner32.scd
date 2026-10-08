(
~synthCreator.value("spectralPanner32", {|n|
	var input, bands, output, finalOutput;
	var freqs, pans;
	var levels, mix;
	var numBands = 32;
	var rq = 0.08;
	var scaling = 1 / numBands.sqrt;

	input = In.ar(OceanodeInput.kr(\in), n);

	// Fixed 32-band MIDI spread from 28 to 128, shared by all input channels.
	freqs = Array.fill(numBands, {|band|
		(24 + ((128 - 24) * (band / (numBands - 1)))).midicps
	});

	// One pan value per band, shared by all input channels.
	pans = OceanodeParameterLag.kr(\panarray, 0, numBands, -2, 2, "vf", 1/30, true);
	mix = OceanodeParameterLag.kr(\mix, 1, n, 0, 1, "vf", 1/30, true);
	levels = OceanodeParameterLag.ar(\levels, 1, n, 0, 1, "vf", 1/30, true).lincurve(0, 1, 0, 1, 4);

	bands = Array.fill(n, {|chan|
		var chanInput = input[chan];

		Mix.fill(numBands, {|band|
			var panPos = pans[band] * 0.5;
			var bpf = BPF.ar(chanInput, freqs[band], rq);
			bpf = BPF.ar(bpf, freqs[band], rq);
			PanAz.ar(n, bpf, panPos, orientation: 0.5)
		}) * scaling
	});

	output = Mix(bands) * levels;

	finalOutput = Array.fill(n, {|chan|
		XFade2.ar(input[chan], output[chan], mix[chan].linlin(0, 1, -1, 1))
	});

	Out.ar(OceanodeOutput.kr(\out), finalOutput);
}, description: "32-band per-input-channel spectral panner with shared per-band pan controls", category: "Spatial/Panning");
)
