(
~synthCreator.value("FormantFilter", {|n|
	// Five formants per vowel (A E I O U) for five voice types: centre
	// frequency in Hz, level in dB and bandwidth in Hz. These are the values of
	// SC3plugins' FormantTable.
	var freqs = [
		[[800, 1150, 2900, 3900, 4950], [350, 2000, 2800, 3600, 4950], [270, 2140, 2950, 3900, 4950], [450, 800, 2830, 3800, 4950], [325, 700, 2700, 3800, 4950]], // soprano
		[[800, 1150, 2800, 3500, 4950], [400, 1600, 2700, 3300, 4950], [350, 1700, 2700, 3700, 4950], [450, 800, 2830, 3500, 4950], [325, 700, 2530, 3500, 4950]], // alto
		[[660, 1120, 2750, 3000, 3350], [440, 1800, 2700, 3000, 3300], [270, 1850, 2900, 3350, 3590], [430, 820, 2700, 3000, 3300], [370, 630, 2750, 3000, 3400]], // counter tenor
		[[650, 1080, 2650, 2900, 3250], [400, 1700, 2600, 3200, 3580], [290, 1870, 2800, 3250, 3540], [400, 800, 2600, 2800, 3000], [350, 600, 2700, 2900, 3300]], // tenor
		[[600, 1040, 2250, 2450, 2750], [400, 1620, 2400, 2800, 3100], [250, 1750, 2600, 3050, 3340], [400, 750, 2400, 2600, 2900], [350, 600, 2400, 2675, 2950]]  // bass
	];
	var dbs = [
		[[0, -6, -32, -20, -50], [0, -20, -15, -40, -56], [0, -12, -26, -26, -44], [0, -11, -22, -22, -50], [0, -16, -35, -40, -60]],
		[[0, -4, -20, -36, -60], [0, -24, -30, -35, -60], [0, -20, -30, -36, -60], [0, -9, -16, -28, -55], [0, -12, -30, -40, -64]],
		[[0, -6, -23, -24, -38], [0, -14, -18, -20, -20], [0, -24, -24, -36, -36], [0, -10, -26, -22, -34], [0, -20, -23, -30, -34]],
		[[0, -6, -7, -8, -22], [0, -14, -12, -14, -20], [0, -15, -18, -20, -30], [0, -10, -12, -12, -26], [0, -20, -17, -14, -26]],
		[[0, -7, -9, -9, -20], [0, -12, -9, -12, -18], [0, -30, -16, -22, -28], [0, -11, -21, -20, -40], [0, -20, -32, -28, -36]]
	];
	var bws = [
		[[80, 90, 120, 130, 140], [60, 100, 120, 150, 200], [60, 90, 100, 120, 120], [70, 80, 100, 130, 135], [50, 60, 170, 180, 200]],
		[[80, 90, 120, 130, 140], [60, 80, 120, 150, 200], [50, 100, 120, 150, 200], [70, 80, 100, 130, 135], [50, 60, 170, 180, 200]],
		[[80, 90, 120, 130, 140], [70, 80, 100, 120, 120], [40, 90, 100, 120, 120], [40, 80, 100, 120, 120], [40, 60, 100, 120, 120]],
		[[80, 90, 120, 130, 140], [70, 80, 100, 120, 120], [40, 90, 100, 120, 120], [40, 80, 100, 120, 120], [40, 60, 100, 120, 120]],
		[[60, 70, 110, 120, 130], [40, 80, 100, 120, 120], [60, 90, 100, 120, 120], [40, 80, 100, 120, 120], [40, 80, 100, 120, 120]]
	];
	var numVowels = 5, numFormants = 5;

	var input = In.ar(OceanodeInput.kr(\in), n).asArray;
	// 0=A 1=E 2=I 3=O 4=U, morphing in between; 5 is A again, so a 0..5 ramp loops
	var vowel = OceanodeParameterLag.ar(\vowel, 0, n, 0, 5, "vf", 0.05, true).asArray;
	var voice = OceanodeParameterDropdown.kr(\voice, 3, n, "Soprano:Alto:CounterTenor:Tenor:Bass").asArray;
	// moves every formant together, in semitones (smaller or bigger throat)
	var shift = OceanodeParameterLag.ar(\shift, 0, n, -36, 36, "vf", 0.05, true).asArray;
	// 1 = natural bandwidths; higher narrows them until the formants ring
	var res = OceanodeParameterLag.ar(\res, 1, n, 0.25, 40, "vf", 0.05, true).asArray;
	// 0 = natural formant levels, 1 = all five equally loud
	var bright = OceanodeParameterLag.ar(\bright, 0, n, 0, 1, "vf", 0.05, true).asArray;
	var drive = OceanodeParameterLag.ar(\drive, 0, n, 0, 1, "vf", 0.05, true).asArray;
	var mix = OceanodeParameterLag.ar(\mix, 1, n, 0, 1, "vf", 1/30, true).asArray;
	var level = OceanodeParameterLag.ar(\levels, 1, n, 0, 1, "vf", 1/30, true).asArray.lincurve(0, 1, 0, 1, 4);

	var sig = n.collect { |ch|
		// BPF reads its coefficients once per block, so the table maths runs
		// at control rate (audio cables into these controls still work)
		var kVowel = A2K.kr(vowel[ch]), kShift = A2K.kr(shift[ch]);
		var kRes = A2K.kr(res[ch]), kBright = A2K.kr(bright[ch]);
		// triangular weight of each vowel around the circle A E I O U A
		var weights = numVowels.collect { |v|
			(1 - (kVowel - v).wrap(numVowels / -2, numVowels / 2).abs).max(0);
		};
		// the tables of the chosen voice, morphed between vowels: frequency
		// as pitch, level in dB, bandwidth in Hz
		var morph = { |table, func|
			numVowels.collect { |v|
				Select.kr(voice[ch], table.collect { |voiceTable| func.(voiceTable[v]) }) * weights[v];
			}.sum;
		};
		var wet = numFormants.collect { |i|
			var note = morph.(freqs, { |x| x[i].cpsmidi });
			var db = morph.(dbs, { |x| x[i] });
			var bw = morph.(bws, { |x| x[i] });
			var freq = (note + kShift).midicps.clip(20, SampleRate.ir * 0.45);
			// bandwidth follows the shift, so the vowel keeps its character
			var rq = (bw / (note.midicps * kRes)).clip(0.0005, 2);
			BPF.ar(input[ch], freq, rq) * (db * (1 - kBright)).dbamp;
		}.sum;
		var pre, out;

		// narrower bands pass less of a broadband input, but a tone sitting
		// on a formant passes whole: compensate part of the way
		wet = wet * kRes.pow(0.3) * 5;
		// saturation after the formants, roughly level matched
		pre = 1 + (drive[ch] * 31);
		wet = wet + (drive[ch] * (((wet * pre).tanh / pre.pow(0.7)) - wet));

		// soft ceiling: clean below half scale, never above full scale
		wet = wet.softclip;

		out = XFade2.ar(input[ch], wet, mix[ch].linlin(0, 1, -1, 1));
		out * level[ch];
	};

	Out.ar(OceanodeOutput.kr(\out), sig);
}, description: "Five band vocal formant filter with vowel morph A E I O U; voice type; formant shift; resonance and drive", category: "Effect/Filter");
)
