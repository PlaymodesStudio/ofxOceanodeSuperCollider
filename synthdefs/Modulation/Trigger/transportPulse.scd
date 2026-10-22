(
~synthCreator.value("TransportPulse", {|n|
	var beat, run, bpm, division, barbeats, period, anchor, hard, pos, index, top, pulse, onGrid;
	// Audio controls read the new anchor on its first sample. Beat Transport
	// updates the anchor from the host; Sweep fills the samples between frames.
	beat = \beattransport.ar(0, spec: ControlSpec(0, 1e12, default: 0, units: "vf"));
	run = \playing.ar(0, spec: ControlSpec(0, 1, default: 0, units: "vf")).clip(0, 1);
	bpm = \bpm.ar(120, spec: ControlSpec(0, 999, default: 120, units: "vf"));
	division = \division.ar(3, spec: ControlSpec(default: 3, units:
		"d:1/64:1/32:1/16 T:1/16:1/8 T:1/8:1/4 T:1/8 dotted:1/4:1/2 T:1/4 dotted:1/2:1 bar:2 bars:4 bars"));
	barbeats = \barbeats.ar(4, spec: ControlSpec(1, 16, default: 4, units: "vf"));
	period = Select.ar(division.clip(0, 14),
		[1/16, 1/8, 1/6, 1/4, 1/3, 1/2, 2/3, 3/4, 1, 4/3, 3/2, 2, barbeats, barbeats*2, barbeats*4]
		.collect { |value| if(value.isNumber, { DC.ar(value) }, { value }) });
	anchor = T2A.ar((Changed.kr(A2K.kr(beat)) + Changed.kr(A2K.kr(run))) > 0);
	pos = beat + Sweep.ar(anchor, bpm / 60 * run);
	// A restart, backwards seek/loop, large forward seek or new division
	// discards the old high-water mark. Small anchor corrections never double-fire.
	hard = (Impulse.ar(0) + (HPZ1.ar(run) > 0) + (HPZ1.ar(beat) < -0.001)
		+ (HPZ1.ar(beat) > (bpm / 60 * 0.25).max(0.5))
		+ (HPZ1.ar(division).abs > 0) + (HPZ1.ar(barbeats).abs > 0)) > 0;
	index = (pos / period).floor;
	top = RunningMax.ar(index, hard);
	onGrid = (pos - (index * period)).abs < (bpm / 60 * SampleDur.ir * 1.5).max(1e-7);
	pulse = (((index > Delay1.ar(top)) * (1 - hard)) + (hard * onGrid)) > 0;
	Out.ar(OceanodeOutput.kr(\out), (pulse * (run > 0) * (bpm > 0)) ! n);
}, description: "Musical pulses locked to host Beat Transport; interpolates between position anchors", category: "Modulation/Trigger");
)
