(
~synthCreator.value("Bells", {|n|
	var pitch, t60, pitchY, levels, gate, input, exciter, sig;

	pitch = OceanodeParameterLag.kr(\pitch, 30, n, 20, 130, "vf", 1/30, true).midicps;
	pitchY = OceanodeParameterLag.kr(\pitchy, 1, n, 1, 6, "vf", 1/30, true);
	t60 = OceanodeParameterLag.kr(\t60, 6, n, 0.1, 10, "vf", 1/30, true);
	levels = OceanodeParameterLag.kr(\levels, 0.25, n, 0, 1, "vf", 1/30, true);
	gate = OceanodeParameter.kr(\gate, 1, n, 0, 1, "vi");
	input = In.ar(OceanodeInput.kr(\in), n) * 0;

	exciter = WhiteNoise.ar() * 0.25;
	sig = DynKlank.ar(
		`[
			[1, 2, 2.803, 3.871, 5.074, 7.81, 10.948, 14.421],
			[1, 0.044, 0.891, 0.0891, 0.794, 0.1, 0.281, 0.079],
			[1, 0.205, 1, 0.196, 0.339, 0.047, 0.058, 0.047] * t60
		],
		exciter + input,
		freqscale: pitch * pitchY
	);

	Out.ar(OceanodeOutput.kr(\out), sig * 0.25 * levels * (gate >= 0));
}, description: "Legacy Bells13 modal bell bank", category: "Source/PhysicalModelling");
)
