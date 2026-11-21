(
// Filter4Pole split by type, with control-rate cutoff and Q. Filter4Pole runs
// a lowpass AND a highpass on every channel and crossfades them, and its
// audio-rate cutoff makes the B-filters recompute their coefficients every
// sample. BLOOBS drives every cutoff from control routers, with a fixed type,
// so one filter at control rate does the same job (8 ch: 0.73% -> ~0.16%).
// Controls match Filter4Pole (Cutp, Q, Mix) minus Type and Fm In.
~synthCreator.value("BloobsLowPass", {|n|
	var input, freq, res, mix;
	freq = OceanodeParameterLag.kr(\cutp, 128, n, 20, 135, "vf", 0.05, true).midicps;
	res = OceanodeParameter.kr(\q, 1, n, 0.1, 4, "vf");
	mix = OceanodeParameter.kr(\mix, 1, n, 0, 1, "vf");
	input = In.ar(OceanodeInput.kr(\in), n);
	Out.ar(OceanodeOutput.kr(\out), XFade2.ar(input, BLowPass4.ar(input, freq, res), mix * 2 - 1));
}, description: "BLOOBS 4-pole lowpass (Filter4Pole at control rate)", category: "Optimized/Bloobs");
);
(
~synthCreator.value("BloobsHiPass", {|n|
	var input, freq, res, mix;
	freq = OceanodeParameterLag.kr(\cutp, 20, n, 20, 135, "vf", 0.05, true).midicps;
	res = OceanodeParameter.kr(\q, 1, n, 0.1, 4, "vf");
	mix = OceanodeParameter.kr(\mix, 1, n, 0, 1, "vf");
	input = In.ar(OceanodeInput.kr(\in), n);
	Out.ar(OceanodeOutput.kr(\out), XFade2.ar(input, BHiPass4.ar(input, freq, res), mix * 2 - 1));
}, description: "BLOOBS 4-pole highpass (Filter4Pole at control rate)", category: "Optimized/Bloobs");
)
