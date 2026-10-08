(
~synthCreator.value("EchoBlur", {|n|
    var input, localFeedback, results, output, feedbackOutput;
    var time, feedback, blur, blurtime, spread, lowpass, hipass, mix;
    var reaktorAllpass;
    var maxEchoDelay = 4.0;
    var maxBlurDelay = 0.5;

    /*
    EchoBlur
    --------
    A dispersive feedback delay derived from the R66 investigation.

    Signal flow per channel:

        dry input -------------------------------> dry/wet mix
             |
             +-> main echo delay -> audible echo
                                   |
                                   +-> four-stage allpass diffuser
                                   |
                                   +-> dry/diffused feedback blend
                                   +-> feedback filters
                                   +-> feedback gain -> LocalOut

    The first echo stays clear. On every circulation, the feedback signal
    passes through the diffuser again, so later repeats become progressively
    more blurred.

    Multichannel strategy:
    - Every channel owns an independent feedback loop.
    - All parameters are n-channel Oceanode vectors.
    - The allpass-delay family receives a small deterministic offset per
      channel, controlled by Spread, to avoid identical resonances across
      multichannel outputs.
    - Spread = 0 makes all channels use the same diffuser geometry.
    */

    // Oceanode parameters.
    time = OceanodeParameterLag.ar(
        \time, 350, n, 2, 4000, "vf", 1/30, true
    ).asArray; // milliseconds

    feedback = OceanodeParameterLag.ar(
        \feedback, 0.65, n, 0, 0.995, "vf", 1/30, true
    ).asArray;

    blur = OceanodeParameterLag.ar(
        \blur, 0.65, n, 0, 1, "vf", 1/30, true
    ).asArray;

    blurtime = OceanodeParameterLag.ar(
        \blurtime, 8, n, 0.1, 100, "vf", 1/30, true
    ).asArray; // milliseconds

    spread = OceanodeParameterLag.ar(
        \spread, 1, n, 0, 1, "vf", 1/30, true
    ).asArray;

    lowpass = OceanodeParameterLag.ar(
        \lowpass, 12000, n, 100, 20000, "vf", 1/30, true
    ).asArray;

    hipass = OceanodeParameterLag.ar(
        \hipass, 20, n, 10, 5000, "vf", 1/30, true
    ).asArray;

    mix = OceanodeParameterLag.ar(
        \mix, 0.33, n, 0, 1, "vf", 1/30, true
    ).asArray;

    input = In.ar(OceanodeInput.kr(\in), n).asArray;
    localFeedback = LocalIn.ar(n).asArray;

    /*
    Reaktor-style no-interpolation diffuser delay:

        y[n] = g*x[n] + x[n-D] - g*y[n-D]

    AllpassN uses the opposite recursive sign and exposes decay time rather
    than g. The extra SampleDur reproduces the one-sample offset measured in
    Reaktor's Diffuser Delay with interpolation disabled.
    */
    reaktorAllpass = { |signal, delaySeconds, coefficient|
        var delay = (delaySeconds + SampleDur.ir).clip(
            SampleDur.ir,
            maxBlurDelay
        );
        var coef = coefficient.clip(0.0001, 0.999);
        var decayTime = delay * (0.001.log / coef.log) * -1;

        AllpassN.ar(signal, maxBlurDelay, delay, decayTime)
    };

    results = Array.fill(n, { |channelIndex|
        var chanInput = input[channelIndex];
        var requestedTime = (time[channelIndex] * 0.001).clip(
            ControlDur.ir + SampleDur.ir,
            maxEchoDelay
        );
        var compensatedFeedbackTime = (
            requestedTime - ControlDur.ir
        ).clip(SampleDur.ir, maxEchoDelay);

        var firstEcho;
        var recirculatedEcho;
        var echo;

        var channelSemitoneOffset;
        var channelScale;
        var baseBlurTime;
        var stageRatios = [1.0, 1.25992105, 1.58740105, 2.0];
        var diffuserCoefficient;
        var blurred;
        var feedbackSignal;
        var newFeedback;
        var chanOutput;

        /*
        Keep the first echo at the requested Time while compensating only the
        recirculating path for LocalIn/LocalOut's one-block latency.
        */
        firstEcho = DelayN.ar(
            chanInput,
            maxEchoDelay,
            requestedTime
        );

        recirculatedEcho = DelayN.ar(
            localFeedback[channelIndex],
            maxEchoDelay,
            compensatedFeedbackTime
        );

        echo = firstEcho + recirculatedEcho;

        /*
        Deterministic channel decorrelation, bounded to approximately
        +/-0.64 semitones at Spread = 1. This changes only the short allpass
        times; it does not alter the principal echo period.
        */
        channelSemitoneOffset = if(n == 1) {
            0
        } {
            ((((channelIndex * 7) % 17) - 8) * 0.08)
            * spread[channelIndex]
        };

        channelScale = 2.pow(channelSemitoneOffset / 12);

        // Blur Time is the shortest allpass delay. The other stages use the
        // geometrical delay family recovered from the R66 diffuser research.
        baseBlurTime = (
            blurtime[channelIndex] * 0.001 * channelScale
        ).clip(SampleDur.ir, maxBlurDelay * 0.45);

        // Higher Blur increases both the contribution and recursion of the
        // diffuser. The crossfade below guarantees a true bypass at Blur = 0.
        diffuserCoefficient = (
            0.45 + (blur[channelIndex] * 0.4)
        ).clip(0.0001, 0.9);

        blurred = echo;
        stageRatios.do { |ratio|
            blurred = reaktorAllpass.(
                blurred,
                baseBlurTime * ratio,
                diffuserCoefficient
            )
        };

        /*
        Linear interpolation keeps the feedback-path magnitude bounded by the
        two unity-magnitude endpoints. Blur = 0 is a normal feedback delay;
        Blur = 1 sends the fully diffused signal around the loop.
        */
        feedbackSignal = echo + (
            (blurred - echo) * blur[channelIndex]
        );

        // Filters live inside the loop, so later repeats become progressively
        // darker and/or thinner.
        feedbackSignal = LPF.ar(
            feedbackSignal,
            lowpass[channelIndex].clip(100, SampleRate.ir * 0.45)
        );

        feedbackSignal = HPF.ar(
            feedbackSignal,
            hipass[channelIndex].clip(10, SampleRate.ir * 0.45)
        );

        newFeedback = LeakDC.ar(feedbackSignal)
            * feedback[channelIndex].clip(0, 0.995);

        // The audible wet signal is the main echo. Its later repetitions have
        // already accumulated more diffusion on each trip around the loop.
        chanOutput =
            (chanInput * (1 - mix[channelIndex]).sqrt)
            + (echo * mix[channelIndex].sqrt);

        [chanOutput, newFeedback]
    });

    output = results.collect { |channelResult| channelResult[0] };
    feedbackOutput = results.collect { |channelResult| channelResult[1] };

    LocalOut.ar(feedbackOutput);
    Out.ar(OceanodeOutput.kr(\out), output);

}, description: "Feedback delay whose repeats become progressively diffused", category: "Effect/Time/Delay");
)
