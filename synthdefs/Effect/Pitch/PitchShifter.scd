(
~synthCreator.value("PitchShifter", {|n|
	var input, sig, grainSize, pitchShift, pitchDispersion, timeDispersion, mix;

	// Parameters with appropriate ranges
	grainSize = OceanodeParameterLag.ar(\grainsize, 0.2, n, 0.01, 1, "vf", 1/30, true);
	pitchShift = OceanodeParameterLag.ar(\pitchshift, 0, n, -24, 24, "vf", 1/30, true).midiratio;
	pitchDispersion = OceanodeParameterLag.ar(\pitchdisp, 0, n, 0, 1, "vf", 1/30, true);
	timeDispersion = OceanodeParameterLag.ar(\timedisp, 0.0001, n, 0.0001, 0.1, "vf", 1/30, true);
	mix = OceanodeParameter.ar(\mix, 1, n, 0, 1, "vf");

	// Input signal
	input = In.ar(OceanodeInput.kr(\in), n);

	// Pitch shifting
	// PitchShift window size cannot be modulated (it is read once at synth start), so 5 fixed sizes
	// across the parameter range are crossfaded by the parameter.
	sig = SelectX.ar(grainSize.explin(0.01, 1, 0, 4), [0.01, 0.0316, 0.1, 0.316, 1].collect { |ws|
		PitchShift.ar(input, ws, pitchShift, pitchDispersion, timeDispersion) });

	// Mix between dry and wet signals
	sig = XFade2.ar(input, sig, mix.linlin(0, 1, -1, 1));

	Out.ar(OceanodeOutput.kr(\out), sig);
}, description: "Pitch shifter with grain size and dispersion controls", category: "Effect/Pitch");
)
