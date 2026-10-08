~synthCreator.value("Balance", {|n, variables|
    var numSpeakers, input, balance, level, sig;
    var normPositions, rightFavor, leftFavor, balanceGains;
    var balanceSq, panPos, leftGain, rightGain;
    var inPositions, outPositions, mapped, output;
    var sumOfSquares, rms;

    numSpeakers = variables[0];

    input = In.ar(OceanodeInput.kr(\in), n);
    input = input.asArray;

    balance = OceanodeParameterLag.ar(\balance, 0, 1, -1, 1, "vf", 1/30, true);
    level = OceanodeParameterLag.ar(\levels, 1, 1, 0, 1, "vf", 1/30, true);

    // Map input channels to output channels (upmix/downmix)
    inPositions = Array.series(n, -1, 2/(n-1).max(1));
    outPositions = Array.series(numSpeakers, -1, 2/(numSpeakers-1).max(1));
    mapped = Array.fill(numSpeakers, { |outIndex|
        var outPos = outPositions[outIndex];
        var weights = Array.fill(n, { |inIndex|
            var inPos = inPositions[inIndex];
            var distance = (outPos - inPos).abs;
            if(distance < (2.0/n.max(2))) {
                (1.0 - (distance * n.max(2) / 2.0)).max(0)
            } { 0.0 }
        });
        weights = weights / weights.sum.max(0.000001);
        Mix(input * weights)
    });

    // Balance logic
    balanceGains = if(numSpeakers == 2, {
        // STEREO: constant-power sqrt pan law (inherently equal power)
        panPos = balance * 0.5 + 0.5;
        leftGain = (1 - panPos).sqrt;
        rightGain = panPos.sqrt;
        [leftGain, rightGain];
    }, {
        // MULTICHANNEL: aggressive quadratic tilt (matching polymixer)
        normPositions = Array.series(numSpeakers, 0, 1/(numSpeakers-1).max(1));
        balanceSq = balance.sign * (balance.abs ** 2);
        leftFavor  = 1.0 - (normPositions * balanceSq.min(0).abs);
        rightFavor = 1.0 - ((1.0 - normPositions) * balanceSq.max(0));
        (leftFavor * rightFavor) ** 2;
    });

    // Equal power normalization:
    // sum(g_i^2) / numSpeakers = 1 at all balance positions
    // (stereo sqrt law already satisfies this, but normalize anyway for consistency)
    sumOfSquares = balanceGains.collect({|g| g * g}).sum;
    rms = (sumOfSquares / numSpeakers).sqrt.max(0.000001);
    balanceGains = balanceGains / rms;

    output = mapped * balanceGains * level;

    Out.ar(OceanodeOutput.kr(\out), output);

}, description: "Multichannel balance with equal power", category: "Spatial/Panning", variables: ["NumSpeakers"], variableDimensions: [16]);
