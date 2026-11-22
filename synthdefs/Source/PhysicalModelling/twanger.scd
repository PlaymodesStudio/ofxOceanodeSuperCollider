(
// t'wanGer: the Reaktor ensemble twangerv4.ens, voice section ("t'Wanger" macro).
// One channel is one note, played by TwangerVoice (ugens/OceanodeTwanger): an
// impulse train through a choice of filters, a saturating resonator and a
// plucked string. Two voices per note, spread by Spread, as the ensemble's
// 16 voices / 2 unison. The voices are mixed and go through the ensemble's
// equalizer; the output is mono, for TwangerFX (reverb, master, limiter).
// Controls carry the Reaktor panel's names and units: dB, semitones, Reaktor
// log times (0 = 1 ms, 20 = 10 ms, 40 = 100 ms, 60 = 1 s). A gate is a level:
// it is the note's velocity.
~synthCreator.value("Twanger", {|n|
	var gate, pitch, att, rel, cut, res, vel, spread, filter, unison;
	var r1damp, r1undamp, r1cut, r1res, r1amt, r1dec, r1rel;
	var r2hicut, r2tune, r2damp, r2on, timbre, scale, trans, dton;
	var sub, bass, mid, freq, treb, sup, sig;

	gate = OceanodeParameter.ar(\gate, 0, n, 0, 1, "vf").asArray;
	pitch = OceanodeParameter.ar(\pitch, 60, n, 0, 127, "vf").asArray;

	// exciter and filter
	att = OceanodeParameter.kr(\att, 11.3, 1, 0, 30, "f");          // atT: impulse attack
	rel = OceanodeParameter.kr(\rel, 40.5, 1, 30, 65, "f");         // reL: impulse release
	cut = OceanodeParameter.kr(\cut, 103, 1, 0, 160, "f");          // cuT: filter cutoff (semitones)
	res = OceanodeParameter.kr(\res, 0.823, 1, 0, 0.995, "f");      // reS: filter resonance
	vel = OceanodeParameter.kr(\vel, 55, 1, 24, 100, "f");          // veL: velocity -> cutoff
	spread = OceanodeParameter.kr(\spread, 4.13, 1, 0, 7, "f");     // spreAd: unison spread (semitones)
	filter = OceanodeParameterDropdown.kr(\filter, 2, 1, "hp4:hp1:bh4:bp4:bl4:lp1:lp4:off");
	unison = OceanodeParameter.kr(\unison, 2, 1, 1, 4, "i");

	// reSonator 1
	r1damp = OceanodeParameter.kr(\r1damp, 4.78, 1, 0.02, 6, "f");
	r1undamp = OceanodeParameter.kr(\r1undamp, 10.8, 1, 0, 12, "f");
	r1cut = OceanodeParameter.kr(\r1cut, -13.2, 1, -60, 0, "f");
	r1res = OceanodeParameter.kr(\r1res, 0.208, 1, 0, 0.6, "f");
	r1amt = OceanodeParameter.kr(\r1amt, 8600, 1, 0, 12700, "f");
	r1dec = OceanodeParameter.kr(\r1dec, 25.1, 1, 0, 65, "f");
	r1rel = OceanodeParameter.kr(\r1rel, 45.0, 1, 0, 65, "f");

	// reSonator 2 (the string)
	r2hicut = OceanodeParameter.kr(\r2hicut, 68.9, 1, 24, 100, "f");
	r2tune = OceanodeParameter.kr(\r2tune, -8.79, 1, -36, 36, "f");
	r2damp = OceanodeParameter.kr(\r2damp, -0.459, 1, -2, -0.1, "f");
	r2on = OceanodeParameter.kr(\r2on, 1, 1, 0, 1, "b");

	// (de)Tuner
	timbre = OceanodeParameter.kr(\timbre, 0.47, 1, -60, 60, "f");
	scale = OceanodeParameter.kr(\scale, 0.47, 1, 0.1, 2, "f");
	trans = OceanodeParameter.kr(\trans, 35.4, 1, -60, 60, "f");
	dton = OceanodeParameter.kr(\dton, 0, 1, 0, 1, "b");

	// eQualizer, dB
	sub = OceanodeParameter.kr(\sub, 7.4, 1, -20, 20, "f");         // suB: low shelf 500 Hz
	bass = OceanodeParameter.kr(\bass, -4.57, 1, -20, 20, "f");     // baSs: low shelf 2 kHz
	mid = OceanodeParameter.kr(\mid, 6.17, 1, -16, 16, "f");        // miD: peak at frEq
	freq = OceanodeParameter.kr(\freq, 1702, 1, 200, 2000, "f");    // frEq
	treb = OceanodeParameter.kr(\treb, -1.2, 1, -8, 8, "f");        // treB: high shelf 2 kHz
	sup = OceanodeParameter.kr(\sup, -1.2, 1, -8, 8, "f");          // suP: high shelf 5 kHz

	sig = n.collect({ |c|
		TwangerVoice.ar(gate[c], pitch[c], att, rel, cut, res, vel, spread, filter,
			r1damp, r1undamp, r1cut, r1res, r1amt, r1dec, r1rel,
			r2hicut, r2tune, r2damp, r2on, timbre, scale, trans, dton, unison)
	}).sum;

	// Low Shelf 500, Low Shelf 2000, Peak (Res 0.5586), High Shelf 2000, High Shelf 5000
	sig = BLowShelf.ar(sig, 500, 1, sub);
	sig = BLowShelf.ar(sig, 2000, 1, bass);
	sig = BPeakEQ.ar(sig, freq, 2 * (1 - 0.5586), mid);
	sig = BHiShelf.ar(sig, 2000, 1, treb);
	sig = BHiShelf.ar(sig, 5000, 1, sup);

	Out.ar(OceanodeOutput.kr(\out), sig);

}, description: "Reaktor t'wanGer voices: impulse, filters, resonator and plucked string, with its equalizer (mono out)", category: "Source/PhysicalModelling");
)
