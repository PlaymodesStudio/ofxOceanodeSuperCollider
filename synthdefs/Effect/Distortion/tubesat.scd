(
// TubeSat — asymmetric tube-style saturation with dynamic bias (blocking) and sag.
//  * Transfer curve, asymmetric and continuous with slope 1 at 0:
//      u >= 0 : tanh(u)              soft early limit (grid conduction)
//      u <  0 : h * tanh(u / h)      later limit, headroom h = 1 + 2 * asym (cut-off side)
//    The two halves saturating at different levels produce even harmonics.
//  * Anti-aliasing: first-order antiderivative (ADAA) with the exact antiderivative
//      F(u) = log cosh(u)  (u >= 0),   h^2 * log cosh(u / h)  (u < 0)
//    as in SaturatorPro; near-equal samples fall back to f of their midpoint.
//  * Operating point = bias - blocking shift. Blocking: positive peaks charge the coupling
//    capacitor and push the operating point towards cut-off (recovering over ~200 ms), so on
//    loud passages the clipping moves from the early-limiting positive side to the negative side.
//    The tone changes with the dynamics; even harmonics are not monotonic in blocking
//    (measured at -6 dB in: H2 -18.5 / -26.8 / -19.6 dBc for blocking 0 / 0.3 / 1).
//  * Sag: the supply droops with the average level, lowering the drive (up to 6 dB at sag 1).
//  * The DC of the moving operating point, f(op), is subtracted; LeakDC removes the rest.
//  * Auto gain uses the static drive and bias, so sag and blocking stay audible as dynamics.
//  * Tone is a lowpass on the wet only; Mix is a linear (parallel) crossfade.
~synthCreator.value("TubeSat", {|n|
	var in, wet, drive, bias, asym, blocking, sag, auto, tone, output, mix;
	var logcosh = { |v| v.abs + (1 + (-2 * v.abs).exp).log - 2.log };   // overflow-safe
	var curve = { |u, h| Select.ar(u < 0, [u.tanh, h * (u / h).tanh]) };
	var prim  = { |u, h| Select.ar(u < 0, [logcosh.(u), h.squared * logcosh.(u / h)]) };

	drive    = OceanodeParameterLag.ar(\drive, 12, n, 0, 36, "vf", 1/30, true).dbamp;     // dB
	bias     = OceanodeParameterLag.ar(\bias, 0.2, n, 0, 1, "vf", 1/30, true) * 0.5;
	asym     = OceanodeParameterLag.ar(\asym, 0.5, n, 0, 1, "vf", 1/30, true);
	blocking = OceanodeParameterLag.ar(\blocking, 0.3, n, 0, 1, "vf", 1/30, true);
	sag      = OceanodeParameterLag.ar(\sag, 0.2, n, 0, 1, "vf", 1/30, true);
	auto     = OceanodeParameterDropdown.kr(\autogain, 1, n, "Off:On");
	tone     = OceanodeParameterLag.ar(\tone, 16000, n, 500, 20000, "vf", 1/30, true);    // Hz
	output   = OceanodeParameterLag.ar(\output, 0, n, -24, 12, "vf", 1/30, true).dbamp;   // dB
	mix      = OceanodeParameterLag.ar(\mix, 1, n, 0, 1, "vf", 1/30, true);

	in = In.ar(OceanodeInput.kr(\in), n);

	wet = n.collect { |i|
		var h = 1 + (2 * asym[i]);
		var x = in[i] * drive[i];
		var sagEnv, shift, op, u, u1, du, y, slope, comp;

		// sag: slow average level of the driven input (bounded by tanh)
		sagEnv = LagUD.ar(x.abs, 0.02, 0.3).tanh;
		x = x / (1 + (sag[i] * sagEnv));

		// blocking: positive peaks shift the operating point towards cut-off
		shift = LagUD.ar(x.max(0), 0.005, 0.2).tanh * blocking[i];
		op = bias[i] - shift;

		u  = (x + op).clip(-40, 40);
		u1 = Delay1.ar(u) + (Impulse.ar(0) * u);   // previous sample = current at start: no click from the bias
		du = u - u1;
		y  = Select.ar(du.abs < 0.01, [
			(prim.(u, h) - prim.(u1, h)) / du,
			curve.(((u + u1) * 0.5), h)
		]);
		y = LeakDC.ar(y - curve.(op, h), 0.9995);   // ~4 Hz

		// auto gain: inverse small-signal slope at the static operating point
		slope = 1 - bias[i].tanh.squared;
		comp  = Select.ar(auto[i], [K2A.ar(1), (drive[i] * slope).reciprocal]);
		y * comp
	};

	wet = LPF.ar(wet, tone.min(SampleRate.ir * 0.45)) * output;
	Out.ar(OceanodeOutput.kr(\out), LinXFade2.ar(in, wet, mix * 2 - 1));

}, description: "Asymmetric tube style saturation with dynamic bias blocking sag anti aliasing and auto gain", category: "Effect/Distortion");
)
