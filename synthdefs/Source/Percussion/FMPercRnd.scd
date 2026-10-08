(
~synthCreator.value("FMPercRnd", {|n|
    var a, freq, mFreq, mInd;
    var gate, t_gate, latchedAmp, rnd;
    var pitch;
    var peatk, pedec, pitchEnv;
    var mratio, mhnum, mhden, mfatk, mfdec, mFreqEnv;
    var miscale, mioff, modindex, miatk, midec, mIndEnv;
    var atk, decay, ampEnv;
    var level;

    // Gate / trigger — vi type, carries velocity
    gate       = OceanodeParameter.kr(\gate,    0, n, 0, 1, "vi");
    t_gate     = Trig.kr(gate > 0, 0.001);
    latchedAmp = Latch.kr(gate, t_gate);

    // Randomisation toggle: 0 = Manual, 1 = Auto
    rnd = OceanodeParameterDropdown.kr(\rnd, 0, n, "Manual:Auto");

    // --- Carrier frequency ---
    // Always manual, latched at trigger so pitch changes only take effect on the next hit.
    pitch = OceanodeParameter.kr(\pitch, 60, n, 20, 128, "vf").midicps;
    freq  = Latch.kr(pitch, t_gate);

    // --- Pitch envelope ---
    peatk    = Latch.kr(Select.kr(rnd, [OceanodeParameter.kr(\peatk, 0.002, n, 0.001, 0.02,  "vf"), TRand.kr(0.001, 0.004, t_gate)]), t_gate);
    pedec    = Latch.kr(Select.kr(rnd, [OceanodeParameter.kr(\pedec, 0.01,  n, 0.001, 0.02,  "vf"), TRand.kr(0.001, 0.02,  t_gate)]), t_gate);
    pitchEnv = EnvGen.kr(Env.perc(peatk, pedec), gate: t_gate, doneAction: 0);

    // --- Modulator frequency ---
    // mratio: carrier freq multiplier (manual 0.5–8, auto 2.pow(-1..3))
    mratio   = Latch.kr(Select.kr(rnd, [OceanodeParameter.kr(\mratio, 2.0,   n, 0.5,   8.0,   "vf"), 2.pow(TRand.kr(-1, 3, t_gate))]),        t_gate);
    mhnum    = Latch.kr(Select.kr(rnd, [OceanodeParameter.kr(\mhnum,  3,     n, 1,     5,     "vi"), TIRand.kr(1, 5, t_gate)]),                 t_gate);
    mhden    = Latch.kr(Select.kr(rnd, [OceanodeParameter.kr(\mhden,  2,     n, 1,     5,     "vi"), TIRand.kr(1, 5, t_gate)]),                 t_gate);
    mfatk    = Latch.kr(Select.kr(rnd, [OceanodeParameter.kr(\mfatk,  0.002, n, 0.001, 0.5,   "vf"), TExpRand.kr(0.001, 0.005, t_gate)]),       t_gate);
    mfdec    = Latch.kr(Select.kr(rnd, [OceanodeParameter.kr(\mfdec,  0.003, n, 0.001, 0.02,  "vf"), TRand.kr(0.001, 0.005, t_gate)]),          t_gate);
    mFreqEnv = EnvGen.kr(Env.perc(mfatk, mfdec), gate: t_gate, doneAction: 0);
    mFreq    = freq * mratio * mFreqEnv + (freq * (mhnum / mhden));

    // --- Modulation index ---
    // miscale/mioff: exponents for 2.pow() applied to carrier freq.
    // Range starts at -3/-2 — values below that are inaudible at typical pitches.
    miscale  = Latch.kr(Select.kr(rnd, [OceanodeParameter.kr(\miscale, -1.0, n, -3, 2, "vf"), TRand.kr(-3, 1, t_gate)]),        t_gate);
    mioff    = Latch.kr(Select.kr(rnd, [OceanodeParameter.kr(\mioff,    0.0, n, -2, 4, "vf"), TRand.kr(-2, 3, t_gate)]),         t_gate);
    miatk    = Latch.kr(Select.kr(rnd, [OceanodeParameter.kr(\miatk,   0.05, n, 0.01, 0.5, "vf"), TExpRand.kr(0.01, 0.5,  t_gate)]),  t_gate);
    midec    = Latch.kr(Select.kr(rnd, [OceanodeParameter.kr(\midec,    0.3, n, 0.01, 1.5, "vf"), TExpRand.kr(0.01, 1.5,  t_gate)]),  t_gate);
    modindex = Latch.kr(OceanodeParameter.kr(\modindex, 1.0, n, 0.0, 4.0, "vf"),                                                  t_gate);
    mIndEnv  = EnvGen.kr(Env.perc(miatk, midec), gate: t_gate, doneAction: 0);
    mInd     = (freq * 2.pow(miscale) * mIndEnv + (freq * 2.pow(mioff))) * modindex;

    // --- Amplitude envelope ---
    atk    = Latch.kr(Select.kr(rnd, [OceanodeParameter.kr(\atk,   0.002, n, 0.001, 0.01, "vf"), TExpRand.kr(0.001, 0.005, t_gate)]), t_gate);
    decay  = Latch.kr(OceanodeParameter.kr(\decay, 0.05, n, 0.001, 5.0, "vf"),                                                        t_gate);
    ampEnv = EnvGen.kr(Env.perc(atk, decay), gate: t_gate, doneAction: 0);

    // --- FM oscillator with noise excitation ---
    a = SinOsc.ar(
        freq * pitchEnv
        + SinOsc.ar(mFreq, mul: mInd)
        + Dust2.ar(500, freq * 0.5).lag(0.01)
    );
    a = a * ampEnv * 0.3;
    a = LeakDC.ar(LPF.ar(a, 15000));

    // --- Level ---
    // Mono signal replicated to all n channels — spatial distribution handled externally.
    level = Latch.kr(OceanodeParameter.kr(\level, 0.6, n, 0.0, 1.0, "vf"), t_gate);

    a = a * level * latchedAmp;

    Out.ar(OceanodeOutput.kr(\out), a);

}, description: "Stochastic percussive FM synth — manual or auto-random timbre per hit", category: "Source/Percussion");
)
