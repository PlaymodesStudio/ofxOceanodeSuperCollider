(
~synthCreator.value("GateDuration", {|n|
	var input, duration, trig;
	var durationSec, durLatched, durActive;
	var elapsed, gate, trigFired;
	var sAndH, trigEdge;

	// Audio input for impulse triggers
	input = In.ar(OceanodeInput.kr(\in), n);

	// Duration parameter in milliseconds
	duration = OceanodeParameterLag.ar(\duration, 100, n, 1, 10000, "vf", 1/30, true);

	// S&H toggle: 1 = latch duration at trigger time (default), 0 = immediate response
	sAndH = OceanodeParameter.kr(\sh, 1, 1, 0, 1, "b");

	// Detect impulses — debounced pulse for Latch/PulseCount, single-sample edge for Sweep
	trig          = Trig1.ar(input > 0.1, 0.001);
	trigEdge  = Trig.ar(input > 0.1, SampleDur.ir);  // 1-sample: Sweep resets only once

	// Duration in seconds
	durationSec = duration * 0.001;

	// S&H: latch duration value at trigger moment
	durLatched = Latch.ar(durationSec, trig);

	// Select between live duration (immediate) and latched duration (S&H)
	// sh=1 (default) → latched; sh=0 → live
	durActive = (K2A.ar(1 - (sAndH > 0.5)) * durationSec)
	          + (K2A.ar(sAndH > 0.5)       * durLatched);

	// Sweep: counts up in seconds since last trigger — uses single-sample edge
	// so it starts counting from the very first sample the gate opens
	elapsed = Sweep.ar(trigEdge, 1);

	// Guard: suppress spurious gate before the first trigger fires
	trigFired = PulseCount.ar(trig) > 0;

	// Gate is open while elapsed < active duration
	gate = (elapsed < durActive) * trigFired;

	Out.ar(OceanodeOutput.kr(\out), gate);

}, description: "Gate sustainer: converts impulses to sustained gates with S&H or immediate duration response", category: "Modulation/Trigger");
)
