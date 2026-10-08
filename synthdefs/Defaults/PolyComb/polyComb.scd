// polyComb.scd — polyphonic comb-filtered subtractive synth
// N voices → N output channels.  Uses ~synthCreator → "polyComb1".."polyComb16".
//
// Signal chain per voice:
//   Oscillator bank (w/ FM + vibrato) → Main filter → Comb → RingMod
//   → PostFilter → Tremolo → SineShaper → Master level
//
// 4 audio-rate morph LFOs, with per-voice phase offsets computed in C++.
// ModMatrix: 8 sources × 8 targets: 4 internal LFOs + 4 external LFO vectors.
// Targets: 0=pitch, 1=fmAmp, 2=mainCutMod, 3=combPitchMod,
//          4=tremHzMod, 5=ringModMix, 6=ssMix, 7=oscAmp

~synthCreator.value("polyComb", {|n|

    // ─── ALL VAR DECLARATIONS (SC requires these before any statements) ────────

    // helpers
    var applyPulseWidth, applySkew, customPow, computeShape, applyPow, applyBiPow, superSawDetuneCurve;

    // master
    var masterLevel;

    // lfos
    var lfos, extLfos, modMat, modFor, lfoReset;

    // pitch
    var pitch, pitchMod, pitchModRange, pitchDetune, pitchTranspose, vibFreq, vibAmp, vibrato, pitchHz, pitchDetuneRand;

    // fm
    var fmRatio, fmAmp, fmMod, fmAmpTotal, pitchWithFM;

    // oscillators
    var triLevel, sawLevel, pulseLevel, sineLevel;
    var whiteNLevel, pinkNLevel, dustLevel, sSawLevel, sSawDetune, impulseLevel;
    var pulseWidth, oscAmp;
    var detuneFactor, sSaw, osc, oscAmpTotal;

    // main filter
    var filterType, cutoffMin, cutoffMax, cutoffMod, resonance;
    var cutoffModTotal, filterFreq, filterQ, filterRq;
    var fLPFPlain, fHPFPlain, fLPFRes, fHPFRes, fLPF, fHPF, fBPF, fBRF, ft, mainFiltered;

    // comb
    var combPitch, combPitchMod, combPMRange, combDetune, combTranspose, combFeed, combMix;
    var combPitchModulated, combFreq, combDelay;
    var combFeedback, combFF, combFB, combOut, combDelayed, afterComb;

    // ring mod
    var ringModPitch, ringModDetune, ringModMix, ringMixTotal, ringModSig, afterRingMod;

    // post filter
    var postFType, postCutMin, postCutMax, postCutMod, postRes;
    var postFreq, postQ, postRq;
    var pLPFPlain, pHPFPlain, pLPFRes, pHPFRes, pLPF, pHPF, pBPF, pBRF, pft, afterPostFilter;

    // tremolo
    var minTremHz, maxTremHz, tremHzMod, tremAmp;
    var tremModTotal, tremHz, tremLFO, afterTremolo;

    // sine shaper
    var ssLevel, ssMix, ssMixTotal, shaped, afterSS;

    // ─── LFO SHAPE HELPERS (based on baseOscillator) ───────────────────────────

    applyPulseWidth = { |ph, pulseWidth, skew, pulseCentered|
        var midpoint, leftMax, rightMin, centeredNarrowed, leftNarrowed, narrowed, widenedLo, widenedHi, widened;
        pulseWidth = pulseWidth.clip(0, 1);
        midpoint   = LinLin.ar(skew, -1, 1, 1, 0).clip(0, 1);
        leftMax    = LinLin.ar(1 - pulseWidth, 0, 0.5, 0, midpoint).clip(0, 1);
        rightMin   = LinLin.ar(pulseWidth, 0.5, 1, midpoint, 1).clip(0, 1);
        centeredNarrowed = LinLin.ar(ph, (0.5 - pulseWidth).clip(0, 1), (0.5 + pulseWidth).clip(0, 1), 0, 1).clip(0, 1);
        leftNarrowed = LinLin.ar(ph, 0, (pulseWidth * 2).max(1.0e-7).min(1), 0, 1).clip(0, 1);
        narrowed   = Select.ar(K2A.ar(pulseCentered > 0.5), [leftNarrowed, centeredNarrowed]);
        narrowed   = Select.ar(((skew < 0) * (narrowed >= 0.99999)).asInteger, [narrowed, DC.ar(0)]);
        widenedLo  = LinLin.ar(ph, 0, leftMax.max(1.0e-7), 0, midpoint).clip(0, 1);
        widenedHi  = LinLin.ar(ph, rightMin.min(1 - 1.0e-7), 1, midpoint, 1).clip(0, 1);
        widened    = Select.ar((ph < midpoint).asInteger, [widenedHi, widenedLo]);
        Select.ar((pulseWidth < 0.5).asInteger, [
            Select.ar((pulseWidth >= 0.99999).asInteger, [widened, midpoint]),
            narrowed
        ])
    };

    applySkew = { |ph, skew|
        var threshold = (0.5 - (skew * 0.5)).clip(0, 1);
        var lo = LinLin.ar(ph, 0, threshold.max(1.0e-7), 0, 0.5).clip(0, 0.5);
        var hi = LinLin.ar(ph, threshold.min(1 - 1.0e-7), 1, 0.5, 1).clip(0.5, 1);
        Select.ar((ph >= threshold).asInteger, [lo, hi])
    };

    customPow = { |value, powAmt|
        var k1, k2, k3;
        k1 = 2 * powAmt * 0.99999;
        k2 = k1 / ((powAmt.neg * 0.999999) + 1);
        k3 = (k2 * value.abs) + 1;
        value * (k2 + 1) / k3.max(1.0e-6)
    };

    computeShape = { |ph, roundness|
        var w, triVal, cosRaw, cosMapped, upperRaw;
        w         = ph * 2 * pi;
        triVal    = (1 - (2 * ph - 1).abs).max(0);
        cosRaw    = cos(w + pi);
        cosMapped = LinLin.ar(cosRaw, -1, 1, 0, 1).clip(0, 1);
        upperRaw  = customPow.(cosRaw, (roundness - 0.5) * 2);
        Select.ar(((roundness >= 0.5) + (roundness >= 1)).clip(0, 2).asInteger, [
            LinLin.ar(roundness, 0, 0.5, triVal, cosMapped).clip(0, 1),
            LinLin.ar(upperRaw, -1, 1, 0, 1).clip(0, 1),
            ((ph >= 0.25) * (ph < 0.75))
        ])
    };

    applyPow = { |value, powAmt|
        var curved, reflected;
        curved = customPow.(value, powAmt);
        reflected = curved + ((curved < 0) * (0 - curved)) + ((curved > 1) * (1 - curved));
        Select.ar((powAmt.abs > 0.0001).asInteger, [value, reflected])
    };

    applyBiPow = { |value, biPowAmt|
        var bipolar, curved;
        bipolar = (value * 2) - 1;
        curved = customPow.(bipolar, biPowAmt);
        curved = ((curved + 1) * 0.5);
        curved = curved + ((curved < 0) * (0 - curved)) + ((curved > 1) * (1 - curved));
        Select.ar((biPowAmt.abs > 0.0001).asInteger, [value, curved])
    };

    superSawDetuneCurve = { |x|
        (10028.7312891634 * x.pow(11)) -
        (50818.8652045924 * x.pow(10)) +
        (111363.4808729368 * x.pow(9)) -
        (138150.6761080548 * x.pow(8)) +
        (106649.6679158292 * x.pow(7)) -
        (53046.9642751875 * x.pow(6)) +
        (17019.9518580080 * x.pow(5)) -
        (3425.0836591318 * x.pow(4)) +
        (404.2703938388 * x.pow(3)) -
        (24.1878824391 * x.pow(2)) +
        (0.6717417634 * x) +
        0.0030115596
    };

    // ─── MASTER ───────────────────────────────────────────────────────────────
    masterLevel = OceanodeParameterLag.ar(\masterlevel, 1, n, 0, 1, "vf", 1/30, true);
    lfoReset    = Changed.kr(NamedControl.kr(\lforeset, 0));

    // ─── 4 AUDIO-RATE MORPH LFOS ─────────────────────────────────────────────
    lfos = 4.collect { |li|
        var lname, freq, freqDetune, freqRand, freqTotal, initPhase, phOff, roundness, skew, pulseWidth, pulseCentered, pow;
        var biPowAmt, sampleHoldHz;
        var cyclePhase, phase, morphOut, baseOut, heldOut, finalOut, trig, holdMask;
        lname        = "lfo" ++ li;
        freq         = OceanodeParameterLag.ar((lname ++ "freq").asSymbol,        1,   n, 0.01, 20,  "vf", 1/30, true);
        freqDetune   = K2A.ar(NamedControl.kr((lname ++ "freqdetune").asSymbol,   0 ! n));
        initPhase    = OceanodeParameterLag.ar((lname ++ "initphase").asSymbol,   0,   n, 0,    1,   "vf", 1/30, true);
        phOff        = OceanodeParameterLag.ar((lname ++ "phoff").asSymbol,       0,   n, 0,    1,   "vf", 1/30, true);
        roundness    = OceanodeParameterLag.ar((lname ++ "round").asSymbol,       0.5, n, 0,    1,   "vf", 1/30, true);
        skew         = OceanodeParameterLag.ar((lname ++ "skew").asSymbol,        0,   n, -1,   1,   "vf", 1/30, true);
        pulseWidth   = OceanodeParameterLag.ar((lname ++ "pulsewidth").asSymbol,  0.5, n, 0,    1,   "vf", 1/30, true);
        pulseCentered = NamedControl.kr((lname ++ "pulsecentered").asSymbol, 1 ! n);
        pow          = OceanodeParameterLag.ar((lname ++ "pow").asSymbol,         0,   n, -1,   1,   "vf", 1/30, true);
        biPowAmt     = NamedControl.kr((lname ++ "bipow").asSymbol, 0 ! n);
        sampleHoldHz = NamedControl.kr((lname ++ "samplehold").asSymbol, 0 ! n);
        freqRand     = K2A.ar({ TRand.kr(0.0, 1.0, lfoReset + Impulse.kr(0)) }.dup(n));
        freqTotal    = freq + (freqRand * freqDetune);
        cyclePhase   = Sweep.ar(lfoReset, freqTotal);
        phase        = (cyclePhase + initPhase + phOff).wrap(0, 1);
        phase        = applyPulseWidth.(phase, pulseWidth, skew, pulseCentered);
        phase        = applySkew.(phase, skew);
        morphOut     = computeShape.(phase, roundness);
        baseOut      = morphOut;
        trig         = K2A.ar(Impulse.kr(sampleHoldHz.max(0)));
        heldOut      = Latch.ar(baseOut, trig);
        holdMask     = K2A.ar(sampleHoldHz > 0);
        finalOut     = (baseOut * (1 - holdMask)) + (heldOut * holdMask);
        finalOut     = applyPow.(finalOut, pow);
        applyBiPow.(finalOut, biPowAmt)
    };

    extLfos = 4.collect { |li|
        K2A.ar(NamedControl.kr(("extlfo" ++ li).asSymbol, 0 ! n).clip(0, 1))
    };

    // ─── LFO KR BUS OUTPUT (per-voice values → C++ for display + modulation) ──
    4.do { |li|
        var busIdx = NamedControl.kr(("lfobus" ++ li).asSymbol, 0);
        Out.kr(busIdx, A2K.kr(lfos[li]));
    };

    // ─── MOD MATRIX (8 sources × 8 targets, flat 64 KR values) ───────────────
    modMat = NamedControl.kr(\mod_matrix, 0 ! 64);
    modFor = { |ti|
        (((lfos[0]    * 2) - 1) * modMat[0*8+ti]) + (((lfos[1]    * 2) - 1) * modMat[1*8+ti]) +
        (((lfos[2]    * 2) - 1) * modMat[2*8+ti]) + (((lfos[3]    * 2) - 1) * modMat[3*8+ti]) +
        (((extLfos[0] * 2) - 1) * modMat[4*8+ti]) + (((extLfos[1] * 2) - 1) * modMat[5*8+ti]) +
        (((extLfos[2] * 2) - 1) * modMat[6*8+ti]) + (((extLfos[3] * 2) - 1) * modMat[7*8+ti])
    };

    // ─── PITCH + VIBRATO ──────────────────────────────────────────────────────
    pitch         = OceanodeParameterLag.ar(\pitch,          60,  n, 0,   127, "vf", 1/30, true);
    pitchMod      = OceanodeParameterLag.ar(\pitchmod,        0,  n, 0,   1,   "vf", 1/30, true);
    pitchModRange = OceanodeParameterLag.ar(\pitchmodrange,   0,  n, -24, 24,  "vf", 1/30, true);
    pitchDetune   = K2A.ar(NamedControl.kr(\pitchdetune, 0 ! n));
    pitchTranspose = K2A.ar(NamedControl.kr(\pitchtranspose, 0 ! n));
    vibFreq       = OceanodeParameterLag.ar(\vibfreq,         5,  n, 0,   20,  "vf", 1/30, true);
    vibAmp        = OceanodeParameterLag.ar(\vibamp,          0,  n, 0,   2,   "vf", 1/30, true);
    pitchDetuneRand = { Rand(0.0, 1.0) }.dup(n) * pitchDetune;
    pitch         = pitch + pitchTranspose + (modFor.(0) * 24) + (pitchMod * pitchModRange) + pitchDetuneRand;
    vibrato       = SinOsc.ar(vibFreq, {Rand(0, 2pi)}.dup(n)) * vibAmp;
    pitch         = (pitch + vibrato).clip(0, 135);
    pitchHz       = pitch.midicps;

    // ─── FM ───────────────────────────────────────────────────────────────────
    fmRatio      = OceanodeParameterLag.ar(\fmratio, 1, n, 0.5, 8, "vf", 1/30, true);
    fmAmp        = OceanodeParameterLag.ar(\fmamp,   0, n, 0,   1, "vf", 1/30, true);
    fmMod        = OceanodeParameterLag.ar(\fmmod,   1, n, 0,   1, "vf", 1/30, true);
    fmAmpTotal   = ((fmAmp + (modFor.(1) * 0.5)).clip(0, 2) * 8) * fmMod;
    pitchWithFM  = pitchHz + (SinOsc.ar(pitchHz * fmRatio) * pitchHz * fmAmpTotal);

    // ─── OSCILLATOR BANK ──────────────────────────────────────────────────────
    triLevel     = OceanodeParameterLag.ar(\trilevel,        0,   n, 0, 1, "vf", 1/30, true);
    sawLevel     = OceanodeParameterLag.ar(\sawlevel,        1,   n, 0, 1, "vf", 1/30, true);
    pulseLevel   = OceanodeParameterLag.ar(\pulselevel,      0,   n, 0, 1, "vf", 1/30, true);
    sineLevel    = OceanodeParameterLag.ar(\sinelevel,       0,   n, 0, 1, "vf", 1/30, true);
    whiteNLevel  = OceanodeParameterLag.ar(\whitenoiselevel, 0,   n, 0, 1, "vf", 1/30, true);
    pinkNLevel   = OceanodeParameterLag.ar(\pinknoiselevel,  0,   n, 0, 1, "vf", 1/30, true);
    dustLevel    = OceanodeParameterLag.ar(\dustlevel,       0,   n, 0, 1, "vf", 1/30, true);
    sSawLevel    = OceanodeParameterLag.ar(\supersawlevel,   0,   n, 0, 1, "vf", 1/30, true);
    sSawDetune   = OceanodeParameterLag.ar(\supersawdetune,  0.5, n, 0, 1, "vf", 1/30, true);
    impulseLevel = OceanodeParameterLag.ar(\impulselevel,    0,   n, 0, 1, "vf", 1/30, true);
    pulseWidth   = OceanodeParameterLag.ar(\pulsewidth,      0.5, n, 0, 1, "vf", 1/30, true);
    oscAmp       = OceanodeParameterLag.ar(\oscamp,          0.5, n, 0, 1, "vf", 1/30, true);

    detuneFactor = pitchWithFM * superSawDetuneCurve.(sSawDetune);
    sSaw = (
        LFSaw.ar(pitchWithFM,                               {Rand(0,2)}.dup(n)) +
        LFSaw.ar(pitchWithFM - (detuneFactor * 0.11002313), {Rand(0,2)}.dup(n)) +
        LFSaw.ar(pitchWithFM - (detuneFactor * 0.06288439), {Rand(0,2)}.dup(n)) +
        LFSaw.ar(pitchWithFM - (detuneFactor * 0.01952356), {Rand(0,2)}.dup(n)) +
        LFSaw.ar(pitchWithFM + (detuneFactor * 0.01991221), {Rand(0,2)}.dup(n)) +
        LFSaw.ar(pitchWithFM + (detuneFactor * 0.06216538), {Rand(0,2)}.dup(n)) +
        LFSaw.ar(pitchWithFM + (detuneFactor * 0.10745242), {Rand(0,2)}.dup(n))
    );
    sSaw = HPF.ar(sSaw * (1/7), pitchWithFM);

    osc =
        (triLevel     * LFTri.ar(pitchWithFM)) +
        (sawLevel     * Saw.ar(pitchWithFM)) +
        (pulseLevel   * Pulse.ar(pitchWithFM, pulseWidth)) +
        (sineLevel    * SinOsc.ar(pitchWithFM)) +
        (whiteNLevel  * WhiteNoise.ar(1 ! n)) +
        (pinkNLevel   * PinkNoise.ar(1 ! n)) +
        (dustLevel    * Dust.ar(pitchHz.clip(0.1, 10000))) +
        (sSawLevel    * sSaw) +
        (impulseLevel * Impulse.ar(pitchWithFM));

    oscAmpTotal = (oscAmp * (1 + modFor.(7))).clip(0, 2);
    osc         = osc * oscAmpTotal;

    // ─── MAIN FILTER ──────────────────────────────────────────────────────────
    filterType     = OceanodeParameterLag.ar(\filtertype, 0,   n, 0,   3,   "vf", 1/30, true);
    cutoffMin      = OceanodeParameterLag.ar(\cutoffmin,  30,  n, 0,   135, "vf", 1/30, true);
    cutoffMax      = OceanodeParameterLag.ar(\cutoffmax,  100, n, 0,   135, "vf", 1/30, true);
    cutoffMod      = OceanodeParameterLag.ar(\cutoffmod,  1,   n, 0,   1,   "vf", 1/30, true);
    resonance      = OceanodeParameterLag.ar(\resonance,  0.5, n, 0,   1,   "vf", 1/30, true);
    cutoffModTotal = (cutoffMod + modFor.(2)).clip(0, 1);
    filterFreq     = LinLin.ar(cutoffModTotal, 0, 1, cutoffMin, cutoffMax).midicps.clip(20, 20000);
    filterQ        = resonance.linlin(0, 1, 1.41421356, 0.02);
    filterRq       = resonance.linlin(0, 1, 1, 0.02);
    fLPFPlain      = LPF.ar(osc, filterFreq);
    fHPFPlain      = HPF.ar(osc, filterFreq);
    fLPFRes        = BLowPass4.ar(osc, filterFreq, filterQ);
    fHPFRes        = BHiPass4.ar(osc, filterFreq, filterQ);
    fLPF           = XFade2.ar(fLPFPlain, fLPFRes, resonance * 2 - 1);
    fHPF           = XFade2.ar(fHPFPlain, fHPFRes, resonance * 2 - 1);
    fBPF           = BPF.ar(osc, filterFreq, filterRq);
    fBRF           = BRF.ar(osc, filterFreq, filterRq);
    ft             = filterType.clip(0, 3);
    mainFiltered   =
        (fLPF * (ft < 0.5)) +
        (fHPF * ((ft >= 0.5) * (ft < 1.5))) +
        (fBPF * ((ft >= 1.5) * (ft < 2.5))) +
        (fBRF * (ft >= 2.5));

    // ─── COMB RESONATORS ──────────────────────────────────────────────────────
    combPitch          = OceanodeParameterLag.ar(\combpitch,    60,  n, 0,     127,    "vf", 1/30, true);
    combPitchMod       = OceanodeParameterLag.ar(\combpitchmod,  0,  n, 0,     1,      "vf", 1/30, true);
    combPMRange        = OceanodeParameterLag.ar(\combpmrange,   0,  n, -24,   24,     "vf", 1/30, true);
    combDetune         = K2A.ar(NamedControl.kr(\combdetune, 0 ! n));
    combTranspose      = K2A.ar(NamedControl.kr(\combtranspose, 0 ! n));
    combFeed           = OceanodeParameterLag.ar(\combfeed,      0,  n, -0.999, 0.999, "vf", 1/30, true);
    combMix            = OceanodeParameterLag.ar(\combmix,       0,  n, 0,     1,      "vf", 1/30, true);
    combPitchModulated = combPitch + combTranspose + ({ Rand(0.0, 1.0) }.dup(n) * combDetune) + ((combPitchMod + modFor.(3)).clip(0,1) * combPMRange);
    combFreq           = combPitchModulated.midicps.clip(20, 10000);
    combDelay          = (1 / combFreq).clip(0.0001, 0.05);
    combFeedback       = LocalIn.ar(n);
    combFF             = mainFiltered + (DelayC.ar(mainFiltered, 0.05, combDelay) * combFeed.abs);
    combFB             = mainFiltered + combFeedback;
    combOut            = Select.ar(combFeed >= 0, [combFF, combFB]);
    combDelayed        = DelayC.ar(combOut, 0.05, combDelay);
    LocalOut.ar(Select.ar(combFeed >= 0, [Silent.ar(n), combDelayed * combFeed]));
    combOut            = LeakDC.ar(combOut).tanh;
    afterComb          = XFade2.ar(mainFiltered, combOut, combMix * 2 - 1);

    // ─── RING MOD ─────────────────────────────────────────────────────────────
    ringModPitch  = OceanodeParameterLag.ar(\ringmodpitch, 72, n, 12, 120, "vf", 1/30, true);
    ringModDetune = K2A.ar(NamedControl.kr(\ringmoddetune, 0 ! n));
    ringModMix    = OceanodeParameterLag.ar(\ringmodmix,    0, n, 0,  1,   "vf", 1/30, true);
    ringMixTotal  = (ringModMix + modFor.(5)).clip(0, 1);
    ringModSig    = afterComb * SinOsc.ar((ringModPitch + ({ Rand(0.0, 1.0) }.dup(n) * ringModDetune)).midicps);
    afterRingMod  = XFade2.ar(afterComb, ringModSig.tanh, ringMixTotal * 2 - 1);

    // ─── POST FILTER ──────────────────────────────────────────────────────────
    postFType    = OceanodeParameterLag.ar(\postftype,   0,   n, 0,   3,   "vf", 1/30, true);
    postCutMin   = OceanodeParameterLag.ar(\postcutmin,  30,  n, 0,   135, "vf", 1/30, true);
    postCutMax   = OceanodeParameterLag.ar(\postcutmax,  110, n, 0,   135, "vf", 1/30, true);
    postCutMod   = OceanodeParameterLag.ar(\postcutmod,  1,   n, 0,   1,   "vf", 1/30, true);
    postRes      = OceanodeParameterLag.ar(\postres,     0.5, n, 0,   1,   "vf", 1/30, true);
    postFreq     = LinLin.ar(postCutMod.clip(0,1), 0, 1, postCutMin, postCutMax).midicps.clip(20, 20000);
    postQ        = postRes.linlin(0, 1, 1.41421356, 0.02);
    postRq       = postRes.linlin(0, 1, 1, 0.02);
    pLPFPlain    = LPF.ar(afterRingMod, postFreq);
    pHPFPlain    = HPF.ar(afterRingMod, postFreq);
    pLPFRes      = BLowPass4.ar(afterRingMod, postFreq, postQ);
    pHPFRes      = BHiPass4.ar(afterRingMod, postFreq, postQ);
    pLPF         = XFade2.ar(pLPFPlain, pLPFRes, postRes * 2 - 1);
    pHPF         = XFade2.ar(pHPFPlain, pHPFRes, postRes * 2 - 1);
    pBPF         = BPF.ar(afterRingMod, postFreq, postRq);
    pBRF         = BRF.ar(afterRingMod, postFreq, postRq);
    pft          = postFType.clip(0, 3);
    afterPostFilter =
        (pLPF * (pft < 0.5)) +
        (pHPF * ((pft >= 0.5) * (pft < 1.5))) +
        (pBPF * ((pft >= 1.5) * (pft < 2.5))) +
        (pBRF * (pft >= 2.5));

    // ─── TREMOLO ──────────────────────────────────────────────────────────────
    minTremHz    = OceanodeParameterLag.ar(\mintremhz, 1, n, 0.01, 20, "vf", 1/30, true);
    maxTremHz    = OceanodeParameterLag.ar(\maxtremhz, 8, n, 0.01, 20, "vf", 1/30, true);
    tremHzMod    = OceanodeParameterLag.ar(\tremhzmod, 0, n, 0,    1,  "vf", 1/30, true);
    tremAmp      = OceanodeParameterLag.ar(\trecamp,   0, n, 0,    1,  "vf", 1/30, true);
    tremModTotal = (tremHzMod + modFor.(4)).clip(0, 1);
    tremHz       = LinLin.ar(tremModTotal, 0, 1, minTremHz, maxTremHz);
    tremLFO      = SinOsc.ar(tremHz, {Rand(0, 2pi)}.dup(n)) * 0.5 + 0.5;
    afterTremolo = afterPostFilter * (1 - (tremLFO * tremAmp));

    // ─── SINE SHAPER ──────────────────────────────────────────────────────────
    ssLevel      = OceanodeParameterLag.ar(\sslevel, 1, n, 0, 20, "vf", 1/30, true);
    ssMix        = OceanodeParameterLag.ar(\ssmix,   0, n, 0, 1,  "vf", 1/30, true);
    ssMixTotal   = (ssMix + modFor.(6)).clip(0, 1);
    shaped       = (afterTremolo * ssLevel).sin;
    afterSS      = XFade2.ar(afterTremolo, shaped, ssMixTotal * 2 - 1);

    // ─── OUTPUT ───────────────────────────────────────────────────────────────
    Out.ar(OceanodeOutput.kr(\out), afterSS * masterLevel);

});
