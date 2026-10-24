// Reference: AdditiveBox's voice before the lanes and cores were split into
// chunks (2026-10-21), for tools/additivebox/chunk_null_test.py. Test-only.
// additiveBox.scd — SynthDefs for the AdditiveBox C++ node (src/nodes/boxSynths/additiveBox/scAdditiveBox.*)
//
// A polyphonic additive synth in the Harmor / Razor family: every partial of
// every voice has its own envelope, level, filter gain, sweep, vibrato, noise,
// shimmer, blur and place in space. Modulation: the shared system
// (synthdefs/Defaults/Modulation: ModLfo / ModEnv / ModRoute, run by the
// node) writes one control bus per target, 16 channels, one per voice.
//
// Built from small synths, so a voice runs only the parts in use, and only
// the core depends on the output count. Each voice is a group (paused once
// its gate is off and it is silent; the node resumes it on the next gate):
//
//   AdditiveBoxSrc<P>      always: each partial's level in the spectrum (A..B
//                          by Morph) -> the level stream; the frequency ratios ->
//                          the frequency stream; the positions -> the
//                          position stream. Writes the gate (for the
//                          envelopes) and a random value per note.
//   AdditiveBoxMotion<P>   Partial Vib / Noise: frequency stream *= wobble
//   AdditiveBoxLane<P>     an LFO per partial (up to 2 lanes): level *, pan +,
//                          pitch (frequency stream *)
//   AdditiveBoxLaneN<P>    the same, with Noise: 2D Perlin noise per partial
//                          (x the LFO's rising counter, y = the partial's
//                          index x Noise Scale + the voice's seed), through
//                          the LFO's Gain / Pow / BiPow curve (\l<l>cbuf)
//   AdditiveBoxFilt<P>     filter 1: level *= the curve at each partial
//   AdditiveBoxSweep<P>    level *= the sweep shape moving across the partials
//   AdditiveBoxShim<P>     level *= a random flicker
//   AdditiveBoxBlur<P>     level smeared in time (LagUD)
//   AdditiveBoxDrift<P>    position += a random wander
//   AdditiveBoxV<P>_<O>    one per voice and unison copy: the sine bank at the
//                          voice's pitch (+ its copy's interval / detune) x the
//                          frequency stream, levels, panned on O outputs (+ the
//                          copy's pan offset) -> the mix bus. Unison copies
//                          share their voice's streams.
//   AdditiveBoxOut<O>      bus -> volume -> \out
//
// P = the partial cap (8 .. 256: the node picks the smallest >= Partials).
// The streams: one control bus block per voice, \bus: +0 levels (P), +P
// frequency ratios (P), +2P positions (P). The source writes them each
// block (ReplaceOut), the parts change them in place, in the order above.
//
// Everything about a partial that only changes when a control changes is
// computed by the node in C++ and sent as arrays (spectra A and B - A,
// ratios, envelope times, rates, depths, phases, positions, random draws).
// Filter 1 is a response curve in a buffer (512 points over -128..+128
// semitones from the cutoff); the sweep shape and the lane LFOs are cycles
// in buffers (257 points).
//
// Modulation (\modbus: target t of this voice at modbus + t * 16 + voice,
// already scaled): per voice 0 cutoff (st) 1 morph 2 pitch (st) 3 level
// 4 sweep 5 pan 6 noise; per partial (uniform here) 7 level 8 pan 9 pitch.
//
// Output: <this folder>/CompiledSynthdefs/AdditiveBox/*.scsyndef, to be copied
// to the app's data/Supercollider/Synthdefs/Defaults/AdditiveBox/.

(
var d, caps = [8, 16, 32, 64, 128, 256], maxOut = 16, dispMax = 64, dir;
var mod, lag, streams, part, perlin2;
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};
dir = d +/+ "AdditiveBox";
File.mkdir(d);
File.mkdir(dir);

