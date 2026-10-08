(
// TankReverb - Exact replica of tankreverb13 using synthCreator
// This matches the original's algorithm and compile-time delay structure
~synthCreator.value("TankReverb", {|n|
    var input, src, localin, tank, signal, wet;
    var sr = SampleRate.ir;
    var times, dtimes, delays;
    var earlydiffdtimes, earlydiffsize=4;
    var tanksize = 4;
    var inputhpf, inputlpf, predelay, scaledelays, indiffusion;
    var decay, decay1, damping, feedback;
    var tank0, tank1, tank2, tank3, tail, mix;

    // Parameters using OceanodeParameter for proper GUI integration
    inputhpf = OceanodeParameterLag.kr(\inputhpf, 150, n, 20, 20000, "vf", 1/30, true);
    inputlpf = OceanodeParameterLag.kr(\inputlpf, 0.5, n, 0, 1, "vf", 1/30, true);
    predelay = OceanodeParameterLag.kr(\predelay, 0, n, 0, 0.1, "vf", 1/30, true);
    scaledelays = OceanodeParameterLag.kr(\scaledelays, 1, n, 0.1, 2, "vf", 1/30, true);
    indiffusion = OceanodeParameterLag.kr(\indiffusion, 0.05, n, 0, 1, "vf", 1/30, true);
    decay = OceanodeParameterLag.kr(\decay, 0.5, n, 0, 5, "vf", 1/30, true);
	decay1 = OceanodeParameterLag.kr(\decay1, 0.5, n, 0, 5, "vf", 1/30, true);
    damping = OceanodeParameterLag.kr(\damping, 0, n, 0, 1, "vf", 1/30, true);
    feedback = OceanodeParameterLag.kr(\feedback, 0, n, 0, 1, "vf", 1/30, true);
    tank0 = OceanodeParameterLag.kr(\tank0, 0.25, n, 0, 1, "vf", 1/30, true);
    tank1 = OceanodeParameterLag.kr(\tank1, 0.25, n, 0, 1, "vf", 1/30, true);
    tank2 = OceanodeParameterLag.kr(\tank2, 0.25, n, 0, 1, "vf", 1/30, true);
    tank3 = OceanodeParameterLag.kr(\tank3, 0.25, n, 0, 1, "vf", 1/30, true);
    tail = OceanodeParameterLag.kr(\tail, 1, n, 0, 2, "vf", 1/30, true);
    mix = OceanodeParameterLag.kr(\mix, 0.5, n, 0, 1, "vf", 1/30, true);

    // Get the input signal
    input = In.ar(OceanodeInput.kr(\in), n);

    // Input processing
    // Protection: a NaN/inf arriving at the input would latch inside the filters and the tank
    src = HPF.ar(Sanitize.ar(input), inputhpf);
    src = OnePole.ar(src, inputlpf);
    src = DelayN.ar(src, 0.1, predelay);

    // Ensure src is always an array for consistent processing
    src = src.asArray;

    // Base delay times array - same as original
    times = [ 1447, 727, 613, 673, 1439, 2083, 2011, 1511, 1493, 1277, 2437, 2383, 2341, 997, 1061, 2039,
        1997, 1481, 1627, 1129, 743, 983, 1091, 907, 541, 2393, 1801, 2081, 1787, 1453, 977, 2311,
        691, 479, 2377, 1693, 1013, 1931, 1049, 2243, 839, 739, 1747, 601, 1823, 1123, 2467, 1297,
        1613, 1361, 2207, 593, 619, 1709, 449, 937 ];

    // Create unique delay time sequences for each channel
    // Using scaledelays control array inline like the original
    dtimes = n.collect({ |chan|
        var chandtimes, baseidx;
        baseidx = chan * (times.size.div(n));
        chandtimes = times.rotate(baseidx);

        // Early diffusion times for this channel
        earlydiffdtimes = { chandtimes.removeAt(chandtimes.minIndex) } ! earlydiffsize;

        // Create delay line times for this channel
        // scaledelays[chan] gets the control for this channel
        // Each entry is [delay, max delay] in samples. The max must be a constant: a delay line's
        // buffer is allocated at synth start, so scaledelays (up to 2) could not grow it later.
        delays = chandtimes.collect { |t| [t * scaledelays[chan], t * 2] };

        Routine{ (earlydiffdtimes.collect { |t| [t, t] } ++ delays).do { |it| it.yield } }
    });

    // Early reflections - process each channel separately
    src = src.collect { |channel, i|
        var processed = channel;
        earlydiffsize.do {
            var dt = dtimes[i].next / sr;
            processed = AllpassN.ar(processed, dt[1], dt[0], indiffusion[i]);
        };
        processed
    };

    // Protection: the feedback path is sanitized and bounded so the tank can never run away
    localin = Sanitize.ar(LocalIn.ar(n)).clip2(4);
    // Ensure localin is always an array
    localin = localin.asArray;

    // Main reverb tank - independent processing for each channel
    tank = n.collect { |channel|
        var local = localin[channel];
        tanksize.collect { |i|
            var dt0, dt1, dt2;

            local = local + src[channel];

            dt0 = dtimes[channel].next / sr;
            dt1 = dtimes[channel].next / sr;
            dt2 = dtimes[channel].next / sr;

            local = AllpassC.ar(local, dt0[1], dt0[0], decay[channel]);
            local = AllpassC.ar(local, dt1[1], dt1[0], decay1[channel]);
            local = DelayC.ar(local, dt2[1], dt2[0] - (ControlDur.ir / tanksize) * LFNoise2.kr(0.3).range(0.9,1));

            local = OnePole.ar(local, damping[channel]);
            local
        };
    };

    LocalOut.ar(Sanitize.ar(tank.collect(_.last) * feedback).clip2(4));

    // Output mixing - using all 4 tank stages
    wet = tank.collect { |channeltank, i|
        Mix.new([
            channeltank[0] * tank0[i],
            channeltank[1] * tank1[i],
            channeltank[2] * tank2[i],
            channeltank[3] * tank3[i]
        ])
    };

    wet = LeakDC.ar(Sanitize.ar(wet).clip2(4));
    wet = wet * tail;

    // Ensure input is array for XFade2
    input = input.asArray;

    // Final mix of dry and wet
    signal = XFade2.ar(input, wet, mix * 2 - 1);

    // Send to output
    Out.ar(OceanodeOutput.kr(\out), signal);

}, description: "Tank reverb with 4 stage diffusion network and independent tank controls", category: "Effect/Time/Reverb");
)
