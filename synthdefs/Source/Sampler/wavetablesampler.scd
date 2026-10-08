~synthCreator.value("WavetableSampler", {|n|
    var pitch, levels, bufA_raw, bufB_raw, bufSelect, xfadeTime;
    var bufA, bufB, bufSelectLagged;
    var phasor, sigA, sigB, xfade, finalSignal;
    var bufFrames, freq, phasorRate;
    var gateA_trig, gateB_trig, prevSelect;

    // --- Inputs ---
    bufA_raw = OceanodeParameter.kr(\bufnum_a, 0, n, 0, 1000, "vi");
    bufB_raw = OceanodeParameter.kr(\bufnum_b, 0, n, 0, 1000, "vi");
    bufSelect = OceanodeParameter.kr(\bufselect, 0, n, 0, 1, "vf");
    xfadeTime = OceanodeParameter.kr(\xfadetime, 0.01, n, 0.001, 0.1, "vf");
    pitch = OceanodeParameter.kr(\pitch, 60, n, 0, 127, "vf"); // Fixed default pitch to 60
    levels = OceanodeParameterLag.ar(\levels, 1, n, 0, 1, "vf", 1/30, true);

    // --- SMART LATCHING ---
    // We use Delay1 on the selection state for the *Gate Trigger* only.
    // This gives a 1-control-cycle "grace period" if C++ sends
    // the ID and the Toggle in the same network packet.
     prevSelect = Delay1.kr(bufSelect);

    // If bufSelect is > 0.5 (Playing B), we want A to be OPEN (Trig > 0).
    // We use strict thresholds (0.01 and 0.99) so the gate stays open
    // during the crossfade, preventing clicks if you update while fading.

    // Gate A: Open when we are playing B (or fading to B)
    gateA_trig = (prevSelect > 0.01);
    bufA = Gate.kr(bufA_raw, gateA_trig);

    // Gate B: Open when we are playing A (or fading to A)
    gateB_trig = (prevSelect < 0.99);
    bufB = Gate.kr(bufB_raw, gateB_trig);

    // ----------------------

    // Use the Latched IDs for frame calculation
    // Note: If buffers have different sizes, this switch might cause a jump
    // in the phase, but that is unavoidable without separate phasors.
    bufFrames = BufFrames.kr(Select.kr(bufSelect.round, [bufA, bufB]));

    freq = 440 * (2 ** ((pitch - 69) / 12));
    phasorRate = freq * bufFrames / SampleRate.ir;

    phasor = Phasor.ar(0, phasorRate, 0, bufFrames, 0);

    // Read using latched IDs
    sigA = BufRd.ar(1, bufA, phasor, loop: 1, interpolation: 4);
    sigB = BufRd.ar(1, bufB, phasor, loop: 1, interpolation: 4);

    // Audio-rate crossfade
    xfade = Lag.ar(K2A.ar(bufSelect), xfadeTime);
    finalSignal = LinXFade2.ar(sigA, sigB, xfade * 2 - 1);

    Out.ar(OceanodeOutput.kr(\out), finalSignal * levels.lincurve(0, 1, 0, 1, 4));

}, description: "Click-free double-buffered sampler. Includes 'Grace Period' latching to allow simultaneous C++ updates.", category: "Source/Wavetable");