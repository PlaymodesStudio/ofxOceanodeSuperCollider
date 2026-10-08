(
// Limiter — SC Limiter (lookahead brickwall). Fixes: mix defaults to 1, DC removal is applied
// before the split and the dry path is delayed by the limiter's latency, so Mix doesn't comb.
~synthCreator.value("Limiter", {|n|
	var source, sig, dry, level, send, dur = 0.01;

	level = OceanodeParameter.ar(\level, 1, n, 0.01, 1, "vf");
	send  = OceanodeParameterLag.ar(\send, 1, n, 0, 1, "vf", 1/30, true).lincurve(0, 1, 0, 1, 4);

	source = LeakDC.ar(In.ar(OceanodeInput.kr(\in), n) * send, 0.9995);   // ~4 Hz (default 0.995 = 38 Hz HPF)
	sig = Limiter.ar(source, level, dur) * level.reciprocal;
	dry = DelayN.ar(source, 2 * dur, 2 * dur);

	sig = LinXFade2.ar(dry, sig, OceanodeParameter.ar(\mix, 1, n, 0, 1, "vf") * 2 - 1);
	Out.ar(OceanodeOutput.kr(\out), sig);

}, description: "Hard limiter with DC offset removal", category: "Effect/Dynamics");
)