// helpers (called inside a SynthDef function)
lag = { |name, default| NamedControl.kr(name, default, 1/30, fixedLag: true) };
mod = { |t| In.kr(\modbus.kr(-1).max(0) + (t * 16) + \voice.kr(0)) * (\modbus.kr(-1) >= 0) };
// [levels, ratios, positions] of this voice, each np channels
streams = { |np| var b = \bus.kr(0); [In.kr(b, np), In.kr(b + np, np), In.kr(b + (2 * np), np)] };
part = { |name, np, func| SynthDef((name ++ np).asSymbol, { func.(np) }).writeDefFile(dir) };
// 2D Perlin noise, 0..1 (as Modulation/modulation.scd and Oceanode's Noise node)
perlin2 = { |x, y, salt = 0|
    var ix = x.floor, iy = y.floor, fx = x - ix, fy = y - iy;
    var grad = { |cx, cy, dx, dy|
        var h = ((((cx % 4096) * 12.9898) + ((cy % 4096) * 78.233) + salt).sin * 43758.5453).frac * 2pi;   // salt: each LFO (voice) its own field
        (h.cos * dx) + (h.sin * dy)
    };
    var u = fx * fx * fx * (fx * (fx * 6 - 15) + 10), v = fy * fy * fy * (fy * (fy * 6 - 15) + 10);
    var n00 = grad.(ix, iy, fx, fy), n10 = grad.(ix + 1, iy, fx - 1, fy);
    var n01 = grad.(ix, iy + 1, fx, fy - 1), n11 = grad.(ix + 1, iy + 1, fx - 1, fy - 1);
    var a = n00 + ((n10 - n00) * u), b = n01 + ((n11 - n01) * u);
    ((a + ((b - a) * v)) * 1.4 + 0.5).clip(0, 1)
};

