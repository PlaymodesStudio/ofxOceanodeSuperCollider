// AnimaBox: the Reaktor ensemble AnimaMundi2_tips.ens (Santi Vilanova), decoded with
// tools/ReaktorPatchExtractor (specs: tools/animabox/reaktor/spec_*.md).
// Evaluate this file; needs the OceanodeAnalog extension (ReaktorSVF4, ReaktorMulti4). No server needed.
//
//   AnimaBody          the exciter ("Body"): two sides A / B (Reaktor's L / R), each an
//                      oscillator (noise, saw, pulse, impulse, geiger) or the live input,
//                      an ADSR on the note gate (Reaktor log times: 10^(x/20) ms).
//                      -> \out (audio, 2). Also fans the gate / velocity / a per-note random
//                      out to the modulation buses (one value for every band).
//   AnimaBank16_<n>    16 bands from \first (the node runs ceil(Bands / 16) of them): per band
//                      two Multi/LP 4-Pole BP4 filters (ReaktorSVF4: Reaktor's own, decoded from its binary) (side A at pitch + d, B at pitch - d),
//                      Reaktor's gain correction, energy x damp (x a threshold pluck), placed
//                      at two positions on n speakers (line or ring), summed into \out (n).
//   AnimaPost<n>       Reaktor's Saturator, then the Post Filter (library multimode).
//   AnimaDelay<n>      delay2 ("Delay 4p"): tempo delay, filter in the loop, per channel.
//   AnimaMetaVerb<n>   Metaverb 2.7b (also Effect/Time/Reverb/metaVerb.scd: keep both in step).
//                      2 channels: the figure-8; n: a ring of n chains.
(
var here = thisProcess.nowExecutingPath.dirname;
var root = here +/+ "CompiledSynthdefs", dir;
var hz = { |p| 440 * (2 ** ((p - 69) / 12)) };
var logT = { |x| (10 ** (x / 20)) * 0.001 };          // Reaktor envelope log time -> s
// Reaktor's ADSR decay / release are one-pole time constants: segments of 5 T, curve -5
var spread;
dir = root +/+ "AnimaBox"; File.mkdir(root); File.mkdir(dir);

// a signal at position pos (0..1) on n speakers: line (0 = first, 1 = last) or ring
// (pos wraps; speaker k at k / n). Linear amplitude law (the Reaktor Distributor's).
spread = { |sig, pos, n, ring|
	if(n == 1) { [sig] } {
		var posLine = pos.clip(0, 1) * (n - 1), posRing = pos.wrap(0, 1) * n;
		n.collect { |ch|
			var lineGain = (1 - (posLine - ch).abs).max(0);
			var d = (posRing - ch).abs, ringGain = (1 - d.min(n - d)).max(0);
			sig * ((lineGain * (1 - ring)) + (ringGain * ring))
		}
	}
};

SynthDef(\AnimaBody, {
	var gb = \gtrbus.kr(-1), gg = In.kr(gb.max(0), 31) * (gb >= 0);   // Global Pitch (scGlobalPitch.h)
	var gq = { |p| var r = p.round, q = p + (((((r < gg[17]) * gg[30]) + ((r >= gg[17]) * gg[1])) > 0.5) * Select.kr(r.mod(12) + ((r < gg[17]) * 12), gg[2..13] ++ gg[18..29])), w = gg[16] + (((q - gg[16]) / 12).floor * 12); q + gg[0] - (gg[15] * 12 * ((q + gg[0] - w) / 12).floor) };
	var cable = \gate_ar.ar(0) * \gate_sel.kr(0);
	var drone = \drone.kr(0) > 0.5;
	var gateLevel = Select.ar(\gate_sel.kr(0) > 0, [K2A.ar(\gate.kr(0)), cable]).max(0);
	var gate = ((gateLevel > 0) + drone) > 0;
	var trig = (HPZ1.ar(gate) > 0) + (Impulse.ar(0) * gate);
	var level = Select.ar(drone, [Latch.ar(gateLevel, trig).max(gateLevel * (\gate_sel.kr(0) < 0.5)), DC.ar(1)]);
	var note = gq.(Select.kr(\pitch_sel.kr(0), [\pitch.kr(48), A2K.kr(\pitch_ar.ar(48))]));
	var p = note + (12 * \oct.kr(-3)) + \trans.kr(0), det = \det.kr(0.24);
	var env = EnvGen.ar(Env.adsr(logT.(\atk.kr(25)), 5 * logT.(\dec.kr(40)), \sus.kr(0.5), 5 * logT.(\rel.kr(15)), 1, [0, -5, -5]), gate) * level;
	var live = [In.ar(\livein.kr(0), 1), In.ar(\livein.kr(0) + (\livech.kr(1) > 1), 1)] * (\livein.kr(0) > 0) * \livegain.kr(1);
	var sides = [p + det, p - det].collect { |pp, side|
		var f = hz.(pp).clip(0.01, SampleRate.ir * 0.45);
		var oscs = [
			WhiteNoise.ar * 0.5,                                     // Reaktor Noise: +-0.5 A
			Saw.ar(f),
			Pulse.ar(f, 0.5) ,
			// Reaktor Impulse: positive, DC removed, energy normalised (peak ~ 8 at 44.1 k)
			LeakDC.ar(Impulse.ar(f) * (8 * 44100 / SampleRate.ir), 0.999),
			// Geiger: random clicks around the pitch's rate; Rnd 1 = regular
			LinXFade2.ar(Dust.ar(f), Impulse.ar(f), (\geiger.kr(0) * 2 - 1))
		];
		var osc = SelectX.ar(NamedControl.kr(\osc, [1, 1])[side].clip(0, 4), oscs) * env;
		LinXFade2.ar(osc, live[side], NamedControl.kr(\src, [0, 0])[side].clip(0, 1) * 2 - 1)
	};
	Out.ar(\out.kr(0), sides * \level.kr(1));
	// the modulation sources: every band follows the one note
	Out.ar(\gatebus.kr(0), (gate * level) ! 128);
	Out.kr(\vel.kr(0), A2K.kr(level) ! 128);
	Out.kr(\rnd.kr(0), A2K.kr(TRand.ar(-1, 1, trig)) ! 128);
	Out.kr(\vis.kr(0), [A2K.kr(env), A2K.kr(Amplitude.ar(sides[0])), A2K.kr(Amplitude.ar(sides[1])), note]);
}).writeDefFile(dir);

(1..16).do { |n|
	SynthDef(("AnimaBank16_" ++ n).asSymbol, {
		var kv = { |name, def| NamedControl.kr(name, def ! 16) };
		var first = \first.kr(0), sr = SampleRate.ir;
		var exc = In.ar(\exc.kr(0), 2);
		var mE = In.kr(\menergy.kr(0) + first, 16), mQ = In.kr(\mq.kr(0) + first, 16), mP = In.kr(\mpitch.kr(0) + first, 16);
		var pitch = VarLag.kr(kv.(\pitch, 60), \pglide.kr(0), 0) + mP;
		var vib = SinOsc.kr(kv.(\vibrate, 6.31)) * \vibamt.kr(0);   // per band rate (Vibrato Poly)
		var energy = (kv.(\energy, 0) + mE).clip(0, 1);
		var thr = \pthr.kr(0.07), pluck = \pluck.kr(0);
		var reso = Lag.kr((\reso.kr(30) + mQ).clip(0, 100), 0.1);
		var damp = kv.(\damp, 1), active = kv.(\active, 1), pana = kv.(\pana, 0), panb = kv.(\panb, 1);
		var d = \sdet.kr(0.1), k = \gain.kr(16), fine = \fine.kr(0), ring = \ring.kr(0);
		var s3 = { |x| x = x.clip(0, 1); Select.kr((x > 0.25) + (x > 0.5), [0.4 + (0.68 * x), 0.57 + (0.56 * (x - 0.25)), 0.71]) };
		var out = 0 ! n, vis;
		vis = 16.collect { |b|
			var e = energy[b];
			// Gated: an ADSR gated while the band's energy is above the threshold (a pluck as it rises past it)
			var env = EnvGen.kr(Env.adsr(logT.(\patk.kr(0)), 5 * logT.(\pdec.kr(52)), \psus.kr(0), 5 * logT.(\prel.kr(26)), 1, [0, -5, -5]),
				e > thr);
			var amp = Select.kr(pluck > 0.5, [e, env]) * damp[b] * active[b];
			var p = pitch[b] + vib[b], res = ((1 - (10 ** (reso[b].neg / 40))) + fine).clip(0, 1);
			// Reaktor's Gain Correction, and Reaktor's own Multi/LP 4-Pole (ReaktorSVF4, its BP4)
			var g = k * amp * (-73 * (1 - res) * hz.(p) / sr).dbamp * 0.1585 * (reso[b].neg * s3.(reso[b] / 100)).dbamp;
			var a = ReaktorSVF4.ar(exc[0], hz.(p + d).clip(10, sr * 0.5), res, 3);
			var bb = ReaktorSVF4.ar(exc[1], hz.(p - d).clip(10, sr * 0.5), res, 3);
			var gA = K2A.ar(g);
			out = out + spread.(a * gA, pana[b], n, ring) + spread.(bb * gA, panb[b], n, ring);
			amp
		};
		Out.ar(\out.kr(0), out);
		Out.kr(\vis.kr(0) + first, vis);
	}).writeDefFile(dir);

	SynthDef(("AnimaPost" ++ n).asSymbol, {
		var sig = In.ar(\in.kr(0), n).asArray;
		var sr = SampleRate.ir;
		var cut = Lag.kr(\cutoff.kr(127), 0.03), resL = Lag.kr(\reson.kr(0), 0.03).max(0), gc = Lag.kr(\gainc.kr(0.504), 0.03);
		var type = \type.kr(0), f = hz.(cut).clip(10, sr * 0.45);
		var res2 = 1 - (10 ** (resL.neg / 20)), res4 = (1 - (10 ** (resL.neg / 40))).clip(0, 0.995);
		var shp1 = { |x, a, b, c| x = x.clip(0, 1); Select.kr(x > 0.5, [a + ((b - a) * x * 2), b + ((c - b) * (x - 0.5) * 2)]) };
		var c2 = (resL + shp1.(0.5 * res2, -3, -6, 0)).max(0), c4 = (resL + shp1.(0.5 * res4, -6, -12, 0)).max(0);
		var rq = (2 * (1 - res2)).clip(0.01, 2);
		sig = sig.collect { |x|
			var z = x.clip(-4, 4), s = z * (1 - (z.abs / 8));             // Reaktor Saturator
			var outs = [
				OnePole.ar(s, exp(-2pi * f / sr)),                                                     // LP1
				RLPF.ar(s, f, rq) * (gc.neg * c2).dbamp,                                               // LP2
				ReaktorSVF4.ar(s, f, res4, 5) * (gc.neg * c4).dbamp,                                   // LP4 (Multi/LP 4-Pole)
				BPF.ar(s, f, rq) * (6.0206 - (gc * resL)).dbamp * (-36 * (1 - res2) * f / sr).dbamp,   // BP2
				ReaktorMulti4.ar(s, f, res4, 9) * (12.0412 - (gc * resL) - (73 * (1 - res4) * f / sr)).dbamp, // BP4
				(s - OnePole.ar(s, exp(-2pi * f / sr))) * (1 - (pi * f.min(sr / 2pi) / sr)),           // HP1
				RHPF.ar(s, f, rq) * ((gc.neg * c2) - (51.75 * (1 - res2) * f / sr)).dbamp,             // HP2
				ReaktorMulti4.ar(s, f, res4, 7) * (gc.neg * c4).dbamp,                                  // HP4
				s                                                                                      // Off
			];
			Select.ar(type.clip(0, 8), outs)
		};
		Out.ar(\out.kr(0), sig);
	}).writeDefFile(dir);

	// delay2: T = den / num beats (\bpm), glided by a 14.6 Hz one-pole; the loop
	// delay(in + fb x filter); filter: HP2 BP2 LP2 BP4 BL4 LP4 (Multi/LP 4-Pole) or Peak EQ;
	// out = in + wet x filter. Per channel (Reaktor's L / R are two copies).
	SynthDef(("AnimaDelay" ++ n).asSymbol, {
		var sig = In.ar(\in.kr(0), n).asArray, on = \on.kr(1, 0.05);
		var beats = \den.kr(4).max(0.01) / \num.kr(2).max(0.01);
		var t = OnePole.kr(Lag.kr(beats, 0.1) * 60 / \bpm.kr(120).max(1), exp(-2pi * 14.57 / ControlRate.ir)).clip(0.001, 4);
		var fbk = \feedback.kr(0.4), wet = \wet.kr(0.3) * on, cut = hz.(\cutoff.kr(58)).clip(10, SampleRate.ir * 0.45);
		var res = \res.kr(0.57), mode = \mode.kr(6);
		var loop = LocalIn.ar(n).asArray, outs = Array.newClear(n), filts = Array.newClear(n);
		n.do { |c|
			var dl = DelayC.ar(sig[c] + (loop[c] * fbk * on), 4.1, (t - ControlDur.ir).max(0));
			var f = Select.ar(mode.clip(0, 6), [0, 1, 2, 3, 4, 5].collect { |ty| ReaktorSVF4.ar(dl, cut, res, ty) }
				++ [MidEQ.ar(dl, cut, 1, 10 * res)]);
			filts[c] = f;
			outs[c] = sig[c] + (f * wet);
		};
		LocalOut.ar(filts);
		Out.ar(\out.kr(0), outs);
	}).writeDefFile(dir);

	// Metaverb 2.7b (the maths and its notes: Effect/Time/Reverb/metaVerb.scd)
	SynthDef(("AnimaMetaVerb" ++ n).asSymbol, {
		var input = In.ar(\in.kr(0), n).asArray;
		var size = \size.kr(0.865), locut = \locut.kr(0.005), hicut = \hicut.kr(0.595);
		var mix = \mix.kr(0.5) * \on.kr(1, 0.05);
		var sr = SampleRate.ir, p2f = { |p| 8.17742 * (2 ** (p / 12)) };
		var sz = size.clip(0, 1), s = sz * (0.31 + (0.69 * sz));
		var yq = ((0.2 - s) / 0.2).max(0).squared;
		var pL = 30 - (16 * s) + (12 * yq), dR = 1 + (2 * sz * (2 - sz));
		var g = ((s.max(0.0001) / 24) + 0.5) * (1 - (0.4 * yq)) * -1;
		var rt = sr / p2f.(-96 * s);
		var a = (0.5 + (2 * sz * (1.5 - sz))) * 0.0005 * sr;
		var phi = LFSaw.ar(0.66 * (sr / 4) / 9700) * 0.5;
		var par = { |off| var x = (phi + off).wrap(-0.5, 0.5); x * (8 - (16 * x.abs)) };
		var hx = 1 - hicut.clip(0, 1), lc = locut.clip(0, 1);
		var hsF = VarLag.kr(p2f.(120 - (34 * hx)), 0.05, 0), hiDb = VarLag.kr(-12 * hx * (2 - hx), 0.05, 0);
		var lsF = VarLag.kr(p2f.(40 + (24 * lc)), 0.05, 0), loDb = VarLag.kr(-18 * lc * (2 - lc), 0.05, 0);
		var onePole = { |x, fc, hp|
			var w = tan(pi * fc.clip(1, sr * 0.49) / sr), a0 = 1 + w, b1 = (1 - w) / a0;
			if(hp) { FOS.ar(x, 1 / a0, -1 / a0, b1) } { FOS.ar(x, w / a0, w / a0, b1) } };
		var shelves = { |x|
			var hA = hiDb.dbamp, lA = loDb.dbamp;
			x = x + ((hA - 1) * onePole.(x, hsF * hA.sqrt, true));
			x + ((lA - 1) * onePole.(x, lsF / lA.sqrt, false)) };
		var fb = LocalIn.ar(n).asArray, wet = Array.newClear(n), loop = Array.newClear(n);
		var mixS = Lag.ar(K2A.ar(mix.clip(0, 1).squared), 0.075);
		n.do { |k|
			var frac = if(n > 1) { k / (n - 1) } { 0 };
			var len = VarLag.kr(sr / p2f.(pL - (dR * frac)), 0.3, 0);
			var o = (k div: 2) * (0.5 / n.max(2));
			var m = if(k.even) {
				[a * (1 + par.(o)), a * (1 + par.(o + 0.25)), a * (1 - par.(o)), a * (1 - par.(o + 0.25))]
			} {
				[a * (1 + par.(o + 0.125)), a * (1 - par.(o + 0.125)), a * (1 + par.(o + 0.375)), a * (1 - par.(o + 0.375))]
			};
			var af = VarLag.kr(10 ** (-3 * ((len * 1.498307) + a) / rt), 0.3, 0);
			var y = shelves.(input[k] + fb[(k - 1).wrap(0, n - 1)]);
			[0.506394, 0.803851, 1.0].do { |r, i|
				var dd = ((len * r) + m[i] + 1) / sr;
				y = AllpassC.ar(y, 0.2, dd, dd * (-6.907755) / g.abs.log * g.sign);
			};
			wet[k] = y;
			loop[k] = DelayC.ar(y, 0.25, ((len * 1.498307) + m[3] + 1 - BlockSize.ir).max(1) / sr) * af;
		};
		LocalOut.ar(loop);
		Out.ar(\out.kr(0), n.collect { |k| (input[k] * (1 - mixS)) + (wet[k] * mixS) });
	}).writeDefFile(dir);
};
"AnimaBox SynthDefs written to ".post; dir.postln;
)
