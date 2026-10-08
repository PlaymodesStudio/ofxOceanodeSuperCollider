(
~synthCreator.value("FFTStretchRate", {|n|
	var buf, overlaps, fftSize, chains, pos, rate, levels, hasBuf;

	buf = OceanodeBuffer.kr(\bufnum); // single buffer shared by all channels (OceanodeBuffer has no size arg)
	hasBuf = buf >= 0;
	// BufFFT_BufCopy does not check for a negative buffer number: -1 (no buffer yet) read invalid
	// memory and crashed scsynth intermittently. Read buffer 0 instead and keep the output silent.
	buf = buf.max(0);
	overlaps = 2;
	fftSize = 16384;
	pos = OceanodeParameterLag.ar(\pos, 0, n, 0, 1, "vf", 1/30, true);
	rate = OceanodeParameterLag.ar(\rate, 1, n, -2, 2, "vf", 1/30, true);
	levels = OceanodeParameterLag.ar(\levels, 1, n, 0, 1, "vf", 1/30, true);

	chains = Array.fill(n, { |i|
		var chanBuf = buf;
		var chanPos = pos.asArray[i] * BufFrames.kr(chanBuf);
		var chanRate = rate.asArray[i] * BufRateScale.kr(chanBuf);
		var fftBuf = LocalBuf(fftSize ! overlaps);
		var chain;

		chain = BufFFTTrigger(fftBuf, 1 / overlaps, (0 .. (overlaps - 1)), overlaps);
		chain = BufFFT_BufCopy(chain, chanBuf, chanPos, chanRate);
		chain = BufFFT(chain);
		chain = PV_Diffuser(chain, chain > -1);
		Mix(BufIFFT(chain, 0)) * 0.8 * levels.asArray[i];
	});

	// silent (not NaN / +389 dB) while the buffer is missing or empty, and NaN-safe
	Out.ar(OceanodeOutput.kr(\out), Sanitize.ar(chains) * hasBuf * (BufFrames.kr(buf) > 0));
}, description: "Legacy FFT stretch player with direct per-channel rate control", category: "Source/Sampler");
)
