(
// Maximizer — drive into a lookahead peak limiter.
// Fixes: real lookahead (the detector reads the undelayed signal, only the audio is delayed —
// before, both were delayed, so it reacted late and relied on a hard clip); gain reduction is
// computed in dB with a soft knee and smoothed so it is fully applied by the time the peak
// arrives; character sets knee and release; preventISP uses a 4x oversampled true-peak detector
// instead of a high-passed rectifier; the final hard clip is replaced by a short SC
// Limiter brickwall that only catches residual overshoots.
~synthCreator.value("Maximizer", {|n, variables|
	var in, signal, taps, mid, det, gr, threshold, margin, character, preventISP;
	var release, knee, inputGain, outputGain, lookAhead = 0.005, over, xk;

	threshold  = OceanodeParameter.ar(\threshold, 0, n, -20, 0, "vf");
	margin     = OceanodeParameter.ar(\margin, 0, n, -6, 0, "vf");
	character  = OceanodeParameter.ar(\character, 4, n, 0, 10, "vf");
	preventISP = OceanodeParameterDropdown.kr(\preventisp, 0, n, "Off:On");

	in = LeakDC.ar(In.ar(OceanodeInput.kr(\in), n), 0.9995);   // ~4 Hz

	release    = (character * 0.15).linexp(0, 1.5, 0.05, 0.8);   // as before: 50 ms .. 800 ms
	knee       = character * 0.3;                                 // 0 .. 3 dB soft knee
	inputGain  = threshold.neg.dbamp;
	outputGain = margin.dbamp;

	signal = in * inputGain;

	// detector on the undelayed signal. preventISP: 4x oversampled true-peak estimate
	// (32-tap Hann-windowed sinc, phases 1/4 1/2 3/4), latency 16 samples << lookahead.
	taps = [signal];
	31.do { taps = taps.add(Delay1.ar(taps.last)) };
	mid = [0.25, 0.5, 0.75].collect { |fr|
		var h = (0..31).collect { |k| var t = (15 - k) + fr;
			if(t == 0, 1, { sin(pi * t) / (pi * t) }) * cos(pi * t / 33).squared };
		h = h / h.sum;
		(taps * h).sum.abs
	};
	det = Select.ar(preventISP, [signal.abs, max(taps[15].abs, mid.reduce(\max))]);

	// needed reduction (dB) to keep peaks at 0 dBFS (before margin), soft knee, ratio = inf
	over = det.max(-120.dbamp).ampdb;
	xk   = (over + (knee * 0.5)).clip(0, knee.max(0.01));
	gr   = (xk.squared / (2 * knee.max(0.01))) + (over - (knee * 0.5)).max(0);

	// hold-and-release, then a smoother that settles (60 dB) within the lookahead window
	gr = LagUD.ar(gr, 0, release * 6.9078);
	gr = Lag.ar(gr, lookAhead * 0.8);

	signal = DelayN.ar(signal, lookAhead, lookAhead) * gr.neg.dbamp;
	signal = Limiter.ar(signal, 1, 0.001) * outputGain;

	Out.ar(OceanodeOutput.kr(\out), signal);
}, description: "Dynamic processor inspired by the Ozone 3 Maximizer", category: "Effect/Dynamics");
)
