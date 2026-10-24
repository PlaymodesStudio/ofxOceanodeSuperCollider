// PhysicalBox, Phase 0: playable prototype.
//
// Open this file in the SuperCollider IDE and evaluate the block below
// (Cmd-Return inside it). It boots the server, loads the DynGen voice
// (physicalbox.eel2 via physicalbox_core.scd) and opens the window.
//
// Play: computer keyboard (A W S E D F T G Y H U J K = C..C, Z / X octave),
// any MIDI keyboard (velocity = gate level, CC1 = material, CC2 = breath,
// CC11 = bow pressure), or the Drone button (holds the Pitch note).
// Presets load whole patches; Morph slides every control between preset
// A and preset B. The Material pad moves material (x) and excite + listen
// (y: waveguide at the bottom, modal body at the top).
//
// Needs DynGen (EEL2) and the synthdefs/Oceanode classes in the class library.
(
var dir = PathName(thisProcess.nowExecutingPath).pathOnly;
var guiOnly = "PB_GUI_TEST".getenv.notNil;
var specs, sections, presets, defaults, views = IdentityDictionary(), win;
var setParam, applyPatch, noteOn, noteOff, morph, keyNotes, octave = 60;
var voices = IdentityDictionary(), drone, grp, fxBus, space;
var presetA = 0, presetB = 1, mixA, padView;

(dir +/+ "physicalbox_core.scd").load;

// ---------------------------------------------------------------- controls
specs = (
	atk: [0.001, 4, \exp], rel: [0.01, 8, \exp],
	strike: [0, 1], hardness: [0, 1], mass: [0, 1], bounce: [0, 0.9],
	bow: [0, 1], bowpress: [0, 1.2], bowspeed: [0, 1], rosin: [0, 1],
	blow: [0, 1], breath: [0, 1.2], blowtype: [0, 1], jetratio: [0.1, 0.8], turb: [0, 1],
	scrape: [0, 1], scrapespeed: [0, 1], rough: [0, 1], grit: [0, 1], chatter: [0, 1],
	ext: [0, 1],
	pos: [0, 1], pick1: [0, 1], pick2: [0, 1],
	atune: [-24, 24], adecay: [0.02, 20, \exp], abright: [0, 1], abore: [0, 1], astiff: [0, 1],
	btune: [-24, 24], bdecay: [0.02, 20, \exp], bshape: [0, 5], bstretch: [-1, 1],
	bmodes: [1, 48, \lin, 1], bbright: [0, 1],
	material: [0, 1], excite: [0, 1], listen: [0, 1], couple: [0, 1], mutual: [0, 1],
	rattle: [0, 1], rattleth: [0.01, 1, \exp], tension: [0, 1], cascade: [0, 1],
	drift: [0, 1], driftrate: [0, 1], freeze: [0, 1]
).collect({|v| ControlSpec(v[0], v[1], v[2] ? \lin, v[3] ? 0) });

sections = [
	["Strike", [\strike, \hardness, \mass, \bounce]],
	["Bow", [\bow, \bowpress, \bowspeed, \rosin]],
	["Blow  (type: jet > reed)", [\blow, \breath, \blowtype, \jetratio, \turb]],
	["Scrape", [\scrape, \scrapespeed, \rough, \grit, \chatter]],
	["Contact", [\pos, \ext, \atk, \rel]],
	["A  waveguide  (bore: string > closed tube > open tube)", [\atune, \adecay, \abright, \abore, \astiff]],
	["B  modes  (shape: harm bar glass bell plate memb)", [\btune, \bdecay, \bshape, \bstretch, \bmodes, \bbright]],
	["Routing", [\excite, \listen, \couple, \mutual, \pick1, \pick2]],
	["Break", [\rattle, \rattleth, \tension, \cascade]],
	["Life", [\drift, \driftrate, \freeze]]
];

defaults = ();
~pbSpecs.do({|s| defaults[s[0]] = s[1] });
[\freq, \gate, \strk].do({|k| defaults.removeAt(k) });

// Patches only list what differs from the defaults.
presets = [
	"Struck string" -> (excite: 0, listen: 0, adecay: 3, hardness: 0.6),
	"Stiff string (piano-ish)" -> (excite: 0, listen: 0, astiff: 0.35, adecay: 5, hardness: 0.75, abright: 0.8, mass: 0.4),
	"Sitar" -> (excite: 0, listen: 0, adecay: 4, rattle: 0.6, rattleth: 0.05, hardness: 0.8, pos: 0.1),
	"Bowed string" -> (strike: 0, bow: 1, excite: 0, listen: 0, adecay: 2, pos: 0.12, atk: 0.15),
	"Scratchy bow" -> (strike: 0, bow: 1, excite: 0, listen: 0, adecay: 2, pos: 0.12, bowpress: 1.1, rosin: 0.8, atk: 0.1),
	"Flute" -> (strike: 0, blow: 1, blowtype: 0, abore: 0.5, abright: 0.25, adecay: 0.3, breath: 0.6, pos: 0.02, turb: 0.25, excite: 0, listen: 0),
	"Clarinet" -> (strike: 0, blow: 1, blowtype: 1, abore: 0.5, pos: 0.02, adecay: 0.5, excite: 0, listen: 0),
	"Wood bar" -> (excite: 1, listen: 1, bshape: 1, material: 0, bdecay: 2, hardness: 0.5),
	"Glass bar" -> (excite: 1, listen: 1, bshape: 1, material: 0.5, bdecay: 2, hardness: 0.7),
	"Bell" -> (excite: 1, listen: 1, bshape: 3, material: 1, bdecay: 3, hardness: 0.7),
	"Tam-tam bloom" -> (excite: 1, listen: 1, bshape: 4, material: 1, bdecay: 4, cascade: 0.8, hardness: 0.8, mass: 0.8),
	"Tom (membrane)" -> (excite: 1, listen: 1, bshape: 5, material: 0.4, tension: 0.8, bdecay: 1, hardness: 0.4),
	"Singing glass" -> (strike: 0, bow: 1, excite: 1, listen: 1, bshape: 2, material: 0.5, bdecay: 3, pos: 0.5, bowpress: 0.2, bowspeed: 0.7, atk: 0.3),
	"Bowed metal bar" -> (strike: 0, bow: 1, excite: 1, listen: 1, bshape: 1, material: 1),
	"Blown bell drone" -> (strike: 0, blow: 1, blowtype: 0, excite: 1, listen: 1, bshape: 3, material: 1, atk: 0.5),
	"Scraped plate" -> (strike: 0, scrape: 1, excite: 1, listen: 1, bshape: 4, material: 1, chatter: 0.5),
	"Harsh scrape" -> (strike: 0, scrape: 1, rough: 0.9, grit: 0.8, chatter: 1, cascade: 0.6, rattle: 0.4, rattleth: 0.1, material: 1, bshape: 4, excite: 1, listen: 1),
	"Sympathetic strings" -> (excite: 0, listen: 0.5, btune: 0.15, bshape: 0, couple: 0.4, mutual: 1, adecay: 4, bdecay: 4),
	"Glass drone" -> (strike: 0, bow: 0.6, blow: 0.4, excite: 0.6, listen: 0.6, bshape: 2, drift: 1, freeze: 0.5, atk: 1, rel: 3)
].collect({|a| a.key -> defaults.copy.putAll(a.value) });

~pb = presets[0].value.copy;

// ---------------------------------------------------------------- engine
setParam = {|name, value, updateView = true|
	~pb[name] = value;
	grp !? { grp.set(name, value) };
	if(updateView) { views[name] !? {|v| { v.value = value }.defer } };
	if([\material, \excite].includes(name)) { { padView !? { padView.x = ~pb[\material]; padView.y = ~pb[\excite] } }.defer };
};

applyPatch = {|patch| patch.keysValuesDo({|k, v| setParam.(k, v) }) };

morph = {|x|
	var a = presets[presetA].value, b = presets[presetB].value;
	a.keysValuesDo({|k, va|
		var v = va.blend(b[k], x);
		if(k == \bmodes) { v = v.round };
		if(k == \hold) { v = (x < 0.5).if(va, b[k]) };
		setParam.(k, v);
	});
};

noteOn = {|note, vel = 0.8|
	noteOff.(note);
	if(grp.notNil) {
		voices[note] = Synth(\pbVoice, ~pb.asPairs ++ [\freq, note.midicps, \gate, vel, \out, fxBus], grp);
	};
};

noteOff = {|note|
	voices.removeAt(note) !? {|v| v.set(\gate, 0) };
};

// ---------------------------------------------------------------- window
win = Window("PhysicalBox  Phase 0", Rect(40, 40, 1260, 800)).front;
win.view.decorator = FlowLayout(win.view.bounds, 8 @ 8, 6 @ 4);
win.onClose = {
	voices.do(_.set(\gate, 0)); voices.clear;
	drone !? { drone.set(\gate, 0) };
	if(guiOnly.not) { { grp.free; space.free; fxBus.free }.defer(9) };
};

{
	var col = CompositeView(win, 300 @ 780), d;
	col.decorator = d = FlowLayout(col.bounds, 0 @ 0, 4 @ 4);
	StaticText(col, 296 @ 18).string_("Material: wood  -  glass  -  metal").font_(Font.sansSerif(12, true));
	StaticText(col, 296 @ 16).string_("up: modal body   down: waveguide (excite + listen)").font_(Font.sansSerif(10));
	padView = Slider2D(col, 296 @ 296).x_(~pb[\material]).y_(~pb[\excite])
		.action_({|v| setParam.(\material, v.x); setParam.(\excite, v.y); setParam.(\listen, v.y) });

	StaticText(col, 296 @ 18).string_("Preset").font_(Font.sansSerif(12, true));
	PopUpMenu(col, 296 @ 22).items_(presets.collect(_.key))
		.action_({|m| applyPatch.(presets[m.value].value) });
	StaticText(col, 296 @ 18).string_("Morph A > B").font_(Font.sansSerif(12, true));
	PopUpMenu(col, 296 @ 22).items_(presets.collect(_.key)).value_(presetA).action_({|m| presetA = m.value });
	PopUpMenu(col, 296 @ 22).items_(presets.collect(_.key)).value_(presetB).action_({|m| presetB = m.value });
	mixA = Slider(col, 296 @ 22).action_({|v| morph.(v.value) });

	StaticText(col, 296 @ 18).string_("Play").font_(Font.sansSerif(12, true));
	views[\pitch] = EZSlider(col, 296 @ 20, "Pitch", ControlSpec(24, 96, \lin, 1), {|v|
		drone !? { drone.set(\freq, v.value.midicps) } }, 48, labelWidth: 60);
	Button(col, 96 @ 26).states_([["Drone"], ["Drone", Color.black, Color.green(0.8)]])
		.action_({|b|
			if(b.value == 1) {
				grp !? { drone = Synth(\pbVoice, ~pb.asPairs ++ [\freq, views[\pitch].value.midicps, \gate, 0.8, \out, fxBus], grp) };
			} {
				drone !? { drone.set(\gate, 0) }; drone = nil;
			};
		});
	Button(col, 96 @ 26).states_([["Strike"]]).action_({ grp !? { grp.set(\strk, 1) } });
	Button(col, 96 @ 26).states_([["Hold"], ["Hold", Color.black, Color.green(0.8)]])
		.value_(~pb[\hold]).action_({|b| setParam.(\hold, b.value, false) });
	views[\level] = EZSlider(col, 296 @ 20, "Level", \amp.asSpec, {|v| grp !? { grp.set(\amp, v.value) } }, 0.5, labelWidth: 60);
	views[\space] = EZSlider(col, 296 @ 20, "Space", ControlSpec(0, 1), {|v| space !? { space.set(\mix, v.value) } }, 0.25, labelWidth: 60);
	Button(col, 145 @ 24).states_([["Scope"]]).action_({ s.scope(2) });
	Button(col, 145 @ 24).states_([["Spectrum"]]).action_({ FreqScope.new });
	Button(col, 296 @ 24).states_([["Mic into Ext (input 1)"], ["Mic into Ext (input 1)", Color.black, Color.green(0.8)]])
		.action_({|b| grp !? { grp.set(\micon, b.value) } });
	StaticText(col, 296 @ 60).string_("Keys: A W S E D F T G Y H U J K\nZ / X octave down / up").font_(Font.sansSerif(10));
}.value;

sections.clump(4).do({|group|
	var col = CompositeView(win, 300 @ 780), d;
	col.decorator = d = FlowLayout(col.bounds, 0 @ 0, 4 @ 3);
	group.do({|sec|
		StaticText(col, 296 @ 30).string_(sec[0]).font_(Font.sansSerif(11, true)).align_(\bottomLeft);
		sec[1].do({|name|
			views[name] = EZSlider(col, 296 @ 19, name.asString, specs[name],
				{|v| setParam.(name, v.value, false) }, ~pb[name], labelWidth: 78, numberWidth: 46)
				.font_(Font.sansSerif(10));
		});
	});
});

keyNotes = "awsedftgyhujk";
win.view.keyDownAction = {|v, char|
	var i = keyNotes.indexOf(char);
	case
		{ char == $z } { octave = (octave - 12).max(24) }
		{ char == $x } { octave = (octave + 12).min(96) }
		{ i.notNil and: { voices[octave + i].isNil } } { noteOn.(octave + i, 0.8) };
	true
};
win.view.keyUpAction = {|v, char|
	var i = keyNotes.indexOf(char);
	if(i.notNil) { noteOff.(octave + i) };
	true
};

// ---------------------------------------------------------------- server
if(guiOnly) {
	"PhysicalBox GUI built (PB_GUI_TEST: no server)".postln;
} {
	s.waitForBoot({
		~pbDef.send(s);
		SynthDef(\pbVoice, {|out = 0, amp = 0.5|
			var mic = SoundIn.ar(0) * \micon.kr(0);
			var sig = LeakDC.ar(~pbVoice.(mic)) * amp;
			// free once released and silent; a held gate keeps it alive
			DetectSilence.ar(sig.abs.sum + \gate.ar(0), 0.0001, 1, doneAction: 2);
			Out.ar(out, sig);
		}).add;
		SynthDef(\pbSpace, {|in, out = 0, mix = 0.25|
			var sig = In.ar(in, 2);
			sig = OceanodeMicroSpace.ar(sig, 0.8 * (100/1.2), 0.7, -0.6, -6, 0.8, mix);
			Out.ar(out, Limiter.ar(sig, 0.95));
		}).add;
		s.sync;
		fxBus = Bus.audio(s, 2);
		space = Synth(\pbSpace, [\in, fxBus, \mix, views[\space].value]);
		grp = Group(s, \addToHead);

		MIDIClient.init(verbose: false);
		MIDIIn.connectAll(false);
		MIDIdef.noteOn(\pbOn, {|vel, note| { noteOn.(note, vel / 127) }.defer });
		MIDIdef.noteOff(\pbOff, {|vel, note| { noteOff.(note) }.defer });
		MIDIdef.cc(\pbCC, {|val, num|
			var name = IdentityDictionary[1 -> \material, 2 -> \breath, 11 -> \bowpress][num];
			name !? { { setParam.(name, specs[name].map(val / 127)) }.defer };
		});
		"PhysicalBox ready: play the keyboard with the window focused.".postln;
	});
};
)
