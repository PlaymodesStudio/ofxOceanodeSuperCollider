// additiveBox.scd — SynthDefs for the AdditiveBox C++ node (src/nodes/scAdditiveBox.*)
//
// A polyphonic additive synth in the Harmor / Razor family: every partial of
// every voice has its own envelope, level, filter gain, sweep, vibrato, noise,
// shimmer, blur and place in space, with a modulation envelope and two LFOs
// routed to cutoff, morph, pitch, level, sweep, pan and noise.
//
// One synth per voice and unison copy, all summing (Out.ar) into a private
// bus, then (optionally EQ / Echo / Reverb, then) one output synth:
//
//   AdditiveBoxV<P>_<O>   one voice: P partials (8, 16, 32, 64, 128, 256 — the
//                         node picks the smallest cap >= Partials), panned on
//                         O outputs (1..16). Pauses itself once the gate is
//                         off and the voice is silent; the node resumes it
//                         (n_run 1) on the next gate.
//   AdditiveBoxOut<O>     bus -> volume -> \out
//
// Everything about a partial that only changes when a control changes is
// computed by the node in C++ and sent as arrays of P values (spectra A and
// B with tilt / even-odd / formant filter / mask / random level / harmonic
// blur / Nyquist fade, frequency ratios, envelope times, rates, depths,
// phases, positions, every random draw). Filter 1 is a response curve the
// node writes into a buffer (512 points over -128..+128 semitones from the
// cutoff): each partial reads its gain there, so the cutoff can move with
// the modulation and any curve shape costs the same. The synth runs only
// what moves in time; per partial that is ~27 control-rate UGens, the cost
// that dominates.
//
// Voice controls (scalars unless noted; the node sends each voice its own):
//   out vis                mix bus; display bus (2 * D channels, D = min(P, 64):
//                          the levels of D partials evenly spread over the P,
//                          then their positions)
//   gate trig              envelope gate; trig re-attacks without a gate off
//   pitch glide amp shift  MIDI note, portamento (s), level, Hz shift
//   fr[P] po[P]            frequency ratio of each partial, its offset in
//                          semitones from the voice pitch (for the filter)
//   pa[P] pd[P] morph      spectrum A and B - A; morph 0..1 (+ modulation)
//   maskglide              lag on pa / pd (s): spectrum changes glide
//   ta[P] td[P] tr[P]      attack / decay / release of each partial
//   sus curve              sustain level, envelope curve
//   vibdepth vibrate       voice vibrato (semitones, Hz)
//   vf[P] vd[P]            each partial's vibrato: rate (Hz), depth (ratio)
//   nr[P] nd[P]            each partial's noise: jitter rate (Hz), depth (ratio)
//   shimmer shr[P]         random level flicker of each partial (rate in Hz)
//   sbuf swamt swrate      sweep: a shape (one cycle, 257 points: the node
//   swphase swretrig       writes it into a buffer per voice) moving across
//   sph[P] sw[P]           the partials at swrate (Hz), depth swamt; swphase
//                          offsets it (0..1), swretrig restarts it on a gate;
//                          sph = each partial's place in the cycle (0..256),
//                          sw = each partial's share of the depth (the range)
//   fbuf cutoff keytrack   filter 1: curve buffer, cutoff (MIDI) + keytrack *
//   safe                   (pitch - 60); safe: the fundamental skips filter 1
//   bl[P]                  time blur: each partial's level lag (s)
//   pb[P] drift drr[P]     position 0..1 of each partial around the outputs
//   width                  (O >= 3: a ring, O = 2: L..R), random wander of
//                          depth drift at rate drr; PanAz width
//   matk mdec msus mrel mcurve   modulation envelope (gated with the voice)
//   l1shape l1rate l1retrig      LFO 1: 0 sine 1 triangle 2 saw 3 square 4 S&H,
//   l2shape l2rate l2retrig      Hz, restart on gate; LFO 2 the same
//   mm[21]                 amounts, [env, lfo1, lfo2] x [cutoff (x 48 st),
//                          morph, pitch (x 12 st), level, sweep, pan (x 0.5),
//                          noise]
//
// Output: <this folder>/CompiledSynthdefs/AdditiveBox/*.scsyndef, to be copied
// to the app's data/Supercollider/Synthdefs/Defaults/AdditiveBox/.

