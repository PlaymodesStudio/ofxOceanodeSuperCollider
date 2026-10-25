// ModLfoX<N>: shared LFO engine for all box synths. Classic, Noise and
// Random modes share one clock; modeweights blends them without rebuilding.
//
//   \rtype 1  Stepped: a new random value each cycle (sample & hold), at the
//             voice's own phase; glided by a one-pole of time \rsmooth (s)
//   \rtype 2  Smooth: the cycle's values joined by a cubic (Catmull-Rom) curve
//   \rtype 3  Jumps: Stepped + Smooth wrapped into 0..1
//   \steps    the output quantized to that many levels (0: off)
// The random values are a hash of the cycle number (and \seed) and, unless
// \rshare 1 (one series for every voice, each at its own phase), of the
// voice's index: each voice its own random.
(
var root, dir, valueNoise2;
root = if(thisProcess.nowExecutingPath.notNil,
    { thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs" },
    { Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs" });
dir = root +/+ "Modulation";
File.mkdir(root); File.mkdir(dir);

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

(1..32).do { |n|
    var kv = { |name, def| NamedControl.kr(name, def ! n) };
    SynthDef(("ModLfoX" ++ n).asSymbol, {
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
        // the random types: per voice, a new value each cycle at the voice's own phase
        var rpos = (sweep * (1 - \hold.kr(0))) + \initphase.kr(0) + phoff;
        var rcycle = rpos.floor, rfrac = rpos - rcycle;
        // \rshare 0: each voice its own values; 1: one series for every voice
        var rsalt = (\seed.kr(0) * 7.13) + ((0 .. (n - 1)) * 3.7193 * (1 - (\rshare.kr(0) > 0.5)));
        var rhash = { |c| ((((c % 8192) * 12.9898) + rsalt).sin * 43758.5453).frac };
        var v3 = rhash.(rcycle), v2 = rhash.(rcycle - 1), v1 = rhash.(rcycle - 2), v0 = rhash.(rcycle - 3);
        var rstepped = OnePole.kr(v3, exp(-1 / (\rsmooth.kr(0).max(1.0e-4) * ControlRate.ir)) * (\rsmooth.kr(0) > 0));
        var rsmooth = (v1 + (0.5 * rfrac * ((v2 - v0) + (rfrac * (((2 * v0) - (5 * v1) + (4 * v2) - v3)
            + (rfrac * ((3 * (v1 - v2)) + v3 - v0))))))).clip(0, 1);
        var rjumps = ((rstepped + rsmooth) - 1).wrap(0, 1);
        var steps = \steps.kr(0);
        // Hold: stopped at the start, each voice at its offset (an indexer)
        var ph = ((sweep * (1 - \hold.kr(0))) + \initphase.kr(0) + phoff).wrap(0, 1);
        var pw = kv.(\pw, 0.5).clip(0.001, 1), skew = kv.(\skew, 0), round = kv.(\round, 0.5);
        var midpoint, leftMax, rightMin, centered, left, narrowed, wLo, wHi, widened;
        var threshold, lo, hi, w, tri, cosRaw, cosMapped, upper, shape, noise, sh, held, out;
        var modeWeights = NamedControl.kr(\modeweights, 0 ! 6).max(0);
        var modeTotal = modeWeights.sum, activeWeight = modeWeights.copyRange(0, 4).sum;
        var manualWeight = Select.kr(modeTotal > 0, [0, modeWeights[5] / modeTotal.max(1e-8)]);
        var classic, legacy;
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
        // Noise: smooth 2D value noise: x = the ever-rising
        // counter (the LFO's rate: Hz or Beats), y = each voice's seed = its
        // index x \nscale (how different the voices are); \seed: this LFO's
        // own noise field (no two LFOs share values)
        noise = valueNoise2.(sweep + \initphase.kr(0), (0 .. (n - 1)) * \nscale.kr(1), \seed.kr(0));
        classic = shape;
        legacy = Select.kr(\noise.kr(0) > 0.5, [classic, noise]);
        legacy = Select.kr(\rtype.kr(0).clip(0, 3), [legacy, rstepped, rsmooth, rjumps]);
        shape = Select.kr(modeTotal > 0, [legacy,
            ((classic * modeWeights[0]) + (noise * modeWeights[1]) + (rstepped * modeWeights[2])
             + (rsmooth * modeWeights[3]) + (rjumps * modeWeights[4])) / activeWeight.max(1e-8)]);
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
        // Steps: that many levels (0 / 1: smooth), as GrainBox's
        out = Select.kr(steps >= 2, [out, (out * steps).floor.min(steps - 1) / (steps - 1).max(1)]);
        out = (out * (1 - manualWeight)) + (\manualvalue.kr(0).clip(0, 1) * manualWeight);
        Out.kr(\out.kr(0), Sanitize.kr(out));
    }).writeDefFile(dir);
};
"Modulation LFOs (random / steps) written to ".post; dir.postln;
)
