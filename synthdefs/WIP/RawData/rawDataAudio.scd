// Simple and robust SynthDef for reading any file as raw audio data
(
SynthDef(\rawFilePlayer, {
    |out = 0, bufnum, rate = 1, startPos = 0, loop = 0,
     amp = 0.1, pan = 0, gate = 1, attack = 0.01, release = 0.1|

    var sig, env, player;

    // Create envelope for smooth on/off
    env = EnvGen.kr(Env.asr(attack, 1, release), gate, doneAction: 2);

    // Play the buffer as raw audio
    player = PlayBuf.ar(
        numChannels: 1,
        bufnum: bufnum,
        rate: rate * BufRateScale.kr(bufnum),
        startPos: startPos,
        loop: loop
    );

    // Apply envelope and amplitude scaling
    sig = player * env * amp;

    // Pan and output
    Out.ar(out, Pan2.ar(sig, pan));
}).add;

// Simple function to load any file as raw audio buffer
~loadRawFile = { |path, interpret = \int8|
    var file, rawData, audioData, buffer;

    file = File.open(path, "rb");
    if(file.isNil) {
        "File not found!".postln;
        nil
    } {
        // Read all bytes
        rawData = Int8Array.newClear(file.length);
        file.read(rawData);
        file.close;

        ("Read" + rawData.size + "bytes from file").postln;

        // Convert to audio data based on interpretation
        audioData = switch(interpret,
            \int8, {
                // Convert signed 8-bit to floats (-1 to 1)
                rawData.collect({ |byte| byte / 127.0 })
            },
            \uint8, {
                // Convert unsigned 8-bit to floats (-1 to 1)
                rawData.collect({ |byte|
                    var unsigned = if(byte < 0) { byte + 256 } { byte };
                    (unsigned / 127.5) - 1.0
                })
            },
            \int16, {
                // Convert pairs of bytes to 16-bit signed integers
                var result = Array.newClear(rawData.size div: 2);
                var i = 0;
                (0, 2 .. rawData.size - 2).do({ |idx|
                    var low = rawData[idx];
                    var high = rawData[idx + 1];
                    var val = (high << 8) | (low & 0xFF);
                    if(val > 32767) { val = val - 65536 };
                    result[i] = val / 32768.0;
                    i = i + 1;
                });
                result
            }
        );

        ("Converted to" + audioData.size + "audio samples").postln;
        ("Sample range:" + audioData.minItem + "to" + audioData.maxItem).postln;

        // Create buffer on server
        buffer = Buffer.alloc(s, audioData.size, 1);

        // Load data into buffer and return it
        buffer.loadCollection(audioData, action: {
            ("Buffer" + buffer.bufnum + "loaded successfully").postln;
        });

        buffer;
    }
};

// Function to play file with proper synchronization
~playRawFile = { |path, rate = 1, amp = 0.1, loop = 0, interpret = \int8|
    var buffer = ~loadRawFile.value(path, interpret);
    if(buffer.notNil) {
        // Wait a moment for buffer to load, then play
        SystemClock.sched(0.1, {
            ("Playing buffer" + buffer.bufnum).postln;
            Synth(\rawFilePlayer, [
                \bufnum, buffer,
                \rate, rate,
                \amp, amp,
                \loop, loop
            ]);
        });
    };
};

// Test function to verify buffer contents
~testBuffer = { |bufnum|
    var buffer = Buffer.cachedBufferAt(s, bufnum);
    if(buffer.notNil) {
        buffer.get(0, { |val| ("First sample:" + val).postln });
        buffer.get(100, { |val| ("Sample 100:" + val).postln });
        buffer.get(1000, { |val| ("Sample 1000:" + val).postln });
    };
};
)

// Usage:
// Start with 8-bit interpretation (usually works best for most files)
~playRawFile.value("/Users/santiagovilanova/Desktop/a.png", rate: 0.1, amp: 0.3, interpret: \int8);

// Test the buffer contents
~testBuffer.value(3); // Replace 3 with your buffer number

// Try different interpretations:
 ~playRawFile.value("/Users/santiagovilanova/Desktop/a.png", rate: 0.1, amp: 0.2, interpret: \uint8);
 ~playRawFile.value("/Users/santiagovilanova/Desktop/a.png", rate: 0.05, amp: 0.1, interpret: \int16);

// Manual approach for more control:

b = ~loadRawFile.value("/Users/santiagovilanova/Desktop/Late-Reverberation_Synthesis_Using_Interleaved_Velvet-Noise_Sequences.pdf", \int8);
// Wait a moment, then:
x = Synth(\rawFilePlayer, [\bufnum, b, \rate, 0.31, \amp, 0.5]);
x.set(\rate, 0.32);  // Change speed
x.set(\amp, 0.1);   // Change volume
x.release;          // Stop
