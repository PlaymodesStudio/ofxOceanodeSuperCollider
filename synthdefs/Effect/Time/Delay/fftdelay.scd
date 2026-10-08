(
~synthCreator.value("FFTDelay", {|n, variables|
	var inAmp, delayTime, feed, threshAbove, threshBelow, smooth, smear;
	var stretch, freqHP, freqLP, qHP, qLP, noise;
	var signalIn, signal, ampAdj, mix, send;
	var i_fft = 11;  // FFT size parameter

	// Parameters
	delayTime = OceanodeParameter.ar(\delaytime, 0.1, n, 0, 1, "vf");
	feed = OceanodeParameter.ar(\feed, 0.4, n, 0, 0.99, "vf");
	threshAbove = OceanodeParameter.ar(\threshabove, 0, n, 0, 3, "vf");
	threshBelow = OceanodeParameter.ar(\threshbelow, 3, n, 0, 3, "vf");
	smooth = OceanodeParameter.ar(\smooth, 0.5, n, 0, 1, "vf");
	smear = OceanodeParameter.ar(\smear, 0, n, 0, 1, "vf");
	stretch = OceanodeParameter.ar(\stretch, 0, n, -1, 1, "vf");
	freqHP = OceanodeParameter.ar(\hp, 0, n, 0, 130, "vf").midicps;
	freqLP = OceanodeParameter.ar(\lp, 130, n, 0, 130, "vf").midicps;
	qHP = OceanodeParameter.ar(\qhp, 0, n, 0, 0.99, "vf");
	qLP = OceanodeParameter.ar(\qlp, 0, n, 0, 0.99, "vf");
	mix = OceanodeParameter.ar(\mix, 0.5, n, 0, 1, "vf");
	send = OceanodeParameterLag.ar(\send, 1, n, 0, 1, "vf", 1/30, true);

	// Fixed parameters
	inAmp = 0.5;
	noise = 0.5 ** -2;

	// Input processing (n channels; it was hardcoded to stereo while the parameters had n channels.
	// The fixed swap/ping-pong/pan options were all 0, i.e. no-ops, and have been removed)
	signalIn = In.ar(OceanodeInput.kr(\in), n);

	// Threshold calculations
	threshAbove = threshAbove ** 2;
	threshBelow = threshBelow ** 2;
	ampAdj = 1 / ((threshBelow**0.8).clip(0.005, 1.5));

	// One feedback loop with FFT processing per channel
	signal = n.collect { |i|
		FbL({|feedback|
			var chain, out;
			chain = FFT(LocalBuf(2**i_fft), (signalIn[i] * send[i] * inAmp) + (feedback.tanh * feed[i]));
			chain = PV_MagBelow(chain, threshBelow[i]);
			chain = PV_MagAbove(chain, threshAbove[i]);
			chain = PV_MagSmooth(chain, smooth[i]);
			chain = PV_MagSmear(chain, smear[i]);
			chain = PV_MagShift(chain, 2**stretch[i]);
			out = IFFT(chain) * 2;

			// Apply noise and filtering
			out = out * ampAdj[i] * noise;
			out = DFM1.ar(out, freqHP[i], qHP[i], 0.445, 1, 0);
			out = DFM1.ar(out, freqLP[i], qLP[i], 0.445, 0, 0);
			out / ampAdj[i] / noise
		}, 1, delayTime[i].lag, 1)
	};

	// Mix dry and wet signals
	signal = XFade2.ar(signalIn, signal * ampAdj, mix*2-1);

	// Output
	Out.ar(OceanodeOutput.kr(\out), signal);
}, description: "FFT-based delay with spectral processing", category: "Effect/Time/Delay");
)