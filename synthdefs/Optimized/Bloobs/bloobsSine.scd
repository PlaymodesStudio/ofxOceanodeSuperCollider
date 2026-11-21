(
// SinOs without the 4x oversampling: a sine has no harmonics to alias, so
// SinOscOS bought nothing and cost ~60x the CPU (8 ch: 4.0% vs 0.07% of a
// core). Same controls as SinOs, so a SinOs node can be swapped for it.
~synthCreator.value("BloobsSine", {|n|
	var freq, phase, fmin, levels;
	freq = OceanodeParameterLag.ar(\pitch, 36, n, 0, 130, "vf", 1/30, true).midicps;
	phase = OceanodeParameterLag.ar(\phase, 0, n, -1, 1, "vf", 1/30, true);
	fmin = OceanodeParameterLag.ar(\fm_in, 0, n, -1, 1, "vf", 1/30, true);
	levels = OceanodeParameterLag.ar(\levels, 0, n, 0, 1, "vf", 1/30, true);
	Out.ar(OceanodeOutput.kr(\out), SinOsc.ar(freq + fmin, phase * pi) * levels);
}, description: "BLOOBS sine oscillator (SinOs without oversampling)", category: "Optimized/Bloobs");
)
