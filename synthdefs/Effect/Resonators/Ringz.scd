(
~synthCreator.value("Ringz", {|n|
	var inputGain, input, pitch, decay, signal;

	inputGain = OceanodeParameterLag.kr(\inputgain, 0.01, n, 0, 0.1, "vf", 1/30, true);
	input = In.ar(OceanodeInput.kr(\in), n) * inputGain;
	pitch = OceanodeParameterLag.kr(\pitch, 48, n, 0, 128, "vf", 1/30, true).midicps;
	decay = OceanodeParameterLag.kr(\decay, 1, n, 0, 10, "vf", 1/30, true);
	signal = Ringz.ar(input, pitch, decay);

	Out.ar(OceanodeOutput.kr(\out), signal);
}, description: "Legacy per-channel Ringz resonator", category: "Effect/Resonators");
)
