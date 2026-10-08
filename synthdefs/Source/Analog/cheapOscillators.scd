(
~synthCreator.value("CheapSaw", {|n|
	var p, pulsew, finalSig, waves, freq, vib, trem, tremRate, tremAmount, vibRate, vibAmount,fma,fmratio,fm;

	// Main pitch control with lag for smooth transitions
	p = OceanodeParameterLag.ar(\pitch, 36, n, 0, 127, "vf", 1/30, true);
	fm = OceanodeParameterLag.ar(\fm_in, 0, n, -1, 1, "vf", 1/30, true);
	//pulsew = OceanodeParameter.ar(\pw, 0, n, 0, 1, "vf");

	p = p.midicps;

	// Define waveforms including the new impulse oscillator
	finalSig= Saw.ar(p+fm, 1, 0);

	// Apply level control
	finalSig = finalSig * OceanodeParameterLag.ar(\levels, 0, n, 0, 1, "vf", 1/30, true);

	// Output the signal
	Out.ar(OceanodeOutput.kr(\out), finalSig);

}, description: "Cheap Saw", category: "Source/Analog");
);

(
~synthCreator.value("CheapTri", {|n|
	var p, pulsew, finalSig, waves, freq, vib, trem, tremRate, tremAmount, vibRate, vibAmount,fma,fmratio,fm;

	// Main pitch control with lag for smooth transitions
	p = OceanodeParameterLag.ar(\pitch, 36, n, 0, 127, "vf", 1/30, true);
		fm = OceanodeParameterLag.ar(\fm_in, 0, n, -1, 1, "vf", 1/30, true);

	//pulsew = OceanodeParameter.ar(\pw, 0, n, 0, 1, "vf");

	p = p.midicps;

	// Define waveforms including the new impulse oscillator
	finalSig= LFTri.ar(p+fm, 0);

	// Apply level control
	finalSig = finalSig * OceanodeParameterLag.ar(\levels, 0, n, 0, 1, "vf", 1/30, true);

	// Output the signal
	Out.ar(OceanodeOutput.kr(\out), finalSig);

}, description: "Cheap Tri", category: "Source/Analog");
);

(
~synthCreator.value("CheapPulse", {|n|
	var p, pulsew, finalSig, waves, freq, vib, trem, tremRate, tremAmount, vibRate, vibAmount,fma,fmratio,fm;

	// Main pitch control with lag for smooth transitions
	p = OceanodeParameterLag.ar(\pitch, 36, n, 0, 127, "vf", 1/30, true);
	pulsew = OceanodeParameter.ar(\pw, 0, n, 0, 1, "vf");
		fm = OceanodeParameterLag.ar(\fm_in, 0, n, -1, 1, "vf", 1/30, true);


	p = p.midicps;

	// Define waveforms including the new impulse oscillator
	finalSig= Pulse.ar(p+fm, (pulsew+0.5)%1, 1);

	// Apply level control
	finalSig = finalSig * OceanodeParameterLag.ar(\levels, 0, n, 0, 1, "vf", 1/30, true);

	// Output the signal
	Out.ar(OceanodeOutput.kr(\out), finalSig);

}, description: "Cheap Pulse", category: "Source/Analog");
);

(
~synthCreator.value("CheapSin", {|n|
	var p, pulsew, finalSig, waves, freq, vib, trem, tremRate, tremAmount, vibRate, vibAmount,fma,fmratio,fm;

	// Main pitch control with lag for smooth transitions
	p = OceanodeParameterLag.ar(\pitch, 36, n, 0, 127, "vf", 1/30, true);
		fm = OceanodeParameterLag.ar(\fm_in, 0, n, -1, 1, "vf", 1/30, true);

	//pulsew = OceanodeParameter.ar(\pw, 0, n, 0, 1, "vf");

	p = p.midicps;

	// Define waveforms including the new impulse oscillator
	finalSig= SinOsc.ar(p+fm, 0);

	// Apply level control
	finalSig = finalSig * OceanodeParameterLag.ar(\levels, 0, n, 0, 1, "vf", 1/30, true);

	// Output the signal
	Out.ar(OceanodeOutput.kr(\out), finalSig);

}, description: "Cheap Sin", category: "Source/Analog");
);

(
~synthCreator.value("CheapImpulse", {|n|
	var p, pulsew, finalSig, waves, freq, vib, trem, tremRate, tremAmount, vibRate, vibAmount,fma,fmratio,fm;

	// Main pitch control with lag for smooth transitions
	p = OceanodeParameterLag.ar(\pitch, 36, n, 0, 127, "vf", 1/30, true);
	fm = OceanodeParameterLag.ar(\fm_in, 0, n, -1, 1, "vf", 1/30, true);

	//pulsew = OceanodeParameter.ar(\pw, 0, n, 0, 1, "vf");

	p = p.midicps;

	// Define waveforms including the new impulse oscillator
	finalSig= Impulse.ar(p+fm, 0);

	// Apply level control
	finalSig = finalSig * OceanodeParameterLag.ar(\levels, 0, n, 0, 1, "vf", 1/30, true);

	// Output the signal
	Out.ar(OceanodeOutput.kr(\out), finalSig);

}, description: "Cheap Impulse", category: "Source/Analog");
);

