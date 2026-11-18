(
// Struck wood / glass bar: the sound of SteamPipeBox's GlassWoodTemplate
// snapshot as an instrument of its own (WoodPipe, ugens/OceanodeWood).
// A burst of noise rings a short inverted delay loop with an allpass inside
// it, which gives a bar's uneven partials (about 1 : 2.8 : 4.1 : 5.9), an
// uneven clip gives the knock. The defaults are the snapshot without its
// vibrato, played in tune: the delays are solved for each note (the snapshot
// sounds about 13 semitones above the key played).
// A gate is a level: it sets how loud and how bright the strike is. Each
// channel rings two notes: strikes take two pipes in turn, so the last note
// rings on at its own pitch.
~synthCreator.value("MarimbaPro", {|n|
	var pitch, gate, levels, hard, strike, decay, cutp, diff, ratio, push, sym, drive, vib, vibf;
	var velamp, velbright, keybright, keycut, keydecay, sig;

	pitch = OceanodeParameter.ar(\pitch, 60, n, 24, 108, "vf").asArray;
	gate = OceanodeParameter.ar(\gate, 0, n, 0, 1, "vf").asArray;
	levels = OceanodeParameterLag.ar(\levels, 1, n, 0, 1, "vf", 1/30, true).asArray;

	// the strike
	hard = OceanodeParameter.kr(\hardness, 0.76, n, 0, 1, "vf").asArray;        // brightness of the burst, at middle C
	strike = OceanodeParameter.kr(\strike, 4, n, 0.2, 100, "vf").asArray;       // its length, ms
	push = OceanodeParameter.kr(\push, 0.646, n, 0, 2, "vf").asArray;           // how much the bar's own motion shapes it
	drive = OceanodeParameter.kr(\drive, 1, n, 0.1, 4, "vf").asArray;           // how hard it hits the clip
	sym = OceanodeParameter.kr(\symmetry, 0.669, n, 0, 1, "vf").asArray;        // 0 even clip .. 1 one-sided

	// the bar
	decay = OceanodeParameter.kr(\decay, 2.7, n, 0.05, 20, "vf").asArray;         // ring time at middle C, s
	cutp = OceanodeParameter.kr(\cutp, 101, n, 40, 135, "vf").asArray;          // damping of the highs, at middle C
	diff = OceanodeParameter.kr(\diffusion, 0.417, n, -0.95, 0.95, "vf").asArray;   // spreads the partials: 0 is a plain tube
	ratio = OceanodeParameter.kr(\ratio, 3.23, n, 0.25, 12, "vf").asArray;      // allpass length / delay length: where the partials sit
	vib = OceanodeParameter.kr(\vibrato, 0, n, 0, 2, "vf").asArray;             // wobble of the delay, semitones (the snapshot: 0.21 at 10.3 Hz)
	vibf = OceanodeParameter.kr(\vibfreq, 10.3, n, 0, 20, "vf").asArray;

	// how the gate's level and the note change the strike and the bar
	velamp = OceanodeParameter.kr(\velamp, 1, n, 0, 1, "vf").asArray;             // level > loudness (0: every strike as loud)
	velbright = OceanodeParameter.kr(\velbright, 0.354, n, 0, 1, "vf").asArray;   // level > brightness of the burst
	keybright = OceanodeParameter.kr(\keybright, 0.315, n, -2, 2, "vf").asArray;  // note > brightness of the burst (1: follows the note)
	keycut = OceanodeParameter.kr(\keycut, 0.803, n, -2, 2, "vf").asArray;        // note > damping (1: cutp follows the note)
	keydecay = OceanodeParameter.kr(\keydecay, 0.819, n, -2, 2, "vf").asArray;    // note > ring time (1: half an octave up, 0: the same everywhere)

	sig = n.collect({ |c|
		WoodPipe.ar(gate[c], pitch[c], hard[c], strike[c], decay[c], cutp[c], diff[c], ratio[c],
			push[c], sym[c], drive[c], vib[c], vibf[c],
			velamp[c], velbright[c], keybright[c], keycut[c], keydecay[c]) * levels[c]
	});

	Out.ar(OceanodeOutput.kr(\out), sig);

}, description: "Struck wood and glass bars after the SteamPipe GlassWoodTemplate sound", category: "Source/PhysicalModelling");
)
