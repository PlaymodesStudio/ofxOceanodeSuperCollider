(
~synthCreator.value("Analog", {|n|
	var p, pulsew, finalSig, waves, vib, trem, tremRate, tremAmount, vibRate, vibAmount;
	var fma, fmratio, fmpitch, fmmode, fmfreq, fm, input, inputmode, levels;

	// Main pitch control with lag for smooth transitions
	p = OceanodeParameterLag.ar(\pitch, 36, n, 0, 127, "vf", 1/30, true);
	pulsew = OceanodeParameter.ar(\pw, 0, n, 0, 1, "vf");

	// Tremolo and vibrato parameters
	tremRate = OceanodeParameterLag.ar(\tremf, 0, n, 0, 8, "vf", 1/30, true);
	tremAmount = OceanodeParameterLag.ar(\trema, 0, n, 0, 1, "vf", 1/30, true);
	vibRate = OceanodeParameterLag.ar(\vibf, 0, n, 0, 8, "vf", 1/30, true);
	vibAmount = OceanodeParameterLag.ar(\viba, 0, n, 0, 1, "vf", 1/30, true);
	fma = OceanodeParameterLag.ar(\fmamp, 0, n, 0, 1000, "vf", 1/30, true);
	fmratio = OceanodeParameterLag.ar(\fmratio, 1, n, 0, 16, "vf", 1/30, true);
	fmpitch = OceanodeParameterLag.ar(\fmpitch, 48, n, 12, 96, "vf", 1/30, true);
	// FM mode is global. Declaring it with n controls made the single dropdown
	// update only part of a multichannel SynthDef.
	fmmode = OceanodeParameterDropdown.kr(\fmmode, 0, 1, "Ratio:Pitch");
	inputmode = OceanodeParameterDropdown.kr(\inputmode, 0, 1, "Off:Multiply");
	input = In.ar(OceanodeInput.kr(\in), n);

	// Ratio mode retains the current dynamic Analog behavior. Pitch mode is
	// compatible with the old Analog/Analog13 FMPitch control.
	// The dropdown produces 0 (Ratio) or 1 (Pitch). Arithmetic interpolation
	// broadcasts the scalar selector across every voice and remains switchable
	// while the Synth is running.
	fmfreq = (p.midicps * fmratio * (1 - fmmode)) + (fmpitch.midicps * fmmode);
	fm = SinOsc.ar(fmfreq) * fma;

	// Apply vibrato to pitch
	vib = SinOsc.ar(vibRate, 0, vibAmount, 0);
	p = (p + vib).midicps;

	// Define waveforms including the new impulse oscillator
	waves = [
		Saw.ar(p+fm, 1, 0),
		LFTri.ar(p+fm, 0),
		Pulse.ar(p+fm, (pulsew+0.5)%1, 1),
		SinOsc.ar(p+fm, 0),
		Impulse.ar(p+fm, 0)  // Added Impulse oscillator
	];

	// Updated dropdown options to include Impulse
	finalSig = Select.ar(OceanodeParameterDropdown.kr(\type, 0, n, "Saw:Triangle:Pulse:Sine:Impulse"), waves);

	// Apply tremolo to amplitude
	trem = 1 - ((SinOsc.ar(tremRate, 0, 1, 1) / 2) * tremAmount);

	// Apply level control in normal mode. Analog13ar used the input itself as
	// the amplitude signal and therefore did not have a Levels parameter.
	levels = OceanodeParameterLag.ar(\levels, 0, n, 0, 1, "vf", 1/30, true);
	finalSig = finalSig * trem;

	// The old Analog13ar multiplied the oscillator by an audio input. Keeping
	// this disabled by default preserves existing new Analog presets.
	finalSig = Select.ar(inputmode, [finalSig * levels, finalSig * input]);

	// Output the signal
	Out.ar(OceanodeOutput.kr(\out), finalSig);

}, description: "Analog synth with different wave types", category: "Source/Analog");
)
