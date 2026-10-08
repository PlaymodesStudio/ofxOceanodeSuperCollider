(
~synthCreator.value("XYPanner", {|n|
	var input, xpos, ypos, width, doppler, distAmpAmt, distDiffAmt, absorbAmt, absorbLowCut, reverbAmt, revDecay;
	var itdAmt, ildAmt, shadowAmt, rearCueAmt, levels, output;
	var maxITD = 0.00065;
	var maxDopplerDelay = 0.08;

	// Array of mono sound objects. Listener is at x=0, y=0.
	// Coordinates: x=-1 left, x=1 right, y=1 front, y=-1 rear.
	input = In.ar(OceanodeInput.kr(\in), n);
	xpos = OceanodeParameterLag.ar(\xpos, 0, n, -1, 1, "vf", 1/30, true);
	ypos = OceanodeParameterLag.ar(\ypos, 0, n, -1, 1, "vf", 1/30, true);
	width = OceanodeParameterLag.kr(\width, 0, n, 0, 1, "vf", 1/30, true);
	doppler = OceanodeParameterLag.kr(\doppler, 0, n, 0, 1, "vf", 1/30, true);
	distAmpAmt = OceanodeParameterLag.kr(\distamp, 0, n, 0, 1, "vf", 1/30, true);
	distDiffAmt = OceanodeParameterLag.kr(\distdiff, 0, n, 0, 1, "vf", 1/30, true);
	absorbAmt = OceanodeParameterLag.kr(\absorb, 1, n, 0, 1, "vf", 1/30, true);
	absorbLowCut = OceanodeParameterLag.kr(\abslowcut, 76, n, 20, 135, "vf", 1/30, true).midicps;
	reverbAmt = OceanodeParameterLag.kr(\reverb, 0, n, 0, 1, "vf", 1/30, true);
	revDecay = OceanodeParameterLag.kr(\revdecay, 0.55, n, 0, 1, "vf", 1/30, true);
	itdAmt = OceanodeParameterLag.kr(\itd, 1, n, 0, 1, "vf", 1/30, true);
	ildAmt = OceanodeParameterLag.kr(\ild, 1, n, 0, 1, "vf", 1/30, true);
	shadowAmt = OceanodeParameterLag.kr(\shadow, 1, n, 0, 1, "vf", 1/30, true);
	rearCueAmt = OceanodeParameterLag.kr(\rearcue, 1, n, 0, 1, "vf", 1/30, true);
	levels = OceanodeParameterLag.ar(\levels, 1, n, 0, 1, "vf", 1/30, true).lincurve(0, 1, 0, 1, 4);

	output = Mix.fill(n, {|i|
		var src, x, y, dist, distAmp, directScale, diffuseScale, azimuth, rear, side;
		var absorbDepth, absorbFreq, verbSend, verbTime;
		var leftDelay, rightDelay, leftGain, rightGain, left, right;
		var direct, haloA, haloB, halo, rearA, rearB, rearHalo, verbSrc, verb, voice;
		var widthRadians, panA, panB, panPos, speakerWidth, haloOffset;
		var directSrc, haloSrcA, haloSrcB, rearSrcA, rearSrcB;

		x = xpos[i].clip(-1, 1);
		y = ypos[i].clip(-1, 1);
		dist = hypot(x, y).clip(0, 1);
		distAmp = (1 - (dist * distAmpAmt[i])).clip(0, 1).squared;
		directScale = 1 - (dist * distDiffAmt[i] * 0.35);
		diffuseScale = 1 + (dist * distDiffAmt[i] * 0.9);
		azimuth = atan2(x, y);
		rear = y.neg.clip(0, 1);
		side = x.abs;
		absorbDepth = (dist * absorbAmt[i]).clip(0, 1);
		absorbFreq = absorbDepth.linexp(0, 1, 20000, absorbLowCut[i]);
		verbSend = (dist * reverbAmt[i]).clip(0, 1);
		verbTime = revDecay[i].linexp(0, 1, 0.35, 7);

		src = LeakDC.ar(input[i]);
		src = DelayC.ar(src, maxDopplerDelay, dist * doppler[i] * maxDopplerDelay);
		src = LPF.ar(src, absorbFreq);
		src = BHiShelf.ar(src, 3600, 0.7, absorbDepth * -9);
		verbSrc = src * distAmp * verbSend;

		voice = if(n <= 2, {
			// Stereo branch: ITD, ILD, head-shadow, rear cues, and diffuse width.
			leftDelay = x.max(0) * maxITD * itdAmt[i];
			rightDelay = x.neg.max(0) * maxITD * itdAmt[i];
			left = DelayC.ar(src, 0.002, leftDelay);
			right = DelayC.ar(src, 0.002, rightDelay);

			leftGain = ((1 - (x * ildAmt[i]).clip(-1, 1)) * 0.5).sqrt;
			rightGain = ((1 + (x * ildAmt[i]).clip(-1, 1)) * 0.5).sqrt;
			left = left * leftGain;
			right = right * rightGain;

			left = BHiShelf.ar(left, 4500, 0.7, -7 * x.max(0) * side * shadowAmt[i]);
			right = BHiShelf.ar(right, 4500, 0.7, -7 * x.neg.max(0) * side * shadowAmt[i]);
			left = BHiShelf.ar(left, 3200, 0.7, -5 * rear * rearCueAmt[i]);
			right = BHiShelf.ar(right, 3200, 0.7, -5 * rear * rearCueAmt[i]);
			left = BPeakEQ.ar(left, 6500, 0.9, -5 * rear * rearCueAmt[i]);
			right = BPeakEQ.ar(right, 6500, 0.9, -5 * rear * rearCueAmt[i]);

			direct = [left, right] * (1 - (rear * 0.22 * rearCueAmt[i])) * directScale;

			widthRadians = width[i] * (20 + (rear * 55 * rearCueAmt[i])) * (pi / 180);
			panA = (azimuth - widthRadians).sin.clip(-1, 1);
			panB = (azimuth + widthRadians).sin.clip(-1, 1);
			haloA = AllpassC.ar(src, 0.08, 0.006 + (width[i] * 0.009) + (rear * 0.004) + (dist * distDiffAmt[i] * 0.018), 0.04 + (width[i] * 0.14) + (dist * distDiffAmt[i] * 0.1));
			haloB = AllpassC.ar(src, 0.08, 0.011 + (width[i] * 0.011) + (rear * 0.006) + (dist * distDiffAmt[i] * 0.026), 0.06 + (width[i] * 0.18) + (dist * distDiffAmt[i] * 0.12));
			halo = (Pan2.ar(haloA, panA) + Pan2.ar(haloB, panB)) * 0.5;
			halo = BHiShelf.ar(halo, 5000, 0.7, -3 * rear * rearCueAmt[i]);
			halo = halo * width[i] * (0.35 + (rear * 0.45 * rearCueAmt[i])) * diffuseScale;

			rearA = DelayC.ar(BHiShelf.ar(src, 2500, 0.7, -8 * rearCueAmt[i]), 0.08, 0.014 + (dist * distDiffAmt[i] * 0.018));
			rearB = DelayC.ar(BHiShelf.ar(src, 2500, 0.7, -8 * rearCueAmt[i]), 0.08, 0.021 + (dist * distDiffAmt[i] * 0.026));
			rearHalo = [rearA, rearB] * rear * rearCueAmt[i] * (0.18 + (width[i] * 0.18) + (dist * distDiffAmt[i] * 0.12)) * diffuseScale;

			verb = JPverb.ar(verbSrc, verbTime, 0.25, 1.15, 0.7, 0.08, 1.6, 1, 1, 0.75, 80, 12000);
			verb = Balance2.ar(verb[0], verb[1], x); // JPverb is already stereo: Pan2 of it nested to 2x2

			((direct + halo + rearHalo) * distAmp) + verb;
		}, {
			// Multichannel branch: XY position becomes circular azimuth for PanAz.
			panPos = (azimuth / pi).clip(-1, 1);
			speakerWidth = 1 + (width[i] * n.min(8) * 0.75);

			directSrc = BHiShelf.ar(src, 3200, 0.7, -3 * rear * rearCueAmt[i]);
			directSrc = BPeakEQ.ar(directSrc, 6500, 0.9, -3 * rear * rearCueAmt[i]);
			direct = PanAz.ar(n, directSrc, panPos, 1, speakerWidth, orientation: 0.5) * directScale;

			haloOffset = width[i] * (0.16 + (rear * 0.32 * rearCueAmt[i]));
			haloSrcA = AllpassC.ar(src, 0.08, 0.007 + (width[i] * 0.011) + (rear * 0.004) + (dist * distDiffAmt[i] * 0.016), 0.05 + (width[i] * 0.14) + (dist * distDiffAmt[i] * 0.08));
			haloSrcB = AllpassC.ar(src, 0.08, 0.013 + (width[i] * 0.013) + (rear * 0.006) + (dist * distDiffAmt[i] * 0.024), 0.07 + (width[i] * 0.18) + (dist * distDiffAmt[i] * 0.1));
			halo = (
				PanAz.ar(n, haloSrcA, (panPos - haloOffset).wrap(-1, 1), 1, speakerWidth + 1, orientation: 0.5) +
				PanAz.ar(n, haloSrcB, (panPos + haloOffset).wrap(-1, 1), 1, speakerWidth + 1, orientation: 0.5)
			) * width[i] * 0.22 * diffuseScale;

			rearSrcA = DelayC.ar(BHiShelf.ar(src, 2600, 0.7, -7 * rearCueAmt[i]), 0.08, 0.015 + (dist * distDiffAmt[i] * 0.016));
			rearSrcB = DelayC.ar(BHiShelf.ar(src, 2600, 0.7, -7 * rearCueAmt[i]), 0.08, 0.023 + (dist * distDiffAmt[i] * 0.024));
			rearHalo = (
				PanAz.ar(n, rearSrcA, (panPos - 0.16).wrap(-1, 1), 1, speakerWidth + 1.5, orientation: 0.5) +
				PanAz.ar(n, rearSrcB, (panPos + 0.16).wrap(-1, 1), 1, speakerWidth + 1.5, orientation: 0.5)
			) * rear * rearCueAmt[i] * (0.08 + (width[i] * 0.12) + (dist * distDiffAmt[i] * 0.08)) * diffuseScale;

			verb = JPverb.ar(verbSrc, verbTime, 0.25, 1.15, 0.7, 0.08, 1.6, 1, 1, 0.75, 80, 12000);
			verb = PanAz.ar(n, verb.sum * 0.5.sqrt, /* JPverb is stereo: sum it before PanAz */ panPos, 1, speakerWidth + 2, orientation: 0.5);

			((direct + halo + rearHalo) * distAmp) + verb;
		});

		voice * levels[i];
	});

	output = LeakDC.ar(output);

	Out.ar(OceanodeOutput.kr(\out), output);
}, description: "XY multi-source panner with distance, Doppler, and psychoacoustic stereo/multichannel cues", category: "Spatial/Panning");
)
