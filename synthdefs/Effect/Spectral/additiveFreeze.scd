~synthCreator.value("AdditiveFreeze", {|n|
	var input, sig, dry, freeze, mix;
	var fftSize = 1024;
	var numBins = (fftSize / 2).asInteger;

	// Parameters
	freeze = OceanodeParameter.ar(\freeze, 0, n, 0, 1, "vi");
	mix = OceanodeParameter.ar(\mix, 1, n, 0, 1, "vf");

	input = In.ar(OceanodeInput.kr(\in), n);
	dry = input;

	sig = n.collect({ |i|
		var m_chain, binData, magnitudes, frequencies, trigger, freezeBus;

		// 1. Analyze input
		m_chain = FFT(LocalBuf(fftSize), input[i]);

		// 2. Unpack FFT
		binData = UnpackFFT(m_chain, fftSize, 0, numBins - 1);

		// 3. Fix: Convert audio-rate freeze to control-rate for the trigger
		freezeBus = A2K.kr(freeze[i]);
		trigger = HPZ1.kr(freezeBus) > 0;

		// 4. Extract magnitudes (Resynthesize 128 bins for efficiency)
		magnitudes = 128.collect({ |j|
			// Latch holds the magnitude value when trigger fires
			Latch.kr(binData[j * 2], trigger);
		});

		frequencies = 128.collect({ |j|
			j * (SampleRate.ir / fftSize);
		});

		// 5. Klang: The pure sine bank (Zero grain)
		Klang.ar(`[frequencies, magnitudes, nil]) * 0.5;
	});

	// Crossfade
	sig = XFade2.ar(dry, sig, mix * 2 - 1);

	Out.ar(OceanodeOutput.kr(\out), sig);
}, description: "True Sinusoidal Resynthesis (Zero-Grain)", category: "Effect/Spectral");