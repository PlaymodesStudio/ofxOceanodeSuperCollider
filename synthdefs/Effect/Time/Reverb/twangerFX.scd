(
// t'wanGer's reVerberator and maesTro (the Reaktor ensemble twangerv4.ens).
// Mono in (Twanger), stereo out:
//   in + feedback -> 1-pole LP (lPf) -> 3 allpasses -> allpass 4 = left, allpass 5 = right
//   allpass 4 -> delay (delAy + slow random, smoothed) -> feedback, -60 dB per reV seconds
//   dry/wet crossfade, oN; then leVel, a 32 Hz 4-pole high-pass and the peak limiter
// The allpasses' times and polarities and the delay's random wobble are not on the
// Reaktor panel but differ per preset: they travel in Diffusers, 17 values:
//   allpass delays 1-5 (ms), fines 1-5 (ms), polarities 1-5 (+1 / -1), random amount (ms), rate (Hz).
// Each allpass's gain gives -60 dB in tiMe seconds; the feedback loop runs through
// LocalIn, whose one block is taken off the delay, so the loop keeps its length.
~synthCreator.value("TwangerFX", {|n|
	var in, delay, time, rev, lpf, drywet, rvon, level, diff;
	var tms, fb, loop, x, ap, l, r, wet, rnd, dl, sel, env;

	in = In.ar(OceanodeInput.kr(\in), 1);
	delay = OceanodeParameter.kr(\delay, 252.5, 1, 5, 500, "f");      // delAy, ms
	time = OceanodeParameter.kr(\time, 0.55, 1, 0.1, 1, "f");         // tiMe: allpass decay, s
	rev = OceanodeParameter.kr(\rev, 5.05, 1, 0.1, 10, "f");          // reV: reverb time, s
	lpf = OceanodeParameter.kr(\lpf, 107.5, 1, 80, 135, "f");         // lPf, semitones
	drywet = OceanodeParameter.kr(\drywet, 0.5, 1, 0, 1, "f");
	rvon = OceanodeParameter.kr(\rvon, 1, 1, 0, 1, "b");
	level = OceanodeParameter.kr(\level, -12, 1, -60, 0, "f");         // leVel, dB
	diff = OceanodeParameter.kr(\diffusers, 0, 17, -1, 100, "vf");

	tms = time * 1000;
	// Amp/Mixer level: -60 dB x (delAy + tiMe/2) / reV
	fb = (-60 * (delay + (0.5 * tms)) / (rev * 1000)).dbamp;
	loop = LocalIn.ar(1);
	x = in + (loop * fb);
	// HP/LP 1-Pole (Euler, 0.99 cap)
	x = OnePole.ar(x, 1 - (lpf.midicps * 6.283 / SampleRate.ir).min(0.99));

	// Diffuser Delay: Dly = Delay + Fine ms (plus its one sample of state),
	// diffusion = polarity x 10^(-3 Dly / tiMe); AllpassC's feedback has the opposite sign
	ap = { |sig, i|
		var d = (diff[i] + diff[5 + i]).max(0), pol = diff[10 + i].sign.max(0) * 2 - 1;
		var secs = (d / 1000) + SampleDur.ir;
		var dec = Select.kr(d > 0, [1e9, secs * tms / d.max(1e-6)]);
		AllpassC.ar(sig, 0.21, secs, dec * pol.neg)
	};
	x = ap.(x, 0); x = ap.(x, 1); x = ap.(x, 2);
	l = ap.(x, 3);
	r = ap.(x, 4);

	// Slow Random: a stepped random value smoothed by a 1-pole low-pass at its rate
	rnd = OnePole.kr(LFNoise0.kr(400) * diff[15], exp(-2pi * diff[16].max(0.01) / ControlRate.ir));
	dl = Ramp.kr(delay + rnd, 0.1);
	LocalOut.ar(DelayL.ar(l, 1.0, ((dl / 1000) - ControlDur.ir).clip(0, 1.0)));

	wet = [in + ((l - in) * drywet), in + ((r - in) * drywet)];
	wet = Select.ar(rvon, [[in, in], wet]);

	// maesTro: level, 4-pole high-pass at 32 Hz, peak limiter (zero attack, 1/10 in 230 ms)
	wet = wet * level.dbamp;
	4.do { wet = wet - OnePole.ar(wet, exp(-2pi * 32 / SampleRate.ir)) };
	sel = Select.ar(wet[1].squared > wet[0].squared, [wet[0], wet[1]]);
	env = PeakFollower.ar(sel.abs, 10 ** (-1 / (0.23 * SampleRate.ir)));
	wet = wet / env.max(1);

	Out.ar(OceanodeOutput.kr(\out), wet);

}, description: "Reaktor t'wanGer reverberator and master: allpass reverb, level, high-pass and limiter (mono in, stereo out)", category: "Effect/Time/Reverb");
)
