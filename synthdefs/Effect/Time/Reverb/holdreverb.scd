(
~synthCreator.value("HoldReverb", {|n|
	var sig, local, wet, dry, clearGate, decayTime;
	// Input gain parameter with proper lag for smooth transitions
	var inputGain = OceanodeParameterLag.ar(\send, 1, n, 0, 1, "vf", 1/30, true).lincurve(0, 1, 0, 1, 4);
	// Feedback parameter with lag
	var feedback = OceanodeParameterLag.ar(\feedback, 0.5, n, 0, 1, "vf", 1/30, true);
	// Mix parameter with lag
	var mix = OceanodeParameterLag.ar(\mix, 0, n, 0, 1, "vf", 1/30, true);
	// Damping: LPF cutoff in the feedback loop - lower values = darker, warmer tail
	var damp = OceanodeParameterLag.ar(\damp, 20000, n, 200, 20000, "vf", 1/30, true);
	// Clear gate - when triggered (goes to 1), clears the reverb tail immediately
	var clear = OceanodeParameter.ar(\clear, 0, n, 0, 1, "vi");

	// Get input signal from the specified bus
	sig = In.ar(OceanodeInput.kr(\in), n) * inputGain;

	// clearGate is 1 during normal operation, 0 when clearing
	clearGate = 1 - clear;

	// Decay time for allpass filters - 3 seconds normally, near-zero when clearing
	decayTime = clearGate * 3;

	// Initialize the local signal buffer
	// When clear is 1, the feedback from LocalIn is zeroed out
	local = (LocalIn.ar(n) * clearGate) + sig;

	// Apply the all-pass filter chain to each channel individually
	// Using decayTime which becomes ~0 when clear=1, draining the buffers
	30.do {
		local = Array.fill(n, {|i|
			AllpassN.ar(local[i], 0.06, rrand(0.001, 0.06), decayTime[i]);
		});
	};

	// Apply clearGate to the processed signal to immediately silence the tail
	local = local * clearGate;

	// Feedback the processed signal, with LPF damping applied in the loop
	// Each recirculation loses highs progressively — early reflections stay bright,
	// long tail becomes darker, matching the physical behavior of real spaces
	LocalOut.ar(LPF.ar(local * feedback, damp));

	// Prepare wet and dry signals
	wet = local * mix;
	dry = sig * (1 - mix);

	// Output the mix of dry and wet signals
	Out.ar(OceanodeOutput.kr(\out), wet + dry);
}, description: "Ambient reverb with infinite hold and clear gate", category: "Effect/Time/Reverb");
)
