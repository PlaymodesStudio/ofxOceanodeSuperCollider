// BeatRepeat / BeatRepeat Pro
//
// A coherent multichannel rewrite of the original BeatRepeat SynthDef.  A
// single write/read phase is shared by every channel: BufRd therefore returns
// exactly n channels (never the accidental n*n expansion produced by an array
// of phases).  The dry/wet path is linear and bounded, with an optional final
// limiter.  The original parameter names and ranges are retained; the extra
// tone/safety controls default to transparent values.

(
~synthCreator.value("BeatRepeatPro", {|n|
    var input, recordInput, buffer, output, loopOutput, filteredLoop;
    var bufferSeconds, bufferFrames, writePos, readPos, readPhase, captureAnchor, startAnchor;
    var musicalDur, pitchedDur, loopDur, loopSamples, offsetSamples;
    var goRise, resetRise, manualTrig, autoTrig, captureTrig, captureActive, captureFill, captureDone, ready;
    var cycleDur, cyclePhase, cycleBoundary, cycleTrig;
    var feedback, stepCount, stepModulo, accumulatedPitch, pitchRatio, playRate;
    var cutoffStepModulo, accumulatedCutoffPitch, cutoffHz;
    var beatDur, autoPeriod, autoSeed, autoPhase, autoIncrement, autoAdvanced, autoWrap;
    var nextStep, nextAutoPhase, nextReady;
    var reverseSign, phaseInLoop, loopStart, loopEnd;
    var writerGate, fadeFraction, declickEnv, gateEnv, activeWet, wetAmount, limitAmount, limited;
    var tick, high, low;

    // Existing BeatRepeat controls.  Time/transport controls intentionally use
    // channel 0 as a shared clock; levels remains per-channel.
    var goAll = OceanodeParameter.ar(\go, 0, n, 0, 1, "vi");
    var divisionAll = OceanodeParameter.ar(\division, 16, n, 0.6666667, 96, "vf");
    var bpmAll = OceanodeParameter.ar(\bpm, 120, n, 1, 999, "vf");
    var pitchModeAll = OceanodeParameter.ar(\pitchmode, 0, n, 0, 1, "vi");
    var repeatPitchAll = OceanodeParameter.ar(\repeatpitch, 60, n, 0, 127, "vf");
    var adaptAll = OceanodeParameter.ar(\adaptlength, 0, n, 0, 1, "vi");
    var offsetAll = OceanodeParameter.ar(\offset, 0, n, 0, 1000, "vf");
    var gateAll = OceanodeParameter.ar(\gate, 1, n, 0, 1, "vf");
    var declickAll = OceanodeParameter.ar(\declick, 0.005, n, 0.0001, 0.1, "vf");
    var rateAll = OceanodeParameter.ar(\rate, 1, n, 0.125, 4, "vf");
    var reverseAll = OceanodeParameter.ar(\reverse, 0, n, 0, 1, "vi");
    var pitchStepAll = OceanodeParameter.ar(\pitchstep, 0, n, -12, 12, "vf");
    var numStepsAll = OceanodeParameter.ar(\numpitchsteps, 12, n, 0, 24, "vi");
    var loopAmpAll = OceanodeParameter.ar(\grainamp, 1, n, 0, 2, "vf");
    var resetAll = OceanodeParameter.ar(\reset, 0, n, 0, 1, "vi");
    var mixAll = OceanodeParameter.ar(\mix, 1, n, 0, 1, "vf");
    var levels = OceanodeParameterLag.ar(\levels, 1, n, 0, 2, "vf", 1/50, true);

    // Pro additions.
    var inputGainAll = OceanodeParameter.ar(\inputgain, 1, n, 0, 2, "vf");
    var filterTypeAll = OceanodeParameter.ar(\filtertype, 0, n, 0, 2, "vi");
    var cutoffPitchAll = OceanodeParameter.ar(\cutoffpitch, 135.076, n, 15.487, 135.076, "vf");
    var cutoffStepAll = OceanodeParameter.ar(\cutoffstep, 0, n, -12, 12, "vf");
    var numCutoffStepsAll = OceanodeParameter.ar(\numcutoffsteps, 0, n, 0, 24, "vi");
    var rqAll = OceanodeParameter.ar(\rq, 1, n, 0.05, 1, "vf");
    var limiterAll = OceanodeParameter.ar(\limiter, 1, n, 0, 1, "vi");
    var ceilingAll = OceanodeParameter.ar(\ceiling, 0.98, n, 0.1, 1, "vf");
    var autoResetAll = OceanodeParameter.ar(\autoreset, 0, n, 0, 1, "vi");
    var autoResetModeAll = OceanodeParameter.ar(\autoresetmode, 0, n, 0, 1, "vi");
    var autoLoopsAll = OceanodeParameter.ar(\autoloops, 4, n, 1, 64, "vf");
    var autoBeatsAll = OceanodeParameter.ar(\autobeats, 4, n, 0.125, 64, "vf");
    var vis = \vis.kr(0);

    var go = A2K.kr(goAll[0]).clip(0, 1);
    var division = A2K.kr(divisionAll[0]).clip(0.6666667, 96);
    var bpm = A2K.kr(bpmAll[0]).clip(1, 999);
    var pitchMode = A2K.kr(pitchModeAll[0]).clip(0, 1);
    var repeatPitch = A2K.kr(repeatPitchAll[0]).clip(0, 127);
    var adapt = A2K.kr(adaptAll[0]).clip(0, 1);
    var offsetMs = A2K.kr(offsetAll[0]).clip(0, 1000);
    var gate = A2K.kr(gateAll[0]).clip(0, 1);
    var declick = A2K.kr(declickAll[0]).clip(0.0001, 0.1);
    var rate = A2K.kr(rateAll[0]).clip(0.125, 4);
    var reverse = A2K.kr(reverseAll[0]).clip(0, 1);
    var pitchStep = A2K.kr(pitchStepAll[0]).clip(-12, 12);
    var numSteps = A2K.kr(numStepsAll[0]).clip(0, 24).round(1);
    var loopAmp = A2K.kr(loopAmpAll[0]).clip(0, 2);
    var reset = A2K.kr(resetAll[0]);
    var mix = A2K.kr(mixAll[0]).clip(0, 1);
    var inputGain = A2K.kr(inputGainAll[0]).clip(0, 2);
    var filterType = A2K.kr(filterTypeAll[0]).clip(0, 2).round(1);
    var cutoffPitch = A2K.kr(cutoffPitchAll[0]).clip(15.487, 135.076);
    var cutoffStep = A2K.kr(cutoffStepAll[0]).clip(-12, 12);
    var numCutoffSteps = A2K.kr(numCutoffStepsAll[0]).clip(0, 24).round(1);
    var rq = A2K.kr(rqAll[0]).clip(0.05, 1);
    var limiterOn = A2K.kr(limiterAll[0]).clip(0, 1);
    var ceiling = A2K.kr(ceilingAll[0]).clip(0.1, 1);
    var autoReset = A2K.kr(autoResetAll[0]).clip(0, 1);
    var autoResetMode = A2K.kr(autoResetModeAll[0]).clip(0, 1).round(1);
    var autoLoops = A2K.kr(autoLoopsAll[0]).clip(1, 64);
    var autoBeats = A2K.kr(autoBeatsAll[0]).clip(0.125, 64);

    bufferSeconds = 8;
    bufferFrames = (bufferSeconds * SampleRate.ir).round(1).max(64);
    buffer = LocalBuf(bufferFrames, n).clear;
    input = In.ar(OceanodeInput.kr(\in), n);
    recordInput = input * K2A.ar(inputGain);

    // Musical denominator convention: 16 = sixteenth, 8 = eighth. Dotted and
    // triplet entries arrive as their duration-equivalent denominator.
    musicalDur = (60 / bpm) * (4 / division);
    // Pitch Mode treats the repeat window as one oscillator period. A MIDI 69
    // setting therefore captures/repeats 1/440 second, enabling tonal stutters.
    pitchedDur = 1 / repeatPitch.midicps;
    loopDur = Select.kr(pitchMode > 0.5, [musicalDur, pitchedDur])
        .clip(SampleDur.ir, bufferSeconds - 0.01);
    loopSamples = (loopDur * SampleRate.ir).round(1).clip(1, bufferFrames - 2);

    // reset is a monotonically increasing counter in the Pro node, and a
    // conventional 0/1 pulse in the metadata-generated node.  Positive edges
    // work for both and cannot create the old double-trigger on pulse release.
    goRise = HPZ1.kr(go) > 0;
    resetRise = HPZ1.kr(reset) > 0;
    manualTrig = (goRise + resetRise).clip(0, 1);

    // Explicit control-block feedback holds the pitch-step counter, automatic
    // reset clock and ready state without creating a UGen dependency cycle.
    feedback = LocalIn.kr(3, 0);
    stepCount = feedback[0];
    autoPhase = feedback[1];
    ready = feedback[2];

    stepModulo = numSteps.max(1);
    // The live pass heard during capture is the only untransposed occurrence.
    // Wet repetitions cycle through pitch steps 1..N (never 0..N-1), so the
    // very first repeated fragment already has one Pitch Step applied.
    accumulatedPitch = ((stepCount % stepModulo) + 1) * pitchStep * (numSteps > 0);
    pitchRatio = 2 ** (accumulatedPitch / 12);
    playRate = rate * pitchRatio;

    // Cutoff has an independent semitone sequence but follows the same repeat
    // counter. Clamp after modulation and MIDI conversion so the filter never
    // receives an unsafe frequency, even when the sequence exceeds its base range.
    cutoffStepModulo = numCutoffSteps.max(1);
    accumulatedCutoffPitch = ((stepCount % cutoffStepModulo) + 1)
        * cutoffStep * (numCutoffSteps > 0);
    cutoffHz = (cutoffPitch + accumulatedCutoffPitch).midicps.clip(20, 20000);

    // Adapt Length makes one repetition last exactly as long as it takes to
    // traverse the same captured sample region at the current playback rate.
    cycleDur = loopDur * Select.kr(adapt > 0.5, [1, 1 / playRate.abs.max(0.001)]);

    // Repetitions mode preserves the existing occurrence count: the live
    // capture is occurrence one, so the clock is seeded by 1 / autoLoops.
    // Beats mode instead measures elapsed quarter-note beats from the capture
    // trigger, independently of loop size and playback rate.  Its seed accounts
    // for the capture time that has already elapsed before ready becomes true.
    beatDur = 60 / bpm;
    autoPeriod = Select.kr(autoResetMode, [cycleDur * autoLoops, beatDur * autoBeats]);
    autoSeed = Select.kr(autoResetMode, [1 / autoLoops, loopDur / (beatDur * autoBeats)]);
    autoIncrement = ControlDur.ir / autoPeriod.max(ControlDur.ir);
    autoAdvanced = autoPhase + (autoIncrement * autoReset * go * ready);
    autoWrap = autoAdvanced >= 1;
    autoTrig = autoWrap * autoReset * go * ready;
    captureTrig = (manualTrig + autoTrig).clip(0, 1);

    // Playback starts after the current musical duration at Go/Reset. The
    // writer keeps filling the full forward window in the background, so later
    // duration increases still grow from the invariant Go point.
    captureActive = Trig1.kr(captureTrig, loopDur);
    captureFill = Trig1.kr(captureTrig, bufferSeconds);
    captureDone = TDelay.kr(captureTrig, loopDur);

    // The writer never jumps.  This keeps Offset meaningful: the requested
    // pre-roll really is the material immediately preceding the new loop.
    writerGate = ((1 - go) + captureFill).clip(0, 1);
    writePos = Phasor.ar(0, K2A.ar(writerGate),
        0, bufferFrames, 0);
    // Capture the new location immediately, but keep the previous loop anchor
    // alive while its wet signal fades out. Installing the new anchor only
    // when capture completes makes the position jump inaudible (wet gain is 0).
    captureAnchor = Latch.ar(writePos, K2A.ar(captureTrig));
    startAnchor = Latch.ar(captureAnchor, K2A.ar(captureDone));
    // BufWr has no write-enable input.  Zero the parked boundary frame while
    // repeating instead of continuously leaking the live input into it.
    BufWr.ar(recordInput * K2A.ar(writerGate), buffer, writePos, loop: 1);

    // The musical duration remains live after capture and resizes forward from
    // the original anchor without requiring a new Go/Reset.
    cyclePhase = Phasor.ar(K2A.ar(captureDone),
        1 / (SampleRate.ir * cycleDur.max(SampleDur.ir)), 0, 1, 0);
    cycleBoundary = (HPZ1.ar(cyclePhase) < 0) * K2A.ar(ready);
    cycleTrig = K2A.ar(captureDone) + cycleBoundary;
    offsetSamples = Latch.kr((offsetMs * 0.001 * SampleRate.ir).round(1), A2K.kr(cycleTrig));

    nextStep = Select.kr(captureTrig > 0,
        [(stepCount + T2K.kr(cycleBoundary)) % 65536, 0]);
    nextAutoPhase = Select.kr(captureTrig > 0,
        [Select.kr(captureDone > 0, [autoAdvanced.wrap(0, 1), autoSeed]), 0]) * autoReset;
    nextReady = Select.kr(captureTrig > 0,
        [Select.kr(captureDone > 0, [ready, 1]), 0]) * go;
    LocalOut.kr([nextStep, nextAutoPhase, nextReady]);

    readPhase = Phasor.ar(cycleTrig, K2A.ar(playRate.abs), 0, K2A.ar(loopSamples), 0);
    reverseSign = K2A.ar(reverse);
    phaseInLoop = (readPhase * (1 - reverseSign))
        + ((K2A.ar(loopSamples) - 1 - readPhase) * reverseSign);
    // Offset zero means the first sample is exactly where Go/Reset was pressed.
    // Musical duration changes resize forward from that invariant anchor.
    loopStart = (startAnchor - K2A.ar(offsetSamples)).wrap(0, bufferFrames);
    loopEnd = (loopStart + K2A.ar(loopSamples)).wrap(0, bufferFrames);
    readPos = (loopStart + phaseInLoop).wrap(0, bufferFrames - 1);

    loopOutput = BufRd.ar(n, buffer, readPos, loop: 1, interpolation: 4);
    filteredLoop = (loopOutput * K2A.ar(BinaryOpUGen('==', filterType, 0)))
        + (RLPF.ar(loopOutput, K2A.ar(cutoffHz), K2A.ar(rq))
            * K2A.ar(BinaryOpUGen('==', filterType, 1)))
        + (RHPF.ar(loopOutput, K2A.ar(cutoffHz), K2A.ar(rq))
            * K2A.ar(BinaryOpUGen('==', filterType, 2)));

    fadeFraction = (K2A.ar(declick) / K2A.ar(cycleDur.max(SampleDur.ir))).clip(0.000001, 0.49);
    declickEnv = (((cyclePhase / fadeFraction).min((1 - cyclePhase) / fadeFraction))
        .clip(0, 1) * (pi * 0.5)).sin;
    gateEnv = LagUD.ar(K2A.ar(cyclePhase < gate), K2A.ar(declick), K2A.ar(declick));
    loopOutput = filteredLoop * declickEnv * gateEnv * K2A.ar(loopAmp);

    // Capture/reset monitoring stays dry.  Linear mixing cannot add the +3 dB
    // midpoint boost of XFade2, and exact dry remains exact when Mix is zero.
    // Auto/manual recapture changes the source from the loop to live input.
    // Smooth both edges with the existing Declick time; an instantaneous
    // source swap is discontinuous whenever live and buffered samples differ.
    activeWet = Ramp.ar(K2A.ar(go * ready * (1 - captureActive)), declick);
    wetAmount = activeWet * K2A.ar(mix);
    output = (input * (1 - wetAmount)) + (loopOutput * wetAmount);
    output = output * levels;
    // Mix = 0 is a true bypass, including the safety limiter.
    limitAmount = K2A.ar(limiterOn * (mix > 0)) * activeWet;
    limited = Limiter.ar(LeakDC.ar(output), K2A.ar(ceiling), 0.005);
    output = (output * (1 - limitAmount)) + (limited * limitAmount);
    Out.ar(OceanodeOutput.kr(\out), output);

    // One compact message drives the docked waveform and exact heads.  Max/min
    // across channels visualizes the full image without summing (and therefore
    // without stereo cancellation).
    tick = Impulse.ar(60);
    high = RunningMax.ar(input.asArray.reduce({|a, b| a.max(b)}), tick);
    low = RunningMin.ar(input.asArray.reduce({|a, b| a.min(b)}), tick);
    SendReply.ar(tick * K2A.ar(vis), '/beatRepeatProVis', [
        high, low,
        writePos / bufferFrames,
        readPos / bufferFrames,
        loopStart / bufferFrames,
        loopEnd / bufferFrames,
        activeWet,
        K2A.ar(captureActive),
        cyclePhase
    ]);

}, description: "Coherent multichannel beat repeater with live waveform GUI",
category: "Effect/Time/Looping");
)
