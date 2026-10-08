(
~synthCreator.value("CompressorB", {|n|
	var source, sig, thresh, ratio, attack, release, makeup, knee, amplitudeDb, over, xk, gainDb, send;

	thresh  = OceanodeParameter.ar(\threshold, -6, n, -60, 0, "vf");
	ratio   = OceanodeParameter.ar(\ratio, 4, n, 1, 60, "vf").max(1);          // was 0..60: ratio 0 divided by zero
	attack  = OceanodeParameter.ar(\attack, 0.01, n, 0, 0.2, "vf");             // was max 10 ms
	release = OceanodeParameter.ar(\release, 0.1, n, 0, 2, "vf");               // was max 50 ms
	knee    = OceanodeParameter.ar(\knee, 0, n, 0, 24, "vf").max(0.01);         // dB, 0 = hard knee
	makeup  = OceanodeParameter.ar(\makeup, 1, n, 1, 8, "vf");
	send    = OceanodeParameterLag.ar(\send, 1, n, 0, 1, "vf", 1/30, true).lincurve(0, 1, 0, 1, 4);

	source = In.ar(OceanodeInput.kr(\in), n) * send;

	amplitudeDb = Amplitude.ar(source, attack, release).max(-120.dbamp).ampdb;
	over   = amplitudeDb - thresh;
	xk     = (over + (knee * 0.5)).clip(0, knee);
	gainDb = (1 - ratio.reciprocal) * ((xk.squared / (2 * knee)) + (over - (knee * 0.5)).max(0));
	sig = source * gainDb.neg.dbamp * makeup;

	sig = LinXFade2.ar(source, sig, OceanodeParameter.ar(\mix, 1, n, 0, 1, "vf") * 2 - 1);
	Out.ar(OceanodeOutput.kr(\out), sig);

}, description: "Alternative compressor with ratio-based compression", category: "Effect/Dynamics");
)
