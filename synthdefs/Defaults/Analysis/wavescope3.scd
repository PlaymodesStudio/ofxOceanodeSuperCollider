var d, wavescope3Dir;

if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "/CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "/SuperCollider/CompiledSynthdefs";
};

wavescope3Dir = d +/+ "/wavescope3";
File.mkdir(d);
File.mkdir(wavescope3Dir);

// Samples returned per channel per frame.
// These are evenly spaced across the requested time window inside SC,
// so the C++ side always gets a gapless, correctly-windowed snapshot.
// 128 gives enough density for smooth short-window display and adequate
// envelope coverage for long windows, at half the cost of 256.
~samplesPerChannel = 128;

// Ring-buffer length (seconds). Must be >= max timeWindow the user can set.
~maxBufferSeconds = 10.0;

(1..24).do({|numChannels|
    var synthName = ("wavescope3_" ++ numChannels).asSymbol;

    SynthDef(synthName, {
        arg in = 0, out = 0, timeWindow = 0.1;

        var sig, sr, bufSize, writePhase, localBufs, samplesToRead, outs;

        sig = In.ar(in, numChannels);
        sr  = SampleRate.ir;

        bufSize    = (~maxBufferSeconds * sr).round.asInteger;
        writePhase = Phasor.ar(0, 1, 0, bufSize);
        localBufs  = Array.fill(numChannels, { LocalBuf(bufSize, 1) });

        // Continuously record every audio sample into the ring buffers
        numChannels.do({|ch|
            BufWr.ar(sig[ch], localBufs[ch], writePhase);
        });

        // How many real audio samples the current time window spans
        samplesToRead = (timeWindow * sr).clip(1, bufSize);

        // Build output array: ch0[0..N-1], ch1[0..N-1], ...
        // i=0 is oldest (writePhase - samplesToRead), i=N-1 is newest (writePhase)
        outs = Array.new(numChannels * ~samplesPerChannel);
        numChannels.do({|ch|
            ~samplesPerChannel.do({|i|
                var frac    = i / (~samplesPerChannel - 1);
                var lookback = (1.0 - frac) * samplesToRead;
                var readPos  = (writePhase - lookback + bufSize) % bufSize;
                var sample   = BufRd.ar(1, localBufs[ch], readPos, loop: 1, interpolation: 1);
                outs.add(A2K.kr(sample));
            });
        });

        // Single contiguous KR output block — one bus on the C++ side
        Out.kr(out, outs);

    }).writeDefFile(wavescope3Dir);

    ("wavescope3_" ++ numChannels ++ " written").postln;
});

"Done. Run this file in SC to recompile synthdefs.".postln;
