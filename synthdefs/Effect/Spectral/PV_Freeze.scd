(
~synthCreator.value("PV_Freeze", {|n|
	var input, sig, dry, freeze, mix, mode, shift;
	var fftSize = 2048;

	// Parameters
	freeze = OceanodeParameter.ar(\freeze, 0, n, 0, 1, "vi");
	mix = OceanodeParameter.ar(\mix, 1, n, 0, 1, "vf");

	// Mode 0 = Loop (Grainy), Mode 1 = Blur (Smooth)
	mode = OceanodeParameterDropdown.ar(\mode, 0, n, "Loop:Blur");

	// Extra control for phase rotation (0 to 360 degrees)
	shift = OceanodeParameter.ar(\phase_shift, 0, n, 0, 360, "vf");

	input = In.ar(OceanodeInput.kr(\in), n);
	dry = input;

	sig = n.collect({ |i|
		var tempChain;
		tempChain = FFT(LocalBuf(fftSize), input[i]);

		// 1. Core Freeze
		tempChain = PV_Freeze(tempChain, freeze[i]);

		// 2. Optional Phase Diffusion (Blur)
		// We multiply the freeze trigger by the mode state
		// This ensures diffusion only happens when Frozen AND Mode is 'Blur'
		tempChain = PV_Diffuser(tempChain, freeze[i] * mode[i]);

		// 3. Manual Phase Shift (for subtle texture adjustments)
		tempChain = PV_PhaseShift(tempChain, shift[i].degrad);

		IFFT(tempChain);
	});

	sig = XFade2.ar(dry, sig, mix * 2 - 1);
	Out.ar(OceanodeOutput.kr(\out), sig);
}, description: "Spectral freeze with selectable Loop or Blur modes", category: "Effect/Spectral");
)
