~synthCreator.value("SuperFMDrone", {|n|
	//Adapted from SuperFM7 - Drone version (no gate/envelope, always on)
	// --- GLOBAL PARAMETERS ---
	var feedback    = NamedControl.kr(\feedback, 1.0);

	// --- PER-VOICE AMPLITUDE ---
	var masterAmp = OceanodeParameterLag.kr(\amp, 1.0, n, 0, 1, "vf");

	// --- INPUT PARAMETERS (Linkable / GUI) ---
	var modMatrix   = NamedControl.kr(\mod_matrix, Array.fill(36, 0));

	// --- PER-VOICE PERFORMANCE PARAMETERS ---
	var pitch = OceanodeParameterLag.ar(\pitch, 60, n, 0, 127, "vf");

	// Mod Scale (Scales the Matrix intensity per voice)
	var modScale = OceanodeParameterLag.kr(\modScale, 1, n, 0, 10, "vf");

	// Per-Voice Vibrato
	var vibFreq = OceanodeParameterLag.kr(\vibFreq, 5, n, 0, 20, "vf");
	var vibAmp  = OceanodeParameterLag.kr(\vibAmp, 0, n, 0, 1, "vf");

	// Per-Voice Tremolo
	var tremFreq = OceanodeParameterLag.kr(\tremFreq, 5, n, 0, 20, "vf");
	var tremAmp  = OceanodeParameterLag.kr(\tremAmp, 0, n, 0, 1, "vf");

	// --- PER-VOICE OPERATOR PARAMS (each is a vector of size n) ---
	// Op Amps (output mixer weights, per voice)
	var opAmp1 = OceanodeParameterLag.kr(\op_amp_1, 1, n, 0, 1, "vf");
	var opAmp2 = OceanodeParameterLag.kr(\op_amp_2, 0, n, 0, 1, "vf");
	var opAmp3 = OceanodeParameterLag.kr(\op_amp_3, 0, n, 0, 1, "vf");
	var opAmp4 = OceanodeParameterLag.kr(\op_amp_4, 0, n, 0, 1, "vf");
	var opAmp5 = OceanodeParameterLag.kr(\op_amp_5, 0, n, 0, 1, "vf");
	var opAmp6 = OceanodeParameterLag.kr(\op_amp_6, 0, n, 0, 1, "vf");

	// Op Ratios (frequency ratio per operator, per voice)
	var opRatio1 = OceanodeParameterLag.kr(\op_ratio_1, 1, n, 0, 20, "vf");
	var opRatio2 = OceanodeParameterLag.kr(\op_ratio_2, 1, n, 0, 20, "vf");
	var opRatio3 = OceanodeParameterLag.kr(\op_ratio_3, 1, n, 0, 20, "vf");
	var opRatio4 = OceanodeParameterLag.kr(\op_ratio_4, 1, n, 0, 20, "vf");
	var opRatio5 = OceanodeParameterLag.kr(\op_ratio_5, 1, n, 0, 20, "vf");
	var opRatio6 = OceanodeParameterLag.kr(\op_ratio_6, 1, n, 0, 20, "vf");

	// Op Detunes (semitone detune per operator, per voice)
	var opDetune1 = OceanodeParameterLag.kr(\op_detune_1, 0, n, -5, 5, "vf");
	var opDetune2 = OceanodeParameterLag.kr(\op_detune_2, 0, n, -5, 5, "vf");
	var opDetune3 = OceanodeParameterLag.kr(\op_detune_3, 0, n, -5, 5, "vf");
	var opDetune4 = OceanodeParameterLag.kr(\op_detune_4, 0, n, -5, 5, "vf");
	var opDetune5 = OceanodeParameterLag.kr(\op_detune_5, 0, n, -5, 5, "vf");
	var opDetune6 = OceanodeParameterLag.kr(\op_detune_6, 0, n, -5, 5, "vf");

	var opRatios  = [opRatio1, opRatio2, opRatio3, opRatio4, opRatio5, opRatio6];
	var opDetunes = [opDetune1, opDetune2, opDetune3, opDetune4, opDetune5, opDetune6];
	var opAmpList = [opAmp1, opAmp2, opAmp3, opAmp4, opAmp5, opAmp6];

	// --- LOGIC ---

	// 1. Vibrato (Pitch Modulation)
	var vibrato = SinOsc.kr(vibFreq) * vibAmp;
	var freq = (pitch + vibrato).midicps;

	// 2. Tremolo (Amplitude Modulation)
	var tremSig = 1 - (tremAmp * (0.5 * (1 + SinOsc.kr(tremFreq, 1.5pi))));

	// 3. Matrix Construction with Per-Voice Scaling
	var mods = Array.fill(6, {|row|
		Array.fill(6, {|col|
			var val = modMatrix[row * 6 + col];
			val = val * modScale;
			if(row == col, { val * feedback }, { val })
		})
	});

	// 4. Operators - always on (amplitude = 1.0 in ctls), opAmpX used only at output mixer
	var ctls = Array.fill(6, {|i|
		var opFreq = freq * opRatios[i] + opDetunes[i];
		[opFreq, 0, 1.0]
	});

	var fmOut = FM7.ar(ctls, mods);
	var sig;

	// --- MIXER (per-voice opAmps) ---
	if(n > 1, {
		sig = fmOut.collect({ |voiceOps, v|
			opAmpList.collect({|ampVec, i| voiceOps[i] * ampVec[v] }).sum
		});
	}, {
		sig = opAmpList.collect({|ampVec, i| fmOut[i] * ampVec }).sum;
	});

	// Apply Tremolo and Master Amp
	Out.ar(OceanodeOutput.kr(\out), sig * tremSig * masterAmp);

},
metadata: (
	description: "6-Operator FM Drone - per-voice op amps/ratios/detunes. No gate/envelope.",
	category: "Synthesis"
));
