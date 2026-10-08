(
~synthCreator.value("BeatRepeat", {|n|
	var inputAll, sigAll;
	var onoffA = OceanodeParameter.ar(\go, 0, n, 0, 1, "vi");
	var recmsA = OceanodeParameter.ar(\repms, 1000, n, 1, 3000, "vf");
	var adaptA = OceanodeParameter.ar(\adaptlength, 0, n, 0, 1, "vi");
	var offsetA = OceanodeParameter.ar(\offset, 0, n, 0, 1000, "vf") / 1000.0;
	var gateA = OceanodeParameter.ar(\gate, 1, n, 0, 1, "vf");
	var declickA = OceanodeParameter.ar(\declick, 0.005, n, 0, 0.1, "vf");
	var rateA = OceanodeParameter.ar(\rate, 1, n, 0, 4, "vf");
	var reverseA = OceanodeParameter.ar(\reverse, 0, n, 0, 1, "vi");
	var pitchstepA = OceanodeParameter.ar(\pitchstep, 0, n, -12, 12, "vf");
	var numstepsA = OceanodeParameter.ar(\numpitchsteps, 12, n, 0, 24, "vi");
	var grainampA = OceanodeParameter.ar(\grainamp, 1, n, 0, 1, "vf");
	var resetA = OceanodeParameter.ar(\reset, 0, n, 0, 1, "vi");
	var drywetA = OceanodeParameter.ar(\mix, 1, n, 0, 1, "vf");
	var levelsA = OceanodeParameterLag.ar(\levels, 1, n, 0, 1, "vf", 1/50, true);

	inputAll = In.ar(OceanodeInput.kr(\in), n);

	// Each channel is an independent voice with its own mono buffer and timing
	// (parameters are per channel, so a shared multichannel buffer expanded into n*n signals)
	sigAll = n.collect { |c|
		var input, buffer, output, loopOutput;
		var bufferSize, numChannels;
		var writePos, readPos, bufFrames, offsetSamples, loopSamples;
		var recording, declickEnv, gateEnv;
		var loopDur, loopReset, loopPhase, gatePhase;
		var envtime, effectiveRate, loopCount, basePitchShift;
		var onoffTrig, resetTrig, resetPulse, resetGate;
		var pitchCounter, accumulatedPitch;
		var gatedDrySignal, gatedWetSignal, transitionOutput;
		var transitionEnv, fadeEnv;
		var recordReset, combinedResetTrig;
		var delayedResetTrig, delayedGoTrig, immediateLoopReset, delayedLoopReset;
		var recordingNewSegment, initialRecording, anyRecording, effectiveLoopOutput, resetPulseCount, resetCountChanged;
		var onoff = onoffA[c], recms = recmsA[c], adapt = adaptA[c], offset = offsetA[c], gate = gateA[c], declick = declickA[c], rate = rateA[c], reverse = reverseA[c], pitchstep = pitchstepA[c], numsteps = numstepsA[c], grainamp = grainampA[c], reset = resetA[c], drywet = drywetA[c], levels = levelsA[c];

		// Setup
		bufferSize = 8;
		numChannels = 1; // one mono voice per channel
		bufFrames = (bufferSize * SampleRate.ir).max(1);
		buffer = LocalBuf(bufFrames, numChannels).clear;
		input = inputAll[c];

		// Calculate loop duration from repms parameter
		loopDur = recms / 1000;
		loopSamples = (loopDur * SampleRate.ir).round(1).max(1);

		// Reset detection - FIXED: detect each individual "1" value, including consecutive ones
		// Count pulses and detect when count changes = new pulse received
		 resetPulseCount = PulseCount.ar(reset > 0.5);
		 resetCountChanged = Changed.ar(resetPulseCount);
		resetTrig = resetCountChanged;

		// Timing triggers
		onoffTrig = Trig.kr(onoff);
		combinedResetTrig = onoffTrig + resetTrig;

		// FIXED: Recording logic - ensure we capture fresh audio when go=1 is triggered
		// Record when go=0 OR when go trigger fires OR when reset is triggered
		recording = (1 - onoff) + Trig1.kr(onoffTrig, loopDur) + Trig1.ar(resetTrig, loopDur);
		recordReset = onoffTrig + A2K.kr(resetTrig);

		// Write position - continuous writing when recording
		writePos = Phasor.ar(recordReset, BufRateScale.kr(buffer) * recording, 0, BufFrames.kr(buffer));
		BufWr.ar(input, buffer, writePos);

		// FIXED: When go first goes from 0->1, we need to ensure there's audio to play
		// Delay the initial go trigger by loopDur to allow initial recording
		delayedGoTrig = DelayN.kr(onoffTrig, 3, loopDur); // constant max (repms max 3000 ms): loopDur only sized it at start

		// FIXED: Loop management - handle both initial go and reset differently
		// For initial go trigger: delay to allow recording first
		// For reset trigger: delay the loop reset by loopDur to allow recording to complete
		delayedResetTrig = DelayN.kr(resetTrig, 3, loopDur);
		immediateLoopReset = delayedGoTrig; // Changed from onoffTrig
		delayedLoopReset = delayedResetTrig;

		loopReset = Trig1.ar(K2A.ar(immediateLoopReset + delayedLoopReset), ControlDur.ir);

		// Phase for timing loop cycles
		loopPhase = Phasor.ar(loopReset, 1 / (SampleRate.ir * loopDur), 0, 1, 0);

		// Detect loop boundaries for continuous cycling
		loopReset = loopReset + Trig1.ar((loopPhase - Delay1.ar(loopPhase)) > 0, ControlDur.ir);

		// FIXED: Track when we're in "recording new segment" mode
		recordingNewSegment = Trig1.ar(resetTrig, loopDur);

		// FIXED: Also track when we're in initial recording mode (go just triggered)
		initialRecording = Trig1.kr(onoffTrig, loopDur) * onoff;

		// Pitch accumulation - use the corrected loop reset timing
		pitchCounter = PulseCount.ar(loopReset, K2A.ar(delayedGoTrig));
		loopCount = pitchCounter % K2A.ar(numsteps);
		accumulatedPitch = loopCount * K2A.ar(pitchstep);
		basePitchShift = 2 ** (accumulatedPitch / 12);

		// Rate calculation - FIXED: much simpler approach
		effectiveRate = K2A.ar(rate) * basePitchShift;

		// Apply reverse - simple conditional
		effectiveRate = Select.ar(reverse, [effectiveRate, effectiveRate * (-1)]);

		// Quantize at loop boundaries
		effectiveRate = Latch.kr(effectiveRate, loopReset);
		offsetSamples = Latch.kr((offset * SampleRate.ir).round(1), loopReset);
		gate = Latch.kr(gate, loopReset);
		grainamp = Latch.kr(grainamp, loopReset);

		// Read position - MUCH simpler, following Stutter pattern exactly
		readPos = Phasor.ar(
			trig: loopReset,
			rate: BufRateScale.kr(buffer) * K2A.ar(effectiveRate),
			start: K2A.ar(reverse) * K2A.ar(loopSamples),
			end: (1 - K2A.ar(reverse)) * K2A.ar(loopSamples),
			resetPos: K2A.ar(reverse) * K2A.ar(loopSamples)
		);

		// FIXED: Simple offset - read from recorded segment
		readPos = (writePos - K2A.ar(loopSamples) - K2A.ar(offsetSamples) + readPos).wrap(0, BufFrames.kr(buffer) - 1);

		// Gate control
		gatePhase = Phasor.ar(K2A.ar(onoffTrig), 1 / (SampleRate.ir * loopDur), 0, 1, 0);

		// Envelope - simplified
		declickEnv = EnvGen.ar(
			Env(
				levels: [0, 1, 1, 0],
				times: [K2A.ar(declick), K2A.ar(loopDur - (2 * declick)).max(0), K2A.ar(declick)],
				curve: [\sin, \step, \sin]
			),
			gate: loopReset
		);

		gateEnv = (gatePhase < K2A.ar(gate)).lag(K2A.ar(declick.min(loopDur / 4)));

		// Audio processing - FIXED: play realtime input during any recording phase
		loopOutput = BufRd.ar(numChannels, buffer, readPos, loop: 1);
		loopOutput = loopOutput * K2A.ar(onoff) * declickEnv * K2A.ar(grainamp);

		// FIXED: During any recording phase (initial or reset), play realtime input
		anyRecording = recordingNewSegment + initialRecording;
		effectiveLoopOutput = Select.ar(K2A.ar(anyRecording > 0), [loopOutput, input]);

		gatedDrySignal = input * gateEnv;
		gatedWetSignal = effectiveLoopOutput * gateEnv;

		fadeEnv = EnvGen.ar(
			Env.asr(K2A.ar(declick), 1, K2A.ar(declick), 'sine'),
			gate: K2A.ar(onoff)
		);

		transitionEnv = EnvGen.ar(
			Env.asr(K2A.ar(declick), 1, K2A.ar(declick), 'sine'),
			gate: K2A.ar(onoff)
		);

		transitionOutput = XFade2.ar(
			gatedDrySignal,
			gatedWetSignal,
			transitionEnv * 2 - 1
		);

		// Final output
		output = SelectX.ar(fadeEnv, [input, transitionOutput]);
		output = XFade2.ar(input, output, drywet * 2 - 1);

		output * levels
	};

	Out.ar(OceanodeOutput.kr(\out), sigAll);

}, description: "Beat repeater effect with accumulative pitch shifting", category: "Effect/Time/Looping");
)