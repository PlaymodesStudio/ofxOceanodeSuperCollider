(
~synthCreator.value("HardClip", {|n|
	var dry, dcCorrected, wet, sig, drive, clip, level, mix;

	// Parameters
	drive = OceanodeParameter.ar(\drive, 10, n, 1, 100, "vf");
	clip = OceanodeParameter.ar(\clip, 0.8, n, 0.01, 1, "vf");
	level = OceanodeParameter.ar(\level, 1, n, 0, 1, "vf").lincurve(0, 1, 0, 1, 4);
	mix = OceanodeParameter.ar(\mix, 1, n, 0, 1, "vf");

	// Get input signal
	dry = In.ar(OceanodeInput.kr(\in), n);

	// Apply DC correction
	dcCorrected = LeakDC.ar(dry);

	// Apply extreme hard clip distortion
	wet = (dcCorrected * drive).clip2(clip);

	// Apply level control (no limiter on the wet path alone: its 10 ms latency
	// misaligned dry and wet and caused comb filtering at intermediate mix values)
	wet = wet * level;

	// Mix dry and wet signals
	sig = XFade2.ar(dry, wet, mix*2-1);

	// Final safety limiter
	sig = Limiter.ar(sig, 0.99);

	// Output
	Out.ar(OceanodeOutput.kr(\out), sig);
}, description: "Hard clipping distortion effect with drive and clip controls", category: "Effect/Distortion");
)