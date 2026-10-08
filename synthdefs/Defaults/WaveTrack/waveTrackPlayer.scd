var d, waveTrackDir;

if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "/CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "/SuperCollider/CompiledSynthdefs";
};

waveTrackDir = d +/+ "/waveTrackPlayer";
File.mkdir(d);
File.mkdir(waveTrackDir);

(1..16).do({|numChannels|
    var synthName = ("waveTrackPlayer" ++ numChannels).asSymbol;

    SynthDef(synthName, {
        arg out=0, gain=1, play=0, jump=0;
        // Buffer args: one per channel
        var bufArgs = Array.fill(numChannels, {|i| ("buf" ++ i).asSymbol });
        var bufs = bufArgs.collect({|name| NamedControl.kr(name, 0) });

        // Transport args
        var bpm = \bpm.kr(120).max(1);
        var beatTransport = \beatTransport.kr(0);
        var clipStartBeat = \clipStartBeat.kr(0);
        var clipLenBeats = \clipLenBeats.kr(4).max(1 / 1024);
        var sourceStartBeat = \sourceStartBeat.kr(0).max(0);
        var sourceFileDurationBeats = \sourceFileDurationBeats.kr(4).max(1 / 1024);
        var playbackRate = \playbackRate.kr(1).max(0);
        var reverse = \reverse.kr(0);

        var clipDurSec, phaseRate, correctPhase, resetTrig, startPhase, phase;
        var sourcePhase, bufferPhase, inRange, playEnv, endFade, sig;

        // Seconds needed to traverse the whole clip content at rate 1.
        // clipLenBeats, sourceStartBeat and sourceFileDurationBeats are all
        // in the same source-beat domain; playbackRate converts that source
        // span to timeline time (contentDuration / visibleDuration for a
        // stretched Wave clip) and is applied exactly once, here.
        clipDurSec = clipLenBeats * 60 / bpm;
        phaseRate = playbackRate / clipDurSec;   // normalised phase per second

        // Transport phase, used ONLY as the value latched at a reset.
        correctPhase = ((beatTransport - clipStartBeat) / clipLenBeats).clip(0, 1);

        // `jump` is a reset request. Any change of its value (a C++ counter,
        // or the legacy 0 -> 1 flag) fires exactly one reset in the control
        // block in which the new value and its matching beatTransport arrive.
        resetTrig = Changed.kr(jump);

        // Latch.kr takes the value of THIS block. (The previous version fed
        // resetPos through K2A, which ramps from the previous block's value,
        // so a reset landed on the stale phase of the previous GUI frame.)
        startPhase = Latch.kr(correctPhase, resetTrig);

        // Audio-rate, non-wrapping read head. Sweep integrates phaseRate per
        // sample and never wraps, so when the head reaches the end of the
        // slice it stays past 1 instead of jumping back to the slice start
        // (Phasor(end: 1) wrapped and replayed the beginning of the slice
        // until the C++ gate caught up on the next GUI frame).
        phase = startPhase + Sweep.ar(K2A.ar(resetTrig), phaseRate);

        sourcePhase = Select.ar(reverse > 0, [phase.clip(0, 1), 1 - phase.clip(0, 1)]);
        bufferPhase = ((sourceStartBeat + (sourcePhase * clipLenBeats)) /
            sourceFileDurationBeats).clip(0, 1);

        // Legacy standalone-node gate. The timeline node always sends a
        // clip-relative transport inside [0, clipLenBeats).
        inRange = (beatTransport >= clipStartBeat) * (beatTransport < (clipStartBeat + clipLenBeats));
        playEnv = EnvGen.kr(Env.asr(0.002, 1, 0.003), gate: play * inRange);

        // The slice ends when the AUDIO read head reaches its end, not when a
        // GUI frame notices the playhead left the clip. The fade is computed
        // from the remaining audio time so it finishes exactly at phase 1 and
        // stays closed afterwards; no sample after the slice end is heard.
        endFade = ((1 - phase) * clipDurSec / playbackRate.max(1e-6) / 0.003).clip(0, 1);

        sig = bufs.collect({|buf|
            // Map the file's full duration onto its full frame count: source
            // time t (normalised) is frame t * N. Non-looping linear BufRd
            // clamps the last read to frame N-1, which is where phase 1 lands.
            var frameCount = BufFrames.kr(buf).max(1);
            var frameIndex = (bufferPhase * frameCount).clip(0, frameCount - 1);
            BufRd.ar(1, buf, frameIndex, loop: 0, interpolation: 2);
        });

        sig = sig * gain * playEnv * endFade;

        Out.ar(out, sig);
    }).writeDefFile(waveTrackDir);

    ("Created: " ++ synthName).postln;
});

"WaveTrackPlayer SynthDefs created (1-16 channels)".postln;
