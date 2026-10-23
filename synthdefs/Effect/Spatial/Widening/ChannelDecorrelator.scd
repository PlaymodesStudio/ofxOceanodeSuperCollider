(
~synthCreator.value("ChannelDecorrelator", {|n|
    var input, output;
    var time, depth, diffusion, spread, width;
    var allpassStage;
    var maxStageDelay = 0.12;
    var stageRatios = [
        1.0,
        1.371,
        1.793,
        2.417,
        3.137,
        4.019
    ];

    /*
    Channel Decorrelator
    --------------------
    Static multichannel widening through independent allpass diffusion.

    Each channel receives a different, deterministic phase response, but
    channels are never mixed together. This makes it useful for widening a
    signal that has been duplicated across several outputs while preserving
    the amplitude spectrum of each fully wet channel.

        input ch. i -> AP1 -> AP2 -> ... -> AP6 -> Width crossfade

    - Time controls the basic allpass-delay scale.
    - Depth moves continuously from no stages to all six stages.
    - Diffusion controls the recursive coefficient of the allpasses.
    - Spread controls how different the delay pattern is for every channel.
    - Width crossfades from the original channel to its decorrelated version.

    There is no inter-channel crossfeed, modulation, outer feedback, filtering,
    or added random state. The channel differences are deterministic and remain
    stable across restarts.
    */

    // Basic delay scale of the first allpass stage, in milliseconds.
    time = OceanodeParameterLag.ar(
        \time, 1.2, n, 0.05, 20, "vf", 1/30, true
    ).asArray;

    // Continuous traversal through the six-stage decorrelation chain.
    depth = OceanodeParameterLag.ar(
        \depth, 1, n, 0, 1, "vf", 1/30, true
    ).asArray;

    // Allpass recursion strength. The useful coefficient range is constrained
    // internally to avoid both weak decorrelation and long reverb-like tails.
    diffusion = OceanodeParameterLag.ar(
        \diffusion, 0.5, n, 0, 1, "vf", 1/30, true
    ).asArray;

    // Amount of deterministic timing difference between output channels.
    spread = OceanodeParameterLag.ar(
        \spread, 1, n, 0, 1, "vf", 1/30, true
    ).asArray;

    // Equal-power original/decorrelated crossfade.
    width = OceanodeParameterLag.ar(
        \width, 1, n, 0, 1, "vf", 1/30, true
    ).asArray;

    input = In.ar(OceanodeInput.kr(\in), n).asArray;

    /*
    Delay-based Schroeder allpass. Cubic interpolation keeps Time and Spread
    changes smooth. The coefficient sign alternates between stages/channels,
    increasing phase diversity without changing the magnitude response.
    */
    allpassStage = { |signal, delaySeconds, coefficient|
        var delay = delaySeconds.clip(SampleDur.ir, maxStageDelay);
        var coef = coefficient.clip(-0.95, 0.95);
        var safeMagnitude = coef.abs.max(0.0001);
        var decayTime =
            delay
            * (0.001.log / safeMagnitude.log)
            * coef.sign;

        AllpassC.ar(signal, maxStageDelay, delay, decayTime)
    };

    output = Array.fill(n, { |channelIndex|
        var baseTime;
        var coefficientMagnitude;
        var stageSignal;
        var stageOutputs;
        var decorrelated;

        baseTime = (time[channelIndex] * 0.001).clip(
            SampleDur.ir,
            maxStageDelay / stageRatios.last
        );

        coefficientMagnitude = diffusion[channelIndex].linlin(
            0, 1,
            0.18, 0.68
        );

        stageSignal = input[channelIndex];
        stageOutputs = [stageSignal];

        stageRatios.do { |ratio, stageIndex|
            var hash;
            var channelVariation;
            var delay;
            var sign;
            var coefficient;

            // Stable pseudo-random value in approximately -1...1, calculated
            // at SynthDef-build time from channel and stage indices.
            hash = (
                ((((channelIndex + 1) * (stageIndex + 3) * 17) % 29) - 14)
                / 14
            );

            // At Spread = 0 every channel has the same timing. At Spread = 1,
            // every stage receives up to roughly +/-15% deterministic offset.
            channelVariation = 1 + (
                hash * 0.15 * spread[channelIndex]
            );

            delay = (
                baseTime * ratio * channelVariation
            ).clip(SampleDur.ir, maxStageDelay);

            // Alternating signs generate complementary phase responses while
            // retaining an allpass magnitude response.
            sign = if(((channelIndex + stageIndex) % 2) == 0) {
                1
            } {
                -1
            };

            coefficient = coefficientMagnitude * sign;

            stageSignal = allpassStage.(
                stageSignal,
                delay,
                coefficient
            );

            stageOutputs = stageOutputs.add(stageSignal);
        };

        // Depth = 0 is the unprocessed input; Depth = 1 uses all six stages.
        decorrelated = SelectX.ar(
            depth[channelIndex] * (stageOutputs.size - 1),
            stageOutputs
        );

        // Equal-power transition. At Width = 1, the result remains allpass-only
        // and therefore has a flat magnitude response in isolation.
        (input[channelIndex] * (1 - width[channelIndex]).sqrt)
        + (decorrelated * width[channelIndex].sqrt)
    });

    Out.ar(OceanodeOutput.kr(\out), output);

}, description: "Static multichannel allpass decorrelator for channel widening", category: "Effect/Spatial/Widening");
)
