(
~synthCreator.value("MicroSpace", {|n|
    var input, output, feedbackIn, feedbackOut, results;
    var size, mod, lo, hi, position, mix;
    var sr = SampleRate.ir, block = BlockSize.ir;
    var pairCount = (n + 1).div(2);
    var p2t, smooth, xfadeShape, shelf, diffuser, eightPhase;
    var maxPredelay = 0.05, maxEarly = 0.06;
    var maxLate = [0.33, 0.39, 0.46, 0.55];   // the three late diffusers, then the tank delay
    var loopsPerPair = 14;                     // 6 early + 6 late diffusers + 2 tank delays

    /*
    Native Instruments "Micro Space" (Reaktor 6 macro), rebuilt from its Core cell
    "Space Master Deluxe" with tools/ReaktorPatchExtractor (core_view.py).

    Panel -> cell (the "Master Adaptor" macro):
        PreDel = Size * 0.00022 + 0.002 s, PDSym = 0
        ErSz = 60 - 0.42 Size, ERSym = -2, ErDff = 0.67      (pitches, P2T = 1 / P2F)
        LtSz = 48 - 0.72 Size, LtSym = 1.5, LtDffs = 0.67
        RT60 = 10^(Size / 20) ms
        damping shelves: high 4186 Hz (P108) min(0, Hi) * 0.5 dB, low 65.4 Hz (P36) min(0, Lo) * 0.72 dB
        EQ shelves: same frequencies, Hi dB and Lo dB
        ModF = Size * Mod / 100 Hz, ModA = Size * Mod / 100 ms
        E/L = Position, D/W = Mix
    Cell, per side (L uses Size - Sym, R uses Size + Sym):
        predelay (integer samples) -> 3 early diffusers (P+8, P+4, P), integer delay
        -> damping shelf = early out
        -> + other side's tank feedback -> 3 late diffusers (P+9, P+6, P+3), Hermite delay,
           modulated by 8Phase -> damping shelf = late out
        -> tank delay (P, Hermite, modulated) * RT60 gain -> other side.
    Diffuser: v = x + g y; y = z^-1 Shelf(Delay(v, T)) - g x (damping inside every loop).
    Output: parabolic crossfade early/late, EQ shelves, parabolic crossfade dry/wet.

    Every recursive loop goes through LocalIn/LocalOut: one block of the loop's
    delay comes from there and DelayN/DelayC supply the rest, so the loop
    lengths match Reaktor's exactly (all loops are longer than a 64-sample block).
    DelayC interpolates with the same 4-point Hermite as Reaktor's Delay H.
    */

    size = OceanodeParameterLag.ar(\size, 50, n, 0, 100, "vf", 1/30, true).asArray;
    mod = OceanodeParameterLag.ar(\mod, 0.5, n, 0, 1, "vf", 1/30, true).asArray;
    lo = OceanodeParameterLag.ar(\lo, 0, n, -12, 12, "vf", 1/30, true).asArray;
    hi = OceanodeParameterLag.ar(\hi, 0, n, -12, 12, "vf", 1/30, true).asArray;
    position = OceanodeParameterLag.ar(\position, 0.5, n, 0, 1, "vf", 1/30, true).asArray;
    mix = OceanodeParameterLag.ar(\mix, 0.5, n, 0, 1, "vf", 1/30, true).asArray;

    input = In.ar(OceanodeInput.kr(\in), n).asArray;

    // Reaktor P2F is 8.17741966 * 2^(P/12) (its correction of the exp approximation).
    p2t = { |pitch| (pitch * (2.log / 12)).exp.reciprocal / 8.17741966 };

    // Core "Smooth": y += (x - y) * ln2 / (T * SR), i.e. half-life T.
    smooth = { |signal, time| OnePole.ar(signal, 1 - (2.log / (time * sr).max(1))) };

    // XFade(Par): x * (2 - x).
    xfadeShape = { |x| x * (2 - x) };

    // Core "Shelf": BiLin 1P high shelf then BiLin 1P low shelf. Gain in dB,
    // corner frequency scaled by 10^(dB/40), Reaktor's tan approximation.
    shelf = { |signal, highFreq, highDb, lowFreq, lowDb|
        var warp = { |freq|
            var x = freq.clip(sr / 24576, sr / 2.125) * (pi / sr);
            x + (x.cubed * 0.333333343) + (x.squared * x.cubed * 0.13333334)
        };
        var e, gain, w, a1, k;
        e = (highDb * (10.log / 40)).exp;
        gain = e.squared;
        w = warp.(highFreq * e);
        a1 = (1 - w) / (1 + w);
        k = (gain - 1) / (1 + w);
        signal = FOS.ar(signal, 1 + k, (a1 + k).neg, a1);
        e = (lowDb * (10.log / 40)).exp;
        gain = e.squared;
        w = warp.(lowFreq / e);
        a1 = (1 - w) / (1 + w);
        k = w / (1 + w) * (gain - 1);
        FOS.ar(signal, 1 + k, k - a1, a1)
    };

    // Allpass diffuser with the damping shelf and one extra sample inside the
    // loop. loopIn is the loop signal v as returned by LocalIn (one block late).
    // Returns [y, v]; v goes to LocalOut.
    diffuser = { |x, loopIn, delaySamples, interpolated, maxDelay, g, damp|
        var delayed, y;
        delayed = if(interpolated) {
            DelayC.ar(loopIn, maxDelay, ((delaySamples + 1 - block).max(1)) / sr)
        } {
            DelayN.ar(loopIn, maxDelay, ((delaySamples.round + 1 - block).max(1) + 0.25) / sr)
        };
        y = damp.(delayed) - (g * x);
        [y, x + (g * y)]
    };

    // 8Phase: one phase, four parabolic sines 45 degrees apart; R gets them inverted.
    eightPhase = { |freq, amp|
        var phase = Phasor.ar(0, freq / sr, -0.5, 0.5);
        var a = smooth.(amp, 0.05);
        [0.125, 0.25, 0.375, 0.5].collect { |offset|
            var p = phase + offset;
            p = p - (p >= 0.5);
            p * (8 - (16 * p.abs)) * a
        }
    };

    feedbackIn = LocalIn.ar(pairCount * loopsPerPair).asArray.clump(loopsPerPair);
    feedbackOut = Array.newClear(pairCount);

    results = Array.fill(pairCount, { |pairIndex|
        var chans = [pairIndex * 2, (pairIndex * 2 + 1).min(n - 1)];
        var loops = feedbackIn[pairIndex];
        var loopOut = Array.newClear(loopsPerPair);
        var dry = chans.collect { |c| input[c] };
        var pairSize = chans.collect { |c| size[c] };
        var sideSign = [-1, 1];   // L = Size - Sym, R = Size + Sym
        var modDepth = (size[chans[0]] * mod[chans[0]] + (size[chans[1]] * mod[chans[1]])) * 0.005;
        var phases = eightPhase.(modDepth, modDepth * 0.001);
        var mods = [phases, phases.neg];
        var damps = chans.collect { |c|
            var highDb = hi[c].min(0) * 0.5, lowDb = lo[c].min(0) * 0.72;
            { |sig| shelf.(sig, 4186.0094, highDb, 65.406392, lowDb) }
        };
        var early, late, tankIn, rtGain, tankDelay, wet;

        // Predelay, integer samples.
        early = [0, 1].collect { |side|
            var t = smooth.(pairSize[side] * 0.00022 + 0.002, 0.05);
            DelayN.ar(dry[side], maxPredelay, ((t * sr).round + 0.25) / sr)
        };

        // Early diffusion.
        early = [0, 1].collect { |side|
            var pitch = (60 - (0.42 * pairSize[side])) + (sideSign[side] * -2);
            var sig = early[side];
            [8, 4, 0].do { |offset, stage|
                var slot = side * 3 + stage;
                var t = smooth.(p2t.(pitch + offset), 0.05);
                var res = diffuser.(sig, loops[slot], t * sr, false, maxEarly, 0.67, damps[side]);
                sig = res[0];
                loopOut[slot] = res[1];
            };
            damps[side].(sig)
        };

        // Late diffusion: cross-coupled tank.
        tankIn = [0, 1].collect { |side|
            var pitch = (48 - (0.72 * pairSize[side])) + (sideSign[side] * 1.5);
            var t = smooth.(p2t.(pitch), 0.05) + mods[side][3];
            var rt = (pairSize[side] * (10.log / 20)).exp * 0.001;
            var gain = ((-60 * p2t.(pitch) / rt).max(-144) * (10.log / 20)).exp;
            DelayC.ar(loops[12 + side], maxLate[3], ((t * sr + 1 - block).max(1)) / sr) * gain
        };

        late = [0, 1].collect { |side|
            var pitch = (48 - (0.72 * pairSize[side])) + (sideSign[side] * 1.5);
            var sig = early[side] + tankIn[1 - side];
            [9, 6, 3].do { |offset, stage|
                var slot = 6 + (side * 3) + stage;
                var t = smooth.(p2t.(pitch + offset), 0.05) + mods[side][stage];
                var res = diffuser.(sig, loops[slot], t * sr, true, maxLate[stage], 0.67, damps[side]);
                sig = res[0];
                loopOut[slot] = res[1];
            };
            sig = damps[side].(sig);
            loopOut[12 + side] = sig;   // into the tank delay
            sig
        };

        feedbackOut[pairIndex] = loopOut;

        [0, 1].collect { |side|
            var c = chans[side];
            var pos = position[c], dw = mix[c];
            wet = (early[side] * xfadeShape.(1 - pos)) + (late[side] * xfadeShape.(pos));
            wet = shelf.(wet, 4186.0094, hi[c], 65.406392, lo[c]);
            (dry[side] * xfadeShape.(1 - dw)) + (wet * xfadeShape.(dw))
        }
    });

    LocalOut.ar(feedbackOut.flatten);

    // An odd last channel runs as its own pair (same input on both sides) and keeps L.
    output = results.flatten.keep(n);
    Out.ar(OceanodeOutput.kr(\out), output);

}, description: "NI Micro Space reverb rebuilt from its Reaktor Core cell", category: "Effect/Time/Reverb");
)
