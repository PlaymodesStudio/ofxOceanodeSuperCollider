(
~synthCreator.value("Looper", {|n|
	var inputAll, sigAll;
	var recA = OceanodeParameter.ar(\rec, 0, n, 0, 1, "vi");
	var rateA = OceanodeParameter.ar(\rate, 1, n, 0.01, 8, "vf");
	var ampA = OceanodeParameterLag.ar(\amp, 1, n, 0, 1, "vf", 1/50, true);
	var mixA = OceanodeParameter.ar(\mix, 1, n, 0, 1, "vf");
	var startA = OceanodeParameter.ar(\start, 0, n, 0, 1, "vf");
	var endA = OceanodeParameter.ar(\end, 1, n, 0, 1, "vf");
	var autoplayA = OceanodeParameter.ar(\autoplay, 1, n, 0, 1, "vi");
	var playA = OceanodeParameter.ar(\play, 0, n, 0, 1, "vi");
	var fixedtimeA = OceanodeParameter.ar(\fixedtime, 0, n, 0, 1, "vi");
	var recmsA = OceanodeParameter.ar(\recms, 1000, n, 0, 10000, "vf");
	var declickA = OceanodeParameter.ar(\declick, 0.1, n, 0, 1, "vf");

	inputAll = In.ar(OceanodeInput.kr(\in), n);

	// Each channel is an independent voice with its own mono buffer and timing
	// (parameters are per channel, so a shared multichannel buffer expanded into n*n signals)
	sigAll = n.collect { |c|
		var input, buffer, playhead, output, loopOutput;
		var recTrig, recTime, playTrig, bufferEnd, isLooping, minLoopDuration;
		var loopStart, loopEnd, playResetTrig, fixedTimeGate, fixedTimeEnd;
		var mixPhase, resetTrig, declickEnv, declickDur, envShape;
		var loopDur, loopPhase, bufferSize, numChannels;
		var rec = recA[c], rate = rateA[c], amp = ampA[c], mix = mixA[c], start = startA[c], end = endA[c], autoplay = autoplayA[c], play = playA[c], fixedtime = fixedtimeA[c], recms = recmsA[c], declick = declickA[c];
	
		// Setup
		bufferSize = 16;
		numChannels = 1; // one mono voice per channel
		minLoopDuration = 0.01;
	
		// Create a local buffer
		buffer = LocalBuf(SampleRate.ir * bufferSize, numChannels).clear;
	
		// Input signal
		input = inputAll[c];
	
		// Detect start of recording
		recTrig = Trig1.kr(rec);
	
		// Create a gate that closes after fixed time when fixedtime=1
		fixedTimeGate = EnvGen.kr(Env([0, 1, 0], [0, recms/1000], ['step', 'step']), recTrig);
		fixedTimeEnd = TDelay.kr(1 - fixedTimeGate, recms/1000);
	
		// Detect end of recording (either manual stop or fixed time end)
		playTrig = Trig1.kr((1 - rec) + (fixedtime * fixedTimeEnd));
	
		// Detect changes in play from 0 to 1
		playResetTrig = Trig1.kr(play);
	
		// Create reset trigger (don't reset if fixedtime=1 and autoplay=1)
		resetTrig = (playTrig + playResetTrig) * (1 - (fixedtime * autoplay));
	
		// Measure recording time
		recTime = Sweep.kr(recTrig);
	
		// Determine buffer end based on fixedtime
		bufferEnd = Select.kr(fixedtime, [
			Latch.kr(recTime, playTrig),
			recms / 1000  // Convert ms to seconds
		]).max(minLoopDuration);
	
		// Calculate actual loop start and end points
		loopStart = start * bufferEnd * SampleRate.ir;
		loopEnd = end * bufferEnd * SampleRate.ir;
	
		// Calculate loop duration
		loopDur = (loopEnd - loopStart) / SampleRate.ir;
	
		// Record to buffer
		RecordBuf.ar(input, buffer, loop: 0, trigger: recTrig);
	
		// Determine if we're looping
		isLooping = (bufferEnd > minLoopDuration) * (autoplay + play - (autoplay * play));
	
		// Playhead for buffer playback
		playhead = Phasor.ar(
			trig: resetTrig,
			rate: BufRateScale.kr(buffer) * rate,
			start: loopStart,
			end: loopEnd,
			resetPos: loopStart
		);
	
		// Calculate loop phase (0 to 1)
		loopPhase = (playhead - loopStart) / (loopEnd - loopStart);
	
		// Create declick envelope using loopPhase
		declickEnv = ((SinOsc.ar(0, loopPhase * 2pi) + 1) / 2).pow(declick * 4);
	
		// Play from buffer only if we're looping
		loopOutput = BufRd.ar(numChannels, buffer, playhead, loop: 1) * isLooping * declickEnv;
	
		// Calculate mix phase for crossfading
		mixPhase = isLooping * mix;
	
		// Mix input and loop output with dry/wet control
		output = XFade2.ar(input, loopOutput, mixPhase * 2 - 1);
	
		output * amp
	};

	Out.ar(OceanodeOutput.kr(\out), sigAll);
	
}, description: "Audio looper with variable speed and crossfade", category: "Effect/Time/Looping");
)