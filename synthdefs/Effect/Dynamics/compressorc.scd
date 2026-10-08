(
// CompressorC — detector HPF, linked detector, ratio, soft knee, lookahead, saturation.
// Fixes: EnvDetect (non-core UGen) replaced by log-domain ballistics; knee is now a real soft knee
// in dB (was a lag on the gain); lookahead is a plain 0..10 ms audio delay (old formula had an
// operator-precedence bug and a 0.4 s glide); dry is delayed with the wet so Mix doesn't comb;
// detector link uses the loudest channel (the mean cancelled out-of-phase content);
// levels defaults to 1 (was 0 = silent).
~synthCreator.value("CompressorC", {|n|
	var in, dry, e, t, r, knee, over, xk, c, lookahead, wet, bias, attack, release;

	in = In.ar(OceanodeInput.kr(\in), n);

	attack  = OceanodeParameterLag.ar(\attack, 0.01, n, 0.0001, 0.1, "vf", 1/30, true);
	release = OceanodeParameterLag.ar(\release, 0.1, n, 0.001, 0.4, "vf", 1/30, true);
	t       = OceanodeParameterLag.ar(\threshold, -20, n, -80, 0, "vf", 1/30, true);
	r       = OceanodeParameterLag.ar(\ratio, 4, n, 1, 20, "vf", 1/30, true).max(1);
	knee    = OceanodeParameterLag.ar(\knee, 0, n, 0, 24, "vf", 1/30, true).max(0.01);

	in = in * OceanodeParameterLag.ar(\trim, 0, n, 0, 60, "vf", 1/30, true).dbamp;

	// detector: HPF against bass pumping, peak level in dB, linked across channels
	e = HPF.ar(in, OceanodeParameterLag.ar(\hpf, 50, n, 10, 1000, "vf", 1/30, true));
	e = e.abs.max(-120.dbamp).ampdb;
	if(n > 1, { e = e.reduce(\max) ! n });

	over = e - t;
	xk   = (over + (knee * 0.5)).clip(0, knee);
	c    = (1 - r.reciprocal) * ((xk.squared / (2 * knee)) + (over - (knee * 0.5)).max(0));
	c    = Lag.ar(LagUD.ar(c, 0, release * 6.9078), attack * 6.9078);
	c    = c.neg.dbamp;

	// lookahead: delays audio (and dry) only. Avoid modulating it: any delay change bends pitch.
	lookahead = OceanodeParameterLag.ar(\lookahead, 0, n, 0, 1, "vf", 1/30, true);
	in  = DelayC.ar(in, 0.011, Lag.ar(lookahead * 0.01, 0.05));
	dry = in;

	in = in * c;
	in = in * OceanodeParameterLag.ar(\gain, 0, n, 0, 60, "vf", 1/30, true).dbamp;

	bias = OceanodeParameterLag.ar(\bias, 0, n, 0, 0.5, "vf", 1/30, true);
	in = Select.ar(OceanodeParameterDropdown.kr(\saturate, 1, n, "Off:On"), [in, (in + bias).softclip - bias.softclip]);
	in = LeakDC.ar(in, 0.9995);   // ~4 Hz (default 0.995 is a 38 Hz high-pass at 48k)

	wet = OceanodeParameterLag.ar(\mix, 1, n, 0, 1, "vf", 1/30, true);
	wet = LinXFade2.ar(LeakDC.ar(dry, 0.9995), in, (wet * 2) - 1);
	wet = wet * OceanodeParameterLag.ar(\levels, 1, n, 0, 1, "vf", 1/30, true).lincurve(0, 1, 0, 1, 4);

	Out.ar(OceanodeOutput.kr(\out), wet);

}, description: "Dynamic range compressor with threshold ratio and makeup gain", category: "Effect/Dynamics");
)
