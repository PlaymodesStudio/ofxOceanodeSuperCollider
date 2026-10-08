~synthCreator.value("FluteB", {|n|
    var pch, ibreath, ifeedbk1, ifeedbk2, vibamp, vibfreq, lpf, noise, levels;
    var signalOut, ifqc, fdbckArray;
    var aflow1, kvibr, asum1, asum2, afqc, ax, apoly, asum3, avalue, aflute1;
    var sr, blockDelay, tapDelay, exciterDelay, maxHalfPeriod, maxFullPeriod;
    var fbTarget, combDecay, bExcite, bExciteLPF, pathB_raw, pathA, pathB;
    var xfadePan;

    // ── Parameters ───────────────────────────────────────────────────────────
    pch      = OceanodeParameterLag.ar(\pitch,    32,   n, 0, 128,   "vf", 1/30, true);
    ibreath  = OceanodeParameterLag.kr(\ibreath,  0.09, n, 0, 1,     "vf", 1/30, true);
    ifeedbk1 = OceanodeParameterLag.kr(\ifeedbk1, 0.4,  n, 0, 1,     "vf", 1/30, true);
    ifeedbk2 = OceanodeParameterLag.kr(\ifeedbk2, 0.4,  n, 0, 1,     "vf", 1/30, true);
    vibamp   = OceanodeParameterLag.kr(\vibamp,   0.2,  n, 0, 1,     "vf", 1/30, true);
    vibfreq  = OceanodeParameterLag.kr(\vibfreq,  5,    n, 0, 10,    "vf", 1/30, true);
    lpf      = OceanodeParameterLag.kr(\lpf,      2000, n, 0, 10000, "vf", 1/30, true);
    noise    = OceanodeParameterLag.kr(\noise,    1,    n, 0, 1,     "vf", 1/30, true);
    levels   = OceanodeParameterLag.ar(\levels,   0,    n, 0, 1,     "vf", 1/30, true);

    // ── Constants ────────────────────────────────────────────────────────────
    sr         = SampleRate.ir;
    blockDelay = BlockSize.ir / sr;
    tapDelay   = 2 / sr;
    ifqc       = pch.midicps;

    // ── Shared exciter ───────────────────────────────────────────────────────
    aflow1 = LFClipNoise.ar(sr!n, noise);
    kvibr  = SinOsc.ar(vibfreq, 0, 0.1 * vibamp);
    asum1  = (ibreath * aflow1) + noise + kvibr;

    // ════════════════════════════════════════════════════════════════════════
    // PATH A — LocalIn/LocalOut waveguide (MIDI 24–68, fully off by MIDI 74)
    //
    // afqc goes negative at MIDI ~75. We clamp it to 4/sr minimum so
    // DelayC never gets a bad value, but the crossfade ensures pathA
    // weight is already zero well before we reach that point.
    // ════════════════════════════════════════════════════════════════════════
    exciterDelay  = (asum1 / 20000) + (9 / sr) - (ifqc / 12000000);
    afqc          = ((ifqc.reciprocal - blockDelay - tapDelay - exciterDelay) / 1.5)
                        .max(4 / sr);    // clamp: DelayC minimum is 4 samples
    maxHalfPeriod = 24.midicps.reciprocal / 1.5 * 1.1;
    maxFullPeriod = maxHalfPeriod * 2;

    fdbckArray = LocalIn.ar(n);
    aflute1    = fdbckArray;
    asum2      = asum1 + (aflute1 * ifeedbk1);
    ax         = DelayC.ar(asum2,  maxHalfPeriod, afqc * 0.5);
    apoly      = ax - ax.cubed;
    asum3      = apoly + (aflute1 * ifeedbk2);
    avalue     = Sanitize.ar(LPF.ar(asum3, lpf));
    aflute1    = DelayC.ar(avalue, maxFullPeriod, afqc);
    LocalOut.ar(Sanitize.ar(aflute1));

    pathA = LeakDC.ar(avalue);

    // ════════════════════════════════════════════════════════════════════════
    // PATH B — CombC waveguide (fully on by MIDI 74, up to MIDI 127)
    // ════════════════════════════════════════════════════════════════════════
    fbTarget  = ((ifeedbk1 + ifeedbk2) * 0.5).clip(0.001, 0.9999);
    combDecay = (ifqc.reciprocal / log(fbTarget).neg) * 6.9078;

    bExcite    = asum1 - asum1.cubed;
    bExciteLPF = LPF.ar(bExcite, lpf);
    pathB_raw  = CombC.ar(bExciteLPF, 24.midicps.reciprocal, ifqc.reciprocal, combDecay);
    pathB      = LeakDC.ar(LPF.ar(Sanitize.ar(pathB_raw), lpf * 1.5));

    // ════════════════════════════════════════════════════════════════════════
    // CROSSFADE — MIDI 65 (pure A) to MIDI 74 (pure B)
    //
    // afqc is healthy up to MIDI ~74 (565 Hz, where afqc*0.5 = 4 samples).
    // Crossfade completes well before pathA degrades:
    //   MIDI 65 → xfadePan = -1 (pure A)
    //   MIDI 74 → xfadePan = +1 (pure B)
    //   centre = 69.5, half-width = 4.5 → pan = (pch - 69.5) / 4.5
    // ════════════════════════════════════════════════════════════════════════
    xfadePan  = ((pch - 69.5) / 4.5).clip(-1, 1);
    signalOut = XFade2.ar(pathA, pathB, xfadePan);

    signalOut = Limiter.ar(signalOut);
    Out.ar(OceanodeOutput.kr(\out), signalOut * levels);

}, description: "Physical modeling flute synthesizer", category: "Source/PhysicalModelling");
