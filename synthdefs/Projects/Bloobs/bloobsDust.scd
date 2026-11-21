(
// BloobsNoise with only its Dust choice. BloobsNoise computes all four noise
// types on every voice and Select only picks one afterwards; BLOOBS' mud
// always uses Dust (78 ch: 3.5% -> ~0.9% of a core). Controls match
// BloobsNoise minus Type.
~synthCreator.value("BloobsDust", {|n|
	var density, levels;
	density = OceanodeParameter.kr(\density, 1, n, 0, 1, "vf");
	levels = OceanodeParameterLag.kr(\levels, 0, n, 0, 1, "vf", 1/30, true);
	Out.ar(OceanodeOutput.kr(\out), Dust2.ar(density * 8, 2) * levels);
}, description: "BLOOBS dust noise source (BloobsNoise, Dust only)", category: "Projects/Bloobs");
)
