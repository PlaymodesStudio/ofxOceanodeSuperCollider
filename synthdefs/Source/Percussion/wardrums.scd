(
// War drums: big skin drums played by a group, by synthesis.
// One channel is one drum section: up to Players drummers (the synth's
// variable; \players picks how many of them play) hit together, each a little
// late, tuned a little apart, and no two hits the same.
//
// A drummer's drum:
//   body   the skin's six lowest partials, each with a twin a little above it
//          (the second head: they beat); the pitch starts high and falls
//   skin   twelve higher partials struck by the stick, the higher the shorter
//   shell  the wooden body: six short fixed resonances
//   slap   a short breath of noise, the skin's smack
//   stick  the contact itself: a click and a small wooden "tok"
// then, per channel, the tone: growl in the mids, low shelf, a cut at 210 Hz,
// drive, compression, air.
//
// A gate is a level: it sets how loud and how bright the hit is, taken when
// the gate stops rising. No reverb inside: put a SpaceMaster after it
// (size 38, decay 5, predelay 30, lowpass 6500, highdamp 9, position 0.7).
//
// Four sections that make a kit (the defaults are the low drums):
//              pitch  players detune decay drop droptime twin  body skin shell slap stick shellfreq tokfreq stickhp push
//   sub        28.1   3       320    2.2   0.6  140      6     0.75 0.6  0.5   1    0.27  170       500     500     2.2
//   low        35.3   5       60     1.15  0.3  80       4.5   1    1    1     1    1     350       1050    900     1
//   toms       43..53 1       0      0.5   0.22 60       4.5   0.5  1.2  1.2   1    1.3   510       1650    1200    0.4    (one channel per tom)
//   clacks     60     3       80     0.09  0.05 20       4.5   0.2  0.8  2     1    3.5   850       2200    1500    0.4
~synthCreator.value("WarDrums", {|n, variables|
	var maxp = variables[0];
	var gate, pitch, levels, players, detune, spread, variation;
	var decay, damp, drop, droptime, stretch, twin;
	var body, skin, shell, slap, stick, shellfreq, shelldecay, slapdecay, tokfreq, stickhp, bright;
	var velamp, velbright, push, low, lowfreq, mud, roar, drive, comp, air;
	var ratios = [1, 1.593, 2.136, 2.296, 2.653, 2.918];
	var amps = [1, 0.55, 0.3, 0.3, 0.18, 0.15];
	var upper = [3.156, 3.5, 3.6, 4.06, 4.6, 5.14, 5.54, 6.2, 7.0, 8.4, 9.9, 11.6];
	var sig;

	gate = OceanodeParameter.ar(\gate, 0, n, 0, 1, "vf").asArray;
	pitch = OceanodeParameter.ar(\pitch, 35.3, n, 12, 84, "vf").asArray;
	levels = OceanodeParameterLag.ar(\levels, 0.5, n, 0, 1, "vf", 1/30, true).asArray;

	// the group
	players = OceanodeParameter.kr(\players, 6, n, 1, 6, "vi").asArray;               // how many of the drummers play (up to Players)
	detune = OceanodeParameter.kr(\detune, 60, n, 0, 1200, "vf").asArray;             // their drums' tunings, spread over +- cents
	spread = OceanodeParameter.kr(\spread, 40, n, 0, 100, "vf").asArray;              // how far apart they land, ms
	variation = OceanodeParameter.kr(\variation, 1, n, 0, 2, "vf").asArray;           // how much one hit differs from the next

	// the skin
	decay = OceanodeParameter.kr(\decay, 1.15, n, 0.03, 8, "vf").asArray;             // ring time of the lowest partial, s
	damp = OceanodeParameter.kr(\damp, 1, n, 0, 3, "vf").asArray;                     // how much sooner the higher partials die
	drop = OceanodeParameter.kr(\drop, 0.3, n, 0, 2, "vf").asArray;                   // pitch fall of a full hit (0.3: starts 30 % sharp)
	droptime = OceanodeParameter.kr(\droptime, 80, n, 5, 500, "vf").asArray;          // ms
	stretch = OceanodeParameter.kr(\stretch, 1, n, 0.5, 1.6, "vf").asArray;           // spacing of the partials (1: a round skin)
	twin = OceanodeParameter.kr(\twin, 4.5, n, 0, 12, "vf").asArray;                  // the second head, % above the first

	// the parts
	body = OceanodeParameter.kr(\body, 1, n, 0, 4, "vf").asArray;
	skin = OceanodeParameter.kr(\skin, 1, n, 0, 4, "vf").asArray;
	shell = OceanodeParameter.kr(\shell, 1, n, 0, 4, "vf").asArray;
	slap = OceanodeParameter.kr(\slap, 1, n, 0, 4, "vf").asArray;
	stick = OceanodeParameter.kr(\stick, 1, n, 0, 8, "vf").asArray;
	shellfreq = OceanodeParameter.kr(\shellfreq, 350, n, 60, 2000, "vf").asArray;     // Hz: the size of the wooden body
	shelldecay = OceanodeParameter.kr(\shelldecay, 1, n, 0.2, 8, "vf").asArray;       // 1: dry wood; more rings like a barrel, then like metal
	slapdecay = OceanodeParameter.kr(\slapdecay, 0.12, n, 0.01, 1, "vf").asArray;     // s
	tokfreq = OceanodeParameter.kr(\tokfreq, 1050, n, 200, 5000, "vf").asArray;       // Hz: the stick's own wood
	stickhp = OceanodeParameter.kr(\stickhp, 900, n, 100, 5000, "vf").asArray;        // Hz: lower is a heavier beater
	bright = OceanodeParameter.kr(\bright, 1, n, 0.2, 2, "vf").asArray;

	// what the gate's level does
	velamp = OceanodeParameter.kr(\velamp, 1, n, 0, 1, "vf").asArray;                 // level > loudness (0: every hit as loud)
	velbright = OceanodeParameter.kr(\velbright, 1, n, 0, 1, "vf").asArray;           // level > brightness and pitch fall

	// the tone
	push = OceanodeParameter.kr(\push, 1, n, 0, 4, "vf").asArray;                   // how hard the group plays into what follows
	low = OceanodeParameter.kr(\low, 8, n, -12, 18, "vf").asArray;                    // dB below lowfreq
	lowfreq = OceanodeParameter.kr(\lowfreq, 95, n, 30, 300, "vf").asArray;
	mud = OceanodeParameter.kr(\mud, -4, n, -18, 6, "vf").asArray;                    // dB at 210 Hz
	roar = OceanodeParameter.kr(\roar, 0.15, n, 0, 1, "vf").asArray;                  // distorted mids added
	drive = OceanodeParameter.kr(\drive, 1, n, 0.1, 8, "vf").asArray;
	comp = OceanodeParameter.kr(\comp, 1, n, 0, 2, "vf").asArray;                     // 0 none, 1 gentle, 2 hard
	air = OceanodeParameter.kr(\air, 5, n, -12, 12, "vf").asArray;                    // dB above 3.5 kHz

	sig = n.collect({ |c|
		var g = gate[c];
		var up = g > Delay1.ar(g);
		// the sample where the gate stops rising: its level is the hit's
		var trig = Delay1.ar(up) * (1 - up) * (g > 0.001);
		var level = A2K.kr(Latch.ar(g, trig));
		var hit = DelayN.ar(trig, 0.01, ControlDur.ir);   // a block later: the level is there
		var hitk = T2K.kr(trig);
		var np = players[c].round.clip(1, maxp);
		var note = A2K.kr(pitch[c]);
		var vary = variation[c];
		var out, mid;

		out = maxp.collect({ |i|
			var on = i < np;
			var place = ((i / (np - 1).max(1)) * 2 - 1) * (np > 1);          // -1 .. 1 across the group
			var late = (TRand.kr(0, 1, hitk) + TRand.kr(0, 1, hitk)) * 0.5 * spread[c] * 0.001;
			var tp = DelayN.ar(hit, 0.11, late);
			var tk = T2K.kr(tp);
			var t = DelayN.ar(tp, 0.01, ControlDur.ir);
			var vel = (Latch.kr(level, tk) * TRand.kr(1 - (0.2 * vary), 1 + (0.1 * vary), tk)).clip(0, 1);
			var where = TRand.kr(0.5 - (0.5 * vary).min(0.5), 0.5 + (0.5 * vary).min(0.5), tk);   // 0 centre .. 1 near the rim
			var freq = (note + (place * detune[c] * 0.01) + (TRand.kr(-0.15, 0.15, tk) * vary)).midicps;
			var dec = decay[c] * Rand(0.85, 1.15) * TRand.kr(1 - (0.12 * vary), 1 + (0.12 * vary), tk);
			var vb = (vel.linlin(0, 1, 0.25, 1) * velbright[c]) + (1 - velbright[c]);
			var br = vb * bright[c] * where.linlin(0, 1, 0.8, 1.15);
			var fall = (vel * velbright[c]) + (1 - velbright[c]);
			var glide = EnvGen.ar(Env([1, 1 + (drop[c] * fall), 1], [0, droptime[c] * 0.001], -5), t);
			var gk = A2K.kr(glide);
			var loud = (vel.squared.linlin(0, 1, 0.08, 1) * velamp[c]) + (1 - velamp[c]);
			var lowSig, pulse, nEnv, exc, skinSig, shellSig, slapSig, stickSig, tok, voice;

			lowSig = ratios.collect({ |r, k|
				var rr = r ** stretch[c];
				var a = amps[k] * TRand.kr(0.925 - (0.275 * vary).min(0.9), 0.925 + (0.275 * vary), tk)
					* if(k == 0) { where.linlin(0, 1, 1.1, 0.8) } { where.linlin(0, 1, 0.55, 1.3) };
				var slope = 1 + (k * 0.55 * damp[c]);
				var e = EnvGen.ar(Env.perc(0.0015 + (0.005 / r), dec / slope, 1, -5), t);
				var e2 = EnvGen.ar(Env.perc(0.004 + (0.005 / r), dec * 0.55 / slope, 1, -5), t);
				var f = freq * rr * glide * Rand(0.985, 1.015);
				((SinOsc.ar(f, Rand(0, 0.5)) * e)
					+ (SinOsc.ar(f * (1 + (twin[c] * 0.01)) * Rand(0.99, 1.01), Rand(0, 6)) * e2 * 0.4 * (twin[c] > 0.01)))
					* a * (br ** (k * 0.5))
			}).sum * body[c] * 1.35;

			pulse = EnvGen.ar(Env.perc(0.0001, 0.0012 + (0.004 * (1 - vel)), 1, -3), t);
			nEnv = EnvGen.ar(Env.perc(0.0003, 0.003 + (0.012 * (1 - vel))), t);
			exc = LPF.ar((pulse * 0.5) + (WhiteNoise.ar * nEnv * 0.5), (700 + (br.squared * 11000)).clip(100, 18000));

			skinSig = upper.collect({ |r, k|
				Ringz.ar(exc, (freq * (r ** stretch[c]) * gk * Rand(0.97, 1.03)).clip(20, 9000),
					(dec * 0.7 * ((3 / r) ** (1.3 * damp[c]))).max(0.035))
					* (1 / (1 + (k * 0.3))) * TRand.kr(0.5, 1.2, tk).lag(0.004)
			}).sum * 0.4 * skin[c] * br;

			shellSig = [0.5, 1, 1.46, 2.43, 3.07, 4.7].collect({ |r, k|
				Ringz.ar(exc + (lowSig * 0.002), (shellfreq[c] * Rand(0.89, 1.12) * r * Rand(0.96, 1.04)).clip(20, 12000),
					[0.13, 0.11, 0.08, 0.06, 0.045, 0.03][k] * shelldecay[c]) * [0.6, 1, 0.8, 0.7, 0.5, 0.4][k]
			}).sum * 0.21 * shell[c];

			slapSig = HPF.ar(LPF.ar(PinkNoise.ar, (600 + (br.squared * 5000)).clip(100, 18000)), 150)
				* EnvGen.ar(Env.perc(0.001, slapdecay[c] * vel.linlin(0, 1, 0.5, 1), 1, -4), t) * 0.2 * slap[c];

			stickSig = HPF.ar(LPF.ar(pulse + (WhiteNoise.ar * nEnv * 0.35), (3000 + (fall.squared * 11000 * bright[c])).clip(500, 19000)), stickhp[c]);
			tok = Ringz.ar(stickSig, (tokfreq[c] * Rand(0.84, 1.19) * [1, 1.52, 2.03, 2.56]).clip(20, 16000), [0.02, 0.014, 0.009, 0.006]).sum * 0.006;

			voice = ((lowSig + skinSig + shellSig + slapSig) * loud)
				+ ((stickSig + tok) * ((vel.squared * velamp[c]) + (1 - velamp[c])) * stick[c] * 76.5);
			voice.tanh * on
		}).sum * (0.67 / np.sqrt) * push[c].lag(0.02);

		// the tone
	push = OceanodeParameter.kr(\push, 1, n, 0, 4, "vf").asArray;                   // how hard the group plays into what follows
		mid = BPF.ar(out, 500, 2.5);
		out = out + ((mid * 8).tanh * roar[c]);
		out = BLowShelf.ar(out, lowfreq[c], 1, low[c]);
		out = BPeakEQ.ar(out, 210, 1, mud[c]);
		out = (out * drive[c]).tanh;
		out = Compander.ar(out, out, 0.35, 1, (1 - (comp[c] * 0.4)).clip(0.1, 1), 0.015, 0.12);
		out = BHiShelf.ar(out, 3500, 1, air[c]);
		out * 1.16 * levels[c]
	});

	Out.ar(OceanodeOutput.kr(\out), sig);

}, description: "Big skin drums played by a group: taikos, toms, sub drums and clacks", category: "Source/Percussion", variables: ["Players"], variableDimensions: [6]);
)
