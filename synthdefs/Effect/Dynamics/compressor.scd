(
// Compressor — same controls as the old Compander version, but the gain is computed per sample.
// (Compander computes its gain once per control block and ramps linearly between blocks, which
//  aliases audibly with fast times — see nathan.ho.name/posts/negative-compression.)
// threshold is linear amplitude; slopebelow < 1 expands/gates below it, slopeabove < 1 compresses above it.
~synthCreator.value("Compressor", {|n|
	var source, sig, thresh, slopebelow, slopeabove, attack, release, makeup, send, env, d, gDb;

	thresh     = OceanodeParameter.ar(\threshold, 0.5, n, 0.01, 1, "vf");
	slopebelow = OceanodeParameter.ar(\slopebelow, 1, n, 0, 1, "vf");
	slopeabove = OceanodeParameter.ar(\slopeabove, 1, n, 0, 1, "vf");
	attack     = OceanodeParameter.ar(\attack, 0.01, n, 0, 0.2, "vf");
	release    = OceanodeParameter.ar(\release, 0.1, n, 0, 2, "vf");
	makeup     = OceanodeParameter.ar(\makeup, 1, n, 1, 8, "vf");
	send       = OceanodeParameterLag.ar(\send, 1, n, 0, 1, "vf", 1/30, true);

	source = In.ar(OceanodeInput.kr(\in), n) * send;

	// same peak follower Compander uses internally, but read every sample
	env = Amplitude.ar(source, attack, release).max(-120.dbamp);
	d   = env.ampdb - thresh.ampdb;
	gDb = (d.min(0) * (slopebelow - 1)) + (d.max(0) * (slopeabove - 1));
	sig = source * gDb.max(-120).dbamp * makeup;

	sig = LinXFade2.ar(source, sig, OceanodeParameter.ar(\mix, 1, n, 0, 1, "vf") * 2 - 1);
	Out.ar(OceanodeOutput.kr(\out), sig);

}, description: "Dynamic range compressor and expander with threshold and slope controls", category: "Effect/Dynamics");
)
