(
// OTT — 3-band upward + downward compressor.
// Fixes: Linkwitz-Riley 4 split with allpass compensation (bands sum flat; BLowPass4/BHiPass4
// with a residual mid did not); gain computed in dB from a per-sample follower and smoothed;
// upward boost capped by upmax and faded out below -80..-68 dB so the noise floor isn't pumped
// up (the old silence switch jumped from 1e4x to 1x); per-band hard clip replaced by a soft clip
// that is linear up to half of targetamp; Mix is linear against the phase-matched band sum.
~synthCreator.value("OTT", {|n|
	var input, low, mid, high, rest, dryAP, processed;
	var outputVol, preclip, targetAmp, mix, lowFreq, highFreq;
	var upRatio, downRatio, upThresh, downThresh, upMax;
	var attackTime, releaseTime, ottBandProcess;

	outputVol = OceanodeParameterLag.ar(\outputvol, 0.5, n, 0, 1, "vf", 1/30, true).lincurve(0, 1, 0, 1, 4);
	preclip   = OceanodeParameter.ar(\preclip, 0.5, n, 0, 1, "vf");
	targetAmp = OceanodeParameter.ar(\targetamp, 1, n, 0.1, 2, "vf");
	mix       = OceanodeParameter.ar(\mix, 1, n, 0, 1, "vf");

	lowFreq  = OceanodeParameter.ar(\lowfreq, 250, n, 50, 500, "vf");
	highFreq = OceanodeParameter.ar(\highfreq, 2000, n, 1000, 8000, "vf").max(lowFreq * 1.5);

	upRatio    = OceanodeParameter.ar(\upratio, 5, n, 1, 10, "vf").max(1);
	downRatio  = OceanodeParameter.ar(\downratio, 10, n, 1, 20, "vf").max(1);
	upThresh   = OceanodeParameter.ar(\upthresh, 0.1, n, 0.01, 0.5, "vf");
	downThresh = OceanodeParameter.ar(\downthresh, 0.5, n, 0.3, 0.9, "vf");
	upMax      = OceanodeParameter.ar(\upmax, 24, n, 0, 48, "vf");

	attackTime  = OceanodeParameter.ar(\attack, 0.002, n, 0.0001, 0.05, "vf");
	releaseTime = OceanodeParameter.ar(\release, 0.05, n, 0.01, 0.5, "vf");

	ottBandProcess = {|signal|
		var lvl, up, down, fade, gDb, c;
		lvl  = Amplitude.ar(signal, attackTime, releaseTime).max(-120.dbamp).ampdb;
		fade = ((lvl + 80) / 12).clip(0, 1);
		up   = ((upThresh.ampdb - lvl).max(0) * (1 - upRatio.reciprocal)).min(upMax) * fade;
		down = (lvl - downThresh.ampdb).max(0) * (1 - downRatio.reciprocal);
		gDb  = Lag.ar(up - down, attackTime);
		c    = signal * gDb.dbamp * targetAmp * preclip;
		(c / targetAmp).softclip * targetAmp;
	};

	input = In.ar(OceanodeInput.kr(\in), n);

	low  = BAllPass.ar(LPF.ar(LPF.ar(input, lowFreq), lowFreq), highFreq, 2.sqrt);
	rest = HPF.ar(HPF.ar(input, lowFreq), lowFreq);
	mid  = LPF.ar(LPF.ar(rest, highFreq), highFreq);
	high = HPF.ar(HPF.ar(rest, highFreq), highFreq);
	dryAP = low + mid + high;

	processed = (ottBandProcess.(low) + ottBandProcess.(mid) + ottBandProcess.(high)) * outputVol;

	Out.ar(OceanodeOutput.kr(\out), LinXFade2.ar(dryAP, processed, mix.linlin(0, 1, -1, 1)));
}, description: "OTT-style multiband upward/downward compressor", category: "Effect/Dynamics");
)
