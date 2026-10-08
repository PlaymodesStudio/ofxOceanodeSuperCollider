(
// ------------------------------------------------------------
// MetroSequentialIndex (Robust Reset Forcing)
// ------------------------------------------------------------
~synthCreator.value("MetroSequentialIndex", {|n|
	var signals;
	var hz, chance, reset, seed, reseed, fixednum, random;
	var sequenceVector, numSteps;
	var normalRand, seededRand, instanceID, combinedReset;
	var perChannelRand, perChannelSeeded;
	var shouldTrigger, totalTriggersSent;
	var randomStep, currentStep, targetChannel;
	var seedKr, reseedKr, randomKr, invertKr, invert;
	var trigWithReset;
	var phase, phaseRate, rawTrig;
	var maxVectorSize = 64;

	// --- PARAMETERS ---
	hz       = OceanodeParameterLag.ar(\hz, 1, n, 0.01, 60, "vf", 1/30, true);
	chance   = OceanodeParameterLag.ar(\chance, 1, n, 0, 1, "vf", 1/30, true);
	reset    = OceanodeParameter.ar(\reset, 0, n, 0, 1, "vi");
	seed     = OceanodeParameter.kr(\seed, 0, 1, 0, 65536, "i");
	reseed   = OceanodeParameter.kr(\reseed, 0, 1, 0, 1, "vi");
	fixednum = OceanodeParameter.ar(\fixednum, 0, 1, 0, 1000, "vi");
	random   = OceanodeParameter.kr(\random, 0, 1, 0, 1, "b");
	numSteps = OceanodeParameter.kr(\numsteps, n, 1, 1, maxVectorSize, "vi");
	invert   = OceanodeParameter.kr(\invert, 0, 1, 0, 1, "b");
	sequenceVector = OceanodeParameter.ar(\index, 0, maxVectorSize, 0, n-1, "vf");

	seedKr   = seed;
	reseedKr = reseed;
	randomKr = random;
	invertKr = invert;

	instanceID = (Date.localtime.rawSeconds * 1000 + 1000.rand).asInteger;

	// --- CLOCK LOGIC ---
	// combinedReset is 1 when we manually reset
	combinedReset = Trig.ar(K2A.ar(reset.asArray[0]) + K2A.ar(reseedKr > 0.5), SampleDur.ir);

	phaseRate = hz.asArray[0] * SampleDur.ir;
	phase = Phasor.ar(combinedReset, phaseRate, 0, 1, 0);
	rawTrig = (HPZ1.ar(phase) < 0) + combinedReset;

	// --- COUNTER ---
	// PulseCount: Reset=0, Next=1, Next=2...
	totalTriggersSent = PulseCount.ar(rawTrig, combinedReset);

	// --- TRIGGER GATE (The Fix) ---
	// 1. If fixednum == 0, Always allow.
	// 2. If fixednum > 0:
	//    Allow if count < fixednum OR if this is the Reset Frame.
	//    (Forcing on Reset ensures the first beat is never skipped/silenced).
	shouldTrigger = Select.ar(K2A.ar(fixednum > 0), [
		K2A.ar(1),
		((totalTriggersSent < fixednum) + (combinedReset > 0)) > 0
	]);

	trigWithReset = rawTrig * shouldTrigger;

	// --- INDEX LOGIC (The Fix) ---
	// 1. Calculate step from counter
	currentStep = totalTriggersSent % numSteps.max(1);

	// 2. Force Index 0 on Reset Frame
	// This ensures we start on Channel 1 immediately, overriding any counter lag.
	currentStep = Select.ar(combinedReset > 0, [currentStep, K2A.ar(0)]);

	// Invert Step Logic
	currentStep = Select.ar(K2A.ar(invertKr > 0.5), [
		currentStep,
		(numSteps - 1) - currentStep
	]);

	// Random Step Logic
	RandID.ir(seedKr + 23);
	RandSeed.kr(
		(seedKr > 0) * (Changed.kr(seedKr) + (reseedKr > 0.5) + Impulse.kr(0)),
		(seedKr + 23).max(1)
	);
	randomStep = TIRand.ar(0, numSteps - 1, trigWithReset);

	currentStep = Select.ar(K2A.ar(randomKr > 0.5), [currentStep, randomStep]);
	currentStep = currentStep.floor.clip(0, maxVectorSize - 1);

	// --- INDEX LOOKUP ---
	targetChannel = Select.ar(currentStep, sequenceVector);

	// --- OUTPUT DISTRIBUTION ---
	perChannelRand = Array.fill(n, {|i|
		RandID.ir(instanceID + (i * 1000));
		TRand.ar(0, 1, trigWithReset);
	});
	perChannelSeeded = Array.fill(n, {|i|
		RandID.ir(seedKr + i);
		RandSeed.kr(
			(seedKr > 0) * (Changed.kr(seedKr) + (reseedKr > 0.5) + Impulse.kr(0)),
			(seedKr + i).max(1)
		);
		TRand.ar(0, 1, trigWithReset);
	});

	signals = Array.fill(n, {|outputChan|
		var isTarget, thisChannelRand, chance_test;
		isTarget = (targetChannel - outputChan).abs < 0.1;
		thisChannelRand = Select.ar(
			K2A.ar(seedKr > 0),
			[perChannelRand[outputChan], perChannelSeeded[outputChan]]
		);
		chance_test = BinaryOpUGen.new('<', thisChannelRand, chance.asArray[outputChan]);
		trigWithReset * isTarget * chance_test;
	});

	Out.ar(OceanodeOutput.kr(\out), signals);
}, description: "Metro with Vector Index input (sequencable channel order)", category: "Modulation/Trigger");
)