(
var d, caps = [8, 16, 32, 64, 128, 256], maxOut = 16, dispMax = 64, voiceDef, outDef;
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};
File.mkdir(d);
File.mkdir(d +/+ "AdditiveBox");

voiceDef = { |np, no|
    var nd = np.min(dispMax);
    SynthDef(("AdditiveBoxV" ++ np ++ "_" ++ no).asSymbol, {
        var lag = { |name, default| NamedControl.kr(name, default, 1/30, fixedLag: true) };
        var out = \out.kr(0), vis = \vis.kr(0);
        var gate = \gate.kr(0), trig = \trig.tr(0);
        var pitch = Lag.kr(lag.(\pitch, 48), \glide.kr(0));
        var amp = lag.(\amp, 0.5), shift = lag.(\shift, 0);
        var fr = \fr.kr((1..np)), po = \po.kr(0 ! np);
        var maskGlide = \maskglide.kr(0.05);
        var pa = \pa.kr(0 ! np), pd = \pd.kr(0 ! np), morph = lag.(\morph, 0);
        var ta = \ta.kr(0.01 ! np), td = \td.kr(0.5 ! np), tr = \tr.kr(2 ! np);
        var sus = \sus.kr(1), curve = \curve.kr(-4);
        var vibDepth = lag.(\vibdepth, 0), vibRate = lag.(\vibrate, 5);
        var vf = \vf.kr(1 ! np), vd = \vd.kr(0 ! np), nr = \nr.kr(200 ! np), ndp = \nd.kr(0 ! np);
        var shimmer = lag.(\shimmer, 0), shr = \shr.kr(4 ! np);
        var sbuf = \sbuf.kr(-1), swAmt = lag.(\swamt, 0), swRate = lag.(\swrate, 0.2);
        var swPhase = lag.(\swphase, 0), swRetrig = \swretrig.kr(0);
        var sph = \sph.kr(0 ! np), sw = \sw.kr(1 ! np);
        var fbuf = \fbuf.kr(-1), cutoff = lag.(\cutoff, 100), keytrack = lag.(\keytrack, 0), safe = \safe.kr(0);
        var bl = \bl.kr(0 ! np);
        var pb = lag.(\pb, 0.5 ! np), drift = lag.(\drift, 0), drr = \drr.kr(0.1 ! np);
        var width = lag.(\width, 2);
        var mm = \mm.kr(0 ! 21);
        var envGate, gateOn, env, menv, lfo, l1, l2, mod;
        var mCut, mMorph, mPitch, mLevel, mSweep, mPan, mNoise;
        var p, freqs, cut, fg, g0, spec, swA, swPh, lobe, aw, levels, pos, sigs, sig, step;

        // A trig closes the gate for one block: the envelopes re-attack from
        // where they are
        envGate = gate * (trig <= 0);
        gateOn = (HPZ1.kr(gate > 0) > 0) + (trig > 0);
        env = EnvGen.kr(Env.adsr(ta, td, sus, tr, 1, curve), envGate);

        // ── Modulation ───────────────────────────────────────────────────────
        menv = EnvGen.kr(Env.adsr(\matk.kr(0.01), \mdec.kr(0.5), \msus.kr(0), \mrel.kr(0.5), 1, \mcurve.kr(-4)), envGate);
        lfo = { |shape, rate, retrig|
            var ph = Phasor.kr(gateOn * retrig, rate.max(0) * ControlDur.ir, 0, 1, 0);
            Select.kr(shape.clip(0, 4), [
                sin(ph * 2pi),
                (1 - (ph * 2 - 1).abs) * 2 - 1,
                ph * 2 - 1,
                (ph < 0.5) * 2 - 1,
                Latch.kr(WhiteNoise.kr, (HPZ1.kr(ph) < -0.25) + Impulse.kr(0) + (gateOn * retrig))
            ])
        };
        l1 = lfo.(\l1shape.kr(0), lag.(\l1rate, 1), \l1retrig.kr(0));
        l2 = lfo.(\l2shape.kr(0), lag.(\l2rate, 1), \l2retrig.kr(0));
        mod = { |dest| (menv * mm[dest]) + (l1 * mm[7 + dest]) + (l2 * mm[14 + dest]) };
        mCut = mod.(0) * 48;
        mMorph = mod.(1);
        mPitch = mod.(2) * 12;
        mLevel = mod.(3);
        mSweep = mod.(4);
        mPan = mod.(5) * 0.5;
        mNoise = (1 + mod.(6)).max(0);

        // ── Frequencies: voice pitch (+ modulation, + vibrato), then each
        // partial's ratio, own vibrato and noise jitter ───────────────────────
        p = pitch + mPitch;
        freqs = (fr * (p + (SinOsc.kr(vibRate) * vibDepth)).midicps
            * SinOsc.kr(vf, { Rand(0, 2pi) } ! np, vd, LFNoise2.kr(nr, ndp * mNoise, 1))) + shift;

        // ── Filter 1: each partial's gain read from the curve at its distance
        // from the (modulated) cutoff ────────────────────────────────────────
        cut = cutoff + (keytrack * (pitch - 60)) + mCut;
        fg = IndexL.kr(fbuf, (po * (511 / 256)) + ((p - cut + 128) * (511 / 256)));
        g0 = fg[0];
        fg = [g0 + ((1 - g0) * safe)] ++ fg[1..];

        // ── Levels ───────────────────────────────────────────────────────────
        spec = Lag.kr(pa, maskGlide) + (Lag.kr(pd, maskGlide) * (morph + mMorph).clip(0, 1));
        // Sweep: each partial reads the shape at its place in the cycle plus
        // the moving phase; the depth is the range weight x the amount
        swA = (swAmt + mSweep).clip(0, 1);
        swPh = Phasor.kr(gateOn * swRetrig, swRate * ControlDur.ir * 256, 0, 256) + (swPhase * 256);
        lobe = IndexL.kr(sbuf, (sph + swPh) % 256);
        aw = sw * swA;
        levels = spec * env * fg * (lobe * aw + (1 - aw))
            * LFNoise2.kr(shr, shimmer * -0.5, 1 - (shimmer * 0.5));
        // Time blur: every level change smeared over bl (attacks a quarter of it)
        levels = LagUD.kr(levels, bl * 0.25, bl);

        // ── Space ────────────────────────────────────────────────────────────
        pos = LFNoise1.kr(drr, drift, pb) + mPan;
        if(no == 2) { pos = pos.fold(0, 1) };

        sigs = SinOsc.ar(freqs, { Rand(0, 2pi) } ! np);
        sig = case
            { no == 1 } { Mix(sigs * levels) }
            { no == 2 } { Mix(Pan2.ar(sigs, pos * 2 - 1, levels)) }
            { Mix(PanAz.ar(no, sigs, pos * 2, levels, width, 0)) };
        sig = sig * (amp * (1 + mLevel).max(0));
        Out.ar(out, sig);

        // Display: D partials' levels and positions
        step = np div: nd;
        Out.kr(vis, levels[0, step ..] ++ pos[0, step ..]);

        // Gate off and the voice silent for 0.1 s: pause until the node's next
        // gate (re-checked every 0.25 s, so a resume while silent pauses again)
        PauseSelf.kr((gate <= 0) * A2K.kr(DetectSilence.ar(Mix(sig), 1e-5, 0.1)) * LFPulse.kr(4));
    });
};

outDef = { |no|
    SynthDef(("AdditiveBoxOut" ++ no).asSymbol, {
        var sig = In.ar(\in.kr(0), no) * \volume.kr(1, 1/30, fixedLag: true);
        Out.ar(\out.kr(0), Sanitize.ar(sig));
    });
};

(1..maxOut).do { |no|
    outDef.(no).writeDefFile(d +/+ "AdditiveBox");
    caps.do { |np| voiceDef.(np, no).writeDefFile(d +/+ "AdditiveBox") };
};
"AdditiveBox: % voice defs, % output defs written to %".format(caps.size * maxOut, maxOut, d +/+ "AdditiveBox").postln;
)
