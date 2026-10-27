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
//                          position stream; the vibrato.
//   AdditiveBoxNote        one stable writer per note voice, shared across
//                          partial-cap banks: gate, velocity and random value.
//   AdditiveBoxMotion<P>   Partial Vib / Noise: frequency stream *= wobble
//   AdditiveBoxLaneClock   with the lanes: each LFO's run, noise counter and
//                          S&H trigger, shared by the lane chunks
//   AdditiveBoxLanes<C>    an LFO per partial (up to 2 lanes): level *, pan +,
//                          pitch (frequency stream *), on C partials from \first
//   AdditiveBoxLanesN<C>   the same, with Noise: Smooth 2D value noise per partial
//                          (x the LFO's rising counter, y = the partial's
//                          index x Noise Scale + the voice's seed), through
//                          the LFO's Gain / Pow / BiPow curve (\l<l>cbuf)
//   AdditiveBoxFilt<P>     filter 1: level *= the curve at each partial
//   AdditiveBoxSweep<P>    level *= the sweep shape moving across the partials
//   AdditiveBoxShim<P>     level *= a random flicker
//   AdditiveBoxBlur<P>     level smeared in time (LagUD)
//   AdditiveBoxDrift<P>    position += a random wander
//   AdditiveBoxCore<C>_<O>    one per voice, unison copy and chunk of C partials
//                          (from \first): the sine bank at the voice's pitch
//                          (+ its copy's interval / detune) x the frequency
//                          stream, levels, panned on O outputs (+ the copy's pan
//                          offset) -> the mix bus. Unison copies share their
//                          voice's streams.
//   AdditiveBoxTail<P>     last: the display (levels and positions, raw) and
//                          the voice's pause once its gate is off and it is silent
//   AdditiveBoxOut<O>      bus -> volume -> \out
//
// P = the partial cap (8 .. 256: the node picks the smallest >= Partials);
// C = a chunk, min(P, 64), min(P, 32) for LanesN: P / C lane and core synths
// per voice (and copy). Chunks keep every SynthDef small: sclang's build time
// grows much faster than a def's size, and the full 256 partials in one def
// took minutes (LaneN256 alone 200 s; 64 noise partials still 7 s, so LanesN stops at 32).
// Up to 64 partials a voice runs one core per copy, as it always did.
// Nothing that moves on its own is inside a chunk (the lanes' LFOs are the
// clock's, the vibrato the source's), so the chunks of a voice stay in step
// whenever they start.
//
// The streams: one control bus block per voice, \bus: +0 levels (P), +P
// frequency ratios (P), +2P positions (P), +3P the silence sum (the cores of
// copy 0: 1 each while sounding), +3P+1 the vibrato's sine and +3P+2 its
// depth (st; the cores multiply them, as one synth did), +3P+3 the lanes'
// clock (lane l: +3+3l run 0..1, +4+3l noise counter, +5+3l S&H trigger).
// The source writes the first three each block (ReplaceOut), the parts change
// them in place, in the order above.
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
var d, caps = [8, 16, 32, 64, 128, 256], chunks = [8, 16, 32, 64], noiseChunk = 32, maxOut = 16, dispMax = 64, dir;
var mod, lag, streams, chunk, part, valueNoise2;
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
// a chunk: the stream bus offsets of nc partials from \first, in a voice of \np
chunk = { |nc| var b = \bus.kr(0), np = \np.kr(8), f = \first.kr(0); [b + f, b + np + f, b + (2 * np) + f] };
part = { |name, np, func| SynthDef((name ++ np).asSymbol, { func.(np) }).writeDefFile(dir) };
// Smooth 2D value noise, 0..1 (as Modulation/modulation.scd)
valueNoise2 = { |x, y, salt = 0|
    // Stagger knots between voices; random values have no forced 0.5 crossing.
    var nx = x + 0.371 - (y * 0.754877666), ny = y + 0.619;
    var ix = nx.floor, iy = ny.floor, fx = nx - ix, fy = ny - iy;
    var value = { |cx, cy|
        ((((cx % 4096) * 12.9898) + ((cy % 4096) * 78.233) + salt).sin * 43758.5453).frac
    };
    var u = fx * fx * fx * (fx * (fx * 6 - 15) + 10), v = fy * fy * fy * (fy * (fy * 6 - 15) + 10);
    var n00 = value.(ix, iy), n10 = value.(ix + 1, iy);
    var n01 = value.(ix, iy + 1), n11 = value.(ix + 1, iy + 1);
    var a = n00 + ((n10 - n00) * u), b = n01 + ((n11 - n01) * u);
    (a + ((b - a) * v)).clip(0, 1)
};

