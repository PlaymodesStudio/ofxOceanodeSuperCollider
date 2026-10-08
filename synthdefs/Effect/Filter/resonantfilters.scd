(
~synthCreator.value("RLPF", {|n|
    var input, filtered, freq, res, filterType, drywet;
    // Get input from source
    input = In.ar(OceanodeInput.kr(\in), n);
    // Control parameters
    freq = OceanodeParameterLag.ar(\pitch, 60, n, 0, 127, "vf", 0.05, true).midicps;
    res = 1-(OceanodeParameterLag.ar(\q, 0, n, 0, 0.99999, "vf"));

    filtered = RLPF.ar(input, freq, res);

    // Use XFade2 for equal-power crossfading between dry and wet signals
    // XFade2 expects a pan value from -1 to 1, so map drywet from 0-1 to -1 to 1
    drywet = OceanodeParameterLag.ar(\mix, 1, n, 0, 1, "vf", 1/30, true);
    filtered = XFade2.ar(input, filtered, drywet.linlin(0, 1, -1, 1));

    // Output filtered signal
    Out.ar(OceanodeOutput.kr(\out), filtered);
}, description: "Resonant LPF", category: "Effect/Filter");
);

(
~synthCreator.value("RHPF", {|n|
    var input, filtered, freq, res, filterType, drywet;
    // Get input from source
    input = In.ar(OceanodeInput.kr(\in), n);
    // Control parameters
    freq = OceanodeParameterLag.ar(\pitch, 60, n, 0, 127, "vf", 0.05, true).midicps;
    res = 1-(OceanodeParameterLag.ar(\q, 0, n, 0, 0.99999, "vf"));

    filtered = RHPF.ar(input, freq, res);              // Resonant High Pass Filter


    // Use XFade2 for equal-power crossfading between dry and wet signals
    // XFade2 expects a pan value from -1 to 1, so map drywet from 0-1 to -1 to 1
    drywet = OceanodeParameterLag.ar(\mix, 1, n, 0, 1, "vf", 1/30, true);
    filtered = XFade2.ar(input, filtered, drywet.linlin(0, 1, -1, 1));

    // Output filtered signal
    Out.ar(OceanodeOutput.kr(\out), filtered);
}, description: "Resonant HPF", category: "Effect/Filter");
);

(
~synthCreator.value("RBPF", {|n|
	var input, output, levels;
	var pitch, freq, q, amp, filtered, drywet;

	// Get input from the bus
	input = In.ar(OceanodeInput.kr(\in), n);

	// Parameter controls with proper lag for smooth transitions
	pitch = OceanodeParameterLag.ar(\pitch, 60, n, 0, 130, "vf", 1/30, true);
	freq = pitch.midicps;
	q = 1-(OceanodeParameterLag.ar(\q, 1, n, 0.005, 1, "vf", 1/30, true));
	amp = OceanodeParameterLag.ar(\amp, 1, n, 0, 1, "vf", 1/30, true);

	// Level control with proper dB scaling
	levels = OceanodeParameterLag.ar(\levels, 1, n, 0, 1, "vf", 1/30, true);

	// Apply two BPFs in series for steeper filtering
	output = input;
	output = BPF.ar(output, freq, q);
	output = BPF.ar(output, freq, q);

	// Apply amplitude and level controls
	output = output * amp * levels;

	drywet = OceanodeParameterLag.ar(\mix, 1, n, 0, 1, "vf", 1/30, true);
    filtered = XFade2.ar(input, output, drywet.linlin(0, 1, -1, 1));

	// Output the processed signal
	Out.ar(OceanodeOutput.kr(\out), filtered);
}, description: "Resonant BPF", category: "Effect/Filter");
);
