(
~synthCreator.value("Blur", {|n|
    var input, output;
    var time, blur, density, spread, hipass, lowpass, mix;
    var allpassStage;
    var maxStageDelay = 0.35;
    var stageRatios = [
        1.0,
        1.12246205,
        1.25992105,
        1.41421356,
        1.58740105,
        1.78179744,
        2.0,
        2.24492410
    ];

    /*
    Blur
    ----
    Real-time multichannel transient smearing based on a serial allpass
    diffusion network.

    Per channel:

        dry input --------------------------------------------> dry/wet mix
             |
             +-> AP1 -> AP2 -> ... -> AP8
                    ^                    |
                    |                    +-> selectable blur depth
                    +------ Time / Density
                                         |
                                         +-> high-pass -> low-pass -> wet

    Blur continuously selects how deeply the signal travels through the
    eight-stage diffuser:

        Blur = 0   : undiffused signal
        Blur = 1   : all eight diffusion stages

    There is no outer feedback loop and no explicit echo delay. Each allpass
    stage has its own recursive tail, so attacks are redistributed over time
    without turning the effect into a conventional delay or reverb.

    Every Oceanode channel is processed independently. Spread introduces a
    small deterministic time-scale offset per channel, avoiding identical
    diffusion patterns in multichannel systems.
    */

    // Shortest allpass delay, in milliseconds.
    time = OceanodeParameterLag.ar(
        \time, 8, n, 0.1, 100, "vf", 1/30, true
    ).asArray;

    // Continuous depth through the eight-stage diffuser.
    blur = OceanodeParameterLag.ar(
        \blur, 0.75, n, 0, 1, "vf", 1/30, true
    ).asArray;

    // Controls the allpass recursive coefficient and therefore tail density.
    density = OceanodeParameterLag.ar(
        \density, 0.55, n, 0, 1, "vf", 1/30, true
    ).asArray;

    // Deterministic decorrelation between channels.
    spread = OceanodeParameterLag.ar(
        \spread, 1, n, 0, 1, "vf", 1/30, true
    ).asArray;

    // Wet-path filters, always in series.
    hipass = OceanodeParameterLag.ar(
        \hipass, 20, n, 20, 20000, "vf", 1/30, true
    ).asArray;

    lowpass = OceanodeParameterLag.ar(
        \lowpass, 20000, n, 20, 20000, "vf", 1/30, true
    ).asArray;

    // Equal-power dry/wet mix.
    mix = OceanodeParameterLag.ar(
        \mix, 0.5, n, 0, 1, "vf", 1/30, true
    ).asArray;

    input = In.ar(OceanodeInput.kr(\in), n).asArray;

    /*
    Schroeder allpass stage using cubic delay interpolation. Unlike the R66
    clone, Blur is a new effect rather than a forensic replica, so cubic
    interpolation is used to keep Time changes smooth in real time.

    The coefficient is expressed directly as a musically useful 0.25..0.72
    range. A negative decay time gives the Reaktor-style recursive sign:

        y[n] = g*x[n] + x[n-D] - g*y[n-D]
    */
    allpassStage = { |signal, delaySeconds, coefficient|
        var delay = delaySeconds.clip(SampleDur.ir, maxStageDelay);
        var coef = coefficient.clip(0.0001, 0.9999);
        var decayTime = delay * (0.001.log / coef.log) * -1;

        AllpassC.ar(signal, maxStageDelay, delay, decayTime)
    };

    output = Array.fill(n, { |channelIndex|
        var channelOffset;
        var channelScale;
        var baseTime;
        var coefficient;
        var stageSignal;
        var stageOutputs;
        var wet;
        var hp;
        var lp;

        // Small non-random pitch-domain offset, stable across restarts.
        channelOffset = if(n == 1) {
            0
        } {
            ((((channelIndex * 7) % 17) - 8) * 0.08)
            * spread[channelIndex]
        };

        channelScale = 2.pow(channelOffset / 12);

        baseTime = (
            time[channelIndex] * 0.001 * channelScale
        ).clip(SampleDur.ir, maxStageDelay / stageRatios.last);

        // Higher Density produces longer recursive diffusion tails.
        coefficient = density[channelIndex].linlin(0, 1, 0.25, 0.72);

        stageSignal = input[channelIndex];
        stageOutputs = [stageSignal];

        stageRatios.do { |ratio|
            stageSignal = allpassStage.(
                stageSignal,
                baseTime * ratio,
                coefficient
            );

            stageOutputs = stageOutputs.add(stageSignal);
        };

        // Smoothly move from input through AP1...AP8.
        wet = SelectX.ar(
            blur[channelIndex] * (stageOutputs.size - 1),
            stageOutputs
        );

        hp = hipass[channelIndex].clip(20, SampleRate.ir * 0.45);
        lp = lowpass[channelIndex].clip(20, SampleRate.ir * 0.45);

        wet = HPF.ar(wet, hp);
        wet = LPF.ar(wet, lp.max(hp + 1));

        // Exact equal-power dry/wet crossfade used elsewhere in this family.
        (input[channelIndex] * (1 - mix[channelIndex]).sqrt)
        + (wet * mix[channelIndex].sqrt)
    });

    Out.ar(OceanodeOutput.kr(\out), output);

}, description: "Real-time multichannel allpass diffusion smear", category: "Effect/Time/Blur");
)
