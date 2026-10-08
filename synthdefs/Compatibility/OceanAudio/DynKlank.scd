(
~synthCreator.value("DynKlank", {|n|
	var partials = 16;
	var chans = n;
	var sig = In.ar(OceanodeInput.kr(\in), n);
	var freqs = OceanodeParameterLag.kr(\freqarray, 220, partials*chans, 0, 10000, "vf", 1/30, true).reshape(chans, partials).flop;
	var decays = OceanodeParameterLag.kr(\decayarray, 0.5, partials*chans, 0, 4, "vf", 1/30, true).reshape(chans, partials).flop;
	var amps = OceanodeParameterLag.kr(\amparray, 0, partials*chans, 0, 1, "vf", 1/30, true).reshape(chans, partials).flop;
	var largeSig = Ringz.ar(sig,freqs,decays,amps,0);
	var flat8 = Mix.ar(largeSig);
	Out.ar(OceanodeOutput.kr(\out), flat8);
}, description: "Sixteen-resonator bank per input channel", category: "Compatibility/OceanAudio");
)
