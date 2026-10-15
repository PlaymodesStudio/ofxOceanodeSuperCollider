// ------------------------------------------------------------
// MetroChoose
// Metro whose interval is a weighted random choice from a list of durations
// in beats (Choose + Metro).
//   values  = available intervals in beats (1 = one beat at bpm, 0.5 = half...)
//   weights = relative probability of each value (only the first `size`
//             count; all zero = uniform)
//   mode    = Free:    every pulse picks the next interval. With probability
//                      repeat[i] a pick of value i is held for repeatn[i]
//                      more pulses (one entry per value).
//             Trigger: each trigger picks an interval and restarts the
//                      pulse train with a pulse, looping the interval until
//                      the next trigger.
//   reset   = restart the pulse train (Free mode also picks again)
// Outputs: out      = every pulse
//          firstout = the first pulse after a new interval is picked
// ------------------------------------------------------------
~synthCreator.value("MetroChoose", {|n|
	var mode, trigger, bpm, size, values, weights, repeat, repeatN, chance, reset, seed, reseed, duplicate;
	var reseedK, reseedTrig, startTrig, resetTrig, chooseTrig, totalWeight, normWeights, cumulativeWeights;
	var beatDur, secs, freeChain, freeTrigs, randomValue, beats, holdTrigs, trigs, gate;
	var maxSize = 24;
	var minDur = 0.002; // seconds

	// ————— 1) parameters —————
	mode      = OceanodeParameterDropdown.kr(\mode, 0, 1, "Free:Trigger");
	trigger   = OceanodeParameter.ar(\trigger, 0, n, 0, 1, "vi").asArray.collect {|x| x > 0.5 };
	bpm       = OceanodeParameterLag.ar(\bpm, 120, 1, 1, 999, "vf", 1/30, true);
	size      = OceanodeParameter.kr(\size, 4, 1, 1, maxSize, "vi");
	values    = OceanodeParameter.ar(\values, 1, maxSize, 0, 64, "vf");
	weights   = OceanodeParameter.ar(\weights, 1, maxSize, 0, 1, "vf");
	repeat    = OceanodeParameter.ar(\repeat, 0, maxSize, 0, 1, "vf");
	repeatN   = OceanodeParameter.ar(\repeatn, 1, maxSize, 0, 64, "vi");
	chance    = OceanodeParameter.ar(\chance, 1, n, 0, 1, "vf");
	reset     = OceanodeParameter.ar(\reset, 0, n, 0, 1, "vi").asArray.collect {|x| x > 0.5 };
	duplicate = OceanodeParameterDropdown.kr(\duplicate, 0, 1, "Individual:Shared");
	seed      = OceanodeParameter.kr(\seed, 0, 1, 0, 65536, "i");
	reseed    = OceanodeParameter.ar(\reseed, 0, 1, 0, 1, "vi").asArray[0] > 0.5;

	// ————— 2) seeding: one generator for the whole synth —————
	// The reseed is detected once per control block so RandSeed (kr) and the
	// restart pulse (ar, at the block's first sample) land in the same block:
	// the generator is reseeded before TDuty pulls its first pick. (A 1-sample
	// audio pulse would fall mid-block, where A2K never sees it.)
	reseedK = Trig.kr(A2K.kr(reseed), ControlDur.ir);
	reseedTrig = T2A.ar(reseedK);
	RandID.ir(seed + 23);
	RandSeed.kr(
		(seed > 0) * (Changed.kr(seed) + reseedK + Impulse.kr(0)),
		(seed + 23).max(1)
	);

	// Shared: channel 0's trigger / reset drive every channel
	startTrig = Impulse.ar(0);
	resetTrig = Array.fill(n, {|i| Trig.ar(reset[i], SampleDur.ir) + reseedTrig });
	resetTrig = Select.ar(K2A.ar(duplicate), [resetTrig, resetTrig[0].dup(n)]).asArray;
	chooseTrig = Array.fill(n, {|i| Trig.ar(trigger[i], SampleDur.ir) + reseedTrig + startTrig });
	chooseTrig = Select.ar(K2A.ar(duplicate), [chooseTrig, chooseTrig[0].dup(n)]).asArray;

	// ————— 3) weights over the first `size` values (all zero: uniform) —————
	weights = weights.collect {|w, i| w.max(0) * (i < size) };
	totalWeight = weights.sum;
	weights = weights.collect {|w, i| Select.ar(totalWeight > 0, [K2A.ar(i < size), w]) };
	normWeights = weights / weights.sum.max(0.000001);
	cumulativeWeights = normWeights.integrate;

	beatDur = 60 / bpm.asArray[0].max(1);
	secs = values.collect {|v| (v * beatDur).max(minDur) };

	// ————— 4) Free mode: TDuty picks the next interval on every pulse —————
	// The same stream feeds TDuty's dur and level, which it pulls once each
	// per pulse, so it carries [seconds, flag] pairs: flag 2 on a new pick, 1
	// on its repeats. Picking value i plays [secs, 2] and then, with
	// probability repeat[i], repeatn[i] more [secs, 1] pairs.
	// (The repeat coin chooses between two sub-patterns: Dser reads a
	// demand-rate length only once, so the count itself cannot be random.)
	freeChain = {|resetIn|
		var picks = Array.fill(maxSize, {|i|
			var r = repeat[i].clip(0, 1);
			var repeats = Dwrand([Dser([0], 0), Dser([secs[i], 1], repeatN[i].round.max(0) * 2)], [1 - r, r], 1);
			Dseq([secs[i], 2, repeats], 1)
		});
		var stream = Dwrand(picks, normWeights, inf);
		TDuty.ar(stream, resetIn, stream);
	};
	freeTrigs = Select.ar(K2A.ar(duplicate), [
		Array.fill(n, {|i| freeChain.(resetTrig[i]) }),
		freeChain.(resetTrig[0]).dup(n)
	]).asArray;

	// ————— 5) Trigger mode: a phasor looping the interval each trigger picks —————
	randomValue = Array.fill(n, {|i| TRand.ar(0, 1, chooseTrig[i]) });
	randomValue = Select.ar(K2A.ar(duplicate), [randomValue, randomValue[0].dup(n)]).asArray;
	beats = Array.fill(n, {|i|
		var index = (cumulativeWeights <= randomValue[i]).sum.min(size - 1);
		Latch.ar(Select.ar(index, values), chooseTrig[i]);
	});
	holdTrigs = Array.fill(n, {|i|
		var restartTrig = Trig.ar(resetTrig[i] + chooseTrig[i], SampleDur.ir);
		var rate = SampleDur.ir / (beats[i] * beatDur).max(minDur);
		var phase = Phasor.ar(restartTrig, rate, 0, 1, 0);
		var pulse = ((HPZ1.ar(phase) < 0) + restartTrig).min(1);
		// a pick pulses at once, so its first pulse is the trigger itself
		pulse * (1 + (chooseTrig[i] > 0));
	});

	// ————— 6) mode, chance gate (a dropped pulse is dropped on both outputs) —————
	trigs = Select.ar(K2A.ar(mode), [freeTrigs, holdTrigs]).asArray;
	gate = Array.fill(n, {|i| TRand.ar(0, 1, trigs[i] > 0) < chance.asArray[i] });

	Out.ar(OceanodeOutput.kr(\out), Array.fill(n, {|i| (trigs[i] > 0) * gate[i] }));
	Out.ar(OceanodeOutput.kr(\firstout), Array.fill(n, {|i| (trigs[i] > 1.5) * gate[i] }));
},
description: "Metro picking weighted random beat durations: a new one every pulse (with per-value repeats) or one per trigger, looped until the next",
category: "Modulation/Trigger");
