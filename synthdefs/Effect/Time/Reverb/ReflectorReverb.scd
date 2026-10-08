(
~synthCreator.value("ReflectorReverb", {|n|
    var source, sig, dry;
    var numReflcs = 16;
    var delayOffset, scaleDelays, spread, reflPan, lpfRefl, hpfRefl, mx;

    // Control parameters
    delayOffset = OceanodeParameterLag.ar(\delayoffset, 0.02,  n, 0.001, 0.2,   "vf", 1/30, true);
    scaleDelays = OceanodeParameterLag.ar(\scaledelays, 1.0,   n, 0.1,   3.0,   "vf", 1/30, true);
    spread      = OceanodeParameterLag.ar(\spread,      1.0,   n, 0.0,   1.0,   "vf", 1/30, true);
    reflPan     = OceanodeParameterLag.ar(\pan,         0.0,   n, -1.0,  1.0,   "vf", 1/30, true);
    lpfRefl     = OceanodeParameterLag.ar(\lpfcoef,     0.9,   n, 0.0,   0.999, "vf", 1/30, true);
    hpfRefl     = OceanodeParameterLag.ar(\hpffreq,     150,   n, 20,    5000,  "vf", 1/30, true);
    mx          = OceanodeParameterLag.ar(\mix,         0.5,   n, 0.0,   1.0,   "vf", 1/30, true);

    // Input signal
    source = In.ar(OceanodeInput.kr(\in), n);

    // Reflector outputs stereo; it runs once per channel with that channel's parameters and the
    // L/R outputs alternate between channels (it always wrote 2 channels before, whatever n was).
    sig = n.collect { |i|
        Reflector.ar(source[i], numReflcs, delayOffset[i], scaleDelays[i], spread[i], reflPan[i], lpfRefl[i], hpfRefl[i])[i % 2]
    };

    dry = source;

    // Mix dry and wet
    sig = XFade2.ar(dry, sig, mx * 2 - 1);

    // Output
    Out.ar(OceanodeOutput.kr(\out), sig);

}, description: "Early reflections reverb using prime-spaced delays with stereo panning", category: "Effect/Time/Reverb");
)
