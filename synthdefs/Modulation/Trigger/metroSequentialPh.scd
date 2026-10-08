(
// ------------------------------------------------------------
// MetroSequentialPh
// Generates n sequential impulses per phasor cycle.
// An index vector defines which output channel receives each impulse:
//   e.g. index=[0,1,2,...] triggers channels in ascending order,
//        index=[8,1,20,4,...] sends impulse 0→ch8, 1→ch1, 2→ch20, etc.
// ------------------------------------------------------------
~synthCreator.value("MetroSequentialPh", {|n|
	var signals;
	var phasorIn, stepNum, trig, chance;
	var seed, reseed, seedKr, reseedKr;
	var sequenceVector, numSteps;
	var currentStep, targetChannel;
	var perChannelRand, perChannelSeeded;
	var instanceID;
	var maxVectorSize = 64;

	// --- PHASOR INPUT ---
	// Single-channel AR phasor (0..1, wrapping)
	phasorIn = In.ar(OceanodeInput.kr(\in), 1);

	// --- PARAMETERS ---
	chance         = OceanodeParameterLag.ar(\chance, 1, n, 0, 1, "vf", 1/30, true);
	seed           = OceanodeParameter.kr(\seed, 0, 1, 0, 65536, "i");
	reseed         = OceanodeParameter.kr(\reseed, 0, 1, 0, 1, "vi");
	numSteps       = OceanodeParameter.kr(\numsteps, n, 1, 1, maxVectorSize, "vi");
	sequenceVector = OceanodeParameter.ar(\index, 0, maxVectorSize, 0, n-1, "vf");

	seedKr     = seed;
	reseedKr   = reseed;
	instanceID = (Date.localtime.rawSeconds * 1000 + 1000.rand).asInteger;

	// --- STEP DETECTION ---
	// Scale 0..1 phasor into discrete integer steps: 0, 1, ..., numSteps-1
	stepNum = (phasorIn.clip(0.0, 1.0) * K2A.ar(numSteps)).floor;
	stepNum = stepNum.clip(0, K2A.ar(numSteps) - 1);

	// Fire a single-sample trigger on each step boundary crossing
	// (includes the wrap from step numSteps-1 → 0 at the phasor cycle boundary)
	trig = Changed.ar(stepNum);

	// Capture the current step index at the time of the trigger
	currentStep = stepNum.floor.clip(0, maxVectorSize - 1);

	// --- INDEX LOOKUP ---
	// Map step index → output channel via the sequenceVector
	targetChannel = Select.ar(currentStep, sequenceVector);
	targetChannel = targetChannel.round(1).clip(0, n - 1);

	// --- PER-CHANNEL RANDOM (for chance) ---
	perChannelRand = Array.fill(n, {|i|
		RandID.ir(instanceID + (i * 1000));
		TRand.ar(0, 1, trig);
	});

	perChannelSeeded = Array.fill(n, {|i|
		RandID.ir(seedKr + i);
		RandSeed.kr(
			(seedKr > 0) * (Changed.kr(seedKr) + (reseedKr > 0.5) + Impulse.kr(0)),
			(seedKr + i).max(1)
		);
		TRand.ar(0, 1, trig);
	});

	// --- OUTPUT DISTRIBUTION ---
	signals = Array.fill(n, {|outputChan|
		var isTarget = (targetChannel - outputChan).abs < 0.1;
		var thisChannelRand = Select.ar(
			K2A.ar(seedKr > 0),
			[perChannelRand[outputChan], perChannelSeeded[outputChan]]
		);
		var chance_test = BinaryOpUGen.new('<', thisChannelRand, chance.asArray[outputChan]);
		trig * isTarget * chance_test;
	});

	Out.ar(OceanodeOutput.kr(\out), signals);
},
description: "Phasor-driven sequential trigger: n impulses per cycle, channel order set by index vector",
category: "Modulation/Trigger");
)
