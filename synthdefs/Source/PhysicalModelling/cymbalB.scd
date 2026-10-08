(
~synthCreator.value("CymbalB", {|n|
	var gate, trig, atrig, amp;
	var pitchHi, pitchLo, pitchDecay, hitDecay, noiseDecay, outRelease;
	var noiseHPF, noiseLPF, resHPF, resLPF, fbHPF;
	var drive, hitSend, noiseSend, hitMix, noiseMix, resMix;
	var feedback, predelay, ap1Delay, ap1Decay, ap2Delay, ap2Decay;
	var size, inharm, resDecay;
	var morph, bright, hardness, wash, bloom, damp, pingFreq, pingMix, shimmer;
	var baseDelays, pingRatios, fbIn, dispOut, sig;

	// Trigger follows Crash: first gate channel fires one shared hit for all outputs.
	gate = OceanodeParameter.kr(\gate, 0, n, 0, 1, "vi");
	trig = Trig.kr(gate[0], 0.01);
	atrig = T2A.ar(trig);

	// Exciter and output controls. Defaults match the original cymbals SynthDef.
	amp = OceanodeParameterLag.ar(\amp, 1, n, 0, 1, "vf", 1/30, true).lincurve(0, 1, 0, 1, 4);
	drive = OceanodeParameterLag.kr(\drive, 2, n, 0.1, 12, "vf", 1/30, true);
	pitchHi = OceanodeParameterLag.kr(\pitchhi, 8000, n, 100, 20000, "vf", 1/30, true);
	pitchLo = OceanodeParameterLag.kr(\pitchlo, 2500, n, 100, 20000, "vf", 1/30, true);
	pitchDecay = OceanodeParameterLag.kr(\pitchdec, 0.005, n, 0.001, 0.1, "vf", 1/30, true);
	hitDecay = OceanodeParameterLag.kr(\hitdec, 0.012, n, 0.001, 1, "vf", 1/30, true);
	noiseDecay = OceanodeParameterLag.kr(\noisedec, 0.4, n, 0.001, 5, "vf", 1/30, true);
	outRelease = OceanodeParameterLag.kr(\release, 3.5, n, 0.01, 20, "vf", 1/30, true);

	// Macro controls: morph travels Ride -> Cymbal -> Crash.
	morph = OceanodeParameterLag.kr(\morph, 0.5, n, 0, 1, "vf", 1/30, true);
	bright = OceanodeParameterLag.kr(\bright, 0.5, n, 0, 1, "vf", 1/30, true);
	hardness = OceanodeParameterLag.kr(\hardness, 0.5, n, 0, 1, "vf", 1/30, true);
	wash = OceanodeParameterLag.kr(\wash, 0.5, n, 0, 1, "vf", 1/30, true);
	bloom = OceanodeParameterLag.kr(\bloom, 0.25, n, 0, 1, "vf", 1/30, true);
	damp = OceanodeParameterLag.kr(\damp, 0.35, n, 0, 1, "vf", 1/30, true);
	pingFreq = OceanodeParameterLag.kr(\pingfreq, 4200, n, 500, 12000, "vf", 1/30, true);
	pingMix = OceanodeParameterLag.kr(\pingmix, 0.15, n, 0, 1, "vf", 1/30, true);
	shimmer = OceanodeParameterLag.kr(\shimmer, 0.18, n, 0, 1, "vf", 1/30, true);

	noiseLPF = OceanodeParameterLag.kr(\noiselpf, 16000, n, 100, 20000, "vf", 1/30, true);
	noiseHPF = OceanodeParameterLag.kr(\noisehpf, 2500, n, 20, 20000, "vf", 1/30, true);
	resLPF = OceanodeParameterLag.kr(\reslpf, 18000, n, 100, 20000, "vf", 1/30, true);
	resHPF = OceanodeParameterLag.kr(\reshpf, 2500, n, 20, 20000, "vf", 1/30, true);
	fbHPF = OceanodeParameterLag.kr(\fbhpf, 60, n, 20, 1000, "vf", 1/30, true);

	// Balance controls for the dispersion input and final dry/resonator mix.
	hitSend = OceanodeParameterLag.kr(\hitsend, 0.8, n, 0, 2, "vf", 1/30, true);
	noiseSend = OceanodeParameterLag.kr(\noisesend, 0.8, n, 0, 2, "vf", 1/30, true);
	hitMix = OceanodeParameterLag.kr(\hitmix, 0.4, n, 0, 2, "vf", 1/30, true);
	noiseMix = OceanodeParameterLag.kr(\noisemix, 0.5, n, 0, 2, "vf", 1/30, true);
	resMix = OceanodeParameterLag.kr(\resmix, 1, n, 0, 4, "vf", 1/30, true);

	// Dispersion feedback path. Keep feedback below 1 for stable cymbal hits.
	feedback = OceanodeParameterLag.kr(\feedback, 0.62, n, 0, 0.99, "vf", 1/30, true);
	predelay = OceanodeParameterLag.kr(\predelay, 0.002, n, 0.0001, 0.1, "vf", 1/30, true);
	ap1Delay = OceanodeParameterLag.kr(\ap1delay, 0.0005, n, 0.0001, 0.1, "vf", 1/30, true);
	ap1Decay = OceanodeParameterLag.kr(\ap1decay, 0.005, n, 0.0001, 1, "vf", 1/30, true);
	ap2Delay = OceanodeParameterLag.kr(\ap2delay, 0.004, n, 0.0001, 0.1, "vf", 1/30, true);
	ap2Decay = OceanodeParameterLag.kr(\ap2decay, 0.04, n, 0.0001, 1, "vf", 1/30, true);

	// Comb-bank material controls.
	size = OceanodeParameterLag.kr(\size, 1, n, 0.25, 4, "vf", 1/30, true);
	inharm = OceanodeParameterLag.kr(\inharm, 0, n, -0.75, 0.75, "vf", 1/30, true);
	resDecay = OceanodeParameterLag.kr(\resdecay, 2, n, 0.01, 20, "vf", 1/30, true);
	baseDelays = #[3.5, 4.1, 4.8, 5.6, 6.5, 7.6, 8.8, 10.3, 12.0, 14.0, 16.3, 19.0];
	pingRatios = #[1, 1.37, 1.92, 2.51, 3.24];

	fbIn = LeakDC.ar(LocalIn.ar(n));
	dispOut = Array.newClear(n);

	// Run the original mono cymbal topology independently for each Oceanode channel.
	sig = Array.fill(n, {|chan|
		var pitch, trans, noise, disp, reso, ping, bloomSig, shimmerSig, out;
		var chMorph, rideAmt, cymAmt, crashAmt, chSize, chInharm, chBloom;
		var exciteScale, hitScale, noiseScale, feedbackAmt, decayScale, releaseScale;
		var toneLPF, toneHPF, tailTone, bloomEnv;

		chMorph = morph[chan];
		rideAmt = (1 - (chMorph * 2)).clip(0, 1);
		cymAmt = (1 - ((chMorph - 0.5).abs * 2)).clip(0, 1);
		crashAmt = ((chMorph - 0.5) * 2).clip(0, 1);

		chSize = (size[chan] * (1 + (crashAmt * 0.45) - (rideAmt * 0.22))).clip(0.25, 4);
		chInharm = (inharm[chan] + (crashAmt * 0.08) - (rideAmt * 0.03)).clip(-0.75, 0.75);
		chBloom = bloom[chan] * (0.3 + (cymAmt * 0.7) + (crashAmt * 1.5) - (rideAmt * 0.15)).clip(0, 2);
		exciteScale = (1 + ((hardness[chan] - 0.5) * 0.9)).clip(0.35, 2);
		hitScale = (1 + (rideAmt * 0.7) + ((hardness[chan] - 0.5) * 0.8) - (crashAmt * 0.15)).clip(0.2, 2.5);
		noiseScale = (1 + (crashAmt * 1.1) + ((wash[chan] - 0.5) * 1.2) - (rideAmt * 0.45)).clip(0.1, 3);
		feedbackAmt = (feedback[chan] * (1 + (crashAmt * 0.2) + (chBloom * 0.16) - (rideAmt * 0.12))).clip(0, 0.97);
		decayScale = (1 + (crashAmt * 1.25) + (chBloom * 0.8) - (rideAmt * 0.45)).clip(0.2, 4);
		releaseScale = (1 + (crashAmt * 0.75) + (chBloom * 0.45) - (rideAmt * 0.25)).clip(0.25, 3);

		tailTone = Env([1, 0], [outRelease[chan] * releaseScale], curve: -3).ar(0, atrig);
		toneLPF = (resLPF[chan] * (0.8 + (bright[chan] * 0.4)) * ((1 - (damp[chan] * 0.65)) + (damp[chan] * 0.65 * tailTone))).clip(100, 20000);
		toneHPF = (resHPF[chan] * (1 + (rideAmt * 0.25) - (crashAmt * 0.25) - ((wash[chan] - 0.5) * 0.3))).clip(20, 20000);

		// Sine transient for the stick layer and resonator excitation.
		pitch = Env([pitchHi[chan], pitchLo[chan]], [pitchDecay[chan]], curve: -5).ar(0, atrig);
		trans = (SinOsc.ar(pitch, 0, 1) * drive[chan] * exciteScale).tanh;
		trans = trans * Env.perc(0.001, hitDecay[chan], curve: -5).ar(0, atrig);

		// Noise for the stick layer and resonator excitation.
		noise = HPF.ar(
			LPF.ar(Hasher.ar(Sweep.ar()), (noiseLPF[chan] * (0.8 + (bright[chan] * 0.4))).clip(100, 20000)),
			(noiseHPF[chan] * (1 + (rideAmt * 0.25) - (crashAmt * 0.2))).clip(20, 20000)
		);
		noise = (noise * drive[chan] * exciteScale).tanh * Env.perc(0.001, noiseDecay[chan] * (1 + (crashAmt * 0.7)), curve: -5).ar(0, atrig);

		// Local feedback input for the dispersion path.
		disp = (noise * noiseSend[chan] * noiseScale) + (trans * hitSend[chan] * hitScale) + ((fbIn[chan] * feedbackAmt).clip2(0.99));
		disp = DelayC.ar(disp, 0.1, predelay[chan]);
		disp = AllpassC.ar(disp, 0.1, ap1Delay[chan], ap1Decay[chan]);
		disp = AllpassC.ar(disp, 0.1, ap2Delay[chan], ap2Decay[chan]);
		disp = HPF.ar(LeakDC.ar(disp), fbHPF[chan]);
		dispOut[chan] = disp;

		// Comb bank with short delay times acting as the resonator.
		reso = HPF.ar(disp, fbHPF[chan]);
		reso = Mix.fill(baseDelays.size, {|i|
			var highNorm = 1 - (i / (baseDelays.size - 1));
			var tilt = 1 + ((((i / (baseDelays.size - 1)) * 2) - 1) * chInharm);
			var modeDecay = (resDecay[chan] * decayScale * (1 - (highNorm * damp[chan] * 0.55))).clip(0.01, 20);
			CombC.ar(reso, 0.1, (baseDelays[i] * 0.001 * chSize * tilt).clip(0.0001, 0.1), modeDecay)
		}) * baseDelays.size.reciprocal;

		// Lowpass and highpass shaping for the resonator output.
		reso = HPF.ar(LPF.ar(reso, toneLPF), toneHPF);

		// Ride-like ping and crash-like bloom layers.
		ping = Mix.fill(pingRatios.size, {|i|
			var spread = 1 + ((((i / (pingRatios.size - 1)) * 2) - 1) * chInharm * 0.18);
			Ringz.ar(trans + (noise * 0.12), (pingFreq[chan] * pingRatios[i] * spread).clip(200, 19000), 0.04 + (rideAmt * 0.42) + (cymAmt * 0.12))
		}) * pingRatios.size.reciprocal;
		ping = HPF.ar(LPF.ar(ping, toneLPF), toneHPF);
		ping = ping * pingMix[chan] * (0.2 + (rideAmt * 1.5) + (cymAmt * 0.45)) * (0.65 + hardness[chan]);

		bloomEnv = Env.perc(0.035 + (chBloom * 0.16) + (crashAmt * 0.08), outRelease[chan] * releaseScale * (0.8 + chBloom), curve: -3).ar(0, atrig);
		bloomSig = reso + (disp * 0.35) + (noise * wash[chan] * 0.25);
		bloomSig = AllpassC.ar(bloomSig, 0.1, (0.011 * chSize).clip(0.0001, 0.1), 0.08 + (chBloom * 0.5));
		bloomSig = AllpassC.ar(bloomSig, 0.1, (0.019 * chSize).clip(0.0001, 0.1), 0.12 + (chBloom * 0.7));
		bloomSig = HPF.ar(LPF.ar(bloomSig, toneLPF * 0.9), toneHPF * 0.7);
		bloomSig = bloomSig * bloomEnv * chBloom * (0.45 + (crashAmt * 0.9) + (wash[chan] * 0.5));

		shimmerSig = HPF.ar(reso, 7000) * shimmer[chan] * (0.4 + (crashAmt * 0.9) + (bright[chan] * 0.5));

		// Mix resonator, noise, and transient, then apply the output envelope.
		out = ((reso * resMix[chan]) + (noise * noiseMix[chan] * noiseScale) + (trans * hitMix[chan] * hitScale) + ping + bloomSig + shimmerSig);
		out = out * amp[chan] * Env.perc(0.001, outRelease[chan] * releaseScale, curve: -3).ar(0, atrig);
		LeakDC.ar(out);
	});

	LocalOut.ar(dispOut);

	Out.ar(OceanodeOutput.kr(\out), sig);
}, description: "Dispersive physical-model cymbal with feedback diffusion and comb resonator bank", category: "Source/PhysicalModelling");
)
