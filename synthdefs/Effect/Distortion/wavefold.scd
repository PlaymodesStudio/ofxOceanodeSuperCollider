(
~synthCreator.value("WaveFold", {|n|
	var dist, sig, wet, dry, amt, input;

	// Parameters
	amt = OceanodeParameterLag.ar(\amount, 0, n, 0, 1, "vf", 1/30, true);
	wet = OceanodeParameterLag.ar(\mix, 0, n, 0, 1, "vf", 1/30, true);

	// Convert amount parameter (0-1 becomes smaller values for tighter folding)
	amt = (1 - amt).max(0.001);   // amount = 1 gave a 0-wide fold and 1/0

	// Input stage
	input = In.ar(OceanodeInput.kr(\in), n);
	dry = input;

	// Apply wavefolder distortion
	dist = Fold.ar(input, amt.neg, amt) * (1/amt);

	// Mix dry and wet signals using XFade2
	sig = XFade2.ar(dry, dist, wet * 2 - 1);

	// Output
	Out.ar(OceanodeOutput.kr(\out), sig);
}, description: "Wavefolder distortion for creating harmonically rich sounds", category: "Effect/Distortion");
)