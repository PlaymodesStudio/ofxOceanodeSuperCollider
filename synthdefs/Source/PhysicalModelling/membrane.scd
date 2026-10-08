(
~synthCreator.value("Membrane", {|n|
	var p, signal, exmode, behavior;
	var tension, loss, exciterLevel, masterLevel, input, trigger, duration;

	// Excitation mode selector as dropdown
	exmode = OceanodeParameterDropdown.kr(\exciter, 0, n, "Internal:External");
	behavior = OceanodeParameterDropdown.kr(\behavior, 0, n, "Continuous:Percussive");

	// Main parameters with lag for smooth transitions
	tension = OceanodeParameterLag.ar(\tension, 0.05, n, 0, 4, "vf", 1/30, true);
	loss = OceanodeParameterLag.ar(\loss, 2.9, n, 0, 4, "vf", 1/30, true);
	p = OceanodeParameterLag.ar(\pitch, 36, n, 0, 128, "vf", 1/30, true).midicps;
	trigger = OceanodeParameter.ar(\trigger, 0, n, 0, 1, "vi");
	duration = OceanodeParameterLag.ar(\duration, 1, n, 0.01, 4, "vf", 1/30, true);

	// Amplitude parameters
	exciterLevel = OceanodeParameterLag.ar(\exciterlevel, 0.15, n, 0, 1, "vf", 1/30, true);
	masterLevel = OceanodeParameterLag.ar(\levels, 0, n, 0, 1, "vf", 1/30, true);

	// Input for external excitation
	input = In.ar(OceanodeInput.kr(\in), n);

	// Generate signal for each channel
	signal = n.collect({ |chan|
		var continuousExcitation, continuousSig, continuousLoss;
		var percussiveExcitation, percussiveSig, percussiveLoss, ampEnvelope;

		// Select excitation source based on mode
		continuousExcitation = Select.ar(exmode[chan], [
			// Internal pink noise
			PinkNoise.ar(exciterLevel[chan]),
			// External input
			input[chan] * exciterLevel[chan]
		]);

		continuousLoss = LinLin.ar(loss[chan], 0.9, 4.0, 0.9, 1.0);
		continuousSig = MembraneCircle.ar(
			continuousExcitation,
			tension[chan] * (p[chan] / 60.midicps),
			continuousLoss
		) * masterLevel[chan];

		// PercMembrane13-compatible triggered excitation and amplitude envelope.
		percussiveLoss = LinLin.ar(loss[chan], 0.0, 1.0, 0.9, 1.0);
		percussiveExcitation = EnvGen.kr(
			Env.perc,
			trigger[chan],
			timeScale: 1
		) * PinkNoise.ar();
		percussiveSig = MembraneCircle.ar(
			percussiveExcitation,
			tension[chan] * (p[chan] / 60.midicps),
			percussiveLoss
		);
		ampEnvelope = EnvGen.ar(
			Env.perc,
			trigger[chan],
			levelScale: masterLevel[chan],
			timeScale: duration[chan]
		);
		percussiveSig = Limiter.ar(Sanitize.ar(LeakDC.ar(percussiveSig * ampEnvelope)));

		Select.ar(behavior[chan], [continuousSig, percussiveSig])
	});

	// Output the signal
	Out.ar(OceanodeOutput.kr(\out), signal);

}, description: "Circular membrane physical model with internal or external excitation", category: "Source/PhysicalModelling");
)
