(
~synthCreator.value("PlateReverb", {|n|
    var src, monoSrc, input, local, er;
    var input_diff;
    var dltimes;
    var tank0, tank1, tankdelays0, tankdelays1, outdelaysL, outdelaysR;
    var n_out_1, n_out_2, n_out_3, n_out_4, n_out_5, n_out_6;
    var wetL, wetR, sig;

    // Parameters — all use .asArray[0] because the Dattorro algorithm is
    // inherently mono-internal (n input channels summed to mono, stereo output).
    // Using n in OceanodeParameterLag keeps Oceanode UI registration correct;
    // .asArray[0] ensures we always get a scalar for the internal UGen graph.
    var predelay     = OceanodeParameterLag.kr(\predelay,   0.0,   n, 0.0,    0.08,    "vf", 1/30, true).asArray[0];
    var bandwidth    = OceanodeParameterLag.kr(\bandwidth,  0.75,  n, 0.0,    1.0,     "vf", 1/30, true).asArray[0];
    var decay        = OceanodeParameterLag.kr(\decay,      0.7,   n, 0.0,    0.9999,  "vf", 1/30, true).asArray[0];
    var damping      = OceanodeParameterLag.kr(\damping,    0.5,   n, 0.0,    1.0,     "vf", 1/30, true).asArray[0];
    var input_diff_1 = OceanodeParameterLag.kr(\indiff1,    1.0,   n, 0.001,  3.0,     "vf", 1/30, true).asArray[0];
    var input_diff_2 = OceanodeParameterLag.kr(\indiff2,    1.0,   n, 0.001,  3.0,     "vf", 1/30, true).asArray[0];
    var decay_diff_1 = OceanodeParameterLag.kr(\decdiff1,   1.0,   n, 0.001,  3.0,     "vf", 1/30, true).asArray[0];
    var decay_diff_2 = OceanodeParameterLag.kr(\decdiff2,   1.0,   n, 0.001,  3.0,     "vf", 1/30, true).asArray[0];
    var tdelscale    = OceanodeParameterLag.kr(\tdelscale,  1.0,   n, 0.1,    2.0,     "vf", 1/30, true).asArray[0];
    var mod          = OceanodeParameterLag.kr(\mod,        0.05,  n, 0.0,    1.0,     "vf", 1/30, true).asArray[0];
    var tank_hpf     = OceanodeParameterLag.kr(\tankhpf,    80.0,  n, 20.0,   2000.0,  "vf", 1/30, true).asArray[0];
    var mix          = OceanodeParameterLag.kr(\mix,         0.5,   n, 0.0,    1.0,     "vf", 1/30, true).asArray[0];
    var er_lev       = OceanodeParameterLag.kr(\er,         0.0,   n, 0.0,    1.0,     "vf", 1/30, true).asArray[0];
    var er_send      = OceanodeParameterLag.kr(\ersend,    -1.0,   n, -1.0,   1.0,     "vf", 1/30, true).asArray[0];
    var lpfRefl      = OceanodeParameterLag.kr(\lpfrefl,    0.7,   n, 0.0,    0.999,   "vf", 1/30, true).asArray[0];
    var hpfRefl      = OceanodeParameterLag.kr(\hpfrefl,    40.0,  n, 20.0,   5000.0,  "vf", 1/30, true).asArray[0];

    // Base tank delay times (in seconds), derived from Dattorro (1997)
    // [672, 4453, 1800, 3720] / 27961
    tankdelays0 = [0.022579886428547, 0.1496253486106, 0.060481838647895, 0.12499579987232];
    // [908, 4217, 2656, 3163] / 27961
    tankdelays1 = [0.030509727495716, 0.14169550754343, 0.089244313027116, 0.10628003091294];

    // Input
    src     = In.ar(OceanodeInput.kr(\in), n);
    monoSrc = src.asArray.mean;

    // Early reflections (stereo output)
    // er_send: -1 = dry feeds reverb, +1 = ER feeds reverb
    er = Reflector.ar(
        input:       monoSrc,
        numReflcs:   5,
        delayOffset: 0.01,
        scaleDelays: 1,
        spread:      1,
        reflPan:     0,
        lpfRefl:     lpfRefl,
        hpfRefl:     hpfRefl,
    );

    // Input stage: bandwidth lowpass (Integrator) + predelay
    input = Integrator.ar(
        DelayC.ar(
            LinXFade2.ar(monoSrc, er.mean, er_send) * bandwidth,
            0.08, predelay
        ),
        coef: 1 - bandwidth
    );

    // Input diffusion: 4 cascaded allpass filters
    // [142, 107, 379, 277] / 29761
    dltimes    = [0.0047713450488895, 0.0035953092974026, 0.012734787137529, 0.0093074829474816];
    input_diff = [input_diff_1, input_diff_2];
    dltimes.do { |dt i|
        input = AllpassN.ar(input, dt, dt, decaytime: input_diff[i.div(2)]);
    };

    // Cross-coupled tank feedback
    local = LocalIn.ar(2);

    // ===== Tank 0 (fed by tank1 via local[1]) =====

    // Modulated allpass diffuser
    // maxDelay 0.4 >> actual ~0.023, matching original
    n_out_1 = AllpassC.ar(
        input + local[1],
        0.4,
        tankdelays0[0] + LFNoise2.ar(mod, mul: 0.00025),
        decaytime: decay_diff_1
    );

    // Long delay (scaled by tdelscale for room size)
    // maxDelay: base(0.150)*scale(2) + mod(0.150)*mod(1) = 0.45 -> use 0.55
    tank0 = DelayC.ar(
        n_out_1.neg,
        0.55,
        tankdelays0[1] * tdelscale + LFNoise2.ar(mod, mul: tankdelays0[1] * mod)
    );

    // Damping lowpass (Integrator) + decay gain
    n_out_2 = Integrator.ar(tank0 * (1 - damping), damping) * decay;

    // Second allpass diffuser with HPF to prevent low-end buildup
    // maxDelay: base(0.060)*2 + mod(0.060)*1 = 0.181 -> use 0.2
    n_out_3 = AllpassC.ar(
        HPF.ar(n_out_2, tank_hpf),
        0.2,
        tankdelays0[2] + LFNoise2.ar(mod, mul: tankdelays0[2] * mod),
        decaytime: decay_diff_2
    );

    // Long delay output tap (scaled by tdelscale)
    // maxDelay: base(0.125)*scale(2) + mod(0.125)*1 = 0.375 -> use 0.5
    tank0 = DelayC.ar(
        n_out_3,
        0.5,
        (tankdelays0[3] * tdelscale - ControlDur.ir + LFNoise2.ar(mod, mul: tankdelays0[3] * mod)).max(0.0001)
    ) * decay;

    // ===== Tank 1 (fed by tank0 via local[0]) =====

    n_out_4 = AllpassC.ar(
        input + local[0],
        0.4,
        tankdelays1[0] + LFNoise2.kr(mod, mul: 0.00018),
        decaytime: decay_diff_1
    );

    // maxDelay: base(0.142)*2 + mod(0.142)*1 = 0.425 -> use 0.55
    tank1 = DelayC.ar(
        n_out_4.neg,
        0.55,
        tankdelays1[1] * tdelscale + LFNoise2.ar(mod, mul: tankdelays1[1] * mod)
    );

    n_out_5 = Integrator.ar(tank1 * (1 - damping), damping) * decay;

    // maxDelay: base(0.089)*2 + mod(0.089)*1 = 0.268 -> use 0.3
    n_out_6 = AllpassC.ar(
        HPF.ar(n_out_5, tank_hpf) * decay,
        0.3,
        tankdelays1[2] + LFNoise2.ar(mod, mul: tankdelays1[2] * mod),
        decaytime: decay_diff_2
    );

    // maxDelay: base(0.106)*2 + mod(0.106)*1 = 0.318 -> use 0.45
    tank1 = DelayC.ar(
        n_out_6,
        0.45,
        (tankdelays1[3] * tdelscale - ControlDur.ir + LFNoise2.ar(mod, mul: tankdelays1[3] * mod)).max(0.0001)
    ) * decay;

    // Feed back into each other's input (cross-coupled)
    LocalOut.ar([tank0 * -1, tank1 * -1]);

    // 7-tap stereo output matrix (Dattorro 1997)
    // [266, 2974, 1913, 1996, 1990, 187, 1066] / 29761
    outdelaysL = [0.0089378717113, 0.099929437854911, 0.064278754074124, 0.067067638856221, 0.066866032727395, 0.0062833910150869, 0.035818688888142];
    // [353, 3627, 1228, 2673, 2111, 335, 121] / 29761
    outdelaysR = [0.011861160579282, 0.12187090487551, 0.041262054366453, 0.089815530392124, 0.070931756325392, 0.011256342192803, 0.0040657235979974];

    wetL = Mix([
        DelayN.ar(n_out_4, outdelaysL[0]),
        DelayN.ar(n_out_4, outdelaysL[1]),
        DelayN.ar(n_out_5, outdelaysL[2]).neg,
        DelayN.ar(n_out_6, outdelaysL[3]),
        DelayN.ar(n_out_1, outdelaysL[4]).neg,
        DelayN.ar(n_out_2, outdelaysL[5]).neg,
        DelayN.ar(n_out_3, outdelaysL[6]).neg
    ]);

    wetR = Mix([
        DelayN.ar(n_out_1, outdelaysR[0]),
        DelayN.ar(n_out_1, outdelaysR[1]),
        DelayN.ar(n_out_2, outdelaysR[2]).neg,
        DelayN.ar(n_out_3, outdelaysR[3]),
        DelayN.ar(n_out_4, outdelaysR[4]).neg,
        DelayN.ar(n_out_5, outdelaysR[5]).neg,
        DelayN.ar(n_out_6, outdelaysR[6]).neg
    ]);

    // Wet = reverb tail + early reflections
    // XFade2: mix=0 → 100% dry (original per-channel), mix=1 → 100% wet
    sig = LeakDC.ar([wetL, wetR] + (er * er_lev));
    sig = Array.fill(n, {|i|
        XFade2.ar(src.asArray[i], sig[i % 2], mix * 2 - 1)
    });

    Out.ar(OceanodeOutput.kr(\out), sig);

}, description: "Dattorro plate reverb — two cross-coupled tanks with 7-tap stereo output and early reflections", category: "Effect/Time/Reverb");
)
