(
~synthCreator.value("Stutter", {|n|
	var inputAll, sigAll;
	var onA = OceanodeParameter.ar(\on, 0, n, 0, 1, "vi");
	var pitchA = OceanodeParameterLag.ar(\pitch, 12, n, 0, 128, "vf");
	var offsetA = OceanodeParameterLag.ar(\offset, 0, n, 0, 1000, "vf") / 1000.0;
	var gateWidthA = OceanodeParameterLag.ar(\gatepw, 1, n, 0, 1, "vf");
	var declickA = OceanodeParameterLag.ar(\declick, 0.005, n, 0, 0.1, "vf");
	var reverseA = OceanodeParameter.ar(\reverse, 0, n, 0, 1, "vi");
	var resetA = OceanodeParameter.ar(\reset, 0, n, 0, 1, "vi");
	var sendA = OceanodeParameterLag.ar(\send, 1, n, 0, 4, "vf", 1/30, true); // New send parameter
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
		var mix, play, wetGate, mixedOutput;
		var loopDur, loopReset, loopPhase, gatePhase;
		var envtime, effectiveRate;
		var combinedResetTrig, delayedOnoff, onoffTrig;
		var gatedDrySignal, gatedWetSignal, transitionOutput;
		var onoffDelay, transitionEnv, fadeEnv;
		var recordTrig, recordReset, resetTrig, resetPulse, resetGate, processedInput;
		var on = onA[c], pitch = pitchA[c], offset = offsetA[c], gateWidth = gateWidthA[c], declick = declickA[c], reverse = reverseA[c], reset = resetA[c], send = sendA[c], drywet = drywetA[c], levels = levelsA[c];

		// Calculate loop duration from MIDI pitch
		loopDur = pitch.midicps.reciprocal;

		// Setup
		bufferSize = 8; // 8 seconds buffer should be enough
		numChannels = 1; // one mono voice per channel
		bufFrames = (bufferSize * SampleRate.ir).max(1);
		buffer = LocalBuf(bufFrames, numChannels).clear;
		input = inputAll[c];

		// Initialize timing control signals
		onoffTrig = Trig.kr(on);
		onoffDelay = loopDur * on;
		delayedOnoff = DelayN.kr(on, 0.13, onoffDelay); // constant max (loop at pitch 0 = 0.122 s): loopDur only sized it at start

		// Reset detection - Use EnvGen approach like GateDuration to handle audio-rate pulses
		resetPulse = Trig1.ar(reset > 0.5, 0.001); // Detect brief audio pulses
		resetGate = EnvGen.ar(
			Env.new([0, 1, 1, 0], [0, 0.01, 0], \lin), // 10ms sustained gate
			gate: resetPulse
		);
		resetTrig = Trig.kr(A2K.kr(resetGate)); // Now convert the sustained gate to control-rate

		// Reset trigger for combined operations
		combinedResetTrig = Trig1.ar(K2A.ar(delayedOnoff) + K2A.ar(reset), ControlDur.ir);

		// Recording logic
		recordTrig = 1 - on;
		recordReset = Trig.kr(on) + resetTrig; // Also reset recording on reset trigger

		// Enable recording when not in stutter mode or when reset is triggered
		recording = (1 - delayedOnoff) + Trig1.kr(recordTrig, loopDur) + Trig1.kr(resetTrig, loopDur);

		// Apply send gain to input before processing
		processedInput = input * send;

		writePos = Phasor.ar(recordReset, BufRateScale.kr(buffer) * recording, 0, BufFrames.kr(buffer));
		BufWr.ar(processedInput, buffer, writePos);

		// Phases for loop management
		loopPhase = Phasor.ar(combinedResetTrig, 1 / (SampleRate.ir * loopDur), 0, 1, 0);
		gatePhase = Phasor.ar(onoffTrig, 1 / (SampleRate.ir * loopDur), 0, 1, 0);

		loopReset = Trig1.ar((loopPhase - Delay1.ar(loopPhase)) > 0, ControlDur.ir);

		// Calculate effective rate (normal or reverse)
		effectiveRate = 1 * (1 - (reverse * 2));

		loopSamples = (loopDur * SampleRate.ir).round(1).max(1);

		// Quantize parameters at loop reset points
		gateWidth = Latch.kr(gateWidth, loopReset);
		offsetSamples = Latch.kr((offset * SampleRate.ir).round(1), loopReset);

		// Create read position for buffer playback
		readPos = Phasor.ar(
			trig: loopReset,
			rate: BufRateScale.kr(buffer) * effectiveRate,
			start: reverse * loopSamples,
			end: (1 - reverse) * loopSamples,
			resetPos: reverse * loopSamples
		);
		readPos = writePos - offsetSamples - loopSamples + readPos;
		readPos = readPos.wrap(0, BufFrames.kr(buffer) - 1);

		mix = delayedOnoff;
		wetGate = delayedOnoff;
		play = delayedOnoff;

		// Create declicking envelope
		envtime = (loopDur - (2 * declick)).max(0);
		declickEnv = EnvGen.ar(
			Env(
				levels: [0, 1, 1, 0],
				times: [declick, envtime, declick],
				curve: [\sin, \step, \sin]
			),
			gate: loopReset
		);

		gateEnv = (gatePhase < gateWidth).lag(declick.min(loopDur / 4));

		// Read from buffer and apply envelopes
		loopOutput = BufRd.ar(numChannels, buffer, readPos, loop: 1);
		loopOutput = loopOutput * play * declickEnv;

		gatedDrySignal = processedInput * gateEnv;
		gatedWetSignal = loopOutput * gateEnv;

		fadeEnv = EnvGen.ar(
			Env.asr(declick, 1, declick, 'sine'),
			gate: on
		);

		transitionEnv = EnvGen.ar(
			Env.asr(declick, 1, declick, 'sine'),
			gate: delayedOnoff
		);

		transitionOutput = XFade2.ar(
			gatedDrySignal,
			gatedWetSignal,
			transitionEnv * 2 - 1
		);

		// Final output with dry/wet control
		output = SelectX.ar(fadeEnv, [input, transitionOutput]);
		output = XFade2.ar(input, output, drywet * 2 - 1);

		output * levels
	};

	Out.ar(OceanodeOutput.kr(\out), sigAll);

}, description: "Beat repeater effect with variable pitch control", category: "Effect/Time/Looping");
)