caps.do { |np|
    // ── The source: envelopes x spectrum, ratios, positions ─────────────────
    part.("AdditiveBoxSrc", np, { |np|
        var gate = \gate.kr(0), trig = \trig.tr(0);
        var maskGlide = \maskglide.kr(0.05);
        var pa = \pa.kr(0 ! np), pd = \pd.kr(0 ! np), morph = lag.(\morph, 0);
        // a trig closes the gate for one block: the envelopes re-attack from where they are
        var envGate = gate * (trig <= 0);
        var gateOn = (HPZ1.kr(gate > 0) > 0) + (trig > 0);
        var spec = Lag.kr(pa, maskGlide) + (Lag.kr(pd, maskGlide) * (morph + mod.(1)).clip(0, 1));
        var b = \bus.kr(0);
        ReplaceOut.kr(b, spec);
        ReplaceOut.kr(b + np, \fr.kr((1..np)));
        ReplaceOut.kr(b + (2 * np), lag.(\pb, 0.5 ! np));
        // the gate for the modulation envelopes, a random value per note
        Out.ar(\gatebus.kr(0) + \voice.kr(0), K2A.ar(envGate > 0));
        Out.kr(\rndbus.kr(0) + \voice.kr(0), TRand.kr(-1, 1, gateOn + Impulse.kr(0)));
        // the note's velocity: the gate's level (held from its rising edge)
        Out.kr(\velbus.kr(0), Gate.kr(gate, gate > 0));
    });

    // ── Partial Vib / Noise: each partial's own wobble and jitter ───────────
    part.("AdditiveBoxMotion", np, { |np|
        var s = streams.(np);
        var vf = \vf.kr(1 ! np), vd = \vd.kr(0 ! np), nr = \nr.kr(200 ! np), nd = \nd.kr(0 ! np);
        var w = SinOsc.kr(vf, { Rand(0, 2pi) } ! np, vd, LFNoise2.kr(nr, nd * (1 + mod.(6)).max(0), 1));
        ReplaceOut.kr(\bus.kr(0) + np, s[1] * w);
    });

    // ── LFO lanes: each partial reads the LFO's cycle at its own phase; the
    //    N variant also runs Noise (2D Perlin, a seed per partial) ──────────
    [false, true].do { |withNoise|
        part.(if(withNoise, "AdditiveBoxLaneN", "AdditiveBoxLane"), np, { |np|
            var s = streams.(np);
            var lane = 2.collect { |l|
                var k = { |name, def| NamedControl.kr(("l" ++ l ++ name).asSymbol, def) };
                var rst = Changed.kr(k.("reset", 0));
                var run = Phasor.kr(rst, k.("rate", 1) * ControlDur.ir, 0, 1);
                var x = IndexL.kr(k.("buf", -1), ((run + k.("ph", 0 ! np)) % 1) * 256);
                var sh, v, amt;
                if(withNoise) {
                    var counter = Sweep.kr(rst, k.("rate", 1)) + k.("init", 0);
                    // the seed: this LFO's and this voice's own noise field
                    var nz = perlin2.(counter, (0 .. (np - 1)) * k.("nscale", 1), k.("seed", 0));
                    x = Select.kr(k.("noise", 0) > 0.5, [x, IndexL.kr(k.("cbuf", -1), nz * 256)]);
                };
                // S&H (steps / s) with its glide (s)
                sh = k.("sh", 0);
                v = (Select.kr(sh > 0, [x, Lag.kr(Latch.kr(x, Impulse.kr(sh.max(0.01))), k.("shg", 0))]) * 2) - 1;
                amt = k.("amt", [0, 0, 0]);
                [v * amt[0], v * amt[1], v * amt[2]]
            }.sum;
            var b = \bus.kr(0);
            ReplaceOut.kr(b, s[0] * (1 + lane[0]).max(0));
            ReplaceOut.kr(b + np, s[1] * lane[2].midiratio);
            ReplaceOut.kr(b + (2 * np), s[2] + lane[1]);
        });
    };

    // ── Filter 1: each partial's gain on the curve, at its distance from the
    //    (keytracked, modulated) cutoff; Safe Bass keeps the fundamental ────
    part.("AdditiveBoxFilt", np, { |np|
        var s = streams.(np);
        var pitch = Lag.kr(lag.(\pitch, 48) + \pitchoff.kr(0), \glide.kr(0));   // pitchoff: with a cable, the transpose
        var p = pitch + mod.(2) + mod.(9);
        var cut = lag.(\cutoff, 100) + (lag.(\keytrack, 0) * (pitch - 60)) + mod.(0);
        var fg = IndexL.kr(\fbuf.kr(-1), (\po.kr(0 ! np) * (511 / 256)) + ((p - cut + 128) * (511 / 256)));
        var g0 = fg[0];
        fg = [g0 + ((1 - g0) * \safe.kr(0))] ++ fg[1..];
        ReplaceOut.kr(\bus.kr(0), s[0] * fg);
    });

    // ── Sweep: the shape (one cycle) moving across the partials ─────────────
    part.("AdditiveBoxSweep", np, { |np|
        var s = streams.(np);
        var gate = \gate.kr(0), trig = \trig.tr(0);
        var gateOn = (HPZ1.kr(gate > 0) > 0) + (trig > 0);
        var swA = (lag.(\swamt, 0) + mod.(4)).clip(0, 1);
        var swPh = Phasor.kr(gateOn * \swretrig.kr(0), lag.(\swrate, 0.2) * ControlDur.ir * 256, 0, 256)
                   + (lag.(\swphase, 0) * 256);
        var lobe = IndexL.kr(\sbuf.kr(-1), (\sph.kr(0 ! np) + swPh) % 256);
        var aw = \sw.kr(1 ! np) * swA;
        ReplaceOut.kr(\bus.kr(0), s[0] * ((lobe * aw) + (1 - aw)));
    });

    // ── Shimmer: a random flicker of each partial's level ───────────────────
    part.("AdditiveBoxShim", np, { |np|
        var s = streams.(np);
        var shimmer = lag.(\shimmer, 0);
        ReplaceOut.kr(\bus.kr(0), s[0] * LFNoise2.kr(\shr.kr(4 ! np), shimmer * -0.5, 1 - (shimmer * 0.5)));
    });

    // ── Time blur: every level change smeared (attacks a quarter of it) ─────
    part.("AdditiveBoxBlur", np, { |np|
        var s = streams.(np);
        var bl = \bl.kr(0 ! np);
        ReplaceOut.kr(\bus.kr(0), LagUD.kr(s[0], bl * 0.25, bl));
    });

    // ── Drift: each partial's position wanders ──────────────────────────────
    part.("AdditiveBoxDrift", np, { |np|
        var s = streams.(np);
        ReplaceOut.kr(\bus.kr(0) + (2 * np), s[2] + LFNoise1.kr(\drr.kr(0.1 ! np), lag.(\drift, 0)));
    });

    // ── The core: one per voice and unison copy, on O outputs ───────────────
    (1..maxOut).do { |no|
        var nd = np.min(dispMax);
        SynthDef(("AdditiveBoxV" ++ np ++ "_" ++ no).asSymbol, {
            var s = streams.(np);
            var pitch = Lag.kr(lag.(\pitch, 48) + \pitchoff.kr(0), \glide.kr(0));
            var glev = Gate.kr(\gate.kr(0), \gate.kr(0) > 0);   // the gate's level: a gate at 0.2 plays at 0.2
            var p = pitch + mod.(2) + mod.(9) + (SinOsc.kr(lag.(\vibrate, 5)) * lag.(\vibdepth, 0));
            var freqs = (s[1] * p.midicps) + lag.(\shift, 0);
            var levels = s[0] * (1 + mod.(7)).max(0);
            var pos = s[2] + lag.(\panoff, 0) + mod.(5) + mod.(8);
            var line = \line.kr(0), width = lag.(\width, 2), step = np div: nd;
            var sigs, sig;
            if(no == 2) { pos = pos.fold(0, 1) };
            if(no > 2) { pos = Select.kr(line, [pos, pos.fold(0, 1)]) };
            sigs = SinOsc.ar(freqs, { Rand(0, 2pi) } ! np);
            sig = case
                { no == 1 } { Mix(sigs * levels) }
                { no == 2 } { Mix(Pan2.ar(sigs, pos * 2 - 1, levels)) }
                { Mix(PanAz.ar(no, sigs, Select.kr(line, [pos * 2, pos * 2 * (no - 1) / no]), levels, width, 0)) };
            // Level: an envelope routed there is the voice's VCA (\vca 1: the
            // modulation alone sets the level); else it moves the level around 1
            sig = sig * (lag.(\amp, 0.5) * Select.kr(\vca.kr(0) > 0.5, [(1 + mod.(3)).max(0), mod.(3).max(0)])) * glev;
            Out.ar(\out.kr(0), sig);
            // display: D partials' levels and positions (copy 0; the others a junk bus)
            Out.kr(\vis.kr(0), levels[0, step ..] ++ pos[0, step ..]);
            // gate off and the voice silent for 0.1 s: pause the voice's group
            // until the node's next gate (re-checked 4 x / s, so a resume
            // while silent pauses again); copy 0 decides (\pauser 1)
            Pause.kr(1 - ((\gate.kr(0) <= 0) * A2K.kr(DetectSilence.ar(Mix(sig), 1e-5, 0.1))
                          * LFPulse.kr(4) * \pauser.kr(0)), \grp.kr(0));
        }).writeDefFile(dir);
    };
};

(1..maxOut).do { |no|
    SynthDef(("AdditiveBoxOut" ++ no).asSymbol, {
        var sig = In.ar(\in.kr(0), no) * \volume.kr(1, 1/30, fixedLag: true);
        Out.ar(\out.kr(0), Sanitize.ar(sig));
    }).writeDefFile(dir);
};
"AdditiveBox: % defs written to %".format(caps.size * (9 + maxOut) + maxOut, dir).postln;
)
