(
~synthCreator.value("OceanodeLFO", {|n|
    var phase, hz, pow, bipow, phaseoffset, phasereset, skew, pw, roundness, quant, min, max, invert;
    var externalph, extphinput;
    var applyCustomPow, applyPW, applySkew, computeShape;
    var lfo;

    // --- customPow matching C++ baseOscillator::customPow ---
    // range [-1, 1]: 0 = identity, positive pushes toward +1 (or ±1 for bipolar),
    // negative pushes toward 0. Works on both unipolar [0,1] and bipolar [-1,1] signals.
    applyCustomPow = { |sig, curve|
        var k1, k2, k3;
        curve = curve.clip(-0.9999, 0.9999);
        k1 = 2 * curve * 0.99999;
        k2 = k1 / (1 - curve * 0.999999);
        k3 = (k2 * sig.abs) + 1;
        sig * (k2 + 1) / k3;
    };

    // --- Pulse Width: center-anchored, matching C++ baseOscillator ---
    // pw = 0.5 is neutral (identity). pw < 0.5: narrow window centered at 0.5.
    // pw > 0.5: expands with plateau around the skew-dependent midpoint.
    applyPW = { |ph, pw, skew|
        var narrowResult, midpoint, lo, hi, wideLower, wideUpper, wideResult;
        pw = pw.clip(0.001, 0.99999);

        // Narrow (pw < 0.5): window [0.5-pw, 0.5+pw] mapped to [0, 1], clamped outside
        narrowResult = LinLin.ar(ph, 0.5 - pw, 0.5 + pw, 0, 1).clip(0, 1);

        // Wide (pw >= 0.5): expand using skew-dependent midpoint
        // midpoint = ofMap(skew, -1, 1, 1, 0) = (1 - skew) / 2
        midpoint = LinLin.ar(skew, -1, 1, 1, 0).clip(0.00001, 0.99999);
        // lo = ofMap(1-pw, 0, 0.5, 0, midpoint) = 2*(1-pw)*midpoint
        lo = (2 * (1 - pw) * midpoint).clip(0.00001, midpoint);
        // hi = ofMap(pw, 0.5, 1, midpoint, 1) = midpoint + (2*pw-1)*(1-midpoint)
        hi = (midpoint + (2 * pw - 1) * (1 - midpoint)).clip(midpoint, 0.99999);
        wideLower = LinLin.ar(ph, 0, lo, 0, midpoint).clip(0, midpoint);
        wideUpper = LinLin.ar(ph, hi, 1, midpoint, 1).clip(midpoint, 1);
        wideResult = Select.ar((ph >= midpoint).asInteger, [wideLower, wideUpper]);

        Select.ar((pw >= 0.5).asInteger, [narrowResult, wideResult]);
    };

    // --- Skew: matching C++ baseOscillator, range [-1, 1], default 0 ---
    // 0 = symmetric (identity).  +1 = peak at start.  -1 = peak at end.
    // threshold = 0.5 - skew*0.5, allowed to reach exactly 0 or 1 at ±1.
    // Each LinLin has its own safe denominator so the degenerate (never-selected)
    // branch at the extreme values computes a bounded value rather than NaN.
    applySkew = { |ph, skew|
        var threshold, lowerPart, upperPart;
        threshold = (0.5 - skew * 0.5).clip(0, 1);
        lowerPart = LinLin.ar(ph, 0, threshold.max(1.0e-7), 0, 0.5).clip(0, 0.5);
        upperPart = LinLin.ar(ph, threshold.min(1 - 1.0e-7), 1, 0.5, 1).clip(0.5, 1);
        Select.ar((ph >= threshold).asInteger, [lowerPart, upperPart]);
    };

    // --- Shape/Roundness: matching C++ baseOscillator roundness ---
    // 0   = triangle (linear edges)
    // 0→0.5 = lerp from triangle to cosine
    // 0.5 = cosine (smooth)
    // 0.5→1 = customPow sharpening of cosine toward square
    // 1   = square
    computeShape = { |ph, roundness|
        var w, triVal, cosRaw, cosMapped, lerpBlend, lowerResult;
        var sharpenAmount, sharpenedCos, upperResult;

        w = ph * 2 * pi;
        // Triangle: 0 at ph=0, 1 at ph=0.5, 0 at ph=1
        triVal = (1 - (2 * ph - 1).abs).max(0);
        // Cosine raw [-1,1]: -1 at ph=0, +1 at ph=0.5, -1 at ph=1
        cosRaw = cos(w + pi);
        cosMapped = (cosRaw + 1) * 0.5;

        // Lower half [0, 0.5]: lerp(triangle, cosine)
        lerpBlend = LinLin.ar(roundness, 0, 0.5, 0, 1).clip(0, 1);
        lowerResult = (triVal * (1 - lerpBlend) + cosMapped * lerpBlend).clip(0, 1);

        // Upper half [0.5, 1]: sharpened cosine via customPow → square
        sharpenAmount = LinLin.ar(roundness, 0.5, 1, 0, 1).clip(0, 1);
        sharpenedCos = applyCustomPow.(cosRaw, sharpenAmount);
        upperResult = ((sharpenedCos + 1) * 0.5).clip(0, 1);

        Select.ar((roundness >= 0.5).asInteger, [lowerResult, upperResult]);
    };

    // --- Parameters (ranges and defaults match C++ oscillator) ---
    hz          = OceanodeParameterLag.ar(\hz, 5, n, 0, 20, "vf", 1/30, true);
    pow         = OceanodeParameterLag.ar(\pow, 0, n, -0.99, 0.99, "vf", 1/30, true);
    bipow       = OceanodeParameterLag.ar(\bipow, 0, n, -0.99, 0.99, "vf", 1/30, true);
    phaseoffset = OceanodeParameterLag.ar(\offset, 0, n, 0, 1, "vf", 1/30, true);
    phasereset  = OceanodeParameter.ar(\reset, 0, n, 0, 1, "vi");
    skew        = LinLin.ar(OceanodeParameterLag.ar(\skew, 0, n, -1, 1, "vf", 1/30, true), -1, 1, -1.5, 0.5);
    pw          = OceanodeParameterLag.ar(\pw, 0.5, n, 0, 1, "vf", 1/30, true);
    roundness   = OceanodeParameterLag.ar(\roundness, 0.5, n, 0, 1, "vf", 1/30, true);
    quant       = OceanodeParameter.ar(\quant, 0, n, 0, 64, "vi");
    min         = OceanodeParameterLag.ar(\min, 0, n, -128, 128, "vf", 1/30, true);
    max         = OceanodeParameterLag.ar(\max, 1, n, -128, 128, "vf", 1/30, true);
    invert      = OceanodeParameterLag.ar(\invert, 0, n, 0, 1, "vf", 1/30, true);
    externalph  = OceanodeParameter.ar(\externalph, 0, n, 0, 1, "vi");
    extphinput  = OceanodeParameterLag.ar(\extphinput, 0, n, 0, 1, "vf", 1/30, true);

    // --- Phase Generation ---
    phase = Select.ar(externalph, [
        Phasor.ar(phasereset > 0.5, hz * SampleDur.ir, 0, 1),
        extphinput
    ]);
    phase = (phase + phaseoffset).wrap(0, 1);

    // --- Waveform Computation (order matches C++ baseOscillator::computeFunc) ---

    // 1. Pulse Width (center-anchored, matching C++)
    phase = applyPW.(phase, pw, skew);

    // 2. Skew ([-1,1] range, matching C++)
    phase = applySkew.(phase, skew);

    // 3. Shape/Roundness (triangle → cosine → square, matching C++)
    lfo = computeShape.(phase, roundness);

    // 4. Pow (customPow unipolar, matching C++ pow_Param range [-1,1])
    lfo = applyCustomPow.(lfo, pow);

    // 5. BiPow (customPow bipolar, matching C++ biPow_Param)
    lfo = (lfo * 2) - 1;
    lfo = applyCustomPow.(lfo, bipow);
    lfo = (lfo + 1) * 0.5;

    // 6. Quantization
    lfo = Select.ar(K2A.ar(quant > 1), [
        lfo,
        (lfo * (quant - 1)).round / (quant - 1)
    ]);

    // 7. Inversion
    lfo = lfo * (1 - invert) + ((1 - lfo) * invert);

    // 8. Output Scaling
    lfo = lfo * (max - min) + min;

    Out.ar(OceanodeOutput.kr(\out), lfo);
}, description: "LFO matching C++ baseOscillator: roundness (tri/cos/square), center-anchored pw, skew [-1,1], customPow", category: "Modulation/LFO");
)
