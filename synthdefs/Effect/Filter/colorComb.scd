(
~synthCreator.value("ColorComb", {|n|
	var input, pitch, transpose, detune, spread, feedback, color, res, send, mix, wet;

	/*
	One voice of NI's Reaktor "Resonator" macro per channel (ReaktorColorComb,
	OceanodeAnalog): y = in + feedback * delay(LP(y)), one period of the pitch
	in the loop, LP its "Color" 2-pole low-pass (res 0 = Q 0.5, as the macro
	leaves it). Out: y, the input plus the ringing, crossfaded with the input
	by mix. Pitch per channel; transpose is added to every channel.
	*/

	pitch = OceanodeParameterLag.ar(\pitch, 48, n, 0, 130, "vf", 1/30, true);
	transpose = OceanodeParameterLag.ar(\transpose, 0, 1, -48, 48, "vf", 1/30, true);
	detune = OceanodeParameterLag.ar(\detune, 0, 1, 0, 1, "vf", 1/30, true);
	spread = OceanodeParameter.ar(\spread, 0, n, -1, 1, "vf");
	feedback = OceanodeParameterLag.ar(\feedback, 0.97, n, -1, 1, "vf", 1/30, true);
	color = OceanodeParameterLag.ar(\color, 111, n, 0, 135, "vf", 1/30, true);
	res = OceanodeParameterLag.ar(\res, 0, n, 0, 1, "vf", 1/30, true);
	send = OceanodeParameterLag.ar(\send, 1, n, 0, 2, "vf", 1/30, true);
	mix = OceanodeParameterLag.ar(\mix, 1, n, 0, 1, "vf", 1/30, true);

	input = In.ar(OceanodeInput.kr(\in), n);
	wet = ReaktorColorComb.ar(input * send, (pitch + transpose + (detune * spread)).midicps, feedback, color.midicps, res);
	Out.ar(OceanodeOutput.kr(\out), input + ((wet - input) * mix));
}, description: "Feedback comb with a low-pass in the loop (NI Reaktor Resonator voice)", category: "Effect/Filter");
)
