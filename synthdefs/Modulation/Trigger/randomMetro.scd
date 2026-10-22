(
~synthCreator.value("RandomMetro", {|n|
	var range, minms, curve, chance, seed, instanceID, trig, fire, clock, quantize;

	/*
	Triggers at random intervals: each one draws the time to the next,
	random ^ curve * range + minms (ms; curve 1: uniform), sample accurate.
	chance passes each trigger with that probability (the clock keeps
	running). The random event loops of the Reaktor ensemble SpaceSystem-Geiger.
	*/

	range = OceanodeParameterLag.ar(\range, 1000, n, 0, 10000, "vf", 1/30, true);
	minms = OceanodeParameterLag.ar(\minms, 1, n, 0.1, 10000, "vf", 1/30, true);
	curve = OceanodeParameterLag.ar(\curve, 1, n, 0, 40, "vf", 1/30, true);
	chance = OceanodeParameterLag.ar(\chance, 1, n, 0, 1, "vf", 1/30, true);
	fire = OceanodeParameterVoid.ar(\fire);
	quantize = OceanodeParameter.kr(\quantize, 0, 1, 0, 1, "vf");
	clock = In.ar(OceanodeInput.kr(\clock), n);
	seed = OceanodeParameter.kr(\seed, 0, 1, 0, 65536, "i");

	// seed 0: fresh draws (a different stream in every synth), > 0: reproducible
	instanceID = (Date.localtime.rawSeconds * 1000 + 1000.rand).asInteger;
	RandID.ir(instanceID);
	RandSeed.kr((seed > 0) * (Changed.kr(seed) + Impulse.kr(0)), seed.max(1));

	trig = n.collect { |i|
		// Keep the duration arithmetic at demand rate. A multiply followed by add is
		// optimized to MulAdd at audio rate when its operands include audio
		// controls; that tries to run a demand UGen as an audio UGen (crash).
		var random = Dwhite(0, 1, inf) ** curve[i];
		var duration = Dseq([minms[i]], inf) + (Dseq([range[i]], inf) * random);
		var t = TDuty.ar(duration.max(0.1) * 0.001, 0, 1);
		t = Select.ar(quantize > 0.5, [t, clock[i]]);
		t * (TRand.ar(0, 1, t) < chance[i])
	};
	// Default zero preserves the free clock, including its random stream.
	Out.ar(OceanodeOutput.kr(\out), trig + fire);
}, description: "Triggers at random intervals between minms and minms plus range", category: "Modulation/Trigger");
)
