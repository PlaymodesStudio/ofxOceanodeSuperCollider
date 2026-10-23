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
		var divide, dx, d0, d1, offset, numerator, denominator;
		// Keep the original operation order at ordinary settings. Protect the
		// inflection and curve/normalization poles before dividing: clipping
		// the result alone cannot reliably remove NaNs.
		divide = { |a, b|
			a / (b.abs.max(1e-12) * (1 - (2 * (b < 0))));
		};
		inflec = inflec.abs.max(1e-6) * (1 - (2 * (inflec < 0)));
		dx = ((4 * steep) * (ph - inflec).abs) - steep - 1;
		d0 = ((4 * inflec) * steep) - steep - 1;
		d1 = ((4 * steep) * (1 - inflec).abs) - steep - 1;
		offset = divide.(inflec * (steep - 1), d0);
		numerator = divide.(inflec * (steep - 1) * ((ph / inflec) - 1), dx) + offset;
		denominator = divide.(inflec * (steep - 1) * ((1 / inflec) - 1), d1) + offset;
		divide.(numerator, denominator).clip(0, 1);
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
