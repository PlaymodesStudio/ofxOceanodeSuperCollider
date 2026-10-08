(
~synthCreator.value("MultiTapEcho", {|n|
    var dry, monoIn, wet, sig, mx;
    var fb1, fb2, fb3, fb4;
    var time1, vol1, feed1, pan1, hpcut1, lpcut1, res1;
    var time2, vol2, feed2, pan2, hpcut2, lpcut2, res2;
    var time3, vol3, feed3, pan3, hpcut3, lpcut3, res3;
    var time4, vol4, feed4, pan4, hpcut4, lpcut4, res4;
    var w1, w2, w3, w4;

    mx = OceanodeParameter.ar(\mix, 0, n, 0, 1, "vf");

    dry   = In.ar(OceanodeInput.kr(\in), n);
    monoIn = Mix(dry) / n;

    // 4 independent mono feedback lines
    #fb1, fb2, fb3, fb4 = LocalIn.ar(4);

    // Tap parameters are single values (size 1): only element 0 was ever used, so size-n vectors
    // showed n-1 controls that did nothing. Each tap is placed in the n-channel field by PanAz.

    // --- Tap 1 ---
    time1  = OceanodeParameterLag.ar(\time1,  500, 1, 1, 2000, "vf", 1/30, true) / 1000;
    vol1   = OceanodeParameter.ar(   \vol1,   0.5, 1, 0, 1,    "vf");
    feed1  = OceanodeParameter.ar(   \feed1,  0.0, 1, 0, 1,    "vf");
    pan1   = OceanodeParameter.ar(   \pan1,   0.0, 1, -1, 1,   "vf");
    hpcut1 = OceanodeParameterLag.ar(\hpcut1, 12,  1, 12, 120, "vf", 1/30, true).midicps;
    lpcut1 = OceanodeParameterLag.ar(\lpcut1, 120, 1, 12, 120, "vf", 1/30, true).midicps;
    res1   = OceanodeParameter.ar(   \res1,   0.0, 1, 0, 0.99, "vf");

    w1 = LeakDC.ar(fb1 * feed1.at(0) + monoIn);
    w1 = RHPF.ar(w1, hpcut1.at(0), 1 - res1.at(0));
    w1 = RLPF.ar(w1, lpcut1.at(0), 1 - res1.at(0));

    // --- Tap 2 ---
    time2  = OceanodeParameterLag.ar(\time2,  500, 1, 1, 2000, "vf", 1/30, true) / 1000;
    vol2   = OceanodeParameter.ar(   \vol2,   0.5, 1, 0, 1,    "vf");
    feed2  = OceanodeParameter.ar(   \feed2,  0.0, 1, 0, 1,    "vf");
    pan2   = OceanodeParameter.ar(   \pan2,   0.0, 1, -1, 1,   "vf");
    hpcut2 = OceanodeParameterLag.ar(\hpcut2, 12,  1, 12, 120, "vf", 1/30, true).midicps;
    lpcut2 = OceanodeParameterLag.ar(\lpcut2, 120, 1, 12, 120, "vf", 1/30, true).midicps;
    res2   = OceanodeParameter.ar(   \res2,   0.0, 1, 0, 0.99, "vf");

    w2 = LeakDC.ar(fb2 * feed2.at(0) + monoIn);
    w2 = RHPF.ar(w2, hpcut2.at(0), 1 - res2.at(0));
    w2 = RLPF.ar(w2, lpcut2.at(0), 1 - res2.at(0));

    // --- Tap 3 ---
    time3  = OceanodeParameterLag.ar(\time3,  500, 1, 1, 2000, "vf", 1/30, true) / 1000;
    vol3   = OceanodeParameter.ar(   \vol3,   0.5, 1, 0, 1,    "vf");
    feed3  = OceanodeParameter.ar(   \feed3,  0.0, 1, 0, 1,    "vf");
    pan3   = OceanodeParameter.ar(   \pan3,   0.0, 1, -1, 1,   "vf");
    hpcut3 = OceanodeParameterLag.ar(\hpcut3, 12,  1, 12, 120, "vf", 1/30, true).midicps;
    lpcut3 = OceanodeParameterLag.ar(\lpcut3, 120, 1, 12, 120, "vf", 1/30, true).midicps;
    res3   = OceanodeParameter.ar(   \res3,   0.0, 1, 0, 0.99, "vf");

    w3 = LeakDC.ar(fb3 * feed3.at(0) + monoIn);
    w3 = RHPF.ar(w3, hpcut3.at(0), 1 - res3.at(0));
    w3 = RLPF.ar(w3, lpcut3.at(0), 1 - res3.at(0));

    // --- Tap 4 ---
    time4  = OceanodeParameterLag.ar(\time4,  500, 1, 1, 2000, "vf", 1/30, true) / 1000;
    vol4   = OceanodeParameter.ar(   \vol4,   0.5, 1, 0, 1,    "vf");
    feed4  = OceanodeParameter.ar(   \feed4,  0.0, 1, 0, 1,    "vf");
    pan4   = OceanodeParameter.ar(   \pan4,   0.0, 1, -1, 1,   "vf");
    hpcut4 = OceanodeParameterLag.ar(\hpcut4, 12,  1, 12, 120, "vf", 1/30, true).midicps;
    lpcut4 = OceanodeParameterLag.ar(\lpcut4, 120, 1, 12, 120, "vf", 1/30, true).midicps;
    res4   = OceanodeParameter.ar(   \res4,   0.0, 1, 0, 0.99, "vf");

    w4 = LeakDC.ar(fb4 * feed4.at(0) + monoIn);
    w4 = RHPF.ar(w4, hpcut4.at(0), 1 - res4.at(0));
    w4 = RLPF.ar(w4, lpcut4.at(0), 1 - res4.at(0));

    // Delay is inside the feedback loop — echo spacing = tapTime
    LocalOut.ar([
        DelayC.ar(w1, 2, time1.at(0)),
        DelayC.ar(w2, 2, time2.at(0)),
        DelayC.ar(w3, 2, time3.at(0)),
        DelayC.ar(w4, 2, time4.at(0))
    ]);

    // Pan each tap into the n-channel output space
    wet = PanAz.ar(n, w1, pan1.at(0), vol1.at(0), orientation: 0.5)
        + PanAz.ar(n, w2, pan2.at(0), vol2.at(0), orientation: 0.5)
        + PanAz.ar(n, w3, pan3.at(0), vol3.at(0), orientation: 0.5)
        + PanAz.ar(n, w4, pan4.at(0), vol4.at(0), orientation: 0.5);

    sig = XFade2.ar(dry, wet, mx * 2 - 1);
    Out.ar(OceanodeOutput.kr(\out), sig);

}, description: "4-tap delay with independent per-tap feedback, pan (PanAz), serial HP/LP filter and dry/wet mix", category: "Effect/Time/Delay");
)
