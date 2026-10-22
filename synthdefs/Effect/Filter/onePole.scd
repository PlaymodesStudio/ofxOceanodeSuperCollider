(
~synthCreator.value("OnePole", {|n|
	var input, pitch, type, lp, drywet;

	// First-order (6 dB/octave) low-pass or high-pass (HP = input - LP), as
	// Reaktor's HP/LP 1-Pole; cutoff as a pitch.
	pitch = OceanodeParameterLag.ar(\pitch, 84, n, 0, 135, "vf", 1/30, true);
	type = OceanodeParameterDropdown.ar(\type, 0, n, "LP:HP");
	drywet = OceanodeParameterLag.ar(\drywet, 1, n, 0, 1, "vf", 1/30, true);

	input = In.ar(OceanodeInput.kr(\in), n);
	lp = OnePole.ar(input, exp(-2pi * pitch.midicps.min(SampleRate.ir * 0.49) * SampleDur.ir));
	Out.ar(OceanodeOutput.kr(\out), input + ((Select.ar(type, [lp, input - lp]) - input) * drywet));
}, description: "One-pole low-pass or high-pass filter (6 dB per octave)", category: "Effect/Filter");
)
