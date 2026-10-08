(
// TapeSat — tape machine colour: emphasis-shaped saturation, head bump, gap loss, wow/flutter, hiss.
//  * Record/playback emphasis: a high shelf boosts the highs before the saturator and the exact
//    inverse shelf cuts them after it, so small signals come out flat but loud high frequencies
//    saturate first (tape's level-dependent treble compression). `emphasis` = shelf depth (0..12 dB).
//  * Saturator: tanh with first-order antiderivative anti-aliasing (as SaturatorPro), nearly
//    symmetric (odd harmonics, like tape) with a little `bias` for asymmetry.
//  * Speed (7.5 / 15 / 30 ips) sets the head-bump frequency (with the low roll-off below it),
//    the gap-loss lowpass and the wow/flutter rates.
//  * Wow (slow) and flutter (fast + random) modulate a short delay. The modulation is shared by
//    all channels (one transport moves every track), the depth is per channel. The dry path is
//    delayed by the same base delay so Mix stays aligned on average.
//  * Hiss: independent pink noise per channel (0 = off).
//  * Auto gain divides by the small-signal slope (drive * (1 - tanh(bias)^2)).
~synthCreator.value("TapeSat", {|n|
	var in, dry, wet, drive, bias, emphasis, headbump, wow, flutter, hiss, speed, auto, output, mix;
	var bumpFreq, gapFreq, rateScale, wowMod, flutterMod, baseDelay = 0.005;
	var logcosh = { |v| v.abs + (1 + (-2 * v.abs).exp).log - 2.log };   // overflow-safe

	drive    = OceanodeParameterLag.ar(\drive, 6, n, 0, 30, "vf", 1/30, true).dbamp;        // dB
	bias     = OceanodeParameterLag.ar(\bias, 0.1, n, 0, 1, "vf", 1/30, true) * 0.3;
	emphasis = OceanodeParameterLag.ar(\emphasis, 0.5, n, 0, 1, "vf", 1/30, true) * 12;      // dB
	headbump = OceanodeParameterLag.ar(\headbump, 0.5, n, 0, 1, "vf", 1/30, true) * 4;       // dB
	wow      = OceanodeParameterLag.ar(\wow, 0.15, n, 0, 1, "vf", 1/30, true);
	flutter  = OceanodeParameterLag.ar(\flutter, 0.1, n, 0, 1, "vf", 1/30, true);
	hiss     = OceanodeParameterLag.ar(\hiss, 0, n, 0, 1, "vf", 1/30, true);
	speed    = OceanodeParameterDropdown.kr(\speed, 1, 1, "7.5ips:15ips:30ips").asArray[0];   // one transport speed
	auto     = OceanodeParameterDropdown.kr(\autogain, 1, n, "Off:On");
	output   = OceanodeParameterLag.ar(\output, 0, n, -24, 12, "vf", 1/30, true).dbamp;      // dB
	mix      = OceanodeParameterLag.ar(\mix, 1, n, 0, 1, "vf", 1/30, true);

	bumpFreq  = Select.kr(speed, [35, 55, 90]);
	gapFreq   = Select.kr(speed, [11000, 16000, 20000]);
	rateScale = Select.kr(speed, [0.5, 1, 2]);

	// shared transport modulation (normalised to about -1..1)
	wowMod     = SinOsc.ar(0.6 * rateScale, 0, 0.7) + LFNoise2.ar(0.4, 0.3);
	flutterMod = SinOsc.ar(8 * rateScale, 0, 0.6) + LFNoise2.ar(15, 0.4);

	in  = In.ar(OceanodeInput.kr(\in), n);
	dry = DelayN.ar(in, baseDelay, baseDelay);

	wet = n.collect { |i|
		var x, u, u1, du, y, slope, comp, tb = bias[i].tanh;

		// record emphasis -> saturation -> playback de-emphasis
		x = BHiShelf.ar(in[i], 3000, 1, emphasis[i]);
		u  = ((x * drive[i]) + bias[i]).clip(-40, 40);
		u1 = Delay1.ar(u) + (Impulse.ar(0) * u);   // previous sample = current at start: no click from the bias
		du = u - u1;
		y  = Select.ar(du.abs < 0.01, [(logcosh.(u) - logcosh.(u1)) / du, ((u + u1) * 0.5).tanh]);
		y  = LeakDC.ar(y - tb, 0.9995);
		slope = 1 - tb.squared;
		comp  = Select.ar(auto[i], [K2A.ar(1), (drive[i] * slope).reciprocal]);
		y = BHiShelf.ar(y * comp, 3000, 1, emphasis[i].neg);

		// head bump with the low roll-off below it, then gap loss
		y = BPeakEQ.ar(y, bumpFreq, 1, headbump[i]);
		y = BHiPass.ar(y, bumpFreq * 0.4, 0.707);
		y = BLowPass.ar(y, gapFreq.min(SampleRate.ir * 0.45), 0.707);

		// wow / flutter: max excursion 1.2 ms (wow) + 0.1 ms (flutter) around the base delay.
		// Measured pitch deviation: ~0.09 % at the defaults (semi-pro deck), ~0.6 % at wow = flutter = 1.
		y = DelayC.ar(y, baseDelay * 2,
			(baseDelay + (wowMod * wow[i] * 0.0012) + (flutterMod * flutter[i] * 0.0001)).clip(0, baseDelay * 2));

		// hiss: -90 dB at 0+ up to -40 dB at 1, exactly off at 0
		y + (PinkNoise.ar * (hiss[i] * 50 - 90).dbamp * (hiss[i] > 0))
	};

	Out.ar(OceanodeOutput.kr(\out), LinXFade2.ar(dry, wet * output, mix * 2 - 1));

}, description: "Tape machine saturation with emphasis head bump gap loss shared wow and flutter hiss and auto gain", category: "Effect/Distortion");
)
