(
~synthCreator.value("Freezish", {|n|
    var source, sig, chain, numchans, trigger, mix;
    var buffers;
    var atkCoeff, dcyCoeff, useTimeControl, atkTime, dcyTime, hopSize;

    // All variables declared at beginning
    numchans = n;

    // Parameters with appropriate ranges
    trigger = OceanodeParameter.kr(\trigger, 0, n, 0, 1, "vi");
    mix = OceanodeParameter.ar(\mix, 0, n, 0, 1, "vf");

    // New parameters for PV_Freezish
    atkCoeff = OceanodeParameter.kr(\attackcoeff, 0.5, n, 0, 1, "vf"); // Changed default to 0.5
    dcyCoeff = OceanodeParameter.kr(\decaycoeff, 0.9, n, 0, 1, "vf");  // Changed default to 0.9

    // Time-based control parameters
    useTimeControl = OceanodeParameter.kr(\usetimecontrol, 0, n, 0, 1, "vi");
    atkTime = OceanodeParameter.kr(\attacktime, 0.01, n, 0.001, 5, "vf");
    dcyTime = OceanodeParameter.kr(\decaytime, 1.0, n, 0.001, 15, "vf"); // Changed default to 1.0

    // FFT parameters
    hopSize = OceanodeParameter.kr(\hopsize, 0.5, n, 0.25, 0.5, "vf");

    // Input signal
    source = In.ar(OceanodeInput.kr(\in), n);

    // Create one buffer per channel ({}!n, not .dup, so each channel gets its own buffer)
    buffers = { LocalBuf(2048) } ! n;

    // Process each channel individually
    sig = n.collect { |i|
        var buf = buffers[i];
        var chan = source[i];
        var fftChain = FFT(buf, chan, hopSize[i]);
        // Duration of one FFT frame hop, used to turn times into per-frame coefficients
        var frameDur = 2048 * hopSize[i] * SampleDur.ir;
        var atk, dcy;

        // Select the coefficients (not the FFT chains: two PV units on one chain both process the buffer)
        atk = Select.kr(useTimeControl[i], [
            atkCoeff[i],
            exp(frameDur.neg / atkTime[i])
        ]);
        dcy = Select.kr(useTimeControl[i], [
            dcyCoeff[i],
            exp(frameDur.neg / dcyTime[i])
        ]);

        fftChain = PV_Freezish(fftChain,
            (atk * trigger[i]).clip(0, 0.99),  // Clip to prevent exactly 1.0
            (dcy * trigger[i]).clip(0, 0.99)
        );

        IFFT(fftChain);
    };

    // Ensure mix is working correctly
    sig = XFade2.ar(source, sig, (mix * 2 - 1).clip(-1, 1));

    // Output
    Out.ar(OceanodeOutput.kr(\out), sig);

}, description: "Enhanced spectral freeze effect with nuanced control over attack and decay", category: "Effect/Time/Freeze");
)