// Note state belongs to a voice, not to its temporary partial-cap graph.
// The same running writer supplies both rendering banks during a cap morph.
SynthDef(\AdditiveBoxNote, {
    var gate=\gate.kr(0),trig=\trig.tr(0);
    var envGate=gate*(trig<=0);
    var gateOn=(HPZ1.kr(gate>0)>0)+(trig>0);
    Out.ar(\gatebus.kr(0)+\voice.kr(0),K2A.ar(envGate>0));
    Out.kr(\rndbus.kr(0)+\voice.kr(0),TRand.kr(-1,1,gateOn+Impulse.kr(0)));
    Out.kr(\velbus.kr(0),Gate.kr(gate,gate>0));
}).writeDefFile(dir);

caps.do { |np|
    // ── The source: envelopes x spectrum, ratios, positions, the vibrato ────
    part.("AdditiveBoxSrc", np, { |np|
        var maskGlide = \maskglide.kr(0.05);
        var pa = \pa.kr(0 ! np), pd = \pd.kr(0 ! np), morph = lag.(\morph, 0);
        var spec = Lag.kr(pa, maskGlide) + (Lag.kr(pd, maskGlide) * (morph + mod.(1)).clip(0, 1));
        var b = \bus.kr(0);
        ReplaceOut.kr(b, spec);
        ReplaceOut.kr(b + np, \fr.kr((1..np)));
        ReplaceOut.kr(b + (2 * np), lag.(\pb, 0.5 ! np));
        // the voice's vibrato (st), for every core of every copy
        ReplaceOut.kr(b + (3 * np) + 1, [SinOsc.kr(lag.(\vibrate, 5)), lag.(\vibdepth, 0)]);
    });

    // ── Partial Vib / Noise: each partial's own wobble and jitter ───────────
    part.("AdditiveBoxMotion", np, { |np|
        var s = streams.(np);
        var vf = \vf.kr(1 ! np), vd = \vd.kr(0 ! np), nr = \nr.kr(200 ! np), nd = \nd.kr(0 ! np);
        var w = SinOsc.kr(vf, { Rand(0, 2pi) } ! np, vd, LFNoise2.kr(nr, nd * (1 + mod.(6)).max(0), 1));
        ReplaceOut.kr(\bus.kr(0) + np, s[1] * w);
    });

    // ── Filter 1: each partial's gain on the curve, at its distance from the
    //    (keytracked, modulated) cutoff; Safe Bass keeps the fundamental ────
    part.("AdditiveBoxFilt", np, { |np|
        var s = streams.(np);
        var gb = \gtrbus.kr(-1), gg = In.kr(gb.max(0), 15) * (gb >= 0);   // Global Pitch: transpose, scale, detune (scGlobalPitch.h)
        var gt = gg[0] + (gg[14] * 0.01 * \gdr.kr(0));   // + each voice's detune
        var gq = { |p| var r = p.round; Select.kr(gg[1] > 0.5, [p, r + Select.kr(r.mod(12), gg[2..13])]) };
        var pitch = Lag.kr(gq.(lag.(\pitch, 48)) + lag.(\pitchoff, 0), \glide.kr(0)) + gt;   // pitchoff: the transpose
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

    // ── The tail: copy 0's display, raw (the streams at D partials, then what
    //    the cores add to all of them alike: P.Level's factor, the pan offset;
    //    the node applies them as it draws), and the pause: gate off and every
    //    core of copy 0 silent for 0.1 s: pause the voice's group until the
    //    node's next gate (re-checked 4 x / s, so a resume while silent pauses
    //    again; \pauser 1)
    part.("AdditiveBoxTail", np, { |np|
        var nd = np.min(dispMax), step = np div: nd, b = \bus.kr(0), vis = \vis.kr(0);
        Out.kr(vis, In.kr(b, np)[0, step ..] ++ In.kr(b + (2 * np), np)[0, step ..]);
        Out.kr(vis + (2 * dispMax), [(1 + mod.(7)).max(0), lag.(\panoff, 0) + mod.(5) + mod.(8)]);
        Pause.kr(1 - ((\gate.kr(0) <= 0) * (In.kr(b + (3 * np)) <= 0)
                      * LFPulse.kr(4) * \pauser.kr(0)), \grp.kr(0));
    });
};

// ── The lanes' clock: each LFO's run (0..1), the noise's rising counter and
//    the S&H trigger, one set per voice, so every chunk reads the same ───────
SynthDef(\AdditiveBoxLaneClock, {
    var b = \bus.kr(0) + (3 * \np.kr(8)) + 3;
    var clock = 2.collect { |l|
        var k = { |name, def| NamedControl.kr(("l" ++ l ++ name).asSymbol, def) };
        var rst = Changed.kr(k.("reset", 0));
        [Phasor.kr(rst, k.("rate", 1) * ControlDur.ir, 0, 1),
         Sweep.kr(rst, k.("rate", 1)) + k.("init", 0),
         Impulse.kr(k.("sh", 0).max(0.01))]
    };
    ReplaceOut.kr(b, clock.flat);
}).writeDefFile(dir);

chunks.do { |nc|
    // ── LFO lanes: each partial reads the LFO's cycle at its own phase; the
    //    N variant also runs Noise (smooth random values, a seed per partial) ──────────
    [false, true].do { |withNoise|
        if(withNoise.not or: { nc <= noiseChunk }) { part.(if(withNoise, "AdditiveBoxLanesN", "AdditiveBoxLanes"), nc, { |nc|
            var o = chunk.(nc), first = \first.kr(0);
            var s = [In.kr(o[0], nc), In.kr(o[1], nc), In.kr(o[2], nc)];
            var clock = In.kr(\bus.kr(0) + (3 * \np.kr(8)) + 3, 6);
            var lane = 2.collect { |l|
                var k = { |name, def| NamedControl.kr(("l" ++ l ++ name).asSymbol, def) };
                var run = clock[3 * l];
                var x = IndexL.kr(k.("buf", -1), ((run + k.("ph", 0 ! nc)) % 1) * 256);
                var sh, v, amt;
                if(withNoise) {
                    // the seed: this LFO's and this voice's own noise field
                    var nz = valueNoise2.(clock[3 * l + 1], (first + (0 .. (nc - 1))) * k.("nscale", 1), k.("seed", 0));
                    x = Select.kr(k.("noise", 0) > 0.5, [x, IndexL.kr(k.("cbuf", -1), nz * 256)]);
                };
                // S&H (steps / s) with its glide (s)
                sh = k.("sh", 0);
                v = (Select.kr(sh > 0, [x, Lag.kr(Latch.kr(x, clock[3 * l + 2]), k.("shg", 0))]) * 2) - 1;
                amt = k.("amt", [0, 0, 0]);
                [v * amt[0], v * amt[1], v * amt[2]]
            }.sum;
            ReplaceOut.kr(o[0], s[0] * (1 + lane[0]).max(0));
            ReplaceOut.kr(o[1], s[1] * lane[2].midiratio);
            ReplaceOut.kr(o[2], s[2] + lane[1]);
        }) };
    };

    // ── The core: one per voice, unison copy and chunk, on O outputs ────────
    (1..maxOut).do { |no|
        SynthDef(("AdditiveBoxCore" ++ nc ++ "_" ++ no).asSymbol, {
            var o = chunk.(nc);
            var s = [In.kr(o[0], nc), In.kr(o[1], nc), In.kr(o[2], nc)];
            var vib = In.kr(\bus.kr(0) + (3 * \np.kr(8)) + 1, 2);
            var gb = \gtrbus.kr(-1), gg = In.kr(gb.max(0), 15) * (gb >= 0);   // Global Pitch: transpose, scale, detune (scGlobalPitch.h)
            var gt = gg[0] + (gg[14] * 0.01 * \gdr.kr(0));   // + each voice's detune
            var gq = { |p| var r = p.round; Select.kr(gg[1] > 0.5, [p, r + Select.kr(r.mod(12), gg[2..13])]) };
            var pitch = Lag.kr(gq.(lag.(\pitch, 48)) + lag.(\pitchoff, 0), \glide.kr(0)) + gt;   // pitchoff: transpose, unison
            var glev = Gate.kr(\gate.kr(0), \gate.kr(0) > 0);   // the gate's level: a gate at 0.2 plays at 0.2
            // An amplitude envelope owns note-off/release. Without one, close
            // the voice with Gate; glev alone holds the last positive velocity.
            // \vca 0 or 1; fractional while a snapshot morph crossfades the two paths
            var vca = \vca.kr(0, 1/30, fixedLag: true).clip(0, 1);
            var gateAmp = (Lag.kr((\gate.kr(0) > 0), 0.005) * (1 - vca)) + vca;
            var p = pitch + mod.(2) + mod.(9) + (vib[0] * vib[1]);
            var freqs = (s[1] * p.midicps) + lag.(\shift, 0);
            var levels = s[0] * (1 + mod.(7)).max(0);
            var pos = s[2] + lag.(\panoff, 0) + mod.(5) + mod.(8);
            var line = \line.kr(0), width = lag.(\width, 2);
            var sigs, sig;
            if(no == 2) { pos = pos.fold(0, 1) };
            if(no > 2) { pos = Select.kr(line, [pos, pos.fold(0, 1)]) };
            sigs = SinOsc.ar(freqs, { Rand(0, 2pi) } ! nc);
            sig = case
                { no == 1 } { Mix(sigs * levels) }
                { no == 2 } { Mix(Pan2.ar(sigs, pos * 2 - 1, levels)) }
                { Mix(PanAz.ar(no, sigs, Select.kr(line, [pos * 2, pos * 2 * (no - 1) / no]), levels, width, 0)) };
            // Level: an envelope routed there is the voice's VCA (\vca 1: the
            // modulation alone sets the level); else it moves the level around 1
            sig = sig * (lag.(\amp, 0.5) * (((1 + mod.(3)).max(0) * (1 - vca)) + (mod.(3).max(0) * vca))) * glev * gateAmp;
            Out.ar(\out.kr(0), sig);
            // 1 while sounding, summed over copy 0's chunks for the tail (the
            // other copies: a junk bus)
            Out.kr(\silbus.kr(0), 1 - A2K.kr(DetectSilence.ar(Mix(sig), 1e-5, 0.1)));
        }).writeDefFile(dir);
    };
};

(1..maxOut).do { |no|
    SynthDef(("AdditiveBoxOut" ++ no).asSymbol, {
        var sig = In.ar(\in.kr(0), no) * \volume.kr(1, 1/30, fixedLag: true);
        Out.ar(\out.kr(0), Sanitize.ar(sig));
    }).writeDefFile(dir);
};
"AdditiveBox: % defs written to %".format((caps.size * 8) + 2 + (chunks.size * (1 + maxOut)) + chunks.count(_ <= noiseChunk) + maxOut, dir).postln;
)
