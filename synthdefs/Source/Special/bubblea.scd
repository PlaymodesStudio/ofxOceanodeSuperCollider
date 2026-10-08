~synthCreator.value("BubbleA", {|n|
	var t_trig, attack, decay, pitchcurvelen, freq, amp, declick;
	var pitch, son;
	// Parameters
	t_trig = OceanodeParameter.ar(\trig, 0, n, 0, 1, "vi");
	attack = OceanodeParameter.ar(\attack, 0.01, n, 0.001, 0.1, "vf");
	decay = OceanodeParameter.ar(\decay, 0.08, n, 0.01, 1, "vf");
	pitchcurvelen = OceanodeParameter.ar(\curve, 0.1, n, 0.01, 1, "vf");
	freq = OceanodeParameter.ar(\freq, 200, n, 50, 2000, "vf");
	amp = OceanodeParameter.ar(\amp, 1, n, 0, 1, "vf").lincurve(0, 1, 0, 1, 4);
	declick = OceanodeParameter.ar(\declick, 0.001, n, 0, 0.01, "vf");

	// Amplitude envelope
	amp = amp * EnvGen.ar(Env.perc(attack, decay).delay(0.003), t_trig);

	// Pitch envelope (rising)
	pitch = freq * EnvGen.ar(Env.new([0, 0, 1], [0, 1]).exprange(1, 2.718), t_trig, timeScale: pitchcurvelen);

	// Sound generation
	son = SinOsc.ar(pitch);
	amp = Lag.ar(amp, declick);
	son = HPF.ar(son, 500) * amp;

	// Output: one channel per voice (Pan2 turned n voices into 2*n channels summed onto the bus)
	Out.ar(OceanodeOutput.kr(\out), son);
}, description: "Simple bubble sound with rising pitch envelope", category: "Source/Special");