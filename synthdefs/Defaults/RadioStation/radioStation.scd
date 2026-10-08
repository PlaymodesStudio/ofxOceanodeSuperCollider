~synthCreator.value("RadioStation", {|n|
	var sig, amp;

	// Volume — also settable directly by the C++ side via synth->set("amp", v)
	amp = OceanodeParameter.ar(\amp, 1, n, 0, 2, "vf");

	// Read stereo audio from the POSIX shared-memory ring buffer written by
	// RadioStationVLC in openFrameworks.  instanceId selects which radio node's
	// SHM segment to read (0 = first node, 1 = second, …).
	// OceanodeRadioIn always outputs [L, R]; wrapExtend adapts to n channels.
	sig = OceanodeRadioIn.ar(\instanceId.kr(0));
	sig = sig.wrapExtend(n) * amp;

	Out.ar(OceanodeOutput.kr(\out), sig);

},
metadata: (
	description: "VLC internet radio — output connects to any effect chain",
	category: "Source/Radio"
));
