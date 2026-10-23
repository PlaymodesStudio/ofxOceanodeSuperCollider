(
~synthCreator.value("SigmoidShaper", {|n|
	var steep, inflec, level, mix, dry, wet, sig;

	// Parameters
	steep = OceanodeParameterLag.ar(\steep, 0.5, n, -1, 1, "vf", 1/30, true).clip(-0.999, 0.999);   // +-1 made the curve 0/0
	inflec = OceanodeParameterLag.ar(\inflec, 0.5, n, -1, 1, "vf", 1/30, true);
	level = OceanodeParameter.ar(\level, 1, n, 0, 1, "vf");
	mix = OceanodeParameter.ar(\mix, 1, n, 0, 1, "vf");

	// Input stage
	dry = In.ar(OceanodeInput.kr(\in), n);

	// Apply sigmoid shaping function
	wet = { |ph, steep, inflec|
		var y = (((inflec * (steep - 1) * ((ph / inflec) - 1)) /
				((4 * steep) * (ph - inflec).abs - steep - 1)) +
				((inflec * (steep - 1)) / ((4 * inflec * steep) - steep - 1))) /
				(((inflec * (steep - 1) * ((1 / inflec) - 1)) /
				((4 * steep) * (1 - inflec).abs - steep - 1)) +
				((inflec * (steep - 1)) / ((4 * inflec * steep) - steep - 1)));
		y.clip(0, 1)  // Ensure output is between 0 and 1
	}.value((dry * 0.5 + 0.5).clip(0, 1), steep, inflec);
	// The curve works on 0..1: audio is mapped -1..1 -> 0..1 above and back here (it used to be
	// applied to the raw audio and clipped to 0..1, rectifying it with ~0.4 DC on the output)
	wet = LeakDC.ar(wet * 2 - 1, 0.9995);

	// Mix dry and wet signals
	sig = XFade2.ar(dry, wet, mix*2-1);

	// Apply level control
	sig = sig * level;

	// Output
	Out.ar(OceanodeOutput.kr(\out), sig);
}, description: "Sigmoid waveshaping distortion with steepness and inflection controls", category: "Effect/Distortion");
)