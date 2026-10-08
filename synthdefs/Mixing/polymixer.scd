(
~synthCreator.value("PolyMixer", {|n, variables|
    var sig1, sig2, sig3, sig4, sig5, sig6, sig7, sig8;
    var inputs, mixedInput, soloedInput, finalSig;
    var mute, balanceVec, calcBalanceGains;

    // Define mute vector as a regular float parameter with 8 values
    mute = OceanodeParameter.ar(\gainvec, 1, 8, 0, 1, "vf");

    // Define balance vector with 8 values (-1 to 1, default 0)
    balanceVec = OceanodeParameter.ar(\balancevec, 0, 8, -1, 1, "vf");

    // Helper function to calculate balance gains for a given balance value
    calcBalanceGains = {|balance|
        if(n == 2, {
            // STEREO: Constant-power pan law (sqrt)
            var panPos = balance * 0.5 + 0.5; // Convert -1..1 to 0..1
            var leftGain = (1 - panPos).sqrt;
            var rightGain = panPos.sqrt;
            [leftGain, rightGain];
        }, {
            // MULTICHANNEL: Aggressive quadratic tilt
            var normPositions = Array.series(n, 0, 1/(n-1).max(1));
            var balanceSq = balance.sign * (balance.abs.squared);
            var leftFavor = 1.0 - (normPositions * balanceSq.min(0).abs);
            var rightFavor = 1.0 - ((1.0 - normPositions) * balanceSq.max(0));
            (leftFavor * rightFavor).squared;
        });
    };

    // Get all inputs with balance applied
    sig1 = In.ar(OceanodeInput.kr(\in), n) * OceanodeParameterLag.ar(\levels1, 0.5, n, 0, 1, "vf", 1/30, true) * mute[0] * calcBalanceGains.(balanceVec[0]);
    sig2 = In.ar(OceanodeInput.kr(\in2), n) * OceanodeParameterLag.ar(\levels2, 0.5, n, 0, 1, "vf", 1/30, true) * mute[1] * calcBalanceGains.(balanceVec[1]);
    sig3 = In.ar(OceanodeInput.kr(\in3), n) * OceanodeParameterLag.ar(\levels3, 0.5, n, 0, 1, "vf", 1/30, true) * mute[2] * calcBalanceGains.(balanceVec[2]);
    sig4 = In.ar(OceanodeInput.kr(\in4), n) * OceanodeParameterLag.ar(\levels4, 0.5, n, 0, 1, "vf", 1/30, true) * mute[3] * calcBalanceGains.(balanceVec[3]);
    sig5 = In.ar(OceanodeInput.kr(\in5), n) * OceanodeParameterLag.ar(\levels5, 0.5, n, 0, 1, "vf", 1/30, true) * mute[4] * calcBalanceGains.(balanceVec[4]);
    sig6 = In.ar(OceanodeInput.kr(\in6), n) * OceanodeParameterLag.ar(\levels6, 0.5, n, 0, 1, "vf", 1/30, true) * mute[5] * calcBalanceGains.(balanceVec[5]);
    sig7 = In.ar(OceanodeInput.kr(\in7), n) * OceanodeParameterLag.ar(\levels7, 0.5, n, 0, 1, "vf", 1/30, true) * mute[6] * calcBalanceGains.(balanceVec[6]);
    sig8 = In.ar(OceanodeInput.kr(\in8), n) * OceanodeParameterLag.ar(\levels8, 0.5, n, 0, 1, "vf", 1/30, true) * mute[7] * calcBalanceGains.(balanceVec[7]);

    // Combine all signals into an array
    inputs = [sig1, sig2, sig3, sig4, sig5, sig6, sig7, sig8];

    // Mix inputs channel-wise to maintain multi-channel structure
    mixedInput = Array.fill(n, {|chan|
        inputs.collect {|input| input[chan]}.sum
    });

    // Solo functionality using integer parameter (0 for mix, 1-8 for solo)
    soloedInput = Select.ar(OceanodeParameter.ar(\solo, 0, n, 0, 8, "vi"),
        [mixedInput] ++ inputs
    );

    // Apply master level with a proper curve for natural level control
    finalSig = soloedInput * OceanodeParameterLag.ar(\masterlevel, 1, n, 0, 1, "vf", 1/30, true);

    // Safety limiter to prevent extreme peaks
    finalSig = Limiter.ar(finalSig);

    // Output the final signal
    Out.ar(OceanodeOutput.kr(\out), finalSig);
}, description: "Mixer with 8 inputs with level controls solo mute and balance", category: "Mixing");
)