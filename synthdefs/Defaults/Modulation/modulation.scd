// modulation.scd — small modulation SynthDefs, N voices wide (N = 1..16),
// shared by the nodes that build their modulation out of synths (PolyComb
// first). Every one reads and writes buses, so a node creates only what it
// uses and routes it with ModRoute.
//
//   ModGate<N>    the gates: Oceanode values (\gate, N) + audio-rate gates
//                 (\gate_ar mapped, \gate_sel 1), or always on (\drone 1)
//                 -> \out (audio, N): each gate's level (held from its rising
//                 edge). Per note: the velocity (x that level) -> \vel
//                 (control, N), a random -1..1 -> \rnd.
//   ModLfo<N>     a morph LFO per voice (PolyComb's: round / skew / width /
//                 pow / bipow / sample & hold), or Perlin noise (\noise 1, as
//                 Oceanode's Noise node) -> \out (control, N), 0..1.
//                 Per-voice phase offsets (\phoff) come from the node.
//   ModEnv<N>     an ADSR per voice (a tension curve per segment), gated by
//                 \gate (an audio bus, N: ModGate's or the node's voices; read
//                 with InFeedback, so in any order) -> \out (control, N).
//   ModEnvAr<N>   the same, also at audio rate on \aout (for a VCA).
//   ModRoute<N>   one route of a modulation matrix: \src (control bus, N)
//                 (\decor 1: hashed with \salt, each route its own random)
//                 (* 2 - 1 with \bipolar) * \amount * \scale, added into
//                 \out (control bus, N). A target bus with no route left is
//                 zeroed by the node.
(
var root, dir, perlin2;
root = if(thisProcess.nowExecutingPath.notNil,
    { thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs" },
    { Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs" });
dir = root +/+ "Modulation";
File.mkdir(root); File.mkdir(dir);

// 2D Perlin noise (as Oceanode's Noise node: ofNoise(x, y)), 0..1: x an
// ever-rising counter (the LFO's rate), y a fixed seed per voice (index x
// Noise Scale: how different neighbouring voices are). The hash wraps every
// 4096 cells (never repeats in practice). The node's C++ preview runs the same.
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

(1..32).do { |n|
    var kv = { |name, def| NamedControl.kr(name, def ! n) };

    // the gates are levels: a gate rising to 0.2 plays a note at 0.2. \out
    // carries each gate's level (held from its rising edge until it closes;
    // 1 while droning), \vel the note's velocity = that level x Velocity
    // (Oceanode values, or audio: \vel_ar with \vel_sel 1)
    SynthDef(("ModGate" ++ n).asSymbol, {
        var gateK = kv.(\gate, 0), gateA = \gate_ar.ar(0 ! n) * \gate_sel.kr(0);
        var drone = K2A.ar(\drone.kr(0) > 0.5);
        var gate = (((K2A.ar(gateK) + gateA) > 0) + drone) > 0;
        var trig = (HPZ1.ar(gate) > 0) + Impulse.ar(0);
        // the level: a cable's exactly at its edge; Oceanode values held at control
        // rate (their audio ramp would be caught at its start)
        var level = Select.ar(drone, [Latch.ar(gateA, trig).max(K2A.ar(Gate.kr(gateK, gateK > 0))).max(0), DC.ar(1)]);
        // the velocity: a cable's at the edge, Oceanode values a block after it (settled)
        var velocity = Select.ar(\vel_sel.kr(0) > 0.5, [
            Latch.ar(K2A.ar(kv.(\velocity, 1)), DelayN.ar(trig, 0.1, ControlDur.ir * 1.5)),
            Latch.ar(\vel_ar.ar(1 ! n), trig)]);
        Out.ar(\out.kr(0), gate * level);
        Out.kr(\vel.kr(0), A2K.kr(velocity * level));
        Out.kr(\rnd.kr(0), A2K.kr(TRand.ar(-1, 1, trig)));
    }).writeDefFile(dir);

    SynthDef(("ModLfo" ++ n).asSymbol, {
        var customPow = { |value, powAmt|
            var k1 = 2 * powAmt * 0.99999;
            var k2 = k1 / ((powAmt.neg * 0.999999) + 1);
            var k3 = (k2 * value.abs) + 1;
            value * (k2 + 1) / k3.max(1.0e-6)
        };
        var reflect = { |v| v + ((v < 0) * v.neg) + ((v > 1) * (1 - v)) };
        var reset = Changed.kr(\reset.kr(0));
        var freq = kv.(\freq, 1), detune = kv.(\freqdetune, 0);
        var spread = { TRand.kr(0, 1, reset + Impulse.kr(0)) } ! n;
        var sweep = Sweep.kr(reset, freq + (spread * detune));
        var phoff = kv.(\phoff, 0);
        // Hold: stopped at the start, each voice at its offset (an indexer)
        var ph = ((sweep * (1 - \hold.kr(0))) + \initphase.kr(0) + phoff).wrap(0, 1);
        var pw = kv.(\pw, 0.5).clip(0.001, 1), skew = kv.(\skew, 0), round = kv.(\round, 0.5);
        var midpoint, leftMax, rightMin, centered, left, narrowed, wLo, wHi, widened;
        var threshold, lo, hi, w, tri, cosRaw, cosMapped, upper, shape, noise, sh, held, out;
        // width (PolyComb's applyPulseWidth)
        midpoint = LinLin.kr(skew, -1, 1, 1, 0).clip(0, 1);
        leftMax = LinLin.kr(1 - pw, 0, 0.5, 0, midpoint).clip(0, 1);
        rightMin = LinLin.kr(pw, 0.5, 1, midpoint, 1).clip(0, 1);
        centered = LinLin.kr(ph, (0.5 - pw).clip(0, 1), (0.5 + pw).clip(0, 1), 0, 1).clip(0, 1);
        left = LinLin.kr(ph, 0, (pw * 2).max(1.0e-7).min(1), 0, 1).clip(0, 1);
        narrowed = Select.kr(\centered.kr(1) > 0.5, [left, centered]);
        narrowed = narrowed * (1 - ((skew < 0) * (narrowed >= 0.99999)));
        wLo = LinLin.kr(ph, 0, leftMax.max(1.0e-7), 0, midpoint).clip(0, 1);
        wHi = LinLin.kr(ph, rightMin.min(1 - 1.0e-7), 1, midpoint, 1).clip(0, 1);
        widened = Select.kr(ph < midpoint, [wHi, wLo]);
        ph = Select.kr(pw < 0.5, [Select.kr(pw >= 0.99999, [widened, midpoint]), narrowed]);
        // skew
        threshold = (0.5 - (skew * 0.5)).clip(0, 1);
        lo = LinLin.kr(ph, 0, threshold.max(1.0e-7), 0, 0.5).clip(0, 0.5);
        hi = LinLin.kr(ph, threshold.min(1 - 1.0e-7), 1, 0.5, 1).clip(0.5, 1);
        ph = Select.kr(ph >= threshold, [lo, hi]);
        // the shape: triangle .. cosine .. square
        w = ph * 2 * pi;
        tri = (1 - (2 * ph - 1).abs).max(0);
        cosRaw = cos(w + pi);
        cosMapped = LinLin.kr(cosRaw, -1, 1, 0, 1).clip(0, 1);
        upper = customPow.(cosRaw, (round - 0.5) * 2);
        shape = Select.kr(((round >= 0.5) + (round >= 1)).clip(0, 2), [
            LinLin.kr(round, 0, 0.5, tri, cosMapped).clip(0, 1),
            LinLin.kr(upper, -1, 1, 0, 1).clip(0, 1),
            (ph >= 0.25) * (ph < 0.75)]);
        // Noise: 2D Perlin noise (Oceanode's Noise node): x = the ever-rising
        // counter (the LFO's rate: Hz or Beats), y = each voice's seed = its
        // index x \nscale (how different the voices are); \seed: this LFO's
        // own noise field (no two LFOs share values)
        noise = perlin2.(sweep + \initphase.kr(0), (0 .. (n - 1)) * \nscale.kr(1), \seed.kr(0));
        shape = Select.kr(\noise.kr(0) > 0.5, [shape, noise]);
        // Gain: stretched around the middle, clipped at 0 / 1 (before S&H, Pow, BiPow)
        shape = (((shape - 0.5) * kv.(\gain, 1)) + 0.5).clip(0, 1);
        // sample & hold, pow, bipow
        sh = kv.(\sh, 0);
        // S&H Glide (s): each held value slides to the next instead of jumping
        held = Lag.kr(Latch.kr(shape, Impulse.kr(sh.max(0.001))), \shglide.kr(0).max(0));
        shape = (shape * (sh <= 0)) + (held * (sh > 0));
        out = Select.kr(kv.(\pow, 0).abs > 0.0001, [shape, reflect.(customPow.(shape, kv.(\pow, 0)))]);
        out = Select.kr(kv.(\bipow, 0).abs > 0.0001, [out,
            reflect.((customPow.((out * 2) - 1, kv.(\bipow, 0)) + 1) * 0.5)]);
        Out.kr(\out.kr(0), Sanitize.kr(out));
    }).writeDefFile(dir);

    [false, true].do { |audio|
        SynthDef(("ModEnv" ++ if(audio, "Ar", "") ++ n).asSymbol, {
            // InFeedback: right in any order (the gates written before this
            // synth, this block, or after it, the last block)
            var gate = InFeedback.ar(\gate.kr(0), n) > 0;
            // velocity tracking: \veltrack 0 (the same every note) .. 1 (scaled by
            // the note's velocity, \velbus: control, n)
            var vt = \veltrack.kr(0).clip(0, 1);
            var vel = K2A.ar(In.kr(\velbus.kr(0), n));   // held per note by the gates
            var env = EnvGen.ar(Env.adsr(\atk.kr(0.01).max(0.001), \dec.kr(0.3).max(0.001), \sus.kr(0.8).clip(0, 1),
                \rel.kr(1).max(0.001), 1, [\cA.kr(-4), \cD.kr(-4), \cR.kr(-4)].collect(_.clip(-12, 12))), gate)
                * (1 - vt + (vt * vel));
            Out.kr(\out.kr(0), A2K.kr(env));
            if(audio) { Out.ar(\aout.kr(0), env) };
        }).writeDefFile(dir);
    };

    SynthDef(("ModRoute" ++ n).asSymbol, {
        var src = In.kr(\src.kr(0), n);
        // \decor 1 (the Random source): this route's own value per note, a hash
        // of the note's draw salted by \salt (the node's C++ does the same)
        src = Select.kr(\decor.kr(0) > 0.5, [src, (((src * 91.3458) + \salt.kr(0)).sin * 437.5853).frac * 2 - 1]);
        src = Select.kr(\bipolar.kr(0) > 0.5, [src, (src * 2) - 1]);
        Out.kr(\out.kr(0), src * (\amount.kr(0, 0.05) * \scale.kr(1)));
    }).writeDefFile(dir);
};
"Modulation SynthDefs written to ".post; dir.postln;
)
