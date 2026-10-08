(
~synthCreator.value("DynKlang", {|n|
	var partials = 16;
	var freqs, amps, largeSig, flat;

	freqs = OceanodeParameterLag.kr(\freqarray, 220, partials * n, 0, 10000, "vf", 1/30, true)
		.reshape(n, partials).flop;
	amps = OceanodeParameterLag.kr(\amparray, 0, partials * n, 0, 1, "vf", 1/30, true)
		.reshape(n, partials).flop;

	largeSig = SinOsc.ar(freqs, mul: amps);
	flat = Mix.ar(largeSig);

	Out.ar(OceanodeOutput.kr(\out), flat);
}, description: "Legacy 16-partial per-channel sine bank", category: "Source/FM");
)
