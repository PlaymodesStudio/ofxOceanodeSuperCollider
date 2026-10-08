(
~synthCreator.value("RLPF4", {|n|
	var input, filters, filtered, freq, res, type, dry, mix, sig,fmin;

	// Parameters
	freq = OceanodeParameterLag.ar(\pitch, 128, n, 20, 135, "vf", 0.05, true).midicps;
	res = OceanodeParameter.ar(\q, 1, n, 0.1, 4, "vf");
	type = OceanodeParameterDropdown.ar(\type, 0, n, "LowPass:HighPass");
	mix = OceanodeParameter.ar(\mix, 1, n, 0, 1, "vf");
	fmin = OceanodeParameterLag.ar(\fm_in, 0, n, -1, 1, "vf", 1/30, true);

	// Input stage
	input = In.ar(OceanodeInput.kr(\in), n);
	dry = input;

	// Apply 4-pole filter
	filtered=BLowPass4.ar(input, freq+fmin, res);

	// Mix dry and wet signals
	sig = XFade2.ar(dry, filtered, mix * 2 - 1);

	// Output
	Out.ar(OceanodeOutput.kr(\out), sig);
}, description: "4-pole LPF", category: "Effect/Filter");
);

(
~synthCreator.value("RHPF4", {|n|
	var input, filters, filtered, freq, res, type, dry, mix, sig,fmin;

	// Parameters
	freq = OceanodeParameterLag.ar(\pitch, 128, n, 20, 135, "vf", 0.05, true).midicps;
	res = OceanodeParameter.ar(\q, 1, n, 0.1, 4, "vf");
	type = OceanodeParameterDropdown.ar(\type, 0, n, "LowPass:HighPass");
	mix = OceanodeParameter.ar(\mix, 1, n, 0, 1, "vf");
	fmin = OceanodeParameterLag.ar(\fm_in, 0, n, -1, 1, "vf", 1/30, true);

	// Input stage
	input = In.ar(OceanodeInput.kr(\in), n);
	dry = input;

	// Apply 4-pole filter
	filtered = BHiPass4.ar(input, freq+fmin, res);

	// Mix dry and wet signals
	sig = XFade2.ar(dry, filtered, mix * 2 - 1);

	// Output
	Out.ar(OceanodeOutput.kr(\out), sig);
}, description: "4-pole HPF", category: "Effect/Filter");
);
