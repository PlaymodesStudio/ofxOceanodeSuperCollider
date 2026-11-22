(
// Flute's sound (flute.scd, the Csound slide flute) over the whole keyboard,
// with an overblow you can play. The loop runs sample by sample in FluteVoice
// (ugens/OceanodeFlute): Flute closes it through LocalIn, a block late, so it
// cannot rise above about F5, its register drifts with the note, and with high
// feedback it overflows and stays silent until rebuilt.
// With the defaults (gate held at 1) it plays like Flute, in tune.
//   overblow  0 the note, 1 its octave, 2 the twelfth, 3 two octaves; in between
//             the jet hesitates between the two, as a real one does
//   keytrack  the loop's low-pass is at least this many times the note, so high
//             notes still speak (with Lpf 2000 it starts acting above about F5)
//   gate      a level: on = blowing, its level = loudness. Held at 1 it drones,
//             as Flute.
//   envmode   where the ADSR acts:
//             Exciter   it is the breath: the tone speaks, swells and dies as
//                       blown (a low sustain or a slow attack can fail to speak)
//             PostGain  it is the output's loudness; the breath is always full
//             Both      the output's loudness, and the breath rises with it four
//                       times as fast (full by a quarter of the attack, falling in
//                       the last quarter of the release): speaks at any sustain
~synthCreator.value("FlutePro", {|n|
	var pch, ibreath, ifeedbk1, ifeedbk2, vibamp, vibfreq, lpf, noise, levels;
	var gate, attack, decay, sustain, release, envmode, overblow, keytrack, jet, air, chiff, lowcut, highcut;
	var on, vel, env, breath, gain, aflow1, kvibr, asum1, burst, sig, airSig;

	// Flute's controls, same defaults
	pch = OceanodeParameterLag.ar(\pitch, 32, n, 0, 128, "vf", 1/30, true);
	ibreath = OceanodeParameterLag.kr(\ibreath, 0.09, n, 0, 1, "vf", 1/30, true);
	ifeedbk1 = OceanodeParameterLag.kr(\ifeedbk1, 0.4, n, 0, 1, "vf", 1/30, true);
	ifeedbk2 = OceanodeParameterLag.kr(\ifeedbk2, 0.4, n, 0, 1, "vf", 1/30, true);
	vibamp = OceanodeParameterLag.kr(\vibamp, 0.2, n, 0, 1, "vf", 1/30, true);
	vibfreq = OceanodeParameterLag.kr(\vibfreq, 5, n, 0, 10, "vf", 1/30, true);
	lpf = OceanodeParameterLag.kr(\lpf, 2000, n, 0, 10000, "vf", 1/30, true);
	noise = OceanodeParameterLag.kr(\noise, 1, n, 0, 1, "vf", 1/30, true);        // the blowing pressure
	levels = OceanodeParameterLag.ar(\levels, 0, n, 0, 1, "vf", 1/30, true);

	// playing
	gate = OceanodeParameter.ar(\gate, 1, n, 0, 1, "vf");
	attack = OceanodeParameter.kr(\attack, 0.02, n, 0.001, 5, "vf");
	decay = OceanodeParameter.kr(\decay, 0.2, n, 0.001, 5, "vf");
	sustain = OceanodeParameter.kr(\sustain, 1, n, 0, 1, "vf");
	release = OceanodeParameter.kr(\release, 0.2, n, 0.001, 10, "vf");
	envmode = OceanodeParameterDropdown.kr(\envmode, 2, n, "Exciter:PostGain:Both");
	overblow = OceanodeParameterLag.kr(\overblow, 0, n, 0, 3, "vf", 1/30, true);
	keytrack = OceanodeParameterLag.kr(\keytrack, 3, n, 0, 10, "vf", 1/30, true);
	jet = OceanodeParameterLag.kr(\jet, 0.5, n, 0.2, 1, "vf", 1/30, true);        // jet delay / period: shorter is brighter, then overblows
	air = OceanodeParameterLag.kr(\air, 0, n, 0, 1, "vf", 1/30, true);            // breath noise heard beside the tone
	chiff = OceanodeParameterLag.kr(\chiff, 0, n, 0, 1, "vf", 1/30, true);        // a puff of noise as each note starts
	lowcut = OceanodeParameterLag.kr(\lowcut, 10, n, 10, 2000, "vf", 1/30, true);
	highcut = OceanodeParameterLag.kr(\highcut, 20000, n, 200, 20000, "vf", 1/30, true);

	// the gate: on while above 0, its level is the loudness (the highest since the
	// note started: an Oceanode value ramps over a block, up and down)
	on = gate > 0;
	vel = RunningMax.ar(gate, on);
	env = EnvGen.ar(Env.adsr(attack, decay, sustain, release), on);
	breath = Select.ar(envmode, [env, DC.ar(1), (EnvGen.ar(Env.adsr(attack, decay, 1, release), on) * 4).min(1)]);
	gain = Select.ar(envmode, [DC.ar(1), env, env]);

	// Flute's breath, blown through the envelope
	aflow1 = LFClipNoise.ar(SampleRate.ir!n, noise);
	kvibr = SinOsc.ar(vibfreq, 0, 0.1 * vibamp);
	burst = WhiteNoise.ar(chiff) * EnvGen.ar(Env.perc(0.002, 0.06), on);
	asum1 = ((ibreath * aflow1) + noise + kvibr + burst) * breath;

	sig = FluteVoice.ar(asum1, pch, ifeedbk1, ifeedbk2, lpf, jet, -1, overblow, keytrack, 2);
	sig = LeakDC.ar(sig);
	airSig = BPF.ar(WhiteNoise.ar, (pch.midicps * 3).clip(800, 9000), 1.5) * (air * 0.6 * breath * noise);
	sig = sig + airSig;
	sig = LPF.ar(HPF.ar(sig, lowcut), highcut.min(SampleRate.ir * 0.45));
	sig = Limiter.ar(sig);

	Out.ar(OceanodeOutput.kr(\out), sig * gain * vel * levels);

}, description: "Flute's sound over the whole keyboard, with gate, ADSR and a playable overblow", category: "Source/PhysicalModelling");
)
