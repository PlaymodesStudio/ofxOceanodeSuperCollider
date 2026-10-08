(
// CompressorPro — feed-forward log-domain compressor.
// Design: Giannoulis/Massberg/Reiss (JAES 2012) + Nathan Ho's notes on avoiding Compander.
//  * Everything runs per sample at audio rate (Compander computes its gain once per block).
//  * Detector: peak/RMS blend (RMS is crest-normalised so a sine reads the same level in both),
//    sidechain HPF, internal or external key, stereo link 0..1.
//  * Gain computer in dB with a true quadratic soft knee (continuous, no division by zero at knee 0).
//  * Ballistics on the gain reduction, in dB: smooth decoupled peak detector
//    (instant rise + release decay, then a one-pole attack). Attack/release are time constants (63 %).
//  * Lookahead delays only the audio, never the detector; the dry path is delayed too so Mix
//    never comb-filters.
//  * Mix is a linear (parallel-compression) crossfade.
//  * grout = applied gain reduction as linear gain (1 = none), usable for metering or modulation.
~synthCreator.value("CompressorPro", {|n|
	var in, key, dry, wet, pk, rms, lvl, linked, over, slope, xk, gr, grs, gain, la;
	var thr, ratio, knee, attack, release, range, makeup, auto, link, rmsAmt, hpf, mix, keySel;

	thr     = OceanodeParameterLag.ar(\threshold, -18, n, -60, 0, "vf", 1/30, true);
	ratio   = OceanodeParameterLag.ar(\ratio, 4, n, 1, 20, "vf", 1/30, true).max(1);
	knee    = OceanodeParameterLag.ar(\knee, 6, n, 0, 24, "vf", 1/30, true).max(0.01);
	attack  = OceanodeParameterLag.ar(\attack, 10, n, 0.1, 200, "vf", 1/30, true).max(0.01);    // ms
	release = OceanodeParameterLag.ar(\release, 150, n, 5, 2000, "vf", 1/30, true).max(1);      // ms
	range   = OceanodeParameterLag.ar(\range, 40, n, 0, 60, "vf", 1/30, true);                  // max GR dB
	makeup  = OceanodeParameterLag.ar(\makeup, 0, n, -12, 24, "vf", 1/30, true);                // dB
	auto    = OceanodeParameterDropdown.kr(\automakeup, 0, n, "Off:On");
	link    = OceanodeParameterLag.ar(\link, 1, n, 0, 1, "vf", 1/30, true);
	rmsAmt  = OceanodeParameterLag.ar(\rms, 0, n, 0, 1, "vf", 1/30, true);
	hpf     = OceanodeParameterLag.kr(\schpf, 20, n, 20, 1000, "vf", 1/30, true);
	keySel  = OceanodeParameterDropdown.kr(\keysource, 0, n, "Internal:External");
	la      = Select.kr(OceanodeParameterDropdown.kr(\lookahead, 0, n, "Off:1ms:2ms:5ms:10ms"),
		[0, 0.001, 0.002, 0.005, 0.01]);
	mix     = OceanodeParameterLag.ar(\mix, 1, n, 0, 1, "vf", 1/30, true);

	in  = In.ar(OceanodeInput.kr(\in), n);
	key = Select.ar(keySel, [in, In.ar(OceanodeInput.kr(\sidechain), n)]);
	key = HPF.ar(key, hpf);

	// level detection (linear), then dB
	pk  = key.abs;
	rms = OnePole.ar(key.squared, exp(-1 / (0.01 * SampleRate.ir))).sqrt * 2.sqrt;
	lvl = (pk + ((rms - pk) * rmsAmt)).max(-120.dbamp).ampdb;
	if(n > 1, {
		linked = lvl.reduce(\max);
		lvl = lvl + ((linked - lvl) * link);
	});

	// static curve: gain reduction in dB (>= 0)
	over  = lvl - thr;
	slope = 1 - ratio.reciprocal;
	xk    = (over + (knee * 0.5)).clip(0, knee);
	gr    = (slope * ((xk.squared / (2 * knee)) + (over - (knee * 0.5)).max(0))).min(range);

	// ballistics (Lag/LagUD times are 60 dB times = 6.9078 time constants)
	grs = LagUD.ar(gr, 0, release * 0.001 * 6.9078);
	grs = Lag.ar(grs, attack * 0.001 * 6.9078);

	// makeup: manual + optional automatic (half the static reduction of a 0 dBFS signal)
	xk   = (thr.neg + (knee * 0.5)).clip(0, knee);
	gain = makeup + (auto * 0.5 * (slope * ((xk.squared / (2 * knee)) + (thr.neg - (knee * 0.5)).max(0))).min(range));
	gain = (gain - grs).dbamp;

	dry = Select.ar(la > 0, [in, DelayN.ar(in, 0.01, la)]);
	wet = dry * gain;
	Out.ar(OceanodeOutput.kr(\out), LinXFade2.ar(dry, wet, mix * 2 - 1));
	Out.ar(OceanodeOutput.kr(\grout), grs.neg.dbamp);

}, description: "Log domain soft knee compressor with lookahead sidechain HPF stereo link and auto makeup", category: "Effect/Dynamics");
)
