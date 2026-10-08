(
// SaturatorPro — soft tanh saturation with first-order antiderivative anti-aliasing (ADAA).
// Same core as mixerpro's Sat insert (Defaults/MixerPro/mixerpro.scd), with per-channel controls.
//  * ADAA: the output is the average of tanh between two consecutive samples,
//    (F(u) - F(u1)) / (u - u1) with F = log cosh, so high drive folds far fewer harmonics back
//    below Nyquist than a plain tanh. When two samples are almost equal it falls back to tanh of
//    their midpoint (the limit of the same expression), avoiding the 0/0.
//  * log cosh is computed in an overflow-safe form: |v| + log(1 + e^(-2|v|)) - log 2.
//  * Bias shifts the curve off-centre for even harmonics (warmth). The static offset it creates
//    (tanh(bias)) is subtracted and LeakDC removes what is left.
//  * Auto gain divides by the small-signal slope of the curve, drive * (1 - tanh(bias)^2), so quiet
//    material stays at unity whatever the drive and only the peaks are squashed.
//  * Tone is a lowpass on the wet signal only. Mix is a linear (parallel) crossfade.
~synthCreator.value("SaturatorPro", {|n|
	var in, wet, drive, bias, tb, auto, comp, tone, output, mix;
	var logcosh = { |v| v.abs + (1 + (-2 * v.abs).exp).log - 2.log };

	drive  = OceanodeParameterLag.ar(\drive, 6, n, 0, 36, "vf", 1/30, true).dbamp;          // dB
	bias   = OceanodeParameterLag.ar(\bias, 0, n, 0, 1, "vf", 1/30, true) * 0.5;
	auto   = OceanodeParameterDropdown.kr(\autogain, 1, n, "Off:On");
	tone   = OceanodeParameterLag.ar(\tone, 20000, n, 500, 20000, "vf", 1/30, true);       // Hz
	output = OceanodeParameterLag.ar(\output, 0, n, -24, 12, "vf", 1/30, true).dbamp;      // dB
	mix    = OceanodeParameterLag.ar(\mix, 1, n, 0, 1, "vf", 1/30, true);

	tb   = bias.tanh;
	comp = Select.ar(auto, [K2A.ar(1), (drive * (1 - tb.squared)).reciprocal]);

	in = In.ar(OceanodeInput.kr(\in), n);

	wet = n.collect { |i|
		var u  = ((in[i] * drive[i]) + bias[i]).clip(-40, 40);
		var u1 = Delay1.ar(u) + (Impulse.ar(0) * u);   // previous sample = current at start: no click from the bias
		var du = u - u1;
		var y  = Select.ar(du.abs < 0.01, [
			(logcosh.(u) - logcosh.(u1)) / du,
			((u + u1) * 0.5).tanh
		]);
		LeakDC.ar(y - tb[i], 0.9995)   // ~4 Hz
	};

	wet = LPF.ar(wet, tone.min(SampleRate.ir * 0.45)) * comp * output;
	Out.ar(OceanodeOutput.kr(\out), LinXFade2.ar(in, wet, mix * 2 - 1));

}, description: "Anti aliased tanh saturation with bias for even harmonics auto gain tone and parallel mix", category: "Effect/Distortion");
)
