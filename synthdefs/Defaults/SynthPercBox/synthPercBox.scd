// SynthPercBox: the Reaktor ensemble Bits.ens (Santi Vilanova, 2008; saved again in Reaktor 6
// as Bits_r6.ens), six percussion generators, each recalled per hit from 16 pads.
// Decoded with tools/ReaktorPatchExtractor + tools/synthpercbox (specs: tools/synthpercbox/specs/*.md).
// Evaluate this file; needs OceanodeAnalog (ReaktorSVF4: Reaktor's Multi 2-Pole, its own tables),
// OceanodePrimitives (EventADSRKernel: Reaktor's ADSR) and the Oceanode class OceanodeMicroSpace.
//
//   SPIn             the node's note input: Gate (8) and Pitch (8) from controls (\gates / \pitches,
//                    audio-rate controls: a value lands on its block's first sample) or from audio
//                    cables (\gate_ar / \pitch_ar mapped, \gate_sel / \pitch_sel 1). -> \out (16: 8 gates, 8 pitches)
//   SPK1 SPStomps SPNoix SPClix SPCrax SPQuad
//                    one generator each, always running (one voice, like Reaktor's). Reads \gin (SPIn's bus):
//                    \inmode 0 Kit: Gate channel \ch is this generator, its Pitch picks the pad (60 = pad 1);
//                    \inmode 1 Notes: any channel whose note is in \lo .. \lo + 13 plays pad note - \lo.
//                    A pad click: \cgate (held level) + \cpad. On the gate's rising edge the pad is latched;
//                    every sound control is an array of 17 (\p_<key>: 16 pads + 1 live row, the row the
//                    window edits after \t_live) read at that row. The voice's trigger is held back
//                    TWO blocks so the pad's values (control rate) are settled on its first sample.
//                    \rowbus (kr) gets the row playing (the window shows it). Gate level = velocity.
//   SPSpace1 / SPSpace2
//                    the Micro Space of CLIX / NOIX (mono in) and QUAD (stereo in): OceanodeMicroSpace
//                    (it needs its own synth: its loops use LocalIn) + the generator's Level / Pan.
//                    Its settings come from the generator's pad row through \pbus (kr, 9).
//
// Decisions where the file does not say (one place each; see the specs' open questions):
//   WET (Delay, MicroDel)   linear crossfade (Scanner)          ~wetLaw
//   Toggles stored 0.5      on = 1                              (pads store 0 / 1)
//   Selector/Scanner        Scanner (crossfade) for STOMPS Wave, NOIX Wave, CLIX Co..Ca, QUAD Co..Ca
//   Oscillators (STOMPS, CRAX)  one phase, polyBLEP: frozen while F <= 0, as Reaktor's
//   Micro Space             Bits' cell is one side of NI's stereo Space Master; here the full stereo
//                           OceanodeMicroSpace (cross-coupled tank, its long tail)
//   Stereo Amp pan law      L = min(1, 1 - pan), R = min(1, 1 + pan) (centre: unity both sides)
(
var here = thisProcess.nowExecutingPath.dirname;
var root = here +/+ "CompiledSynthdefs", dir;
var rows = 17, live = 16;
var ms = { |x| 10 ** (x / 20) };                                   // Reaktor log time -> ms
var svf = { |in, p, res, type|                                     // Multi 2-Pole: 0 HP 1 BP 2 LP
	ReaktorSVF4.ar(in, p.clip(-40, 140).midicps.clip(1, SampleRate.ir * 0.45), res.clip(0, 1), type.round.clip(0, 2)) };   // < 0.45 SR: the Chamberlin tables are marginal at Nyquist
var adsr = { |trig, gate, a, d, s, r, peak = 1|                    // Reaktor ADSR (log times)
	EventADSRKernel.ar(trig, gate, ms.(a), ms.(d), s, ms.(r), peak) };
var decayEnv = { |trig, amp, d|                                    // Reaktor D: jump to amp, decay (time constant)
	EventADSRKernel.ar(trig, 1, 0.0001, ms.(d), 0, ms.(d), 1) * amp };
var pass = { |sig, drive| sig / (sig.abs + drive.squared) };      // PASS (DRIVE 2 .. 0.1)
var wetLaw = { |dry, wet, w| (dry * (1 - w)) + (wet * w) };
// NI's MultiWave Osc: one phase in [-1, 1) (frozen while F <= 0: the output holds), polyBLEP-corrected
// saw and pulse (duty (1 + pw) / 2), triangle and sine from the same phase. Returns [saw, pulse, tri, sine].
var multiWave = { |f, pw|
	var dt = (f * SampleDur.ir).clip(0, 0.5);                        // F / SR, clipped (Nyquist)
	var ph = Phasor.ar(0, dt * 2, -1, 1, -1);
	var t = (ph + 1) * 0.5;                                          // 0..1
	var blep = { |x|                                                 // 2-sample polyBLEP at the wrap of a 0..1 phase
		var d = dt.max(1e-9), u0 = x / d, u1 = (x - 1) / d;
		((x < d) * ((2 * u0) - u0.squared - 1)) + ((x > (1 - d)) * (u1.squared + (2 * u1) + 1)) };
	var duty = ((pw.clip(-1, 1) + 1) * 0.5).clip(0.01, 0.99);
	var saw = ph - blep.(t);
	var t2 = (t + 1 - duty).wrap(0, 1);
	var pulse = Select.ar(t < duty, [DC.ar(-1), DC.ar(1)]) + blep.(t) - blep.(t2);
	var tri = 1 - (2 * ph.abs);
	var sine = sin(pi * ph);
	[saw, pulse, tri, sine]
};
var stereoAmp = { |sig, level, pan| [sig * (1 - pan).min(1), sig * (1 + pan).min(1)] * level.dbamp };
// Delay (tempo 16ths + fine ms), Multi 2-Pole in the loop; fbIn: this loop's LocalIn channel.
// Returns [out, toLocalOut]. variant 0: wet = the delay; 1: wet = in + delay (CRAX). on: input into the loop.
var delay = { |sig, fbIn, bps, st, fine, feed, pc, res, type, w, on = 1, variant = 0|
	var t = ((st * 0.25 / bps.max(0.1)) + (fine * 0.001)).clip(ControlDur.ir + SampleDur.ir, 4.4);
	var lineIn = (sig * on) + fbIn;
	var dly = DelayL.ar(lineIn, 4.5, t);
	var early = DelayL.ar(lineIn, 4.5, t - ControlDur.ir);              // LocalIn is one block late
	var back = (svf.(early, pc, res, type) * feed).clip2(8);
	[wetLaw.(sig, Select.ar(variant, [dly, sig + dly]), w), Sanitize.ar(back)]
};
// MicroDel: fine ms only, wet = in + delay
var microDel = { |sig, fbIn, fine, feed, pc, res, type, w|
	var t = (fine * 0.001).max(ControlDur.ir + SampleDur.ir);
	var lineIn = sig + fbIn;
	var dly = DelayL.ar(lineIn, 0.05, t);
	var early = DelayL.ar(lineIn, 0.05, t - ControlDur.ir);
	var back = (svf.(early, pc, res, type) * feed).clip2(8);
	[wetLaw.(sig, sig + dly, w), Sanitize.ar(back)]
};

// The common front of every generator: gate, pad, row, velocity. Returns an Event.
var front = { |gen, defaults|
	var all = In.ar(\gin.kr(0), 16), gates = all[0..7], pitches = all[8..15];
	var mode = \inmode.kr(0), ch = \ch.kr(gen), lo = \lo.kr(0);
	var kg = Select.ar(ch.clip(0, 7), gates), kp = Select.ar(ch.clip(0, 7), pitches);
	var inRange = pitches.collect { |p| (p > (lo - 0.5)) * (p < (lo + 13.5)) };
	var on = gates.collect { |g, i| (g > 0) * inRange[i] };
	var ng = gates.collect { |g, i| g * inRange[i] }.reduce(\max);
	var npad = (on.collect { |o, i| o * (pitches[i] - lo) }.sum / on.sum.max(1));
	var g = Select.ar(mode.clip(0, 1), [kg, ng]);
	var pad = Select.ar(mode.clip(0, 1), [kp - 60, npad]).round.clip(0, 15);
	var cg = \cgate.ar(0), clicked = cg > 0;
	var gate = Select.ar(clicked, [g, cg]);
	var opened, edge, latched, vel, edgeK, liveFF, row, e = ();
	pad = Select.ar(clicked, [pad, \cpad.ar(0)]);
	opened = gate > 0;
	edge = opened > Delay1.ar(opened);
	latched = Latch.ar(pad, edge);
	vel = Latch.ar(gate, edge);
	edgeK = A2K.kr(Trig1.ar(edge, 3 * ControlDur.ir));
	liveFF = SetResetFF.kr(\t_live.tr(0), edgeK);                 // an edit plays the live row until the next hit
	row = Select.kr(liveFF, [A2K.kr(latched), live]);
	Out.kr(\rowbus.kr(0), row);
	e.row = row;
	e.p = { |key| Select.kr(row, NamedControl.kr(("p_" ++ key).asSymbol, defaults[key] ! rows)) };
	// held back two blocks: an edge inside block k gives the new row in block k + 1, where
	// control values ramp (audio x control interpolates); settled from block k + 2
	e.gate = DelayN.ar(gate, 0.05, 2 * ControlDur.ir);
	e.edge = DelayN.ar(edge, 0.05, 2 * ControlDur.ir);
	e.vel = DelayN.ar(vel, 0.05, 2 * ControlDur.ir);
	e.bps = \bps.kr(2);
	e.gain = \gain.kr(1) * (1 - \mute.kr(0));
	e
};

dir = root +/+ "SynthPercBox"; File.mkdir(root); File.mkdir(dir);

SynthDef(\SPIn, {
	var gates = Select.ar(\gate_sel.kr(0), [\gates.ar(0 ! 8), \gate_ar.ar(0 ! 8)]);
	var pitches = Select.ar(\pitch_sel.kr(0), [\pitches.ar(60 ! 8), \pitch_ar.ar(60 ! 8)]);
	// a cable with fewer channels: the rest stay closed / at 60
	var gn = \gate_n.kr(8), pn = \pitch_n.kr(8);
	gates = gates.collect { |x, i| x * (i < gn) };
	pitches = pitches.collect { |x, i| Select.ar(i < pn, [DC.ar(60), x]) };
	Out.ar(\out.kr(0), gates ++ pitches);
}).writeDefFile(dir);

// ── K1: phase-reset sine kick, two pitch decays, dB/s amp decay, PASS, filter, delay ─────────────
SynthDef(\SPK1, {
	var d = (bp: 33, p1d: 15, p1a: 34.68, p2d: 36, p2a: 68, p1on: 1, p2on: 0, bdec: 24.96, vol: 1, drive: 2,
		ftype: 2, cut: 120, res: 0, env: 0, fa: 47, fd: 7, fs: 0.48, fr: 69,
		don: 0, dst: 3, dfine: 0, dfeed: 0.6, dpc: 88, dres: 0.64, dtype: 2, dwet: 0, level: -20.6, pan: 0);
	var e = front.(0, d), p = e[\p], t = e.edge, fb = LocalIn.ar(1);
	var pitch = p.(\bp) + (p.(\p1on) * decayEnv.(t, p.(\p1a), p.(\p1d))) + (p.(\p2on) * decayEnv.(t, p.(\p2a), p.(\p2d)));
	var phase = Phasor.ar(t, pitch.midicps * SampleDur.ir, 0, 1);
	var amp = p.(\vol) * (p.(\bdec).neg * Sweep.ar(t, 1)).dbamp * SetResetFF.ar(t, 0);
	var sig = pass.(sin(2pi * phase) * amp * e.vel, p.(\drive));
	var fenv = A2K.kr(adsr.(t, e.gate, p.(\fa), p.(\fd), p.(\fs), p.(\fr)));
	var dl;
	sig = svf.(sig, p.(\cut) + (p.(\env) * fenv), p.(\res), p.(\ftype));
	dl = delay.(sig, fb, e.bps, p.(\dst), p.(\dfine), p.(\dfeed), p.(\dpc), p.(\dres), p.(\dtype), p.(\dwet), p.(\don), 0);
	LocalOut.ar(dl[1]);
	Out.ar(\out.kr(0), stereoAmp.(dl[0], p.(\level), p.(\pan)) * e.gain);
}).writeDefFile(dir);

// ── STOMPS: free-running saw / pulse / tri swept in Hz, AD amp (cut at gate off), PASS, filter, delay ──
SynthDef(\SPStomps, {
	var d = (bf: 33, hf: 2000, p1on: 1, p1d: 17, p2on: 1, p2d: 0, wave: 1.5, pw: 0, aa: 0, ad: 45, ar: 1, drive: 0.5039,
		ftype: 2, cut: 120, res: 0, env: 0, fa: 35, fd: 7, fs: 1, fr: 36,
		don: 0, dst: 0, dfine: 3.543, dfeed: 0.9, dpc: 70, dres: 0, dtype: 1, dwet: 0.425, level: -29, pan: 0.02);
	var e = front.(1, d), p = e[\p], t = e.edge, fb = LocalIn.ar(1);
	var f = (p.(\bf) + (p.(\hf) * ((p.(\p1on) * decayEnv.(t, 1, p.(\p1d))) + (p.(\p2on) * decayEnv.(t, 1, p.(\p2d))))))
		.clip(0, SampleRate.ir * 0.5);
	var a = adsr.(t, e.gate, p.(\aa), p.(\ad), 0, p.(\ar), 1);
	var osc = multiWave.(f, p.(\pw));
	var sig = SelectX.ar(p.(\wave).clip(0, 2), osc[0..2]) * a * e.vel;   // its "sin" port carries the triangle
	var fenv = A2K.kr(adsr.(t, e.gate, p.(\fa), p.(\fd), p.(\fs), p.(\fr)));
	var dl;
	sig = pass.(sig, p.(\drive));
	sig = svf.(sig, p.(\cut) + (p.(\env) * fenv), p.(\res), p.(\ftype));
	dl = delay.(sig, fb, e.bps, p.(\dst), p.(\dfine), p.(\dfeed), p.(\dpc), p.(\dres), p.(\dtype), p.(\dwet), p.(\don), 0);
	LocalOut.ar(dl[1]);
	Out.ar(\out.kr(0), stereoAmp.(dl[0], p.(\level), p.(\pan)) * e.gain);
}).writeDefFile(dir);

// The Micro Space settings + Level / Pan, for SPSpace1/2 (\pbus, 9)
~spSpaceKeys = [\sz, \smod, \slo, \shi, \spos, \smix, \son, \level, \pan];

// ── NOIX: noise with an AD envelope, 2-pole filter + slow envelope, MicroDel, Delay -> Space ──
SynthDef(\SPNoix, {
	var d = (wave: 3, aa: 0, ad: 46, ar: 45, ftype: 2, cut: 94, res: 0.81, env: 120, fa: 100, fd: 100, fs: 0.82, fr: 59,
		mfine: 17.95, mfeed: 0.6, mpc: 45, mres: 0.3, mtype: 0, mwet: 0.7,
		dst: 2, dfine: 0, dfeed: 0.8, dpc: 78, dres: 0.13, dtype: 0, dwet: 0,
		sz: 73, smod: 1, slo: 0, shi: 0, spos: 1, smix: 0.29, son: 0, level: -35, pan: -0.02);
	var e = front.(2, d), p = e[\p], t = e.edge, fb = LocalIn.ar(2);
	var a = adsr.(t, e.gate, p.(\aa), p.(\ad), 0, p.(\ar), 1);
	var x = (p.(\wave) - 1).clip(0, 1);
	var sig = (((WhiteNoise.ar > 0) - 0.5) * (1 - x) + (WhiteNoise.ar * x)) * a * e.vel;
	var fenv = A2K.kr(adsr.(t, e.gate, p.(\fa), p.(\fd), p.(\fs), p.(\fr)));
	var md, dl;
	sig = svf.(sig, p.(\cut) + (p.(\env) * fenv), p.(\res), p.(\ftype));
	md = microDel.(sig, fb[0], p.(\mfine), p.(\mfeed), p.(\mpc), p.(\mres), p.(\mtype), p.(\mwet));
	dl = delay.(md[0], fb[1], e.bps, p.(\dst), p.(\dfine), p.(\dfeed), p.(\dpc), p.(\dres), p.(\dtype), p.(\dwet), 1, 0);
	LocalOut.ar([md[1], dl[1]]);
	Out.kr(\pbus.kr(0), ~spSpaceKeys.collect { |k| p.(k) });
	Out.ar(\out.kr(0), dl[0]);
}).writeDefFile(dir);

// ── CLIX: a one-sample impulse into a resonant multimode filter at a random pitch Co..Ca ───────
SynthDef(\SPClix, {
	var d = (res: 0.99, co: 96.15, ca: 50, mode: 2, clev: -10,
		mfine: 9.21, mfeed: 0.6, mpc: 86, mres: 0, mtype: 2, mwet: 0.613,
		dst: 2.875, dfine: 9.45, dfeed: 0.9, dpc: 79, dres: 0.43, dtype: 2, dwet: 0,
		sz: 73, smod: 0, slo: 0, shi: -5.25, spos: 1, smix: 0.43, son: 1, level: -20, pan: 0.02);
	var e = front.(3, d), p = e[\p], t = e.edge, fb = LocalIn.ar(2);
	var imp = t * e.vel;                                           // Core "Impulse": one sample per hit
	var r = TRand.ar(0, 1, t);                                     // Core "Random": one draw per hit
	var pitch = Latch.ar(p.(\co) + (r * (p.(\ca) - p.(\co))), t);
	var hz = pitch.midicps.clip(1, SampleRate.ir * 0.45), res = p.(\res).clip(0, 1);
	var hp2 = ReaktorSVF4.ar(imp, hz, res, 0), bp2 = ReaktorSVF4.ar(imp, hz, res, 1), lp2 = ReaktorSVF4.ar(imp, hz, res, 2);
	// Multi/HP 4-Pole's 4-pole taps (GUESS: a second identical stage; the presets use LP2 only)
	var sig = Select.ar(p.(\mode).round.clip(0, 5), [hp2, bp2, lp2,
		ReaktorSVF4.ar(hp2, hz, res, 0), ReaktorSVF4.ar(bp2, hz, res, 0), ReaktorSVF4.ar(bp2, hz, res, 1)]);
	var md, dl;
	sig = sig * p.(\clev).dbamp;
	md = microDel.(sig, fb[0], p.(\mfine), p.(\mfeed), p.(\mpc), p.(\mres), p.(\mtype), p.(\mwet));
	dl = delay.(md[0], fb[1], e.bps, p.(\dst), p.(\dfine), p.(\dfeed), p.(\dpc), p.(\dres), p.(\dtype), p.(\dwet), 1, 0);
	LocalOut.ar([md[1], dl[1]]);
	Out.kr(\pbus.kr(0), ~spSpaceKeys.collect { |k| p.(k) });
	Out.ar(\out.kr(0), dl[0]);
}).writeDefFile(dir);

// ── CRAX: multiwave osc in linear Hz + tempo LFO + random jitter; sounds while the gate is held ──
SynthDef(\SPCrax, {
	var d = (owave: 1, p: 150, pw: -0.86, plus: -2600, rrate: 10000, rnd: 0.76, nrnd: 0,
		lwave: 0, bpmr: 0, width: -0.02, lamp: 1,
		aa: -20, ad: 100, as: 0.06, ar: 30, ftype: 2, cut: 120, res: 0.52, fenv: 120, fa: -20, fd: 100, fs: 0.22, fr: 51,
		level: -40.53, dst: 2, dfine: 0, dfeed: 0.7, dpc: 92, dres: 0, dtype: 0, dwet: 0);
	var e = front.(4, d), p = e[\p], fb = LocalIn.ar(1);
	var opened = e.gate > 0;
	var ev = Changed.ar(opened) + e.edge;                         // every gate event, its release too
	var useR = p.(\nrnd) < 0.5;                                // list: 0 NoteRnd, 1 Manual
	var rnd = Select.ar(useR, [K2A.ar(p.(\rnd)), TRand.ar(0, 1, ev)]);
	var clk = Select.ar(useR, [K2A.ar(p.(\rrate)), TRand.ar(0, 1, ev) * 44100]);
	var pwv = Select.ar(useR, [K2A.ar(p.(\pw)), TRand.ar(-1, 1, ev)]).clip(-1, 1);
	// MultiWave LFO, tempo-synced (BPMRate cycles per beat), reset by every event: phase -0.5
	var ph = Phasor.ar(ev, e.bps * p.(\bpmr) * SampleDur.ir, 0, 1, 0);
	var q = ph - 0.5;
	var wrapped = (HPZ1.ar(ph) < 0) + ev;
	var lfo = Select.ar(p.(\lwave).round.clip(0, 5), [
		2 * q, -2 * q,
		(q >= (p.(\width) * 0.5)) * -2 + 1,
		1 - (4 * q.abs),
		cos(2pi * q),
		TRand.ar(-1, 1, wrapped)]);
	var f = (p.(\plus) + (p.(\p) * (1 + (p.(\lamp) * lfo))) + (rnd * 5000 * LFNoise0.ar(clk)))
		.clip(0, SampleRate.ir * 0.5);
	var a = adsr.(e.edge, e.gate, p.(\aa), p.(\ad), p.(\as), p.(\ar), 1);
	var fe = A2K.kr(adsr.(e.edge, e.gate, p.(\fa), p.(\fd), p.(\fs), p.(\fr)));
	var sig = Select.ar(p.(\owave).round.clip(0, 3), multiWave.(f, pwv)) * a * e.vel;
	var dl;
	sig = svf.(sig, p.(\cut) + (p.(\fenv) * fe), p.(\res), p.(\ftype));
	sig = sig - OnePole.ar(sig, exp(-2pi * 8.66 * SampleDur.ir));   // HP 1-Pole at P 1: DC blocker
	sig = sig * p.(\level).dbamp;
	dl = delay.(sig, fb, e.bps, p.(\dst), p.(\dfine), p.(\dfeed), p.(\dpc), p.(\dres), p.(\dtype), p.(\dwet), 1, 1);
	LocalOut.ar(dl[1]);
	Out.ar(\out.kr(0), (dl[0] ! 2) * e.gain);
}).writeDefFile(dir);

// ── QUAD: the quadratic map x' = r (0.25 - x^2), one step per sample at 44.1 kHz, L / R, AD^2 ──
SynthDef(\SPQuad, {
	var d = (off: 0.4567, level: -34, qa: 25.69, qd: 13, qr: 42.48, cvar: -0.38, csrc: 1, co: -0.5333, ca: 0.5407,
		beatl: 16, shape: 1, sz: 65, smod: 1, slo: 0, shi: 0, spos: 1, smix: 0.38, son: 1, pan: 0);
	var e = front.(5, d), p = e[\p];
	var riseK = A2K.kr(Trig1.ar(e.edge, 2 * ControlDur.ir));
	var rate = e.bps / p.(\beatl).max(0.5);
	var ramp = Phasor.kr(riseK, rate.neg * ControlDur.ir, 0, 1, 0);
	var sine = sin(2pi * Phasor.kr(riseK, rate.neg * ControlDur.ir, 0, 1, 0)) * 0.5 + 0.5;
	var rnd = TRand.kr(0, 1, Impulse.kr(rate * 2) + riseK);
	var x = Select.kr(p.(\shape).round.clip(0, 2), [ramp, sine, rnd]);
	var sel = Select.kr(p.(\csrc).round.clip(0, 2), [p.(\cvar), TRand.kr(-1, 1, riseK), p.(\co) + (x * (p.(\ca) - p.(\co)))]);
	var c = Lag.kr(0.9221 + (0.0779 * sel), 0.002);
	var rl = 1 + (1.55 * c) + (0.01 * Lag.kr(p.(\off), 0.002)), rr = 1 + (1.55 * c);
	var xs = [QuadN.ar(44100, rl.neg, 0, rl * 0.25, 0), QuadN.ar(44100, rr.neg, 0, rr * 0.25, 0)];
	var env = adsr.(e.edge, e.gate, p.(\qa), p.(\qd), 0, p.(\qr), 1);
	Out.kr(\pbus.kr(0), ~spSpaceKeys.collect { |k| p.(k) });
	Out.ar(\out.kr(0), Sanitize.ar(xs) * env.squared * e.vel * p.(\level).dbamp);
}).writeDefFile(dir);

// ── the Micro Space (own synth: OceanodeMicroSpace uses LocalIn), then Level / Pan ───────────
[1, 2].do { |n|
	SynthDef(("SPSpace" ++ n).asSymbol, {
		var v = In.kr(\pbus.kr(0), 9);   // sz smod slo shi spos smix son level pan
		var dry = In.ar(\in.kr(0), n).asArray;
		var st = if(n == 1) { [dry[0], dry[0]] } { dry };
		var wet = OceanodeMicroSpace.ar(st, v[0], v[1], v[2], v[3], v[4], v[5]);
		var on = Lag.kr(v[6] > 0.25, 0.01);
		var sig = (st * (1 - on)) + (wet * on);
		// mono generators: the Stereo Amp after the reverb (Level, Pan); QUAD: Level is before it
		if(n == 1) { sig = [sig[0] * (1 - v[8]).min(1), sig[1] * (1 + v[8]).min(1)] * v[7].dbamp };
		Out.ar(\out.kr(0), Sanitize.ar(sig) * \gain.kr(1));
	}).writeDefFile(dir);
};
"SynthPercBox SynthDefs written to ".post; dir.postln;
)
