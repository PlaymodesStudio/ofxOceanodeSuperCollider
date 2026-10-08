(
~synthCreator.value("FDNReverb", {|n|
    var src, monoSrc, sig, delrd, localout, deltimesSec, decayCoef;
    var wetL, wetR, sigout;
    var m, dtimes, nlines;

    // Shared FDN parameters (single-value regardless of n)
    var rt           = OceanodeParameterLag.kr(\rt,           3.0,    n, 0.1,   30.0,   "vf", 1/30, true).asArray[0];
    var ff           = OceanodeParameterLag.kr(\abspitch,   102.23,   n, 0.0,   130.0,  "vf", 1/30, true).midicps.asArray[0];
    var fq           = OceanodeParameterLag.kr(\absslope,     1.0,    n, 0.1,   4.0,    "vf", 1/30, true).asArray[0];
    var fdb          = OceanodeParameterLag.kr(\absdb,       -1.0,    n, -12.0, 0.0,    "vf", 1/30, true).asArray[0];
    var predelay     = OceanodeParameterLag.kr(\predelay,     0.0,    n, 0.0,   0.1,    "vf", 1/30, true).asArray[0];
    var wetHPPitch   = OceanodeParameterLag.kr(\hpf,   0.0,    n, 0.0,   130.0,  "vf", 1/30, true).midicps.asArray[0];
    var wetLPPitch   = OceanodeParameterLag.kr(\lpf, 120.0,    n, 0.0,   130.0,  "vf", 1/30, true).midicps.asArray[0];
    var mix          = OceanodeParameterLag.kr(\mix,          0.5,    n, 0.0,   1.0,    "vf", 1/30, true).asArray[0];

    // 16 prime-based delay times (samples @ 44100 Hz)
    // Sum ≈ 1.35 sec — enough modal density for smooth decay at any RT
    dtimes = [2791, 1327, 5443, 2957, 5077, 5417, 1601, 3541,
              1697, 3989, 5861, 4177, 2351, 3271, 1361, 3733];
    nlines = 16;

    // Hadamard 16 mixing matrix, scaled by 1/sqrt(16) = 0.25
    // Ensures energy-preserving feedback and maximum diffusion
    m = [
        [ 1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1],
        [ 1, -1,  1, -1,  1, -1,  1, -1,  1, -1,  1, -1,  1, -1,  1, -1],
        [ 1,  1, -1, -1,  1,  1, -1, -1,  1,  1, -1, -1,  1,  1, -1, -1],
        [ 1, -1, -1,  1,  1, -1, -1,  1,  1, -1, -1,  1,  1, -1, -1,  1],
        [ 1,  1,  1,  1, -1, -1, -1, -1,  1,  1,  1,  1, -1, -1, -1, -1],
        [ 1, -1,  1, -1, -1,  1, -1,  1,  1, -1,  1, -1, -1,  1, -1,  1],
        [ 1,  1, -1, -1, -1, -1,  1,  1,  1,  1, -1, -1, -1, -1,  1,  1],
        [ 1, -1, -1,  1, -1,  1,  1, -1,  1, -1, -1,  1, -1,  1,  1, -1],
        [ 1,  1,  1,  1,  1,  1,  1,  1, -1, -1, -1, -1, -1, -1, -1, -1],
        [ 1, -1,  1, -1,  1, -1,  1, -1, -1,  1, -1,  1, -1,  1, -1,  1],
        [ 1,  1, -1, -1,  1,  1, -1, -1, -1, -1,  1,  1, -1, -1,  1,  1],
        [ 1, -1, -1,  1,  1, -1, -1,  1, -1,  1,  1, -1, -1,  1,  1, -1],
        [ 1,  1,  1,  1, -1, -1, -1, -1, -1, -1, -1, -1,  1,  1,  1,  1],
        [ 1, -1,  1, -1, -1,  1, -1,  1, -1,  1, -1,  1,  1, -1,  1, -1],
        [ 1,  1, -1, -1, -1, -1,  1,  1, -1, -1,  1,  1,  1,  1, -1, -1],
        [ 1, -1, -1,  1, -1,  1,  1, -1, -1,  1,  1, -1,  1, -1, -1,  1],
    ] * 0.25;

    // Input: sum to mono, apply predelay, fan into all 16 lines
    src     = In.ar(OceanodeInput.kr(\in), n);
    monoSrc = src.asArray.mean;
    monoSrc = DelayN.ar(monoSrc, 0.1, predelay);
    sig     = monoSrc.dup(nlines) / nlines;

    // FDN feedback loop
    delrd = LocalIn.ar(nlines);
    sig   = sig + delrd;

    // Hadamard matrix mixing: distributes energy across all delay lines
    sig = m.collect({|row i| row.collect({|coef j| coef * sig[j]}).sum});

    // Per-line RT60 decay: g_i = 0.001^(dt_i / RT)
    // = exp(ln(0.001) * dt_i / RT)
    deltimesSec = dtimes / SampleRate.ir;
    decayCoef   = deltimesSec.collect({|dt| (log(0.001) * dt / rt).exp});

    // Delay + high-shelf absorption + decay coefficient per line
    // HighShelf inside loop models frequency-dependent air absorption
    localout = nlines.collect({|i|
        var d = DelayN.ar(sig[i], deltimesSec[i], deltimesSec[i] - ControlDur.ir);
        HighShelf.ar(d, ff, fq, fdb) * decayCoef[i];
    });

    LocalOut.ar(localout);

    // Stereo output: even-indexed lines → L, odd-indexed → R
    // Hadamard matrix already ensures strong L/R decorrelation
    wetL = localout.select({|it i| i.even}).sum;
    wetR = localout.select({|it i| i.odd}).sum;

    // Compensate the shelving coloration at the output (Jot 1992)
    // The absorption EQ only accumulates inside the loop, not on the dry signal
    sigout = HighShelf.ar([wetL, wetR], ff, fq, fdb.neg);

    // Normalize loudness vs RT: longer reverb accumulates more energy
    sigout = sigout * sqrt(rt).reciprocal;

    // Final wet-tail filtering happens before mixing the dry source back in
    sigout = LeakDC.ar(sigout);
    sigout = HPF.ar(sigout, wetHPPitch);
    sigout = LPF.ar(sigout, wetLPPitch);

    // XFade2: mix=0 → 100% dry (original per-channel), mix=1 → 100% wet
    sigout = Array.fill(n, {|i|
        XFade2.ar(src.asArray[i], sigout[i % 2], mix * 2 - 1)
    });

    Out.ar(OceanodeOutput.kr(\out), sigout);

}, description: "Jot FDN reverb — 16 delay lines, Hadamard mixing, RT60 decay, frequency-dependent absorption", category: "Effect/Time/Reverb");
)
