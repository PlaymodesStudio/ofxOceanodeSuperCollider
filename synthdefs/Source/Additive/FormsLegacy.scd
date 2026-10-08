(
~synthCreator.value("FormsLegacy", {|n|
	var partials = 1080;
	var freqs, pitch, amps, pans, levels, usedPartials, sigArray, mixedSig;

	freqs = OceanodeParameterLag.kr(\freqarray, 220, partials, 0, 10000, "vf", 1/30, true);
	pitch = OceanodeParameterLag.kr(\pitcharray, 36, partials, 0, 130, "vf", 1/30, true).midicps;
	amps = OceanodeParameterLag.kr(\amparray, 0, partials, 0, 1, "vf", 1/30, true);
	pans = OceanodeParameterLag.kr(\panarray, 0, partials, -1, 1, "vf", 1/30, true);
	levels = OceanodeParameterLag.kr(\levels, 1, n, 0, 128, "vf", 1/30, true);
	usedPartials = OceanodeParameter.kr(\partials, 1080, 1, 1, 1080, "vi");

	sigArray = Array.fill(partials, { |i|
		var baseFreq = freqs[i] + pitch[i];
		var amp = amps[i] * (i < usedPartials);
		var pan = pans[i];
		PanAz.ar(n, SinOsc.ar(baseFreq, Rand(-25, 25), amp), pan);
	});

	mixedSig = Array.fill(n, { |chan|
		Mix.new(sigArray.collect { |sig| sig[chan] }) * levels[chan];
	});

	Out.ar(OceanodeOutput.kr(\out), mixedSig / 100);
}, description: "Legacy multichannel FORMS oscillator with Hz plus pitch arrays", category: "Source/Additive");
)
