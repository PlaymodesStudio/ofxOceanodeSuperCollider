~synthCreator.value("SuperFM7", {|n|
	//Adapted from Loopier's SuperFM synthdef for Tidal
	// --- GLOBAL PARAMETERS ---
	var opRatios    = NamedControl.kr(\op_ratios, [1, 1, 1, 1, 1, 1]);
	var opDetunes   = NamedControl.kr(\op_detunes, [0, 0, 0, 0, 0, 0]);
	var opAmps      = NamedControl.kr(\op_amps,    [1, 0, 0, 0, 0, 0]);
	var egLevels    = NamedControl.kr(\eg_levels, Array.fill(24, 1));
	var egRates     = NamedControl.kr(\eg_rates,  Array.fill(24, 1));
	var feedback    = NamedControl.kr(\feedback, 1.0);
	var masterAmp   = NamedControl.kr(\amp, 0.5);

	// --- INPUT PARAMETERS (Linkable / GUI) ---
	// Mod Matrix is now an input parameter so it can be controlled externally
	var modMatrix   = NamedControl.kr(\mod_matrix, Array.fill(36, 0));

	// --- PER-VOICE PERFORMANCE PARAMETERS ---
	var pitch = OceanodeParameterLag.ar(\pitch, 60, n, 0, 127, "vf");
	var gate  = OceanodeParameterLag.kr(\gate, 0, n, 0, 1, "vf");
	var vel   = OceanodeParameterLag.kr(\velocity, 1, n, 0, 1, "vf");

	// New: Mod Scale (Scales the Matrix intensity per voice)
	var modScale = OceanodeParameterLag.kr(\modScale, 1, n, 0, 10, "vf");

	// New: Per-Voice Vibrato (Renamed from LFO)
	var vibFreq = OceanodeParameterLag.kr(\vibFreq, 5, n, 0, 20, "vf");
	var vibAmp  = OceanodeParameterLag.kr(\vibAmp, 0, n, 0, 1, "vf");

	// New: Per-Voice Tremolo
	var tremFreq = OceanodeParameterLag.kr(\tremFreq, 5, n, 0, 20, "vf");
	var tremAmp  = OceanodeParameterLag.kr(\tremAmp, 0, n, 0, 1, "vf");

	// --- LOGIC ---

	// 1. Vibrato (Pitch Modulation)
	var vibrato = SinOsc.kr(vibFreq) * vibAmp;
	// Apply to freq (Multiplicative for musical interval consistency, or additive)
	// Using additive logic on MIDI pitch then converting to CPS is usually safest for pitch:
	var freq = (pitch + vibrato).midicps;

	// 2. Tremolo (Amplitude Modulation)
	// 0 to 1 scaling: (1 - depth) + (depth * sin)
	var tremSig = 1 - (tremAmp * (0.5 * (1 + SinOsc.kr(tremFreq, 1.5pi))));

	// 3. Matrix Construction with Per-Voice Scaling
	var mods = Array.fill(6, {|row|
		Array.fill(6, {|col|
			var val = modMatrix[row * 6 + col];

			// Apply the per-voice scaler.
			// If 'n' > 1, 'modScale' is an array, causing 'val' to expand to [n].
			val = val * modScale;

			if(row == col, { val * feedback }, { val })
		})
	});

	// 4. Operators
	var ctls = Array.fill(6, {|i|
		var myLevels = egLevels[i*4 .. i*4+3];
		var myRates  = egRates[i*4 .. i*4+3];
		var myTimes  = 0.5 / myRates.max(0.01);

		var envStruct = Env.new(
			[0, myLevels[0], myLevels[1], myLevels[2], 0],
			[myTimes[0], myTimes[1], myTimes[2], myTimes[3]],
			-4,
			3
		);

		var env = EnvGen.kr(envStruct, gate);
		var opFreq = freq * opRatios[i] + opDetunes[i];

		// Note: We apply Velocity to the operator Level
		[opFreq, 0, env * vel]
	});

	var fmOut = FM7.ar(ctls, mods);
	var sig;

	// --- MIXER ---
	if(n > 1, {
		// Polyphonic: fmOut is [ [Voice1_Ops], [Voice2_Ops]... ]
		// We multiply by OpAmps (Broadcasting) then Sum operators within the voice
		sig = fmOut.collect({ |voiceOps|
			(voiceOps * opAmps).sum
		});
	}, {
		// Monophonic: fmOut is [Op1...Op6]
		sig = (fmOut * opAmps).sum;
	});

	// Apply Tremolo and Master Amp
	Out.ar(OceanodeOutput.kr(\out), sig * tremSig * masterAmp);

},
metadata: (
	description: "6-Operator FM with Per-Voice ModScale & Trem/Vib",
	category: "Synthesis"
));