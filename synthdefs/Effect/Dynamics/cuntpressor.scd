(
// Cuntpressor — peak-normalising AGC with a clip stage.
// Fixes: OnePole coefficients now exp(-2pi f / SR), so targetfreq/peakfreq are real cutoffs
// (the old 1 - f/SR put the real cutoff at f/2pi: 160 Hz behaved like ~25 Hz; defaults
// were rescaled to keep the old default sound). The gain is capped by maxgain and fades to unity
// in silence instead of switching from 1e5x to 1x at -100 dB.
~synthCreator.value("Cuntpressor", {|n|
	var input, inputAbs, peakSignal, maxSignal, gain, processed, wetSignal;
	var outputVol, preclip, targetAmp, targetFreq, peakFreq, mix, maxGain, peakCoef, smoothCoef, presence;

	outputVol  = OceanodeParameterLag.ar(\outputvol, 0.5, n, 0, 1, "vf", 1/30, true).lincurve(0, 1, 0, 1, 4);
	preclip    = OceanodeParameter.ar(\preclip, 0.5, n, 0, 1, "vf");
	targetAmp  = OceanodeParameter.ar(\targetamp, 1, n, 0.1, 2, "vf");
	targetFreq = OceanodeParameter.ar(\targetfreq, 25, n, 1, 300, "vf");
	peakFreq   = OceanodeParameter.ar(\peakfreq, 13, n, 1, 150, "vf");
	maxGain    = OceanodeParameter.ar(\maxgain, 24, n, 0, 60, "vf");
	mix        = OceanodeParameter.ar(\mix, 1, n, 0, 1, "vf");

	input = In.ar(OceanodeInput.kr(\in), n);

	peakCoef   = exp(-2pi * peakFreq / SampleRate.ir);
	smoothCoef = exp(-2pi * targetFreq / SampleRate.ir);

	inputAbs   = input.abs;
	peakSignal = OnePole.ar(inputAbs, peakCoef);
	maxSignal  = max(inputAbs, peakSignal);

	// gain = target / level, capped; below -90..-80 dB it eases back to unity (no noise blow-up)
	presence = ((maxSignal.max(-120.dbamp).ampdb + 90) / 10).clip(0, 1);
	gain = (targetAmp / maxSignal.max(-120.dbamp)).min(maxGain.dbamp);
	gain = 1 + ((gain - 1) * presence);
	gain = OnePole.ar(gain, smoothCoef);

	processed = (input * gain * preclip).clip2(targetAmp);
	wetSignal = processed * outputVol;

	Out.ar(OceanodeOutput.kr(\out), LinXFade2.ar(input, wetSignal, mix.linlin(0, 1, -1, 1)));

}, description: "Frequency aware compressor with peak tracking", category: "Effect/Dynamics");
)
