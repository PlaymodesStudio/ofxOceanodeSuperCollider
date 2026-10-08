(
~synthCreator.value("Sidechain", {|n|
	var source, control, sig, compressorSig, legacySig, legacyEnv;
	var thresh, ratio, attack, release, knee, amplitudeDb, over, xk, gainDb, send, mix;
	var mode, strength, smooth;

	// Compressor parameters. Attack/release ranges also cover the wider legacy
	// SideChain13 controls when legacy mode is selected.
	thresh   = OceanodeParameter.ar(\threshold, -6, n, -60, 0, "vf");
	ratio    = OceanodeParameter.ar(\ratio, 4, n, 1, 60, "vf").max(1);    // was 0..60: ratio 0 divided by zero
	attack   = OceanodeParameter.ar(\attack, 0.005, n, 0, 1, "vf");
	release  = OceanodeParameter.ar(\release, 0.1, n, 0, 3, "vf");
	knee     = OceanodeParameter.ar(\knee, 0, n, 0, 24, "vf").max(0.01);
	send     = OceanodeParameterLag.ar(\send, 1, n, 0, 1, "vf", 1/30, true).lincurve(0, 1, 0, 1, 4);
	mix      = OceanodeParameter.ar(\mix, 1, n, 0, 1, "vf");                // was 0 = compressor bypassed
	mode     = OceanodeParameterDropdown.kr(\mode, 0, n, "Compressor:Legacy");
	strength = OceanodeParameterLag.ar(\strength, 1, n, 0, 10, "vf", 1/30, true);
	smooth   = OceanodeParameterLag.ar(\smooth, 0.01, n, 0, 1, "vf", 1/30, true);

	source  = In.ar(OceanodeInput.kr(\in), n) * send;
	control = In.ar(OceanodeInput.kr(\control), n);

	amplitudeDb = Amplitude.ar(control, attack, release).max(-120.dbamp).ampdb;
	over   = amplitudeDb - thresh;
	xk     = (over + (knee * 0.5)).clip(0, knee);
	gainDb = (1 - ratio.reciprocal) * ((xk.squared / (2 * knee)) + (over - (knee * 0.5)).max(0));
	compressorSig = LinXFade2.ar(source, source * gainDb.neg.dbamp, mix * 2 - 1);

	// Compatibility path for SideChain13
	legacyEnv = Lag.ar(Amplitude.ar(control, attack, release), smooth);
	legacySig = source * (1 - (legacyEnv * strength).clip(0, 1));
	sig = Select.ar(mode, [compressorSig, legacySig]);

	Out.ar(OceanodeOutput.kr(\out), sig);

}, description: "Sidechain compression using a control signal", category: "Effect/Dynamics");
)
