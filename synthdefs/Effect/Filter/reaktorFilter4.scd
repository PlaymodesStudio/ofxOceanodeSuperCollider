(
~synthCreator.value("ReaktorFilter4", {|n|
	var input, pitch, res, type, drywet;

	/*
	Reaktor's Multi/LP 4-Pole (ReaktorMulti4, OceanodeAnalog): a 4-pole
	ladder, res 0..1 (1: self-oscillation), its outputs mixed from the
	stages. The first six types are the module's outputs in order (HP2 BP2
	LP2 BP4 BL4 LP4). Type at audio rate (a connected signal) switches per
	sample.
	*/

	pitch = OceanodeParameterLag.ar(\pitch, 84, n, 0, 135, "vf", 1/30, true);
	res = OceanodeParameterLag.ar(\res, 0, n, 0, 1, "vf", 1/30, true);
	type = OceanodeParameterDropdown.ar(\type, 5, n, "HP2:BP2:LP2:BP4:BL4:LP4:HP4:Notch");
	drywet = OceanodeParameterLag.ar(\drywet, 1, n, 0, 1, "vf", 1/30, true);

	input = In.ar(OceanodeInput.kr(\in), n);
	Out.ar(OceanodeOutput.kr(\out), input + ((
		ReaktorMulti4.ar(input, pitch.midicps, res, Select.ar(type.round.clip(0, 7), DC.ar([6, 8, 4, 9, 10, 5, 7, 11])))
		- input) * drywet));
}, description: "Reaktor Multi LP 4-Pole filter: HP2 BP2 LP2 BP4 BL4 LP4 HP4 Notch", category: "Effect/Filter");
)
