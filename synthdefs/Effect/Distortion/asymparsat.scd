(
~synthCreator.value("AsymParSat", {|n|
	var sym, softhard, satpos, satneg, hipos, hineg, sympos, symneg, inputsig,
		eventclippos, eventclipneg, clippedsigpos, clippedsigneg, finalsig,
		satarray, dry, wet, inputGain, mix, send;

	// Parameters
	send = OceanodeParameter.ar(\send, 1, n, 0, 4, "vf");
	softhard = OceanodeParameter.ar(\softhard, 0.5, n, 0, 1, "vf");
	sym = OceanodeParameter.ar(\sym, 0.5, n, 0, 1, "vf").min(0.99);   // sym = 1 made the negative curve 0 wide (1/0)
	mix = OceanodeParameter.ar(\mix, 1, n, 0, 1, "vf");

	// Input stage
	inputsig = In.ar(OceanodeInput.kr(\in), n);
	dry = inputsig;
	inputsig = inputsig * send;

	// Saturation calculations
	hipos = softhard;
	hineg = softhard;
	sympos = sym + 1;
	symneg = sym.neg + 1;

	eventclippos = hipos.clip(0.00001, 0.99999) * sympos;   // was Clip.kr on audio-rate params
	eventclipneg = hineg.clip(0.00001, 0.99999) * symneg;
	clippedsigpos = Clip.ar(inputsig, eventclippos.neg, eventclippos);
	clippedsigneg = Clip.ar(inputsig, eventclipneg.neg, eventclipneg);

	satpos = ((((eventclippos.neg) + sympos) * 0.5) *
			SoftClipAmp.ar((((((eventclippos.neg) + sympos) * 0.5).reciprocal) *
			(clippedsigpos.neg + inputsig)))) + clippedsigpos;

	satneg = ((((eventclipneg.neg) + symneg) * 0.5) *
			SoftClipAmp.ar((((((eventclipneg.neg) + symneg) * 0.5).reciprocal) *
			(clippedsigneg.neg + inputsig)))) + clippedsigneg;

	satarray = [satpos, satneg];
	// Positive half-wave through the positive curve, negative through the negative one.
	// (Was Select.ar((inputsig + 1) / 2, ...): the index truncates, so it picked the positive
	// curve for every sample below +1 and the negative one only for samples >= +1, not by sign.)
	wet = Select.ar(inputsig < 0, satarray);

	// Mix dry and wet signals
	finalsig = XFade2.ar(dry, wet, mix * 2 - 1);

	Out.ar(OceanodeOutput.kr(\out), finalsig);
}, description: "Asymmetric parallel saturation distortion effect", category: "Effect/Distortion");
)