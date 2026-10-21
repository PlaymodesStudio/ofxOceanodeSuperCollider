// PhysicalBox, Phase 0: reference instruments to compare against.
//
// Boot the server, evaluate the SynthDef block, then try the lines below one
// at a time next to the same idea in physicalbox.scd. MiElements is Mutable
// Instruments Elements (bow / blow / strike into modal or string), the closest
// existing instrument to PhysicalBox. The DWG models are the ones our
// Source/PhysicalModelling synths use.
s.boot;

(
SynthDef(\refElements, {|out = 0, gate = 1, note = 48, model = 0, bow = 0, blow = 0, strike = 0.8,
	geom = 0.25, bright = 0.5, damp = 0.7, pos = 0.2, space = 0.3, amp = 0.5|
	var sig = MiElements.ar(gate: gate, pit: note, bow_level: bow, blow_level: blow,
		strike_level: strike, geom: geom, bright: bright, damp: damp, pos: pos,
		space: space, model: model);
	Out.ar(out, sig * amp);
}).add;

SynthDef(\refBowed, {|out = 0, gate = 1, note = 57, force = 0.13, pos = 0.12, amp = 0.5|
	var sig = DWGBowed.ar(note.midicps, 1, force, gate, pos, 0.1, 1, 3000, 0.55, 80);
	sig = DWGSoundBoard.ar(sig);
	Out.ar(out, LeakDC.ar(sig) * amp ! 2);
}).add;

SynthDef(\refFlute, {|out = 0, gate = 1, note = 69, amp = 0.5|
	var env = EnvGen.kr(Env.asr(0.05, 1, 0.2), gate) * (1.2 + (0.001 * 120 * WhiteNoise.ar));
	var sig = DWGFlute.ar(note.midicps, env, 0.45, 0.24, 0.33, 1, 1000);
	Out.ar(out, HPF.ar(sig, 200) * amp ! 2);
}).add;

SynthDef(\refClarinet, {|out = 0, gate = 1, note = 57, amp = 0.5|
	var env = EnvGen.kr(Env.asr(0.05, 1, 0.2), gate) * 0.9;
	var sig = DWGClarinet3.ar(note.midicps, env, 1, 1.2, 1, 1000, 0.2, 7);
	Out.ar(out, HPF.ar(sig, 200) * amp ! 2);
}).add;
)

// Elements: model 0 modal, 1 string, 2 strings (sympathetic)
x = Synth(\refElements, [\strike, 0.8, \geom, 0.25]);  // struck modal body
x.set(\geom, 0.7);                                      // towards bell / metal
x.set(\strike, 0, \bow, 0.9);                           // bowed
x.set(\bow, 0, \blow, 0.9);                             // blown
x.set(\model, 1);                                       // string
x.free;

y = Synth(\refBowed); y.set(\force, 0.4); y.free;
y = Synth(\refFlute); y.free;
y = Synth(\refClarinet); y.free;
