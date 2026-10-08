~synthCreator.value("MiRings", {|n|
	var input, send, mix, amp, sig;
	var pitch, struct, bright, damp, pos, model, poly;
	var internExciter, easteregg, bypass, trig;
	var chanInput, wet;

	// --- MiRings parameters (all exposed) ---

	// pit: pitch as MIDI note number (MiRings takes MIDI directly, no .midicps)
	pitch = OceanodeParameterLag.ar(\pitch, 60, n, 0, 130, "vf", 1/30, true);

	// struct: inharmonicity / material character / string intervals
	struct = OceanodeParameterLag.ar(\struct, 0.25, n, 0, 1, "vf", 1/30, true);

	// bright: brightness and spectral richness (0..1)
	bright = OceanodeParameterLag.ar(\bright, 0.5, n, 0, 1, "vf", 1/30, true);

	// damp: damping/decay time from 100ms to 10s (0..1)
	damp = OceanodeParameterLag.ar(\damp, 0.7, n, 0, 1, "vf", 1/30, true);

	// pos: excitation position along the structure (0..1)
	pos = OceanodeParameterLag.ar(\pos, 0.25, n, 0, 1, "vf", 1/30, true);

	// model: resonator model selector (0..5 integer)
	//   0=Modal Resonator, 1=Sympathetic String, 2=Modulated/Inharmonic String,
	//   3=2-op FM Voice, 4=Sympathetic String (quantized), 5=String and Reverb
	model = OceanodeParameter.ar(\model, 0, n, 0, 5, "vf");

	// poly: polyphony voices (1..4), more voices = fewer partials per voice
	poly = OceanodeParameter.ar(\poly, 1, n, 1, 4, "vf");

	// internexciter: force internal exciter on even when external audio is present (0/1)
	internExciter = OceanodeParameter.ar(\internexciter, 0, n, 0, 1, "vf");

	// easteregg: enable "disastrous peace" easter egg mode (0/1)
	easteregg = OceanodeParameter.ar(\easteregg, 0, n, 0, 1, "vf");

	// bypass: pass exciter input straight through, bypassing resonator (0/1)
	bypass = OceanodeParameter.ar(\bypass, 0, n, 0, 1, "vf");

	// trig: trigger new notes / internal exciter (0..1)
	trig = OceanodeParameterLag.ar(\trig, 0, n, 0, 1, "vf", 1/30, true);

	// --- Standard effect controls ---

	amp = OceanodeParameterLag.ar(\amp, 0.5, n, 0, 1, "vf", 1/30, true);
	mix = OceanodeParameter.ar(\mix, 0.5, n, 0, 1, "vf");
	send = OceanodeParameterLag.ar(\send, 1, n, 0, 1, "vf", 1/30, true).lincurve(0, 1, 0, 1, 4);

	// Audio input (external exciter)
	input = In.ar(OceanodeInput.kr(\in), n);

	// Process each channel through MiRings
	sig = n.collect({ |chan|
		var ringsOut;

		// Scale input by send amount
		chanInput = input[chan] * send[chan];

		// MiRings.ar — all parameters exposed
		ringsOut = MiRings.ar(
			in: chanInput,
			trig: trig[chan],
			pit: pitch[chan],
			struct: struct[chan],
			bright: bright[chan],
			damp: damp[chan],
			pos: pos[chan],
			model: model[chan],
			poly: poly[chan],
			intern_exciter: internExciter[chan],
			easteregg: easteregg[chan],
			bypass: bypass[chan]
		);

		// MiRings outputs 2 channels [EVEN, ODD] — sum to mono per channel
		wet = (ringsOut[0] + ringsOut[1]) * 0.5;

		// Apply amplitude with curve
		wet = wet * amp[chan].lincurve(0, 1, 0, 1, 4);

		// Crossfade dry/wet
		XFade2.ar(input[chan], wet, mix[chan] * 2 - 1);
	});

	// Output
	Out.ar(OceanodeOutput.kr(\out), sig);

}, description: "Mutable Instruments Rings resonator — all parameters exposed: modal/string/FM models, polyphony, internal exciter, easter egg", category: "Effect/Resonators");