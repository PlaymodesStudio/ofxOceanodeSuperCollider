(
~synthCreator.value("GrainSampler", {|n|
    var t, signal, spd, buf, start, gain, dur, buf2, sampleRateCorrection;

    // Controls
    t = OceanodeParameter.ar(\trigger, 0, n, 0, 1, "vi");
    gain = OceanodeParameterLag.ar(\levels, 0, n, 0, 1, "vf", 1/30, true);
    buf = OceanodeParameter.kr(\bufnum, 0, n, 0, 1000, "vi");
    buf2 = OceanodeBuffer.kr(\envbuf);
    spd = OceanodeParameterLag.ar(\speed, 1, n, -32, 32, "vf", 1/30, true);
    start = OceanodeParameterLag.ar(\startpos, 0, n, 0, 1, "vf", 1/30, true);
    dur = OceanodeParameterLag.ar(\grainsize, 0.1, n, 0.00001, 2, "vf", 1/30, true);
    // GrainBuf turns dur into an integer sample count: an inf or huge value
    // (e.g. a stray audio-rate input) makes grains that never end and fill all
    // maxGrains for good, so keep it finite and within the parameter's range.
    dur = Sanitize.ar(dur, 0.1).clip(0.00001, 2);

    // Sample rate correction
    sampleRateCorrection = BufSampleRate.kr(buf) / SampleRate.ir;

    // Create grain signal
    signal = GrainBuf.ar(
        numChannels: 1,
        trigger: t,
        dur: dur,
        sndbuf: buf,
        rate: spd * sampleRateCorrection,
        pos: start,
        interp: 2,
        pan: 0,
        envbufnum: buf2,
        maxGrains: 1024
    ) * gain;

    Out.ar(OceanodeOutput.kr(\out), signal);
}, description: "Basic granular sampler with trigger control", category: "Source/Sampler");
)