(
// MultiBandCompressor — 5 bands.
// Fixes: Linkwitz-Riley 4 crossovers (2x Butterworth LPF/HPF) in a tree, with allpass phase
// compensation on the lower bands, so the bands sum back flat (the old LPF-difference split did
// not); per-band log-domain compressor with soft knee and per-sample ballistics; Mix crossfades
// against the uncompressed band sum (same phase), linearly, so it never combs.
~synthCreator.value("MultiBandCompressor", {|n|
	var sigIn, compressor, lr, ap, lo, bands, comp, dryAP, f1, f2, f3, f4, rest;
	var attack1, attack2, attack3, attack4, attack5;
	var release1, release2, release3, release4, release5;
	var threshold1, threshold2, threshold3, threshold4, threshold5;
	var ratio1, ratio2, ratio3, ratio4, ratio5;
	var gain1, gain2, gain3, gain4, gain5;
	var masterGain, mix, knee;

	sigIn = In.ar(OceanodeInput.kr(\in), n);

	f1 = OceanodeParameter.kr(\lowfreq, 150, n, 20, 500, "f");
	f2 = OceanodeParameter.kr(\lowmidfreq, 500, n, 100, 1500, "f");
	f3 = OceanodeParameter.kr(\midfreq, 1500, n, 500, 5000, "f");
	f4 = OceanodeParameter.kr(\highmidfreq, 5000, n, 2000, 10000, "f");
	// keep crossovers ordered
	f2 = f2.max(f1 * 1.1); f3 = f3.max(f2 * 1.1); f4 = f4.max(f3 * 1.1).min(20000);

	attack1 = OceanodeParameter.kr(\lowattack, 0.01, n, 0.001, 0.2, "f");
	release1 = OceanodeParameter.kr(\lowrelease, 0.1, n, 0.01, 0.5, "f");
	threshold1 = OceanodeParameter.kr(\lowthresh, -12, n, -60, 0, "f");
	ratio1 = OceanodeParameter.kr(\lowratio, 4, n, 1, 20, "f");
	gain1 = OceanodeParameter.kr(\lowgain, 1, n, 0, 2, "f");

	attack2 = OceanodeParameter.kr(\lowmidattack, 0.01, n, 0.001, 0.2, "f");
	release2 = OceanodeParameter.kr(\lowmidrelease, 0.1, n, 0.01, 0.5, "f");
	threshold2 = OceanodeParameter.kr(\lowmidthresh, -12, n, -60, 0, "f");
	ratio2 = OceanodeParameter.kr(\lowmidratio, 4, n, 1, 20, "f");
	gain2 = OceanodeParameter.kr(\lowmidgain, 1, n, 0, 2, "f");

	attack3 = OceanodeParameter.kr(\midattack, 0.01, n, 0.001, 0.2, "f");
	release3 = OceanodeParameter.kr(\midrelease, 0.1, n, 0.01, 0.5, "f");
	threshold3 = OceanodeParameter.kr(\midthresh, -12, n, -60, 0, "f");
	ratio3 = OceanodeParameter.kr(\midratio, 4, n, 1, 20, "f");
	gain3 = OceanodeParameter.kr(\midgain, 1, n, 0, 2, "f");

	attack4 = OceanodeParameter.kr(\highmidattack, 0.01, n, 0.001, 0.2, "f");
	release4 = OceanodeParameter.kr(\highmidrelease, 0.1, n, 0.01, 0.5, "f");
	threshold4 = OceanodeParameter.kr(\highmidthresh, -12, n, -60, 0, "f");
	ratio4 = OceanodeParameter.kr(\highmidratio, 4, n, 1, 20, "f");
	gain4 = OceanodeParameter.kr(\highmidgain, 1, n, 0, 2, "f");

	attack5 = OceanodeParameter.kr(\highattack, 0.01, n, 0.001, 0.2, "f");
	release5 = OceanodeParameter.kr(\highrelease, 0.1, n, 0.01, 0.5, "f");
	threshold5 = OceanodeParameter.kr(\highthresh, -12, n, -60, 0, "f");
	ratio5 = OceanodeParameter.kr(\highratio, 4, n, 1, 20, "f");
	gain5 = OceanodeParameter.kr(\highgain, 1, n, 0, 2, "f");

	knee       = OceanodeParameter.kr(\knee, 6, n, 0, 24, "f").max(0.01);
	masterGain = OceanodeParameter.kr(\mastergain, 1, n, 0, 2, "f");
	mix        = OceanodeParameter.kr(\mix, 1, n, 0, 1, "f");

	// log-domain compressor: soft knee, smooth decoupled ballistics on the gain reduction
	compressor = { |snd, attack, release, threshold, ratio|
		var over, xk, gr;
		over = snd.abs.max(-120.dbamp).ampdb - threshold;
		xk   = (over + (knee * 0.5)).clip(0, knee);
		gr   = (1 - ratio.max(1).reciprocal) * ((xk.squared / (2 * knee)) + (over - (knee * 0.5)).max(0));
		gr   = Lag.ar(LagUD.ar(gr, 0, release * 6.9078), attack * 6.9078);
		snd * gr.neg.dbamp;
	};

	lr = { |x, f| [LPF.ar(LPF.ar(x, f), f), HPF.ar(HPF.ar(x, f), f)] };   // Linkwitz-Riley 4
	ap = { |x, f| BAllPass.ar(x, f, 2.sqrt) };                            // = LR4 low + high

	#lo, rest = lr.(sigIn, f1);
	bands = [ap.(ap.(ap.(lo, f2), f3), f4)];
	#lo, rest = lr.(rest, f2);
	bands = bands.add(ap.(ap.(lo, f3), f4));
	#lo, rest = lr.(rest, f3);
	bands = bands.add(ap.(lo, f4));
	#lo, rest = lr.(rest, f4);
	bands = bands.add(lo).add(rest);

	comp = [
		compressor.(bands[0], attack1, release1, threshold1, ratio1) * gain1,
		compressor.(bands[1], attack2, release2, threshold2, ratio2) * gain2,
		compressor.(bands[2], attack3, release3, threshold3, ratio3) * gain3,
		compressor.(bands[3], attack4, release4, threshold4, ratio4) * gain4,
		compressor.(bands[4], attack5, release5, threshold5, ratio5) * gain5
	];
	dryAP = bands.sum;

	sigIn = LinXFade2.ar(dryAP, comp.sum, (mix * 2) - 1) * masterGain.lincurve(0, 1, 0, 1, 4);
	Out.ar(OceanodeOutput.kr(\out), sigIn);

}, description: "Five band compressor with independent controls for each frequency range", category: "Effect/Dynamics");
)
