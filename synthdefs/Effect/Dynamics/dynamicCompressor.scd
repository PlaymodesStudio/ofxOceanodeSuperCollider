(
// DynamicCompressor — Giannoulis/Massberg/Reiss log-domain design, now actually per sample:
// soft-knee gain computer -> smooth decoupled peak detector on the gain reduction (dB).
~synthCreator.value("DynamicCompressor", {|n|
	var source, lvl, over, xk, gr, grs, output;
	var threshold, ratio, knee, attack, release, makeup, mix;

	source = In.ar(OceanodeInput.kr(\in), n);

	threshold = OceanodeParameter.kr(\threshold, -20, n, -60, 0, "vf");
	ratio     = OceanodeParameter.kr(\ratio, 4, n, 1, 20, "vf").max(1);
	knee      = OceanodeParameter.kr(\knee, 0, n, 0, 20, "vf").max(0.01);
	attack    = OceanodeParameter.kr(\attack, 0.003, n, 0.0001, 1, "vf");
	release   = OceanodeParameter.kr(\release, 0.1, n, 0.01, 2, "vf");
	makeup    = OceanodeParameter.kr(\makeup, 0, n, 0, 20, "vf");
	mix       = OceanodeParameter.kr(\mix, 1, n, 0, 1, "vf");

	lvl  = source.abs.max(-120.dbamp).ampdb;
	over = lvl - threshold;
	xk   = (over + (knee * 0.5)).clip(0, knee);
	gr   = (1 - ratio.reciprocal) * ((xk.squared / (2 * knee)) + (over - (knee * 0.5)).max(0));

	// smooth decoupled peak detector (eq. 17): instant rise + release, then attack one-pole.
	// times are time constants; Lag/LagUD take 60 dB times (x 6.9078).
	grs = LagUD.ar(gr, 0, release * 6.9078);
	grs = Lag.ar(grs, attack * 6.9078);

	output = source * (makeup - grs).dbamp;
	Out.ar(OceanodeOutput.kr(\out), LinXFade2.ar(source, output, mix * 2 - 1));
},
description: "Dynamic range compressor with adjustable threshold ratio knee attack and release times",
category: "Effect/Dynamics");
)
