(
~synthCreator.value("PingPongEcho", {|n, variables|
	var fb, del, cut, res, selfilt, sig, mx, dry, send, spread;
	var fbIn, wets, fbOut;
	
	// Parameters
	fb = OceanodeParameter.ar(\feed, 0.5, n, 0, 1, "vf");
	del = OceanodeParameterLag.ar(\delay, 0.2, n, 0.01, 2, "vf", 1/30, true);
	cut = OceanodeParameterLag.ar(\cutoff, 60, n, 12, 120, "vf", 1/30, true).midicps;
	res = OceanodeParameter.ar(\resonance, 0.2, n, 0, 1, "vf");
	selfilt = OceanodeParameterDropdown.kr(\filtertype, 0, n, "LowPass:HighPass:BandPass:PeakEQ");
	mx = OceanodeParameterLag.ar(\mix, 0, n, 0, 1, "vf", 1/30, true);
	send = OceanodeParameterLag.ar(\send, 1, n, 0, 1, "vf", 1/30, true);
	spread = OceanodeParameterLag.ar(\spread, 1, n, 0, 1, "vf", 1/30, true);
	
	// Input processing
	dry = In.ar(OceanodeInput.kr(\in), n);

	// Multichannel ping-pong: each channel's echo is fed back into the NEXT channel (i -> i+1, last -> first),
	// so with n=2 it is the classic L/R ping-pong and with n>2 the echo travels around all channels.
	// Every channel uses its own parameters. (Before, n was forced to 2 after the parameters were
	// created with n channels, which expanded the graph to n*2 channels; n=1 wrote 2 channels.)
	fbIn = LocalIn.ar(n);

	#wets, fbOut = n.collect { |i|
		var rq = (1 - res[i]).max(0.05); // rq = 0 at resonance 1 made the filters blow up
		var wet, out;

		// Cross-feedback from the previous channel
		wet = LeakDC.ar(fbIn[(i - 1).wrap(0, n - 1)] * fb[i] + (dry[i] * send[i]));

		// Filter options
		wet = Select.ar(selfilt[i], [
			RLPF.ar(wet, cut[i], rq),
			RHPF.ar(wet, cut[i], rq),
			BPF.ar(wet, cut[i], rq),
			BPeakEQ.ar(wet, cut[i], 1, res[i] * 12)
		]);

		// Spread (same level law as the original stereo version)
		out = Balance2.ar(wet, wet, spread[i]).sum;

		[out, DelayC.ar(out, 2, del[i])]
	}.flop;

	// Feedback loop
	LocalOut.ar(fbOut);

	// Mix dry and wet signals
	sig = XFade2.ar(dry, wets, mx * 2 - 1);

	// Output
	Out.ar(OceanodeOutput.kr(\out), sig);
}, description: "Ping-pong echo effect that alternates between left and right channels", category: "Effect/Time/Delay", variables: [], variableDimensions: []);
)