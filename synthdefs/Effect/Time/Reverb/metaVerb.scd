(
// MetaVerb: the Reaktor "Metaverb 2.7b" Core cell (AnimaMundi2_tips.ens, Instrument "Metaverb"),
// decoded with tools/ReaktorPatchExtractor. The same maths runs inside AnimaBox
// (synthdefs/Defaults/AnimaBox/animaBox.scd, AnimaMetaVerb<n>): keep both in step.
//
// Reaktor (stereo): a cross-coupled figure-8. Each side: input + the other side's feedback
// -> high shelf (Hi Cut) -> low shelf (Lo Cut) -> 3 allpasses (wet tap) -> long delay
// -> x decay gain -> the other side. Line lengths in pitch: 30 - 16 s (+ a bend below s 0.2),
// s = Size (0.31 + 0.69 Size); R 1..3 semitones lower. Ratios 0.506394 / 0.803851 / 1 / 1.498307.
// Allpass gain -(s/24 + 0.5), RT = 0.1223 s x 256^s on the long delay. 8-phase parabolic LFO
// 0..2a samples on every line, a = (0.5 + 2 Size (1.5 - Size)) x 0.5 ms. Mix = fader squared.
//
// n channels: a RING of n chains (chain k feeds chain k+1, the last the first). With 2 it is
// exactly the figure-8; chain k's lengths sit between L (k = 0) and R (k = n - 1).
~synthCreator.value("MetaVerb", {|n|
	var input, size, locut, hicut, mix;
	input = In.ar(OceanodeInput.kr(\in), n).asArray;
	size = OceanodeParameter.kr(\size, 0.865, 1, 0, 1, "f");
	locut = OceanodeParameter.kr(\locut, 0.005, 1, 0, 1, "f");
	hicut = OceanodeParameter.kr(\hicut, 0.595, 1, 0, 1, "f");
	mix = OceanodeParameter.kr(\mix, 0.5, 1, 0, 1, "f");
	Out.ar(OceanodeOutput.kr(\out), {
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
			// the L pattern on even chains, the R pattern on odd ones, each pair a quarter apart
			var o = (k div: 2) * (0.5 / n.max(2));
			var m = if(k.even) {
				[a * (1 + par.(o)), a * (1 + par.(o + 0.25)), a * (1 - par.(o)), a * (1 - par.(o + 0.25))]
			} {
				[a * (1 + par.(o + 0.125)), a * (1 - par.(o + 0.125)), a * (1 + par.(o + 0.375)), a * (1 - par.(o + 0.375))]
			};
			var af = VarLag.kr(10 ** (-3 * ((len * 1.498307) + a) / rt), 0.3, 0);
			var y = shelves.(input[k] + fb[(k - 1).wrap(0, n - 1)]);
			[0.506394, 0.803851, 1.0].do { |r, i|
				var d = ((len * r) + m[i] + 1) / sr;     // + 1: the Core's latched sample
				y = AllpassC.ar(y, 0.2, d, d * (-6.907755) / g.abs.log * g.sign);
			};
			wet[k] = y;
			// the long delay, less the block LocalIn adds
			loop[k] = DelayC.ar(y, 0.25, ((len * 1.498307) + m[3] + 1 - BlockSize.ir).max(1) / sr) * af;
		};
		LocalOut.ar(loop);
		n.collect { |k| (input[k] * (1 - mixS)) + (wet[k] * mixS) }
	}.value);
}, description: "Reaktor Metaverb 2.7b (AnimaMundi): figure-8 allpass reverb with in-loop Hi/Lo Cut shelves; n channels run a ring of chains", category: "Effect/Time/Reverb");
)
