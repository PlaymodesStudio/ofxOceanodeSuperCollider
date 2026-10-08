(
~synthCreator.value("360Panner", {|n|
	var input, angle, width, distance, dopplerAmt, pan2dist, absorbAmt, absorbLowCut, reverbAmt, revDecay, levels, output;
	var maxITD = 0.00065;

	// Array of mono inputs. Angle convention: 0 front, 0.25 right, 0.5 rear, 0.75 left.
	input = In.ar(OceanodeInput.kr(\in), n);
	angle = OceanodeParameterLag.ar(\angle, 0, n, 0, 1, "vf", 1/30, true);
	width = OceanodeParameterLag.kr(\width, 0, n, 0, 1, "vf", 1/30, true);
	distance = OceanodeParameterLag.kr(\distance, 0, n, 0, 1, "vf", 1/30, true);
	dopplerAmt = OceanodeParameterLag.kr(\doppler, 0, n, 0, 1, "vf", 1/30, true);
	pan2dist = OceanodeParameterLag.kr(\pan2dist, 0, n, 0, 1, "vf", 1/30, true);
	absorbAmt = OceanodeParameterLag.kr(\absorb, 1, n, 0, 1, "vf", 1/30, true);
	absorbLowCut = OceanodeParameterLag.kr(\abslowcut, 76, n, 20, 135, "vf", 1/30, true).midicps;
	reverbAmt = OceanodeParameterLag.kr(\reverb, 0, n, 0, 1, "vf", 1/30, true);
	revDecay = OceanodeParameterLag.kr(\revdecay, 0.55, n, 0, 1, "vf", 1/30, true);
	levels = OceanodeParameterLag.ar(\levels, 1, n, 0, 1, "vf", 1/30, true).lincurve(0, 1, 0, 1, 4);

	output = Mix.fill(n, {|i|
		var src, radians, x, y, side, rear;
		var dist, panDistance, distAmp, directScale, diffuseScale;
		var absorbDepth, absorbFreq, verbSend, verbTime;
		var leftDelay, rightDelay, leftGain, rightGain, left, right;
		var direct, haloA, haloB, halo, rearA, rearB, rearHalo, verbSrc, verb, voice;
		var widthRadians, panA, panB;
		var panPos, speakerWidth, haloOffset, haloPosA, haloPosB;
		var directSrc, haloSrcA, haloSrcB, rearSrcA, rearSrcB;

		src = LeakDC.ar(input[i]);
		panDistance = (angle[i] - 0.5).abs.min(1 - (angle[i] - 0.5).abs) * 2;
		panDistance = panDistance.clip(0, 1);
		src = DelayC.ar(src, 0.08, panDistance * dopplerAmt[i] * 0.06);
		dist = (distance[i] + (panDistance * pan2dist[i])).clip(0, 1);
		distAmp = (1 - dist).clip(0, 1).squared;
		directScale = 1 - (dist * 0.35);
		diffuseScale = 1 + (dist * 0.9);
		absorbDepth = (dist * absorbAmt[i]).clip(0, 1);
		absorbFreq = absorbDepth.linexp(0, 1, 20000, absorbLowCut[i]);
		verbSend = (dist * reverbAmt[i]).clip(0, 1);
		verbTime = revDecay[i].linexp(0, 1, 0.35, 7);
		src = LPF.ar(src, absorbFreq);
		src = BHiShelf.ar(src, 3600, 0.7, absorbDepth * -9);
		verbSrc = src * distAmp * verbSend;
		radians = angle[i] * 2pi;
		x = radians.sin;        // left/right cue, positive is right
		y = radians.cos;        // front/back cue, positive is front
		side = x.abs;
		rear = y.neg.clip(0, 1);

		voice = if(n <= 2, {
			// Stereo psychoacoustic branch.
			// Interaural time difference: delay the far ear by up to about 0.65 ms.
			leftDelay = x.max(0) * maxITD;
			rightDelay = x.neg.max(0) * maxITD;
			left = DelayC.ar(src, 0.002, leftDelay);
			right = DelayC.ar(src, 0.002, rightDelay);

			// Equal-power interaural level difference.
			leftGain = ((1 - x.clip(-1, 1)) * 0.5).sqrt;
			rightGain = ((1 + x.clip(-1, 1)) * 0.5).sqrt;
			left = left * leftGain;
			right = right * rightGain;

			// Head-shadow and rear spectral cues.
			left = BHiShelf.ar(left, 4500, 0.7, -7 * x.max(0) * side);
			right = BHiShelf.ar(right, 4500, 0.7, -7 * x.neg.max(0) * side);
			left = BHiShelf.ar(left, 3200, 0.7, -5 * rear);
			right = BHiShelf.ar(right, 3200, 0.7, -5 * rear);
			left = BPeakEQ.ar(left, 6500, 0.9, -5 * rear);
			right = BPeakEQ.ar(right, 6500, 0.9, -5 * rear);

			direct = [left, right] * (1 - (rear * 0.22)) * directScale;

			// Width is made from two decorrelated virtual sources around the target angle.
			widthRadians = width[i] * (20 + (rear * 55)) * (pi / 180);
			panA = (radians - widthRadians).sin.clip(-1, 1);
			panB = (radians + widthRadians).sin.clip(-1, 1);
			haloA = AllpassC.ar(src, 0.08, 0.006 + (width[i] * 0.009) + (rear * 0.004) + (dist * 0.018), 0.04 + (width[i] * 0.14) + (dist * 0.1));
			haloB = AllpassC.ar(src, 0.08, 0.011 + (width[i] * 0.011) + (rear * 0.006) + (dist * 0.026), 0.06 + (width[i] * 0.18) + (dist * 0.12));
			halo = (Pan2.ar(haloA, panA) + Pan2.ar(haloB, panB)) * 0.5;
			halo = BHiShelf.ar(halo, 5000, 0.7, -3 * rear);
			halo = halo * width[i] * (0.35 + (rear * 0.45)) * diffuseScale;

			// Rear positions get a small dark, decorrelated early reflection pair.
			rearA = DelayC.ar(BHiShelf.ar(src, 2500, 0.7, -8), 0.08, 0.014 + (dist * 0.018));
			rearB = DelayC.ar(BHiShelf.ar(src, 2500, 0.7, -8), 0.08, 0.021 + (dist * 0.026));
			rearHalo = [rearA, rearB] * rear * (0.18 + (width[i] * 0.18) + (dist * 0.12)) * diffuseScale;

			verb = JPverb.ar(verbSrc, verbTime, 0.25, 1.15, 0.7, 0.08, 1.6, 1, 1, 0.75, 80, 12000);
			verb = Balance2.ar(verb[0], verb[1], x); // JPverb is already stereo: Pan2 of it nested to 2x2

			((direct + halo + rearHalo) * distAmp) + verb;
		}, {
			// Multichannel branch: real circular panning first, subtle psychoacoustic color second.
			panPos = angle[i] * 2;
			speakerWidth = 1 + (width[i] * n.min(8) * 0.75);

			directSrc = BHiShelf.ar(src, 3200, 0.7, -3 * rear);
			directSrc = BPeakEQ.ar(directSrc, 6500, 0.9, -3 * rear);
			direct = PanAz.ar(n, directSrc, panPos, 1, speakerWidth, orientation: 0.5) * directScale;

			haloOffset = width[i] * (0.08 + (rear * 0.16));
			haloPosA = (angle[i] - haloOffset).wrap(0, 1) * 2;
			haloPosB = (angle[i] + haloOffset).wrap(0, 1) * 2;
			haloSrcA = AllpassC.ar(src, 0.08, 0.007 + (width[i] * 0.011) + (rear * 0.004) + (dist * 0.016), 0.05 + (width[i] * 0.14) + (dist * 0.08));
			haloSrcB = AllpassC.ar(src, 0.08, 0.013 + (width[i] * 0.013) + (rear * 0.006) + (dist * 0.024), 0.07 + (width[i] * 0.18) + (dist * 0.1));
			halo = (
				PanAz.ar(n, haloSrcA, haloPosA, 1, speakerWidth + 1, orientation: 0.5) +
				PanAz.ar(n, haloSrcB, haloPosB, 1, speakerWidth + 1, orientation: 0.5)
			) * width[i] * 0.22 * diffuseScale;

			rearSrcA = DelayC.ar(BHiShelf.ar(src, 2600, 0.7, -7), 0.08, 0.015 + (dist * 0.016));
			rearSrcB = DelayC.ar(BHiShelf.ar(src, 2600, 0.7, -7), 0.08, 0.023 + (dist * 0.024));
			rearHalo = (
				PanAz.ar(n, rearSrcA, (angle[i] - 0.08).wrap(0, 1) * 2, 1, speakerWidth + 1.5, orientation: 0.5) +
				PanAz.ar(n, rearSrcB, (angle[i] + 0.08).wrap(0, 1) * 2, 1, speakerWidth + 1.5, orientation: 0.5)
			) * rear * (0.08 + (width[i] * 0.12) + (dist * 0.08)) * diffuseScale;

			verb = JPverb.ar(verbSrc, verbTime, 0.25, 1.15, 0.7, 0.08, 1.6, 1, 1, 0.75, 80, 12000);
			verb = PanAz.ar(n, verb.sum * 0.5.sqrt, /* JPverb is stereo: sum it before PanAz */ panPos, 1, speakerWidth + 2, orientation: 0.5);

			((direct + halo + rearHalo) * distAmp) + verb;
		});

		voice * levels[i];
	});

	output = LeakDC.ar(output);

	Out.ar(OceanodeOutput.kr(\out), output);
}, description: "Multi-source mono-input 360-degree panner with stereo psychoacoustic and multichannel circular modes", category: "Spatial/Panning");
)
