(
~synthCreator.value("ReverseDelay", {|n|
	var input, localin, delay, rate, feedback, sync, chance, mix, wet, output, seed;
	var maxDelay = 2, maxRate = 2, syncTimeout = 3;
	var sr = SampleRate.ir;
	var frames = ((syncTimeout * (1 + maxRate) + 0.1) * 48000).asInteger; // full range up to 48 kHz

	delay = OceanodeParameterLag.ar(\delayms, 500, n, 20, maxDelay * 1000, "vf", 1/30, true) / 1000;
	rate = OceanodeParameterLag.ar(\rate, 1, n, 0.25, maxRate, "vf", 1/30, true);
	feedback = OceanodeParameterLag.ar(\feedback, 0, n, 0, 0.95, "vf", 1/30, true);
	sync = OceanodeParameter.ar(\sync, 0, n, 0, 1, "vi");
	chance = OceanodeParameterLag.ar(\chance, 1, n, 0, 1, "vf", 1/30, true);
	mix = OceanodeParameterLag.ar(\mix, 0.5, n, 0, 1, "vf", 1/30, true);

	input = In.ar(OceanodeInput.kr(\in), n).asArray;
	localin = Sanitize.ar(LocalIn.ar(n)).asArray;
	seed = Rand(0, 1000); // one per synth: channels with the same triggers make the same chance decision

	// Every trigger starts a new chunk on the other of two heads: the head reads backwards
	// from the moment of its trigger (the chunk just recorded) until the next trigger, while
	// the previous head fades out. Triggers come from Sync; without Sync for `syncTimeout`
	// seconds they come every Delayms instead. Each chunk plays with probability Chance, decided
	// from the chunk number so all channels agree.
	wet = n.collect{|i|
		var buf = LocalBuf(frames, 1).clear;
		var writePos = Phasor.ar(0, 1, 0, BufFrames.ir(buf));
		var syncTrig = Trig1.ar(sync.asArray[i] > 0.5, SampleDur.ir);
		var sinceSync = Sweep.ar(syncTrig, 1) + ((PulseCount.ar(syncTrig) < 1) * syncTimeout);
		var trig = syncTrig + (Impulse.ar(1 / delay[i]) * (sinceSync >= syncTimeout));
		var count = PulseCount.ar(trig);
		BufWr.ar(input[i] + (localin[i] * feedback[i]), buf, writePos);
		[1, 0].collect{|parity|
			var active = (count > 0) * ((count % 2) eq: parity);
			var headTrig = trig * active;
			var spd = Latch.ar(rate[i], headTrig);
			var dice = ((count * 12.9898 + seed).sin * 43758.5453).frac.abs;
			var plays = Latch.ar((dice < chance[i]) + (chance[i] >= 0.999) > 0, headTrig); // 1 = always
			var dist = (Sweep.ar(headTrig, 1) * sr * (1 + spd) + 2).min(BufFrames.ir(buf) - 4);
			BufRd.ar(1, buf, (writePos - dist).wrap(0, BufFrames.ir(buf)), 1, 4)
				* EnvGen.ar(Env.asr(0.01, 1, 0.01), active) * plays;
		}.sum;
	};

	LocalOut.ar(wet);

	output = n.collect{|i| XFade2.ar(input[i], wet[i], mix[i] * 2 - 1) };
	Out.ar(OceanodeOutput.kr(\out), output);
}, description: "Reverse delay: plays back chunks of the input reversed. Chunks last Delayms or follow the Sync triggers and play with probability Chance", category: "Effect/Time/Delay");
)
