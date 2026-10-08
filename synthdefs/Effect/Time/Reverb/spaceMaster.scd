(
~synthCreator.value("SpaceMaster", {|n|
    var input, output, localFeedback, feedbackOutput, results;
    var predelay, spread, size, decay, lowpass, highdamp, lowdamp;
    var modfreq, spin, wander, position, mix;
    var pairCount, pairValue, pairMods, tick;
    var p2d, reaktorAllpass, onePoleLP, damp;
    var maxPredelay = 2.0;   // predelay up to 1000 ms x (1 + spread)
    // Stage buffers sized to what the controls can reach (they were 2 s each:
    // ~8 MB per stereo instance, the whole default scsynth real-time pool --
    // with -m 8192 the delays failed to allocate and the tank was silent).
    // Early: P2D(65 - 60 - 10) = 0.164 s. Late: (P2D(30 - 60 - 7) + 40 ms) / 5 = 0.214 s.
    var maxEarly = 0.2;
    var maxLate = 0.25;

    /*
    R66 / SpaceMaster-style reverb reconstruction.

    Multichannel strategy:
    - Channels are processed as adjacent stereo pairs: 0<->1, 2<->3, etc.
    - Each pair owns an independent cross-feedback tank and independent modulation.
    - Parameters that belong to the stereo tank are averaged across each pair.
    - Mix and Position remain independent per output channel.
    - If n is odd, the final channel runs as a mono/self-feedback instance.

    The cross-feedback uses LocalIn/LocalOut. The explicit feedback delay is
    shortened by one ControlDur to compensate for SuperCollider's one-block
    LocalIn/LocalOut latency. This is intended for the usual 64-sample server.
    */

    // Oceanode parameters. Values are vectors of n channels.
    predelay = OceanodeParameterLag.ar(\predelay, 20, n, 0, 1000, "vf", 1/30, true).asArray; // ms
    spread = OceanodeParameterLag.ar(\spread, 0.15, n, -1, 1, "vf", 1/30, true).asArray;
    size = OceanodeParameterLag.ar(\size, 30, n, 0, 60, "vf", 1/30, true).asArray;
    decay = OceanodeParameterLag.ar(\decay, 4, n, 0.05, 60, "vf", 1/30, true).asArray; // seconds
    lowpass = OceanodeParameterLag.ar(\lowpass, 10000, n, 20, 20000, "vf", 1/30, true).asArray; // Hz
    highdamp = OceanodeParameterLag.ar(\highdamp, 6, n, 0, 60, "vf", 1/30, true).asArray; // dB attenuation
    lowdamp = OceanodeParameterLag.ar(\lowdamp, 0, n, 0, 60, "vf", 1/30, true).asArray; // dB attenuation
    modfreq = OceanodeParameterLag.ar(\modfreq, 0.2, n, 0.001, 20, "vf", 1/30, true).asArray; // Hz
    spin = OceanodeParameterLag.ar(\spin, 1, n, 0, 20, "vf", 1/30, true).asArray; // ms before /5
    wander = OceanodeParameterLag.ar(\wander, 0.25, n, 0, 20, "vf", 1/30, true).asArray; // ms before /5
    position = OceanodeParameterLag.ar(\position, 0.8, n, 0, 1, "vf", 1/30, true).asArray;
    mix = OceanodeParameterLag.ar(\mix, 0.33, n, 0, 1, "vf", 1/30, true).asArray;

    input = In.ar(OceanodeInput.kr(\in), n).asArray;

    pairCount = (n + 1).div(2);

    // Average a vector parameter across one adjacent stereo pair.
    pairValue = { |parameter, pairIndex|
        var first = pairIndex * 2;
        var second = (first + 1).min(n - 1);
        (parameter[first] + parameter[second]) * 0.5
    };

    // Standard MIDI pitch to period in seconds. P=69 -> 1/440 s.
    p2d = { |pitch|
        pitch.midicps.reciprocal
    };

    /*
    Reaktor Diffuser Delay:
        y[n] = g*x[n] + x[n-D] - g*y[n-D]

    AllpassN uses the opposite recursive sign and exposes decay time instead
    of g. A negative decay time gives the required sign. Reaktor's measured
    delay was one sample longer than its nominal setting, so one SampleDur is
    added to every diffuser stage.
    */
    // ugen: AllpassN for the fixed early stages, AllpassC for the modulated
    // late stages (a non-interpolating read of a modulated delay jumps by
    // whole samples).
    reaktorAllpass = { |signal, delaySeconds, coefficient, ugen, maxDelay|
        var delay = (delaySeconds + SampleDur.ir).clip(SampleDur.ir, maxDelay);
        var coef = coefficient.clip(0.0001, 0.9999);
        var decayTime = delay * (0.001.log / coef.log) * -1;

        ugen.ar(signal, maxDelay, delay, decayTime)
    };

    // Approximation of Reaktor's Primary 1-Pole Filter low-pass output.
    onePoleLP = { |signal, frequency|
        var freq = frequency.clip(5, SampleRate.ir * 0.45);
        var coefficient = (-2 * pi * freq / SampleRate.ir).exp;
        OnePole.ar(signal, coefficient)
    };

    // Recovered damping order: high shelf at MIDI 96, then low shelf at MIDI 60.
    damp = { |signal, highAmount, lowAmount|
        signal = BHiShelf.ar(signal, 96.midicps, 1.0, highAmount.neg);
        signal = BLowShelf.ar(signal, 60.midicps, 1.0, lowAmount.neg);
        signal
    };

    /*
    Four independent modulation sets, one per stereo pair.
    Reaktor updates its modulation at a 400 Hz system control rate.
    Ph5..Ph8 are the inverted versions used by the right branch.

    The original four LFOs are sync-daisy-chained. This implementation keeps
    the same stepped 400 Hz character and complementary L/R modulation, using
    the phase approximation that produced the successful standalone version.
    */
    tick = Impulse.ar(400);

    pairMods = Array.fill(pairCount, { |pairIndex|
        var f = pairValue.(modfreq, pairIndex);
        var sineAmount = pairValue.(spin, pairIndex);
        var randomAmount = pairValue.(wander, pairIndex);
        var randomCoef = (-2 * pi * f / SampleRate.ir).exp;

        Array.fill(4, { |modIndex|
            var phase = modIndex * (-pi / 8);
            var sine = Latch.ar(SinOsc.ar(f, phase), tick) * sineAmount;
            var randomStep = TRand.ar(-1.0, 1.0, tick);
            var randomSmooth = OnePole.ar(randomStep, randomCoef);
            var randomHeld = Latch.ar(randomSmooth, tick) * randomAmount;

            sine + randomHeld
        })
    });

    localFeedback = LocalIn.ar(n).asArray;

    results = Array.fill(n, { |channelIndex|
        var pairIndex = channelIndex.div(2);
        var partnerIndex;
        var isLeft = channelIndex.even;
        var sideSign;
        var chanInput = input[channelIndex];

        var pairPredelay = pairValue.(predelay, pairIndex) * 0.001;
        var pairSpread = pairValue.(spread, pairIndex);
        var pairSize = pairValue.(size, pairIndex);
        var pairDecay = pairValue.(decay, pairIndex).max(0.05);
        var pairLowpass = pairValue.(lowpass, pairIndex);
        var pairHighDamp = pairValue.(highdamp, pairIndex);
        var pairLowDamp = pairValue.(lowdamp, pairIndex);

        var earlyPitch = 65 - pairSize;
        var latePitch = 30 - pairSize;
        var dffs = (0.5 + (pairSize / 240)).clip(0.0001, 0.999);

        var earlyOffsets;
        var lateOffsets;
        var predelayTime;
        var early;
        var tankInput;
        var modSignals;
        var undividedDelays;
        var physicalDelays;
        var late;
        var feedbackDelay;
        var feedbackDb;
        var feedbackGain;
        var newFeedback;
        var wet;
        var chanOutput;

        // 0<->1, 2<->3, etc. The final odd channel feeds itself.
        partnerIndex = if(isLeft) {
            (channelIndex + 1).min(n - 1)
        } {
            channelIndex - 1
        };

        sideSign = if(partnerIndex == channelIndex) {
            0
        } {
            if(isLeft) { 1 } { -1 }
        };

        predelayTime = (
            pairPredelay * (1 + (pairSpread * sideSign))
        ).clip(0, maxPredelay);

        early = DelayN.ar(chanInput, maxPredelay, predelayTime);

        earlyOffsets = if(isLeft) {
            [0, -4, -8]
        } {
            [-2, -6, -10]
        };

        earlyOffsets.do { |offset|
            early = reaktorAllpass.(early, p2d.(earlyPitch + offset), dffs, AllpassN, maxEarly)
        };

        early = onePoleLP.(early, pairLowpass);

        // The opposite branch's previous-block feedback excites this branch.
        tankInput = damp.(
            early + localFeedback[partnerIndex],
            pairHighDamp,
            pairLowDamp
        );

        lateOffsets = if(isLeft) {
            [8, 4, 0, -7]
        } {
            [10, 6, 2, -5]
        };

        // Left uses Ph1..Ph4; right uses their inverted Ph5..Ph8 counterparts.
        modSignals = if(isLeft) {
            pairMods[pairIndex]
        } {
            pairMods[pairIndex].collect { |signal| signal.neg }
        };

        // DMod = P2D(P) + modulation-in-ms. Every physical delay receives /5.
        undividedDelays = Array.fill(4, { |stageIndex|
            (
                p2d.(latePitch + lateOffsets[stageIndex])
                + (modSignals[stageIndex] * 0.001)
            ).max(SampleDur.ir * 5)
        });

        physicalDelays = undividedDelays / 5;

        late = tankInput;
        3.do { |stageIndex|
            late = reaktorAllpass.(late, physicalDelays[stageIndex], dffs, AllpassC, maxLate)
        };

        // Compensate the one-block LocalIn/LocalOut delay.
        feedbackDelay = (
            physicalDelays[3] - ControlDur.ir
        ).clip(SampleDur.ir, maxLate);

        // RT block: level dB = -60 * undivided fourth DMod / decay time.
        // (Reaktor-faithful. The real loop is the four physical stages, about
        // 0.52 x the undivided fourth DMod, so the measured RT60 is ~0.55 x
        // Decay. The mixer's reverb insert uses the physical loop instead.)
        feedbackDb = (
            -60 * undividedDelays[3] / pairDecay
        ).clip(-60, 0);

        feedbackGain = feedbackDb.dbamp.clip(0, 0.9999);
        newFeedback = DelayC.ar(late, maxLate, feedbackDelay) * feedbackGain;

        // Exact recovered early/late PowerFade.
        wet =
            (early * (1 - position[channelIndex]).sqrt)
            + (late * position[channelIndex].sqrt);

        // Convenient equal-power dry/wet fade. DC removal on the wet only
        // (LeakDC's default 0.995 is a 38 Hz high-pass and was on the dry too).
        chanOutput =
            (chanInput * (1 - mix[channelIndex]).sqrt)
            + (LeakDC.ar(wet, 0.9995) * mix[channelIndex].sqrt);

        [chanOutput, newFeedback]
    });

    output = results.collect { |channelResult| channelResult[0] };
    feedbackOutput = results.collect { |channelResult| channelResult[1] };

    LocalOut.ar(feedbackOutput);
    Out.ar(OceanodeOutput.kr(\out), output);

}, description: "R66-style pairwise multichannel diffusion reverb", category: "Effect/Time/Reverb");
)
