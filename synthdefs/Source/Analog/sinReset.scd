(
~synthCreator.value("SinReset", {|n|
	var freq, signal, ampControl, phase, fmin, reset, ramp;

	freq = OceanodeParameterLag.ar(\pitch, 36, n, 0, 130, "vf", 1/30, true).midicps;
	phase = OceanodeParameter.ar(\phase, 0, n, -1, 1, "vf");
	fmin = OceanodeParameterLag.ar(\fm_in, 0, n, -1, 1, "vf", 1/30, true);

	reset = OceanodeParameter.ar(\reset, 0, n, 0, 1, "vi");

	// Sweep resets to 0 on trigger, then ramps up at freq rate
	// We add the phase offset and wrap to 0-1
	ramp = (Sweep.ar(reset, freq + fmin) + phase).wrap(0, 1);

	signal = SinOsc.ar(0, ramp * 2pi);

	ampControl = OceanodeParameterLag.ar(\levels, 0, n, 0, 1, "vf", 1/30, true);

	signal = signal * ampControl;

	Out.ar(OceanodeOutput.kr(\out), signal);
}, description: "Sinusoidal oscillator with phase control and reset", category: "Source/Analog");
)