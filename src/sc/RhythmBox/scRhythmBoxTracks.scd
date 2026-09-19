// scRhythmBoxTracks.scd
// Typed RhythmBox voices. Each track is a self-contained source, matching the
// existing C++ node lifecycle.
// Compile this file with the Oceanode SC extensions installed, then copy the
// generated .scsyndef files into Supercollider/Synthdefs/Defaults/RhythmBox.
//
// Separate SynthDefs are intentional: changing a track type means recreating
// only that track's voice, with no branching source graph left running.

(
var root, defDir, makeSource;
root = if(thisProcess.nowExecutingPath.notNil) {
    thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs"
} {
    Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs"
};
defDir = root +/+ "RhythmBox";
File.mkdir(root);
File.mkdir(defDir);

makeSource = { |defName, kind|
    SynthDef(defName, {
        |bufnum = 0, bpm = 120, numBeats = 4, stepsPerBeat = 4,
         shift = 0, numSteps = 16, globalVol = 1, globalProb = 1, globalPitch = 0,
         sequenceProb = 1, reset = 0, play = 1, active = 1, stepBus = 0, gateBus = 0,
         mono = 1, volLatch = 1, swing = 0,
         inPoint = 0, outPoint = 1, loopSample = 0, fixDuration = 0, durationBeats = 1,
         envEnabled = 0, envAttack = 0, envHoldSteps = 0, envDecay = 0.3,
         envSustain = 1, envRelease = 0.1, envCurveA = 0, envCurveD = 0, decayRange = 1,
         eqEnabled = 0, eqHPFreq = 80, eqHPRq = 1.41, eqPeakFreq = 1000,
         eqPeakGain = 0, eqPeakRq = 1, eqLPFreq = 12000, eqLPRq = 1.41,
         arpEnabled = 0, arpInterval = 7, arpModulo = 4, arpGateWidth = 1,
         arpSpeedMode = 0, globalArpEnabled = 0, globalArpSpeed = 4,
         lfoEnabled = 0, lfoRate = 1, lfoDepth = 0.5, lfoShape = 0,
         lfoPhase = 0, lfoPulseWidth = 0.5, slicerMode = 0, sliceFit = 0,
         globalStepProbSub = 1, globalCut = 0, globalPanOffset = 0,
         globalRes = 0,
         filterMode = 0, filterRandom = 0, filterRandomRange = 0,
         kickWaveType = 0, kickStartNote = 105, kickEndNote = 36,
         kickPitchAttack = 0.0001, kickPitchDecay = 0.18, kickPitchCurve = -12,
         kickPMPitch = 0, kickPMIndex = 0,
         noiseType = 0, noiseDensity = 0.5,
         snareToneNote = 58, snareToneDecay = 0.11, snareToneLevel = 0.7,
         snareNoiseFreq = 2810, snareNoiseLevel = 1, clickCombOctave = 3,
         out = 0, mixOut = 0|

        var stepOn, stepVol, stepProb, stepProbGroup, stepPan, stepCut, stepRes;
        var stepPitch, stepReverse, stepArp, stepArpSpeed;
        var stepSliceStart, stepSliceEnd, stepSliceOn;
        var stepDecayOffset, stepShape, stepHiRel, stepCombPitch, stepCombDecay;
        var stepFreq, resetTrig, playTrig, combinedReset, phaseRate, rawPhase, phase;
        var stepTrig, stepIdx, shiftedIdx, on, vol, prob, probGroup, pan, cut, res, pitch;
        var reverse, sliceStart, sliceEnd, sliceOn, decayOffset;
        var shapeAmount, hiRel, combPitch, combDecay, randomCut, seqStart, seqActive;
        var liveCut, parentStepNow;
        var gatedVol, gatedPan, gatedCut, gatedRes, gatedPitch, gatedReverse;
        var gatedSliceStart, gatedSliceEnd, gatedDecay, gatedShape, gatedHiRel;
        var gatedCombPitch, gatedCombDecay, gatedArpStep, gatedArpSpeed;
        var group1FirstProb, group2FirstProb, group3FirstProb, group4FirstProb, group5FirstProb;
        var group1Coin, group2Coin, group3Coin, group4Coin, group5Coin;
        var individualCoin, individualPass, groupedCoin, groupedPass, probabilityPass;
        var seqRandVal, newSeqActive, seqReset;
        var gate, baseTrig, stepPeriod, swingDelay, swungTrig;
        var arpStep, arpSpeedStep, parentArpStep, parentArpSpeed;
        var globalAndArp, effectiveArpSpeedStep, latchedStepArpSpeed, speedForFreq;
        var arpFreqDiv, arpFreqPitch, arpFreq, effectiveArpStep, arpActive, arpReset, arpPhase;
        var arpWrap, arpCounter, arpSemis, arpTrig, gateTrig;
        var sharedArpSemis, combinedTrig, voiceCounter, voiceIndex, sig;
        var lfoFreq, lfoPhaseNow, lfoRaw, lfoGain, output;

        stepOn          = OceanodeParameter.kr(\stepOn,          0,     64, 0,   1,   "vf");
        stepVol         = OceanodeParameter.kr(\stepVol,         1,     64, 0,   1,   "vf");
        stepProb        = OceanodeParameter.kr(\stepProb,        1,     64, 0,   1,   "vf");
        stepProbGroup   = OceanodeParameter.kr(\stepProbGroup,   0,     64, 0,  99,   "vf");
        stepPan         = OceanodeParameter.kr(\stepPan,         0,     64,-1,   1,   "vf");
        stepCut         = OceanodeParameter.kr(\stepCut,         0,     64,-1,   1,   "vf");
        stepRes         = OceanodeParameter.kr(\stepRes,         0,     64, 0,   1,   "vf");
        stepPitch       = OceanodeParameter.kr(\stepPitch,       0,     64,-12, 12,   "vf");
        stepReverse     = OceanodeParameter.kr(\stepReverse,     0,     64, 0,   1,   "vf");
        stepArp         = OceanodeParameter.kr(\stepArp,         0,     64, 0,   1,   "vf");
        stepArpSpeed    = OceanodeParameter.kr(\stepArpSpeed,    4,     64, 0, 127,   "vf");
        stepSliceStart  = OceanodeParameter.kr(\stepSliceStart,  0,     64, 0,   1,   "vf");
        stepSliceEnd    = OceanodeParameter.kr(\stepSliceEnd,    1,     64, 0,   1,   "vf");
        stepSliceOn     = OceanodeParameter.kr(\stepSliceOn,     1,     64, 0,   1,   "vf");
        stepDecayOffset = OceanodeParameter.kr(\stepDecayOffset, 0,     64,-1,   1,   "vf");
        stepShape       = OceanodeParameter.kr(\stepShape,       0,     64, 0,  20,   "vf");
        stepHiRel       = OceanodeParameter.kr(\stepHiRel,       0.725, 64, 0.01,2,   "vf");
        stepCombPitch   = OceanodeParameter.kr(\stepCombPitch,   0,     64, 0,  12,   "vf");
        stepCombDecay   = OceanodeParameter.kr(\stepCombDecay,   0,     64, 0,  10,   "vf");

        stepFreq = (bpm * stepsPerBeat / 60).max(0.001);
        resetTrig = HPZ1.kr(reset) > 0;
        playTrig = HPZ1.kr(play) > 0;
        combinedReset = (resetTrig + playTrig).min(1);
        phaseRate = stepFreq / ControlRate.ir;
        rawPhase = Phasor.kr(combinedReset, phaseRate * play, 0, 1, 0);
        phase = (rawPhase + (1 - phaseRate)).mod(1);
        stepTrig = (((HPZ1.kr(phase) < -0.3) + combinedReset).min(1)) * play;
        stepIdx = Stepper.kr(stepTrig, combinedReset, 0, numSteps.max(1) - 1, 1, 0);
        shiftedIdx = (stepIdx - shift).mod(numSteps.max(1)).clip(0, 63).floor;

        on          = Select.kr(shiftedIdx, stepOn);
        vol         = Select.kr(shiftedIdx, stepVol);
        prob        = Select.kr(shiftedIdx, stepProb);
        probGroup   = Select.kr(shiftedIdx, stepProbGroup);
        pan         = Select.kr(shiftedIdx, stepPan);
        cut         = Select.kr(shiftedIdx, stepCut);
        res         = Select.kr(shiftedIdx, stepRes);
        pitch       = Select.kr(shiftedIdx, stepPitch);
        reverse     = Select.kr(shiftedIdx, stepReverse);
        sliceStart  = Select.kr(shiftedIdx, stepSliceStart);
        sliceEnd    = Select.kr(shiftedIdx, stepSliceEnd);
        sliceOn     = Select.kr(shiftedIdx, stepSliceOn);
        decayOffset = Select.kr(shiftedIdx, stepDecayOffset);
        shapeAmount = Select.kr(shiftedIdx, stepShape);
        hiRel       = Select.kr(shiftedIdx, stepHiRel);
        combPitch   = Select.kr(shiftedIdx, stepCombPitch);
        combDecay   = Select.kr(shiftedIdx, stepCombDecay);

        seqStart = stepTrig * (stepIdx < 0.5);
        seqReset = (seqStart + combinedReset).min(1);
        randomCut = Select.kr(shiftedIdx, Array.fill(64, {
            TRand.kr(filterRandomRange.neg, filterRandomRange, seqStart)
        }));

        // Preserve the probability-group contract of the original RhythmBoxTrack:
        // the first member's probability decides whether the group is available
        // for this loop, while every member still applies its own probability.
        group1FirstProb = DC.kr(1);
        group2FirstProb = DC.kr(1);
        group3FirstProb = DC.kr(1);
        group4FirstProb = DC.kr(1);
        group5FirstProb = DC.kr(1);
        (0..15).do({ |i|
            var thisGroup, thisProb, g1match, g2match, g3match, g4match, g5match;
            thisGroup = stepProbGroup.at(i);
            thisProb = stepProb.at(i);
            g1match = (thisGroup > 0.9) * (thisGroup < 1.1) * (group1FirstProb > 0.99);
            g2match = (thisGroup > 1.9) * (thisGroup < 2.1) * (group2FirstProb > 0.99);
            g3match = (thisGroup > 2.9) * (thisGroup < 3.1) * (group3FirstProb > 0.99);
            g4match = (thisGroup > 3.9) * (thisGroup < 4.1) * (group4FirstProb > 0.99);
            g5match = (thisGroup > 4.9) * (thisGroup < 5.1) * (group5FirstProb > 0.99);
            group1FirstProb = Select.kr(g1match, [group1FirstProb, thisProb]);
            group2FirstProb = Select.kr(g2match, [group2FirstProb, thisProb]);
            group3FirstProb = Select.kr(g3match, [group3FirstProb, thisProb]);
            group4FirstProb = Select.kr(g4match, [group4FirstProb, thisProb]);
            group5FirstProb = Select.kr(g5match, [group5FirstProb, thisProb]);
        });
        group1Coin = Latch.kr(Latch.kr(TRand.kr(0, 1, seqReset) < group1FirstProb, seqReset), stepTrig);
        group2Coin = Latch.kr(Latch.kr(TRand.kr(0, 1, seqReset) < group2FirstProb, seqReset), stepTrig);
        group3Coin = Latch.kr(Latch.kr(TRand.kr(0, 1, seqReset) < group3FirstProb, seqReset), stepTrig);
        group4Coin = Latch.kr(Latch.kr(TRand.kr(0, 1, seqReset) < group4FirstProb, seqReset), stepTrig);
        group5Coin = Latch.kr(Latch.kr(TRand.kr(0, 1, seqReset) < group5FirstProb, seqReset), stepTrig);

        individualCoin = TRand.kr(0, 1, stepTrig);
        individualPass = individualCoin < (prob * globalProb * globalStepProbSub);
        groupedCoin = Select.kr((probGroup - 1).clip(0, 4),
            [group1Coin, group2Coin, group3Coin, group4Coin, group5Coin]);
        groupedPass = (TRand.kr(0, 1, stepTrig) < (prob * globalProb * globalStepProbSub)) * groupedCoin;
        probabilityPass = Latch.kr(Select.kr(probGroup > 0.5,
            [individualPass, groupedPass]), stepTrig);

        seqRandVal = TRand.kr(0, 1, seqStart);
        newSeqActive = seqRandVal < sequenceProb;
        seqActive = Latch.kr(Select.kr(combinedReset > 0.5,
            [newSeqActive, DC.kr(1)]), seqReset);

        // Sample steps and slice steps are independent patterns. A Slice track
        // must not also require the (normally empty) sample stepOn array.
        if(kind == \slice) {
            gate = (sliceOn > 0.5) * probabilityPass * seqActive * active
        } {
            if(kind == \legacy) {
                gate = (Select.kr(slicerMode > 0.5, [on, sliceOn]) > 0.5)
                       * probabilityPass * seqActive * active
            } {
                gate = (on > 0.5) * probabilityPass * seqActive * active
            }
        };
        baseTrig = stepTrig * gate;

        stepPeriod = stepFreq.reciprocal;
        swingDelay = (stepIdx % 2 > 0.5) * swing * stepPeriod;
        swungTrig = TDelay.kr(baseTrig, swingDelay);

        arpStep = Select.kr(shiftedIdx, stepArp);
        arpSpeedStep = Select.kr(shiftedIdx, stepArpSpeed);
        liveCut = Select.kr((filterRandom > 0.5) * (cut.abs > 0.0001),
            [cut, (cut * (1 + randomCut)).clip(-1, 1)]);
        parentStepNow = baseTrig > 0.5;

        // Freeze the whole step-operation state at the original unswung step
        // trigger. ARP child triggers inherit this snapshot, while the
        // voice path reads live values only on the exact parent trigger block
        // to prevent stale values leaking into the next unfiltered step.
        gatedVol = Latch.kr(vol, baseTrig);
        gatedPan = Latch.kr(pan, baseTrig);
        gatedCut = Latch.kr(liveCut, baseTrig);
        gatedRes = Latch.kr(res, baseTrig);
        gatedPitch = Latch.kr(pitch, baseTrig);
        gatedReverse = Latch.kr(reverse, baseTrig);
        gatedSliceStart = Latch.kr(sliceStart, baseTrig);
        gatedSliceEnd = Latch.kr(sliceEnd, baseTrig);
        gatedDecay = Latch.kr(decayOffset, baseTrig);
        gatedShape = Latch.kr(shapeAmount, baseTrig);
        gatedHiRel = Latch.kr(hiRel, baseTrig);
        gatedCombPitch = Latch.kr(combPitch, baseTrig);
        gatedCombDecay = Latch.kr(combDecay, baseTrig);
        gatedArpStep = Latch.kr(arpStep, baseTrig);
        gatedArpSpeed = Latch.kr(arpSpeedStep, baseTrig);
        parentArpStep = Select.kr(parentStepNow, [gatedArpStep, arpStep]);
        parentArpSpeed = Select.kr(parentStepNow, [gatedArpSpeed, arpSpeedStep]);

        globalAndArp = (arpEnabled > 0.5) * (globalArpEnabled > 0.5);
        effectiveArpSpeedStep = Select.kr(globalArpEnabled > 0.5, [parentArpSpeed, globalArpSpeed]);
        latchedStepArpSpeed = Latch.kr(effectiveArpSpeedStep, swungTrig);
        speedForFreq = Select.kr(swungTrig > 0.5, [latchedStepArpSpeed, effectiveArpSpeedStep]);
        arpFreqDiv = (bpm * speedForFreq / 60).max(0.001);
        arpFreqPitch = (speedForFreq + globalPitch).midicps.max(0.001);
        arpFreq = Select.kr(arpSpeedMode > 0.5, [arpFreqDiv, arpFreqPitch]);
        effectiveArpStep = Select.kr(globalAndArp, [parentArpStep, 1]);
        arpActive = arpEnabled * effectiveArpStep;
        arpReset = swungTrig * (effectiveArpStep > 0.5);
        arpPhase = Phasor.kr(arpReset, arpFreq / ControlRate.ir, 0, 1, 0);
        arpWrap = (HPZ1.kr(arpPhase) < -0.3) * (1 - swungTrig);
        arpCounter = Stepper.kr(arpWrap, swungTrig, 0, arpModulo.max(1) - 1, 1, 0);
        arpSemis = arpCounter * arpInterval;
        arpTrig = arpActive * (swungTrig + arpWrap).min(1);
        gateTrig = Select.kr(arpActive > 0.5, [swungTrig, arpTrig]);
        sharedArpSemis = Latch.kr(Select.kr(swungTrig > 0.5, [arpSemis, 0]), gateTrig);
        combinedTrig = gateTrig;
        voiceCounter = Stepper.kr(combinedTrig, combinedReset, 0, 7, 1, 7);
        voiceIndex = voiceCounter * (mono < 0.5);

        sig = Mix.fill(8, { |voice|
            var trig, trigAR, latchedVol, latchedPan, latchedCut, latchedRes;
            var latchedPitch, latchedReverse;
            var latchedSliceStart, latchedSliceEnd, latchedDecay, latchedShape;
            var latchedHiRel, latchedCombPitch, latchedCombDecay, source, ampEnv;
            var totalPitch, numFrames, startFrame, endFrame, regionFrames;
            var rate, playPhase, pastEnd, effectiveCut, filterActive, filterFreq, rq, filtered, dedicated;
            var eq, panSig, effectiveVol, voiceDecay, voiceHold;
            var effectiveArpSemis;
            var arpGateSignal, arpGateMult;

            trig = ((voiceIndex - voice).abs < 0.5) * combinedTrig;
            trigAR = T2A.ar(trig);
            latchedVol = Latch.kr(Select.kr(parentStepNow, [gatedVol, vol]), trig);
            latchedPan = Latch.kr(Select.kr(parentStepNow, [gatedPan, pan]), trig);
            latchedCut = Latch.kr(Select.kr(parentStepNow, [gatedCut, liveCut]), trig);
            latchedRes = Latch.kr(Select.kr(parentStepNow, [gatedRes, res]), trig);
            latchedPitch = Latch.kr(Select.kr(parentStepNow, [gatedPitch, pitch]), trig);
            latchedReverse = Latch.kr(Select.kr(parentStepNow, [gatedReverse, reverse]), trig);
            latchedSliceStart = Latch.kr(Select.kr(parentStepNow, [gatedSliceStart, sliceStart]), trig);
            latchedSliceEnd = Latch.kr(Select.kr(parentStepNow, [gatedSliceEnd, sliceEnd]), trig);
            latchedDecay = Latch.kr(Select.kr(parentStepNow, [gatedDecay, decayOffset]), trig);
            latchedShape = Latch.kr(Select.kr(parentStepNow, [gatedShape, shapeAmount]), trig);
            latchedHiRel = Latch.kr(Select.kr(parentStepNow, [gatedHiRel, hiRel]), trig);
            latchedCombPitch = Latch.kr(Select.kr(parentStepNow, [gatedCombPitch, combPitch]), trig);
            latchedCombDecay = Latch.kr(Select.kr(parentStepNow, [gatedCombDecay, combDecay]), trig);
            voiceDecay = (envDecay + (latchedDecay * decayRange)).max(0.0001);
            voiceHold = envHoldSteps.round.max(0) * stepPeriod;
            effectiveArpSemis = Latch.kr(sharedArpSemis, trig);
            totalPitch = globalPitch + latchedPitch
                         + effectiveArpSemis;

            source = if((kind == \sample) or: { kind == \slice } or: { kind == \legacy }, {
                var useSlice, voiceStart, voiceEnd, baseRate, signedRate;
                var farEnd, farStart, voiceFwdHi, voiceRevLo, phasorLo, phasorHi;
                var resetPos, playGate, declickFadeOut, declickActive, phasorTrigAR, declickAmp;
                var cleanSample, shapedSample, drive;
                numFrames = BufFrames.kr(bufnum).max(2);
                useSlice = if(kind == \slice) { 1 } {
                    if(kind == \legacy) { slicerMode > 0.5 } { 0 }
                };
                startFrame = inPoint * numFrames;
                endFrame = (outPoint * numFrames).max(startFrame + 1);
                voiceStart = Select.kr(useSlice, [startFrame, latchedSliceStart * numFrames]);
                voiceEnd = Select.kr(useSlice, [endFrame, (latchedSliceEnd * numFrames).max(voiceStart + 1)]);
                regionFrames = (voiceEnd - voiceStart).max(1);
                baseRate = BufRateScale.kr(bufnum) * totalPitch.midiratio;
                baseRate = Select.kr(fixDuration > 0.5,
                    [baseRate, regionFrames / (durationBeats * 60 / bpm * SampleRate.ir).max(1)]);
                // Avoid feeding Select a language-side false for sample tracks:
                // `useSlice` is compile-time 0 there, so the old comparison was
                // constant-folded to Boolean false once per polyphonic voice.
                if(kind == \slice) {
                    baseRate = Select.kr(sliceFit > 0.5,
                        [baseRate, (regionFrames / (60 / bpm / stepsPerBeat * SampleRate.ir).max(1))
                                   * totalPitch.midiratio])
                };
                if(kind == \legacy) {
                    baseRate = Select.kr((slicerMode * sliceFit) > 0.5,
                        [baseRate, (regionFrames / (60 / bpm / stepsPerBeat * SampleRate.ir).max(1))
                                   * totalPitch.midiratio])
                };
                signedRate = Select.kr(latchedReverse > 0.5, [baseRate, baseRate.neg]);

                // A bounded Phasor always wraps, even when loopSample is off.
                // Let one-shots run toward a distant bound, clip only the read
                // position, and mute after crossing the selected region instead.
                farEnd = 44100 * 1000;
                farStart = farEnd.neg;
                voiceFwdHi = Select.kr(loopSample > 0.5, [farEnd, voiceEnd]);
                voiceRevLo = Select.kr(loopSample > 0.5, [farStart, voiceStart]);
                phasorLo = Select.kr(latchedReverse > 0.5, [voiceStart, voiceRevLo]);
                phasorHi = Select.kr(latchedReverse > 0.5, [voiceFwdHi, voiceEnd]);
                resetPos = Select.kr(latchedReverse > 0.5, [voiceStart, voiceEnd]);
                declickFadeOut = Select.kr(useSlice, [0.001, 0.0002]);
                declickActive = K2A.ar((mono > 0.5) * (arpActive > 0.5));
                phasorTrigAR = Select.ar(declickActive,
                    [trigAR, TDelay.ar(trigAR, declickFadeOut)]);
                declickAmp = Select.ar(declickActive, [DC.ar(1),
                    EnvGen.ar(Env([1, 0, 1], [declickFadeOut, 0.00003], \lin),
                        trigAR)
                ]);
                playPhase = Phasor.ar(phasorTrigAR, signedRate, phasorLo, phasorHi, resetPos);
                pastEnd = Select.ar(K2A.ar(latchedReverse > 0.5),
                    [playPhase >= voiceEnd, playPhase <= voiceStart]);
                playGate = K2A.ar(1) - (pastEnd * K2A.ar(1 - (loopSample > 0.5)));
                cleanSample = BufRd.ar(2, bufnum, playPhase.clip(voiceStart, voiceEnd - 1), 0, 2)
                    * playGate * declickAmp;
                drive = latchedShape.max(0) + 1;
                shapedSample = (cleanSample * drive).tanh;
                Select.ar(K2A.ar(latchedShape > 0.0001), [cleanSample, shapedSample])
            }, {
                if(kind == \kick, {
                    var fenv, freq, pm, sine, pulse, clean, shaped, drive;
                    fenv = EnvGen.ar(Env([kickEndNote, kickStartNote, kickEndNote],
                        [kickPitchAttack.max(0.0001), kickPitchDecay.max(0.0001)],
                        [kickPitchCurve.neg, kickPitchCurve]), trigAR);
                    freq = (fenv + totalPitch).midicps;
                    pm = (freq - kickPMPitch.midicps).max(1);
                    sine = PMOsc.ar(freq, pm, kickPMIndex);
                    pulse = Pulse.ar(freq, 0.5);
                    clean = Select.ar(K2A.ar(kickWaveType), [sine, pulse]);
                    drive = latchedShape.max(0) + 1;
                    shaped = (clean * drive).tanh;
                    Select.ar(K2A.ar(latchedShape > 0.0001), [clean, shaped]) ! 2
                }, {
                    if(kind == \noise, {
                        var density, waves;
                        density = noiseDensity.clip(0, 1);
                        waves = [WhiteNoise.ar, PinkNoise.ar,
                            Crackle.ar(density.linlin(0, 1, 1.0, 1.99)),
                            Dust2.ar(density.linexp(0, 1, 1, 10000))];
                        Select.ar(K2A.ar(noiseType.clip(0, 3)), waves) ! 2
                    }, {
                        if(kind == \snare, {
                            var popFreq, popEnv, noiseEnv, pop, noise;
                            popFreq = EnvGen.ar(Env([3261, 410, (snareToneNote + totalPitch).midicps],
                                [0.005, 0.01], \exp), trigAR);
                            popEnv = EnvGen.ar(Env.perc(0.001, snareToneDecay.max(0.01)), trigAR);
                            noiseEnv = EnvGen.ar(Env.perc(0.0001, latchedHiRel.max(0.01)), trigAR);
                            pop = SinOsc.ar(popFreq) * popEnv * snareToneLevel;
                            noise = BPF.ar(WhiteNoise.ar, snareNoiseFreq.max(20), 1.6)
                                    * noiseEnv * snareNoiseLevel;
                            (pop + noise) ! 2
                        }, {
                            // Click is the single-sample trigger itself: no amplitude envelope.
                            trigAR ! 2
                        })
                    })
                })
            });

            ampEnv = if(kind == \click or: { kind == \snare }, { 1 }, {
                if(kind == \noise, {
                    EnvGen.ar(Env([0, 1, envSustain, envSustain, 0],
                        [envAttack.max(0), voiceDecay, voiceHold, envRelease.max(0.0001)],
                        [envCurveA, envCurveD, 0, envCurveD]), trigAR)
                }, {
                    EnvGen.ar(Env([0, 1, 1, 0, 0],
                        [envAttack.max(0), voiceHold, voiceDecay,
                         envRelease.max(0)],
                        [envCurveA, 0, envCurveD, 0]), trigAR)
                })
            });
            if((kind == \sample) or: { kind == \slice } or: { kind == \legacy }) {
                ampEnv = Select.ar(K2A.ar(envEnabled > 0.5), [DC.ar(1), ampEnv])
            };
            // Preserve the sampler's mono-ARP gate-width behavior for every
            // typed source. At width=1 this is an exact bypass.
            arpGateSignal = Lag.kr(Trig.kr(trig,
                (arpGateWidth / arpFreq.max(0.001)).max(0.001)), 0.003);
            arpGateMult = Select.kr(
                (arpEnabled > 0.5) * (mono > 0.5) * (arpGateWidth < 0.999),
                [1, arpGateSignal]);
            source = source * ampEnv * K2A.ar(arpGateMult);

            if(kind == \click) {
                source = Select.ar(K2A.ar(latchedCombDecay > 0.0001), [source,
                    CombL.ar(source, 0.1,
                        ((clickCombOctave * 12) + latchedCombPitch).midicps.reciprocal.clip(0.00005, 0.1),
                        // Select evaluates both inputs. Keep the inactive comb
                        // numerically safe at decay=0 while the selector still
                        // makes zero an exact dry/no-comb bypass.
                        latchedCombDecay.max(0.0001), 1, 0) / 2])
            };

            latchedCut = latchedCut.clip(-1, 1);
            effectiveCut = (latchedCut + globalCut).clip(-1, 1);
            filterActive = effectiveCut.abs > 0.0001;
            if(kind != \click) {
                source = source * Select.ar(K2A.ar(filterActive), [
                    DC.ar(1),
                    EnvGen.ar(Env([0, 1], [0.0008], \sin), trigAR)
                ])
            };
            rq = (1 - ((latchedRes + globalRes).clip(0, 1) * 0.95)).max(0.01);
            dedicated = filterMode > 0.5;
            filterFreq = effectiveCut.abs.clip(0, 1).linexp(0, 1, 20, SampleRate.ir * 0.45);
            filtered = Select.ar(K2A.ar(filterMode.clip(1, 4) - 1), [
                RLPF.ar(source, filterFreq, rq),
                RHPF.ar(source, filterFreq, rq),
                BPF.ar(source, filterFreq, rq),
                SelectX.ar(K2A.ar(effectiveCut + 1), [
                    BLowPass4.ar(source,
                        effectiveCut.clip(-1, 0).linexp(-1, 0, 100, SampleRate.ir * 0.45), rq),
                    source,
                    BHiPass4.ar(source,
                        effectiveCut.clip(0, 1).linexp(0, 1, 20, SampleRate.ir * 0.45), rq)
                ])
            ]);
            filtered = Select.ar(K2A.ar((dedicated * filterActive) > 0.5),
                [source, filtered]);
            filterFreq = effectiveCut;
            filtered = Select.ar(K2A.ar(dedicated), [
                SelectX.ar(K2A.ar(filterFreq + 1), [
                    RLPF.ar(source, filterFreq.clip(-1, 0).linexp(-1, 0, 100, SampleRate.ir * 0.45), rq),
                    source,
                    RHPF.ar(source, filterFreq.clip(0, 1).linexp(0, 1, 20, SampleRate.ir * 0.45), rq)
                ]),
                filtered
            ]);

            eq = RHPF.ar(filtered, eqHPFreq.max(20), eqHPRq.clip(0.05, 4));
            eq = BPeakEQ.ar(eq, eqPeakFreq.max(20), eqPeakRq.clip(0.01, 10), eqPeakGain);
            eq = RLPF.ar(eq, eqLPFreq.max(20), eqLPRq.clip(0.05, 4));
            eq = (eq * eqEnabled) + (filtered * (1 - eqEnabled));
            effectiveVol = Select.kr(volLatch > 0.5, [gatedVol, latchedVol]);
            panSig = Balance2.ar(eq[0], eq[1], (latchedPan + globalPanOffset).clip(-1, 1));
            panSig = panSig * effectiveVol;
            panSig
        });

        lfoFreq = bpm / 60 / lfoRate.max(0.001);
        lfoPhaseNow = Phasor.kr(baseTrig, lfoFreq / ControlRate.ir, 0, 1, lfoPhase);
        lfoRaw = Select.kr(lfoShape.round.clip(0, 3), [
            sin(lfoPhaseNow * 2pi) * 0.5 + 0.5,
            lfoPhaseNow < lfoPulseWidth,
            lfoPhaseNow,
            1 - lfoPhaseNow
        ]);
        lfoGain = Select.kr(lfoEnabled > 0.5, [1, 1 - lfoDepth + (lfoDepth * lfoRaw)]);
        output = sig * K2A.ar(lfoGain * active * globalVol);
        Out.ar(out, output);
        Out.ar(mixOut, output);
        Out.kr(stepBus, stepIdx);
        Out.kr(gateBus, gate);
    }).writeDefFile(defDir);
};

makeSource.(\RhythmBoxSampleTrack, \sample);
makeSource.(\RhythmBoxSliceTrack,  \slice);
makeSource.(\RhythmBoxKickTrack,   \kick);
makeSource.(\RhythmBoxNoiseTrack,  \noise);
makeSource.(\RhythmBoxSnareTrack,  \snare);
makeSource.(\RhythmBoxClickTrack,  \click);
// Legacy alias retained for old deployments and code that instantiates it directly.
makeSource.(\RhythmBoxTrack,       \legacy);

"RhythmBox typed dry sources written to: ".post; defDir.postln;
)
