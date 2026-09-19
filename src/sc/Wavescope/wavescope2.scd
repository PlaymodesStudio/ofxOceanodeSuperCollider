// Triggered, double-buffered data source for scWavescope.
//
// One instance captures one channel. C++ creates as many instances as needed,
// which removes the old dependency on a small, incomplete list of compiled
// channel-count variants. A complete 256-sample Time Window is captured into a
// hidden buffer, then published atomically. The displayed buffer is never
// rotated or modified in place, eliminating phase flicker and the ring seam.

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
~capturePadding = 2;
~captureBufferSize = ~samplesPerFrame + ~capturePadding;

SynthDef(\wavescope_realtime1, {
    // refreshRate remains for compatibility with older node binaries.
    arg in = 0, out = 0, timeWindow = 0.1, refreshRate = 60,
        stabilize = 1, triggerLevel = 0;
    var signal, safeWindow, sampleFrequency;
    var initialTrigger, risingCrossing, freeRunTrigger, startRequest;
    var previousActive, acceptedTrigger, capturePosition, writePosition;
    var sampleTrigger, heldSample, captureDone, captureActive;
    var displayBufferIndex, bufferA, bufferB, writeBuffer;
    var output;

    signal = In.ar(in, 1);
    safeWindow = timeWindow.clip(0.001, 10.0);

    // N points contain N-1 intervals, so the oldest and newest points are
    // exactly timeWindow seconds apart after the ring has filled once.
    sampleFrequency = ((~samplesPerFrame - 1) / safeWindow)
        .clip(0.1, SampleRate.ir);

    // A stable frame begins on a rising threshold crossing. In free-running
    // mode, leave enough time for the previous capture to finish before the
    // next request. Impulse.ar(0) supplies one initial frame even for DC.
    initialTrigger = Impulse.ar(0);
    risingCrossing = HPZ1.ar(signal > triggerLevel) > 0;
    freeRunTrigger = Impulse.ar(1 / (safeWindow + (ControlDur.ir * 2)));
    startRequest = initialTrigger
        + Select.ar(stabilize > 0.5, [freeRunTrigger, risingCrossing]);

    // LocalIn carries the capture state from the preceding control block. It
    // prevents crossings inside the current Time Window from retriggering it.
    previousActive = LocalIn.ar(1, 0);
    acceptedTrigger = startRequest * (1 - previousActive);
    capturePosition = Sweep.ar(acceptedTrigger, sampleFrequency);
    writePosition = capturePosition.floor;
    sampleTrigger = (HPZ1.ar(writePosition) > 0) + acceptedTrigger;
    heldSample = Latch.ar(signal, sampleTrigger);
    captureDone = HPZ1.ar(writePosition >= ~captureBufferSize) > 0;
    captureActive = SetResetFF.ar(acceptedTrigger, captureDone);
    LocalOut.ar(captureActive);

    // Capture into the buffer not currently displayed. Two guard samples keep
    // the published first and last points away from BufWr's clamped boundary,
    // preventing endpoint kinks. Once the guarded capture is complete,
    // ToggleFF swaps buffers in one sample. A single BufWr
    // is essential: an out-of-range phase is clamped by scsynth, so two BufWr
    // instances would keep corrupting the displayed buffer's final sample.
    bufferA = LocalBuf(~captureBufferSize, 1).clear;
    bufferB = LocalBuf(~captureBufferSize, 1).clear;
    displayBufferIndex = ToggleFF.ar(captureDone);
    writeBuffer = Select.ar(displayBufferIndex,
        [K2A.ar(bufferB), K2A.ar(bufferA)]);
    BufWr.ar(heldSample, writeBuffer, writePosition, loop: 0);

    // Every output frame is a contiguous capture from trigger to
    // trigger + Time Window. There is no circular boundary to draw across.
    output = Array.fill(~samplesPerFrame, {|i|
        // Skip the leading guard sample. The final visible sample is also one
        // position before the guarded buffer boundary.
        var readPosition = K2A.ar(i + 1);
        var sampleA = BufRd.ar(1, bufferA, readPosition, loop: 0, interpolation: 1);
        var sampleB = BufRd.ar(1, bufferB, readPosition, loop: 0, interpolation: 1);
        A2K.kr(Select.ar(displayBufferIndex, [sampleA, sampleB]));
    });

    Out.kr(out, output);
}).writeDefFile(wavescopeDir);

"Created triggered wavescope_realtime1 SynthDef".postln;
