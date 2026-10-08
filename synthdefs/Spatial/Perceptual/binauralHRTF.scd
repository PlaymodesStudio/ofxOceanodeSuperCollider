(
// BinauralHRTF — n virtual sources (or virtual speakers) rendered to binaural stereo for headphones.
// Structural HRTF model (Brown & Duda, "A structural model for binaural sound synthesis",
// IEEE TSAP 1998): no measured data needed, everything is parametric and runs per source.
//  * Head: Woodworth ITD per ear and a one-pole/one-zero head-shadow filter whose high-frequency
//    gain goes from +6 dB (source facing the ear) to -20 dB (150 deg away), bilinear-discretised.
//  * Pinna: 5 short reflections whose delays depend on azimuth and elevation (elevation and
//    front/back cues). Amount scaled by `pinna`.
//  * Torso: one low-passed shoulder echo whose delay depends on elevation. Amount scaled by `torso`.
//  * Distance: 1/r gain (unity at 1 m) and air absorption (lowpass falling with distance).
//  * Room: a small stereo room after the binaural sum helps externalisation (`room` = wet amount).
// Angles follow 360Panner: azimuth 0 front, 0.25 right, 0.5 rear, 0.75 left; elevation in degrees.
// Layout: Auto = 1 source front, 2 sources at -30/+30 deg (stereo pair), n>2 an even ring of
//   virtual speakers starting half a step right of front, clockwise; Ring = always the ring;
//   Manual = only the azimuth parameter. `azimuth` is added to the layout position and `rotate`
//   turns the listener's head. Output is always 2 channels (L, R).
~synthCreator.value("BinauralHRTF", {|n|
	var in, azimuth, elevation, distance, level, layout, rotate, headsize, pinna, torso, room, roomsize, output;
	var autoPos, ringPos, ears, sig;
	var c = 343;
	var pinnaRho = [0.5, -1, 0.5, -0.25, 0.25], pinnaA = [1, 5, 5, 5, 5], pinnaB = [2, 4, 7, 11, 13];
	var pinnaD = [1, 0.5, 0.5, 0.5, 0.5];

	azimuth   = OceanodeParameterLag.ar(\azimuth, 0, n, 0, 1, "vf", 1/30, true);
	elevation = OceanodeParameterLag.ar(\elevation, 0, n, -45, 90, "vf", 1/30, true);       // deg
	distance  = OceanodeParameterLag.ar(\distance, 1.5, n, 0.25, 50, "vf", 1/30, true);     // m
	level     = OceanodeParameterLag.ar(\levels, 1, n, 0, 1, "vf", 1/30, true);
	layout    = OceanodeParameterDropdown.kr(\layout, 0, 1, "Auto:Ring:Manual").asArray[0];
	rotate    = OceanodeParameterLag.ar(\rotate, 0, 1, 0, 1, "f", 1/30, true).asArray[0];
	headsize  = OceanodeParameterLag.ar(\headsize, 8.75, 1, 7, 11, "f", 1/30, true).asArray[0] * 0.01;   // radius, cm -> m
	pinna     = OceanodeParameterLag.ar(\pinna, 1, 1, 0, 1, "f", 1/30, true).asArray[0];
	torso     = OceanodeParameterLag.ar(\torso, 0.5, 1, 0, 1, "f", 1/30, true).asArray[0];
	room      = OceanodeParameterLag.kr(\room, 0.15, 1, 0, 1, "f", 1/30, true).asArray[0];
	roomsize  = OceanodeParameterLag.kr(\roomsize, 0.5, 1, 0, 1, "f", 1/30, true).asArray[0];
	output    = OceanodeParameterLag.ar(\output, 0, 1, -24, 12, "f", 1/30, true).asArray[0].dbamp;      // dB

	// (size-1 controls return 1-element arrays: .asArray[0] takes the scalar)
	in = In.ar(OceanodeInput.kr(\in), n).asArray;

	// layout positions (fractions of a turn)
	ringPos = n.collect { |i| (i + 0.5) / n };
	autoPos = case
		{ n == 1 } { [0] }
		{ n == 2 } { [-30, 30] / 360 }
		{ ringPos };

	ears = n.collect { |i|
		var a = headsize, ac = headsize / c;
		var pos = Select.kr(layout, [autoPos[i], ringPos[i], 0]);
		var az = (azimuth[i] + pos - rotate).wrap(0, 1) * 2pi;          // 0 front, pi/2 right
		var el = elevation[i] * (pi / 180);
		var lateral = az.sin * el.cos;                                  // +1 right ear axis, -1 left
		var azDeg = (az.wrap(-pi, pi)) * (180 / pi);                    // -180..180, + right
		var src, pinnaSig, torsoDelay;

		// distance: 1/r (unity at 1 m) and air absorption
		src = in[i] * level[i] * distance[i].max(0.25).reciprocal.min(4);
		src = BLowPass.ar(src, (22000 * (distance[i].neg / 30).exp).clip(2000, 20000), 0.707);

		// pinna reflections (same cue for both ears, Brown-Duda tau_k in samples at 44.1 kHz)
		pinnaSig = src + (pinna * Mix.fill(5, { |k|
			var tau = (pinnaA[k] * (azDeg * (pi / 360)).cos * (pinnaD[k] * ((pi / 2) - el)).sin) + pinnaB[k];
			DelayC.ar(src, 0.001, (tau / 44100).clip(0, 0.001)) * pinnaRho[k]
		}));

		// shoulder echo: ~1.2 ms below the ear, shorter as the source rises
		torsoDelay = (1.2e-3 * (1 - (0.00004 * (elevation[i] - 80).squared)).clip(0.15, 1));

		[lateral.neg, lateral].collect { |cosTheta|                     // left ear, right ear
			var theta = cosTheta.clip(-1, 1).acos;                      // angle source <-> ear axis
			var alpha = (1.05) + (0.95 * (theta / (150 * pi / 180) * pi).cos);
			var w0 = c / a, k = 2 * SampleRate.ir, g = k / (2 * w0), d0 = 1 + g;
			var itd = Select.ar(theta < (pi / 2), [ac * (theta - (pi / 2)), ac * theta.cos.neg]) + ac;
			var ear = FOS.ar(pinnaSig, (1 + (alpha * g)) / d0, (1 - (alpha * g)) / d0, ((1 - g) / d0).neg);
			ear = ear + (LPF.ar(DelayC.ar(ear, 0.002, torsoDelay), 3000) * 0.3 * torso);
			DelayC.ar(ear, 0.002, itd.clip(0, 0.002))
		}
	};

	// sum the sources per ear, then the small room
	sig = [Mix(ears.collect(_[0])), Mix(ears.collect(_[1]))];
	sig = sig + (FreeVerb2.ar(sig[0], sig[1], 1, roomsize, 0.5) * room * 0.5);

	Out.ar(OceanodeOutput.kr(\out), sig * output);

}, description: "Structural HRTF binaural renderer for n virtual sources or speakers with head shadow ITD pinna torso distance and room", category: "Spatial/Perceptual");
)
