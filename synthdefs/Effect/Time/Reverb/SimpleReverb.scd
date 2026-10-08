(
~synthCreator.value("SimpleReverb", {|n|
	var decay, lpf, hpf, mix, sig, sigIn;

	decay = OceanodeParameterLag.kr(\decay, 4, n, 0, 10, "vf", 1/30, true);
	lpf = OceanodeParameterLag.kr(\lpf, 1500, n, 0, 20000, "vf", 1/30, true);
	hpf = OceanodeParameterLag.kr(\hpf, 100, n, 0, 20000, "vf", 1/30, true);
	mix = OceanodeParameterLag.kr(\mix, 1, n, 0, 1, "vf", 1/30, true);

	sigIn = In.ar(OceanodeInput.kr(\in), n);
	sig = DelayN.ar(sigIn, 0.03, 0.03);
	// 32 combs summed per channel (an unsummed 32-voice comb bank wrote 32 output channels for any n)
	sig = n.collect { |i| Mix(CombN.ar(sig[i], 0.1, Array.fill(32, { Rand(0.01, 0.099) }), decay[i])) * 32.sqrt.reciprocal }; // equal-power sum of the 32 combs
	sig = LPF.ar(sig, lpf);
	sig = HPF.ar(sig, hpf);
	5.do {
		sig = AllpassN.ar(sig, 0.1, Array.fill(n, { Rand(0.01, 0.099) }), 3)
	};
	sig = LPF.ar(sig, lpf);
	sig = LeakDC.ar(sig);
	sig = (sig * mix) + (sigIn * (1 - mix));

	Out.ar(OceanodeOutput.kr(\out), sig);
}, description: "Legacy simple comb/allpass multichannel reverb", category: "Effect/Time/Reverb");
)
