// BeatRepeat / BeatRepeat Pro
//
// A coherent multichannel rewrite of the original BeatRepeat SynthDef.  A
// single write/read phase is shared by every channel: BufRd therefore returns
// exactly n channels (never the accidental n*n expansion produced by an array
// of phases).  The dry/wet path is linear and bounded, with an optional final
// limiter.  The original parameter names and ranges are retained; the extra
// tone/safety controls default to transparent values.

// Built for the BeatRepeat Pro node (scBeatRepeatPro), which asks for
// BeatRepeatPro<N Chan>: 1 to 16 channels, no metadata SynthDef (it is not a
// generic node).
(
var build;
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};
File.mkdir(d);
File.mkdir(d +/+ "BeatRepeatPro");

build = {|n|
    var input, recordInput, buffer, output, loopOutput, filteredLoop;
    var bufferSeconds, bufferFrames, writePos, readPos, readPhase, captureAnchor, startAnchor;
    var musicalDur, pitchedDur, loopDur, loopSamples, offsetSamples, prerollSamples;
    var goRise, resetRise, manualTrigAr, goAr, autoTrigAr, captureTrigAr, captureTrig;
    var captureElapsed, captureLen, captureActive, captureDone, ready, writerGate;
    var cycleDur, cycleSamples, cycleCount, cyclePhase, cycleBoundary, cycleTrig, cycleTrigK;
    var stepCount, currentStep, stepModulo, pitchRatio, playRate, cycleRate;
    var cutoffStepModulo, accumulatedCutoffPitch, cutoffHz;
    var beatDur, autoPeriod, autoSamples, autoClock, autoSoon, stepPitch, stepRatio, repeats, fullCycles, remainder, remWhole, geo, stepSum;
    var reverseSign, phaseInLoop, loopStart, loopEnd, preroll, fadeSamples;
    var declickEnv, gateEnv, gateOpen, dryClosed, activeWet, wetAmount, dryAmount, limitAmount, limited;
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
    // The writer keeps 1.5 s of the buffer behind the capture point intact for
    // Offset (max 1 s) and the declick pre-roll (max 0.1 s), so a loop is at
    // most bufferSeconds - 1.5.
    loopDur = Select.kr(pitchMode > 0.5, [musicalDur, pitchedDur])
        .clip(SampleDur.ir, bufferSeconds - 1.5);
    loopSamples = (loopDur * SampleRate.ir).round(1).clip(1, bufferFrames - 2);

    // reset is a monotonically increasing counter in the Pro node, and a
    // conventional 0/1 pulse in the metadata-generated node.  Positive edges
    // work for both and cannot create the old double-trigger on pulse release.
    // Every timing decision is made per sample: one-sample triggers, sample
    // counters, no control-block rounding.  Go and Reset are read on their
    // audio-rate ramps, which leave zero on the first sample of the block the
    // message lands in (A2K would only see the change a block later, after a
    // kick on that beat had already started).  A repeat started on a beat
    // therefore keeps the transient that sits exactly on that beat.
    goAr = goAll[0] > 0;
    goRise = Trig1.ar(goAr, SampleDur.ir);
    resetRise = Trig1.ar(HPZ1.ar(resetAll[0]) > 0, SampleDur.ir);
    manualTrigAr = (goRise + resetRise).clip(0, 1);

    // The repeat counter (pitch / cutoff steps) is the only control-block
    // feedback; nothing about timing depends on it.
    stepCount = LocalIn.kr(1, 0);
    stepModulo = numSteps.max(1);
    // The live pass heard during capture is the only untransposed occurrence.
    // Wet repetitions cycle through pitch steps 1..N (never 0..N-1), so the
    // very first repeated fragment already has one Pitch Step applied.
    stepPitch = pitchStep * (numSteps > 0);
    cycleRate = rate * (2 ** ((((stepCount % stepModulo) + 1) * stepPitch) / 12));

    // Adapt Length makes one repetition last exactly as long as it takes to
    // traverse the same captured sample region at the current playback rate.
    cycleDur = loopDur * Select.kr(adapt > 0.5, [1, 1 / cycleRate.abs.max(0.001)]);
    // Each repetition fades in over the material just before the capture
    // point and reaches full level exactly on it, so its first sample (a
    // kick on the beat) is never faded.  The pre-roll is that fade.
    preroll = declick.min(cycleDur * 0.49);
    fadeSamples = (preroll * SampleRate.ir).max(1);

    // Auto Reset is an audio-rate clock started by Go/Reset: every recapture
    // lands on the exact sample of its musical position.  Repetitions mode
    // counts occurrences (the live capture is occurrence one), adding each
    // repetition's own length when Adapt Length follows the pitch steps.
    // Beats mode uses the smallest whole number of Reset Beats periods that
    // holds one capture, so recaptures stay on the beat grid.
    beatDur = 60 / bpm;
    stepRatio = 2 ** (stepPitch.neg / 12);
    repeats = (autoLoops - 1).max(0);
    fullCycles = (repeats / stepModulo).floor;
    remainder = repeats - (fullCycles * stepModulo);
    remWhole = remainder.floor;
    geo = { |count| Select.kr(stepPitch.abs < 0.0001,
        [stepRatio * (1 - (stepRatio ** count)) / (1 - stepRatio), count]) };
    stepSum = (fullCycles * geo.(stepModulo)) + geo.(remWhole)
        + ((remainder - remWhole) * (stepRatio ** (remWhole + 1)));
    autoPeriod = Select.kr(autoResetMode, [
        loopDur + Select.kr(adapt > 0.5, [repeats * loopDur, stepSum * loopDur / rate.abs.max(0.001)]),
        (beatDur * autoBeats) * (loopDur / (beatDur * autoBeats)).ceil.max(1)
    ]);
    autoSamples = (autoPeriod * SampleRate.ir).max(1);
    autoClock = Phasor.ar(manualTrigAr, 1, 0, autoSamples, -1);
    autoTrigAr = (HPZ1.ar(autoClock) < 0) * (1 - manualTrigAr) * K2A.ar(autoReset) * goAr;
    // The pre-roll before each recapture hands back to the live input, so the
    // material on the recapture point (the next kick) is heard dry at full
    // level instead of on top of a repetition.
    autoSoon = (autoClock >= (autoSamples - fadeSamples)) * K2A.ar(autoReset);
    captureTrigAr = (manualTrigAr + autoTrigAr).clip(0, 1);
    captureTrig = T2K.kr(captureTrigAr);

    // Capture: the live pass plays dry for one Repeat length minus the
    // pre-roll, then the first repetition fades in and lands on the capture
    // point one Repeat length after it.
    captureElapsed = Sweep.ar(captureTrigAr, SampleRate.ir);
    captureLen = Latch.ar(K2A.ar((loopDur - preroll) * SampleRate.ir), captureTrigAr);
    captureActive = captureElapsed < captureLen;
    captureDone = HPZ1.ar(captureElapsed >= captureLen) > 0;
    ready = SetResetFF.ar(captureDone, captureTrigAr);

    // The writer never jumps.  This keeps Offset meaningful: the requested
    // pre-roll really is the material immediately preceding the new loop.
    writerGate = ((1 - goAr) + (captureElapsed < ((bufferSeconds - 1.5) * SampleRate.ir))).clip(0, 1);
    writePos = Phasor.ar(0, writerGate, 0, bufferFrames, 0);
    // Capture the new location immediately, but keep the previous loop anchor
    // alive while its wet signal fades out. Installing the new anchor only
    // when capture completes makes the position jump inaudible (wet gain is 0).
    captureAnchor = Latch.ar(writePos, captureTrigAr);
    startAnchor = Latch.ar(captureAnchor, captureDone);
    // BufWr has no write-enable input.  Zero the parked boundary frame while
    // repeating instead of continuously leaking the live input into it.
    BufWr.ar(recordInput * writerGate, buffer, writePos, loop: 1);

    // Repetition clock, counted in samples.  The musical duration remains
    // live after capture and resizes forward from the original anchor.
    cycleSamples = (cycleDur * SampleRate.ir).max(1);
    // A triggered Phasor outputs resetPos + rate on its trigger sample, so
    // resetPos = -rate makes every counter read exactly 0 there.
    cycleCount = Phasor.ar(captureDone, 1, 0, cycleSamples, -1);
    cyclePhase = cycleCount / cycleSamples;
    cycleBoundary = (HPZ1.ar(cycleCount) < 0) * ready * (1 - captureDone);
    cycleTrig = captureDone + cycleBoundary;
    cycleTrigK = T2K.kr(cycleTrig);

    currentStep = Select.kr(captureTrig > 0, [(stepCount + T2K.kr(cycleBoundary)) % 65536, 0]);
    LocalOut.kr(currentStep);
    pitchRatio = 2 ** ((((currentStep % stepModulo) + 1) * stepPitch) / 12);
    playRate = rate * pitchRatio;

    // Cutoff has an independent semitone sequence but follows the same repeat
    // counter. Clamp after modulation and MIDI conversion so the filter never
    // receives an unsafe frequency, even when the sequence exceeds its base range.
    cutoffStepModulo = numCutoffSteps.max(1);
    accumulatedCutoffPitch = ((currentStep % cutoffStepModulo) + 1)
        * cutoffStep * (numCutoffSteps > 0);
    cutoffHz = (cutoffPitch + accumulatedCutoffPitch).midicps.clip(20, 20000);

    offsetSamples = Latch.kr((offsetMs * 0.001 * SampleRate.ir).round(1), cycleTrigK);
    // Buffer frames the head covers during the fade-in at this repetition's rate.
    prerollSamples = Latch.kr((preroll * SampleRate.ir * playRate.abs).round(1)
        .min(loopSamples * 0.49), cycleTrigK);

    readPhase = Phasor.ar(cycleTrig, playRate.abs, 0, loopSamples, playRate.abs.neg);
    reverseSign = K2A.ar(reverse);
    phaseInLoop = (readPhase * (1 - reverseSign))
        + ((K2A.ar(loopSamples) - 1 - readPhase) * reverseSign);
    // Offset zero means the capture point is heard exactly one pre-roll into
    // each repetition (forward); Reverse plays the window backwards as before.
    loopStart = (startAnchor - K2A.ar(offsetSamples)
        - (K2A.ar(prerollSamples) * (1 - reverseSign))).wrap(0, bufferFrames);
    loopEnd = (loopStart + K2A.ar(loopSamples)).wrap(0, bufferFrames);
    readPos = (loopStart + phaseInLoop).wrap(0, bufferFrames - 1);

    loopOutput = BufRd.ar(n, buffer, readPos, loop: 1, interpolation: 4);
    filteredLoop = (loopOutput * K2A.ar(BinaryOpUGen('==', filterType, 0)))
        + (RLPF.ar(loopOutput, K2A.ar(cutoffHz), K2A.ar(rq))
            * K2A.ar(BinaryOpUGen('==', filterType, 1)))
        + (RHPF.ar(loopOutput, K2A.ar(cutoffHz), K2A.ar(rq))
            * K2A.ar(BinaryOpUGen('==', filterType, 2)));

    declickEnv = (((cycleCount / fadeSamples).min((cycleSamples - cycleCount) / fadeSamples))
        .clip(0, 1) * (pi * 0.5)).sin;
    // Gate is a fraction of the repetition measured from the capture point,
    // which sits one pre-roll into the cycle.
    gateOpen = (cycleCount < (fadeSamples + (gate * cycleSamples))) + (gate >= 1);
    gateEnv = LagUD.ar(gateOpen.clip(0, 1), K2A.ar(declick), K2A.ar(declick));
    loopOutput = filteredLoop * declickEnv * gateEnv * K2A.ar(loopAmp);

    // Gate < 1 also shapes the live pass while it is being captured: the dry
    // closes at the same point of the Repeat length and stays closed until
    // the first repetition takes over (or Go is released).
    dryClosed = SetResetFF.ar(
        captureActive * (captureElapsed >= (gate * loopDur * SampleRate.ir)) * K2A.ar(gate < 1),
        captureTrigAr + (HPZ1.ar(autoSoon) > 0)) * goAr;
    dryClosed = Slew.ar(dryClosed, 1 / preroll, 1 / preroll);

    // Linear mixing cannot add the +3 dB midpoint boost of XFade2, and exact
    // dry remains exact when Mix is zero.  The wet fades in over the pre-roll
    // (so it is full on the capture point), out over the pre-roll before an
    // Auto Reset (so it is gone on the recapture point) and over Declick
    // otherwise.
    activeWet = Slew.ar(goAr * ready * (1 - captureActive) * (1 - autoSoon),
        1 / preroll, Select.ar(autoSoon, [K2A.ar(1 / declick), K2A.ar(1 / preroll)]));
    wetAmount = activeWet * K2A.ar(mix);
    dryAmount = 1 - (K2A.ar(mix) * (activeWet + (dryClosed * (1 - activeWet))));
    output = (input * dryAmount) + (loopOutput * wetAmount);
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
        captureActive,
        cyclePhase
    ]);

};

(1..16).do { |n|
    SynthDef.new(("BeatRepeatPro" ++ n).asSymbol, {
        var sig = SynthDef.wrap(build, prependArgs: [n, nil]);
    }).writeDefFile(d +/+ "BeatRepeatPro", mdPlugin: AbstractMDPlugin);
};
)
