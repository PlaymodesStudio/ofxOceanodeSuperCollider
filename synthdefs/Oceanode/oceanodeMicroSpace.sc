// Shared DSP for microSpace.scd and the Box inserts. With no decay override
// and freeze = 0 this is the original Micro Space graph, sample for sample.
OceanodeMicroSpace {
    *ar { |input, size=50, mod=0.5, lo=0, hi=0, position=0.5, mix=0.5, decay, freeze=0|
        var n = input.asArray.size;
        var output, feedbackIn, feedbackOut, results;
        var sr = SampleRate.ir, block = BlockSize.ir;
        var pairCount = (n + 1).div(2);
        var p2t, smooth, xfadeShape, shelf, diffuser, eightPhase;
        var maxPredelay = 0.05, maxEarly = 0.06;
        var maxLate = [0.33, 0.39, 0.46, 0.55];
        var loopsPerPair = 14;
        input = input.asArray;
        size = size.asArray.wrapExtend(n).collect { |value| value.asAudioRateInput };
        mod = mod.asArray.wrapExtend(n).collect { |value| value.asAudioRateInput };
        lo = lo.asArray.wrapExtend(n).collect { |value| value.asAudioRateInput };
        hi = hi.asArray.wrapExtend(n).collect { |value| value.asAudioRateInput };
        position = position.asArray.wrapExtend(n).collect { |value| value.asAudioRateInput };
        mix = mix.asArray.wrapExtend(n).collect { |value| value.asAudioRateInput };
        // Reaktor P2F is 8.17741966 * 2^(P/12) (its correction of the exp approximation).
        p2t = { |pitch| (pitch * (2.log / 12)).exp.reciprocal / 8.17741966 };

        // Core "Smooth": y += (x - y) * ln2 / (T * SR), i.e. half-life T.
        smooth = { |signal, time| OnePole.ar(signal.asAudioRateInput, 1 - (2.log / (time * sr).max(1))) };

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
                var highDb = hi[c].min(0) * 0.5 * (1 - freeze), lowDb = lo[c].min(0) * 0.72 * (1 - freeze);
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
                var rt = if(decay.isNil) { (pairSize[side] * (10.log / 20)).exp * 0.001 } { decay.max(0.001) };
                var gain = ((-60 * p2t.(pitch) / rt).max(-144) * (10.log / 20)).exp;
                gain = gain + (freeze * (1 - gain));
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
        ^output
    }

    // Existing Box controls/presets: retain separate RT60 and predelay, with
    // the Micro Space diffuser/tank, modulation and parabolic crossfades.
    // Old 0..60 Size spans Micro Space's 0..100; damping maps to its shelves.
    *box { |input, size=30, decay=4, predelay=20, lowpass=10000, position=0.8, spread=0.15, mix=0.33, highdamp=6, lowdamp=0, modfreq=0.2, spin=1, wander=0.25, on=1|
        var dry = input.asArray;
        var gate = K2A.ar(on).lag(0.02);
        var feed = dry.collect { |sig, i|
            var side = if(i.even) { 1 } { -1 };
            DelayC.ar(sig * gate, 2, (predelay * 0.001 * (1 + (spread * side))).clip(0, 2))
        };
        var wet = this.ar(feed, size.clip(0, 60) * (100/60),
            (modfreq * (spin + wander) * 2).clip(0, 1),
            lowdamp.neg.clip(-12, 0), highdamp.neg.clip(-12, 0), position, 1, decay);
        wet = wet.collect { |sig| LPF.ar(sig, lowpass.asAudioRateInput.clip(20, SampleRate.ir * 0.45)) };
        // Only excitation is gated: bypass restores dry while tails ring out.
        ^(dry * (1 - (mix * gate)).clip(0, 1) * (1 + (mix * gate)))
            + (wet * mix * (2 - mix))
    }
}
