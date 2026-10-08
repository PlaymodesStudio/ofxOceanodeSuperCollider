~synthCreator.value("BasicSampler", {|n|
    // --- Configuration ---
    var numVoices = 32;

    // --- Variables ---
    var trig, k_trig, trigEdge, buf, spd, bucle, startPos, gain, latchSpd;
    var polyMode, voiceIndex, finalSignal;

    // 1) Inputs
    trig = OceanodeParameter.ar(\trigger, 0, n, 0, 1, "vi");
    trigEdge = HPZ1.kr(A2K.kr(trig)) > 0; // Edge detection

    buf = OceanodeParameter.kr(\bufnum, 0, n, 0, 1000, "vi");
    spd = OceanodeParameter.kr(\speed, 1, n, -32, 32, "vf");
    latchSpd = OceanodeParameter.kr(\latchspeed, 1, n, 0, 1, "vi");
    bucle = OceanodeParameter.kr(\loop, 0, n, 0, 1, "vi");
    startPos = OceanodeParameter.kr(\startpos, 0, n, 0, 1, "vf");
    gain = OceanodeParameterLag.ar(\levels, 0, n, 0, 1, "vf", 1/30, true);

    // New parameter to toggle behavior
    polyMode = OceanodeParameter.kr(\poly, 0, n, 0, 1, "vi");

    // 2) Voice Allocator Logic
    // We increment the counter on every trigger
    voiceIndex = Stepper.kr(trigEdge, 0, 0, numVoices - 1);

    // CRITICAL: If polyMode is 0, we force the index to always be 0.
    // If polyMode is 1, we use the Stepper's value.
    voiceIndex = voiceIndex * polyMode;

    // 3) Create the bank of voices
    finalSignal = Mix.fill(numVoices, {|i|
        var voiceTrig, latchedBuf, latchedSpd, latchedStart, currentRateCorrection, voiceFinalSpd, voiceSig;

        // Only trigger this specific voice if the index matches 'i'
        voiceTrig = trigEdge * (voiceIndex - i < 0.5) * (voiceIndex - i > -0.5);

        // Latch parameters per voice
        latchedBuf = Latch.kr(buf, voiceTrig);
        latchedSpd = Latch.kr(spd, voiceTrig);
        latchedStart = Latch.kr(startPos, voiceTrig) * BufFrames.kr(latchedBuf);
        currentRateCorrection = BufSampleRate.kr(latchedBuf) / SampleRate.ir;
        voiceFinalSpd = Select.kr(latchSpd, [spd, latchedSpd]) * currentRateCorrection;

        // Playback
        voiceSig = PlayBuf.ar(
            numChannels: 1,
            bufnum: latchedBuf,
            rate: voiceFinalSpd,
            trigger: voiceTrig,
            startPos: latchedStart,
            loop: bucle
        );

        // OPTIONAL: If you want switching to 'mono' to instantly silence
        // older polyphonic tails, uncomment the line below:
        // voiceSig = voiceSig * Select.kr(i > 0, [1, polyMode]);

        voiceSig;
    });

    // Output
    Out.ar(OceanodeOutput.kr(\out), finalSignal * gain);

}, description: "Sampler with switchable Polyphony. If Poly is off, it behaves monophonically.", category: "Source/Sampler");