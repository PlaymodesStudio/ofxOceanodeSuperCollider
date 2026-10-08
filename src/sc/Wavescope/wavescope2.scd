// Continuously scrolling data source for scWavescope.
//
// One instance captures one channel. C++ creates as many instances as needed,
// which removes the old dependency on a small, incomplete list of compiled
// channel-count variants. The newest sample is always at the right edge; the
// preceding Time Window scrolls left through a circular 256-sample history.

var d, wavescopeDir;

if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};

wavescopeDir = d +/+ "wavescope2";
File.mkdir(d);
File.mkdir(wavescopeDir);

~samplesPerFrame = 256;

SynthDef(\wavescope_realtime1, {
    // refreshRate remains for compatibility with older node binaries.
    arg in = 0, out = 0, timeWindow = 0.1, refreshRate = 60,
        stabilize = 0, triggerLevel = 0;
    var signal, safeWindow, sampleFrequency, historySpan;
    var historyBufferSize, history, writePhase, blockAdvance;
    var readPhase, readStart, readStep, liveSample;
    var crossing, triggerCount, triggerKR, triggerPhase, triggerSample;
    var stableStart, useStable, output;

    signal = In.ar(in, 1);
    safeWindow = timeWindow.clip(0.001, 10.0);

    // The ring stores 255 intervals across the requested window. For windows
    // shorter than 255 audio samples, write at the audio sample rate and read
    // only the requested fraction of the ring, interpolating display points.
    sampleFrequency = ((~samplesPerFrame - 1) / safeWindow)
        .clip(0.1, SampleRate.ir);
    historySpan = safeWindow * sampleFrequency;
    // Control-rate readers see the audio writer after a complete DSP block.
    // Reserve that block plus two slots beyond the visible window so the
    // oldest point cannot read data written later in the same block.
    historyBufferSize = ~samplesPerFrame + BlockSize.ir + 2;
    history = LocalBuf(historyBufferSize, 1).clear;
    writePhase = Phasor.ar(0, sampleFrequency / SampleRate.ir,
        0, historyBufferSize);
    BufWr.ar(signal, history, writePhase.floor, loop: 1);

    // BufWr processes the whole audio block before these control-rate reads.
    // Align the read head with the final written sample of that block, which
    // also keeps sub-block Time Windows aligned with the newest audio.
    blockAdvance = (BlockSize.ir - 1) * sampleFrequency / SampleRate.ir;
    readPhase = A2K.kr(writePhase) + blockAdvance;
    readStart = readPhase - historySpan + historyBufferSize;
    readStep = historySpan / (~samplesPerFrame - 1);
    liveSample = BufRd.kr(1, history, readPhase.floor,
        loop: 1, interpolation: 1);

    // A rising threshold crossing marks the right edge of a stable frame.
    // Count audio-rate crossings so a short period cannot disappear between
    // control blocks. Latch the ring phase at the actual crossing sample.
    crossing = HPZ1.ar(signal > triggerLevel) > 0;
    triggerCount = A2K.kr(PulseCount.ar(crossing));
    triggerKR = Changed.kr(triggerCount);
    triggerPhase = A2K.kr(Latch.ar(writePhase, crossing));
    triggerSample = A2K.kr(Latch.ar(signal, crossing));
    stableStart = triggerPhase - historySpan + historyBufferSize;
    // Until the first crossing, keep showing the live scrolling waveform.
    useStable = (stabilize > 0.5) * SetResetFF.kr(triggerKR, 0);

    // Fractional phases make the rest of the waveform move smoothly between
    // adjacent ring slots. Read at control rate because the UI polls buses.
    output = Array.fill(~samplesPerFrame, {|i|
        var scrollingSample, triggeredSample, stableSample;
        if(i == (~samplesPerFrame - 1)) {
            scrollingSample = liveSample;
            triggeredSample = triggerSample;
        } {
            scrollingSample = BufRd.kr(1, history, readStart + (i * readStep),
                loop: 1, interpolation: 2);
            triggeredSample = BufRd.kr(1, history, stableStart + (i * readStep),
                loop: 1, interpolation: 2);
        };
        stableSample = Latch.kr(triggeredSample, triggerKR);
        Select.kr(useStable, [scrollingSample, stableSample]);
    });

    Out.kr(out, output);
}).writeDefFile(wavescopeDir);

"Created scrolling wavescope_realtime1 SynthDef".postln;
