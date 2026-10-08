(
~synthCreator.value("MorphLFO", {|n|
    var signals, hz, pow, amp, phaseoffset, phasereset, phase;
    var skew, pw, shape, lfo, min, max, invert, quant;
    var bipow, applyCustomPow; // New variable and function
    var externalph, extphinput;
    var getTriangle, getSine, getTrapezoid, getMultiLFO;

    // --- Helper Functions ---

    // 1. Define customPow math (translated from C++)
    // This creates a rational warping curve (sigmoid-like or inverse)
    applyCustomPow = { |sig, curve|
        var k1, k2, k3;
        // Clip curve slightly under 1.0 to prevent division by zero
        curve = curve.clip(-0.9999, 0.9999);
        k1 = 2 * curve;
        k2 = k1 / (1 - curve);
        k3 = (k2 * sig.abs) + 1;
        sig * (k2 + 1) / k3;
    };

    getTriangle = { |phase, skew|
        skew = skew.clip(0.00001, 0.99999);
        phase = phase.linlin(0, 1, skew.neg, 1 - skew);
        phase.bilin(0, skew.neg, 1 - skew, 1, 0, 0);
    };

    getSine = { |phase|
        1 - cos(phase * pi) / 2;
    };

    getTrapezoid = { |phase, shape|
        var steepness = 1 / (1 - shape);
        var trapezoid = (phase * steepness).clip(0, 1);
        var pulse = phase > 0;
        Select.ar(K2A.ar((shape |==| 1).asInteger), [trapezoid, pulse]);
    };

    getMultiLFO = { |phase, skew, shape, pw|
        var effectivePw, scaledPhase, triangle, sine, blend, sineToTriangle, trapShape, triangleToTrap;

        effectivePw = pw * (
            (shape <= 0.5) +
            ((shape > 0.5) * LinLin.ar(shape, 0.5, 1, 1, 0.5))
        );

        scaledPhase = LinLin.ar(phase, 0, 1, 0, 1/effectivePw).clip(0, 1);
        triangle = getTriangle.(scaledPhase, skew);
        sine = getSine.(triangle);
        blend = shape * 2;
        sineToTriangle = sine * (1 - blend.clip(0, 1)) + (triangle * blend.clip(0, 1));
        trapShape = (blend - 1).clip(0, 1);
        triangleToTrap = getTrapezoid.(triangle, trapShape);

        Select.ar(K2A.ar((blend < 1).asInteger), [triangleToTrap, sineToTriangle]);
    };

    // --- Parameters ---
    hz = OceanodeParameterLag.ar(\hz, 5, n, 0, 20, "vf", 1/30, true);
    pow = OceanodeParameterLag.ar(\pow, 1, n, 0.1, 4, "vf", 1/30, true);
    // New bipow parameter (Range -1 to 1 to allow warping in both directions)
    bipow = OceanodeParameterLag.ar(\bipow, 0, n, -0.99, 0.99, "vf", 1/30, true);

    phaseoffset = OceanodeParameterLag.ar(\offset, 0, n, 0, 1, "vf", 1/30, true);
    phasereset = OceanodeParameter.ar(\reset, 0, n, 0, 1, "vi");
    skew = OceanodeParameterLag.ar(\skew, 0.5, n, 0, 1, "vf", 1/30, true);
    pw = OceanodeParameterLag.ar(\pw, 1, n, 0.01, 1, "vf", 1/30, true);
    shape = OceanodeParameterLag.ar(\shape, 0.5, n, 0, 1, "vf", 1/30, true) * 0.999;
    quant = OceanodeParameter.ar(\quant, 0, n, 0, 64, "vi");
    min = OceanodeParameterLag.ar(\min, 0, n, -128, 128, "vf", 1/30, true);
    max = OceanodeParameterLag.ar(\max, 1, n, -128, 128, "vf", 1/30, true);
    invert = OceanodeParameterLag.ar(\invert, 0, n, 0, 1, "vf", 1/30, true);

    externalph = OceanodeParameter.ar(\externalph, 0, n, 0, 1, "vi");
    extphinput = OceanodeParameterLag.ar(\extphinput, 0, n, 0, 1, "vf", 1/30, true);

    // --- Processing ---

    phase = Select.ar(externalph, [
        Phasor.ar(phasereset > 0.5, hz * SampleDur.ir, 0, 1),
        extphinput
    ]);

    phase = (phase + phaseoffset).wrap(0, 1);

    lfo = getMultiLFO.(phase, skew, shape, pw);

    // 1. Standard Power (Unipolar)
    lfo = lfo.pow(pow);

    // 2. Bipow (Bipolar Shaping)
    // Map [0, 1] -> [-1, 1], apply customPow, Map back -> [0, 1]
    lfo = (lfo * 2) - 1;
    lfo = applyCustomPow.(lfo, bipow);
    lfo = (lfo + 1) * 0.5;

    // 3. Quantization
    lfo = Select.ar(K2A.ar(quant > 1), [
        lfo,
        (lfo * (quant - 1)).round / (quant - 1)
    ]);

    // 4. Inversion
    lfo = lfo * (1 - invert) + ((1 - lfo) * invert);

    // 5. Output Scaling
    lfo = lfo * (max - min) + min;

    Out.ar(OceanodeOutput.kr(\out), lfo);
}, description: "Morphable LFO with bipow shaping, pulse width control, skew, and quantization", category: "Modulation/LFO");
)