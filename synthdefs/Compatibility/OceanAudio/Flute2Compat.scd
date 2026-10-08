(
~synthCreator.value("Flute2Compat", {|n|
    var numch=n;

    // Original flute parameters
    var pch, ibreath, ifeedbk1, ifeedbk2, vibamp, vibfreq, lpf;
    var noise, levels, lpf_exciter, q, poles, dcnoise;
    var softhard, sym, push, offset, polarity;
    var gate, atk, rel, curve;
    var sr, cr, block, minDelay, crossoverFreq, crossoverNote;
    var ifqc, exciter_env, exciter_noise, filterednoises;
    var hipos, hineg, sympos, symneg;
    var lowSignal, highSignal, hybridSignal;

    // Variables for the lowSignal function
    var fdbckArray, aflow1, asum1, afqc, aflute1, asum2, ax, asum3, avalue;
    var eventclippos, eventclipneg, clippedsigpos, clippedsigneg;
    var satpos, satneg, saturatedSignal, delayedSignal;

    // Variables for the highSignal function
    var source, vib, freq, res1, res2, res3, mixed;

    // Parameter assignments
    pch = OceanodeParameterLag.kr(\pitch, 24, numch, 0, 130, "vf", 1/30, true);
    ibreath = OceanodeParameterLag.kr(\ibreath, 0.09, numch, 0, 1, "vf", 1/30, true);
    ifeedbk1 = OceanodeParameterLag.kr(\ifeedbk1, 0.4, numch, 0, 1, "vf", 1/30, true);
    ifeedbk2 = OceanodeParameterLag.kr(\ifeedbk2, 0.4, numch, 0, 1, "vf", 1/30, true);
    vibamp = OceanodeParameterLag.kr(\vibamp, 0.2, numch, 0, 1, "vf", 1/30, true);
    vibfreq = OceanodeParameterLag.kr(\vibfreq, 5, numch, 0, 10, "vf", 1/30, true);
    lpf = OceanodeParameterLag.kr(\lpf, 2000, numch, 0, 10000, "vf", 1/30, true);
    noise = OceanodeParameterLag.kr(\noise, 1, numch, 0, 1, "vf", 1/30, true);
    levels = OceanodeParameterLag.kr(\levels, 1, numch, 0, 1, "vf", 1/30, true);

    lpf_exciter = OceanodeParameterLag.kr(\breath_lpf, 80, numch, 0, 130, "vf", 1/30, true).midicps;
    q = OceanodeParameterLag.kr(\q, 1, numch, 0, 10, "vf", 1/30, true);
    poles = OceanodeParameter.kr(\poles, 0, numch, 0, 1, "vi");
    dcnoise = OceanodeParameterLag.kr(\dcnoise, 0, numch, 0, 1, "vf", 1/30, true);

    softhard = OceanodeParameterLag.kr(\softhard, 0.5, numch, 0, 1, "vf", 1/30, true);
    sym = OceanodeParameterLag.kr(\sym, 0.5, numch, 0, 1, "vf", 1/30, true);
    push = OceanodeParameterLag.kr(\push, 1, numch, 0, 10, "vf", 1/30, true);
    offset = OceanodeParameterLag.kr(\offset, 0, numch, -1, 1, "vf", 1/30, true);
    polarity = OceanodeParameterLag.kr(\polarity, 1, numch, -1, 1, "vf", 1/30, true);
    gate = OceanodeParameter.kr(\gate, 1, numch, 0, 1, "vi");
    atk = OceanodeParameterLag.kr(\atk, 0.01, numch, 0, 1, "vf", 1/30, true);
    rel = OceanodeParameterLag.kr(\rel, 0.1, numch, 0, 1, "vf", 1/30, true);
    curve = OceanodeParameterLag.kr(\curve, -4, numch, -8, 8, "vf", 1/30, true);

    // System variables
    sr = SampleRate.ir;
    cr = ControlRate.ir;
    block = cr.reciprocal;
    minDelay = (64/sr);
    crossoverFreq = minDelay.reciprocal;
    crossoverNote = 78;

    ifqc = pch.midicps;
    exciter_env = EnvGen.kr(
        envelope: Env.asr(attackTime: atk, sustainLevel: 1, releaseTime: rel, curve: curve),
        gate: gate
    );

    exciter_noise = WhiteNoise.ar(numch) * ibreath;
    filterednoises = [
        OnePole.ar(exciter_noise, exp(-2pi * (lpf_exciter / sr))),
        RLPF.ar(exciter_noise, lpf_exciter, q)
    ];
    exciter_noise = Select.ar(poles, filterednoises);
    exciter_noise = XFade2.ar(DC.ar(ibreath), exciter_noise, dcnoise);
    exciter_noise = exciter_noise * exciter_env;

    hipos = softhard;
    hineg = softhard;
    sympos = sym + 1;
    symneg = sym.neg + 1;

    // Low frequency model
    fdbckArray = LocalIn.ar(numch);
    aflow1 = exciter_noise;
    asum1 = aflow1 + (SinOsc.ar(vibfreq, 0, 0.1 * vibamp) * exciter_env);
    afqc = ifqc.reciprocal - (asum1/20000) - (9/sr) + (ifqc/12000000) - block;
    aflute1 = fdbckArray;
    asum2 = asum1 + (aflute1 * push * polarity) + (offset * polarity);
    ax = DelayC.ar(asum2, ifqc.reciprocal - block * 0.5, afqc * 0.5 - (asum1/ifqc/cr) + 0.001);

    eventclippos = Clip.kr(hipos, 0.00001, 0.99999) * sympos;
    eventclipneg = Clip.kr(hineg, 0.00001, 0.99999) * symneg;
    clippedsigpos = Clip.ar(ax, eventclippos.neg, eventclippos);
    clippedsigneg = Clip.ar(ax, eventclipneg.neg, eventclipneg);

    satpos = ((((eventclippos.neg)+sympos)*0.5) *
        SoftClipAmp.ar((((((eventclippos.neg)+sympos)*0.5).reciprocal) *
        (clippedsigpos.neg + ax)))) + clippedsigpos;

    satneg = ((((eventclipneg.neg)+symneg)*0.5) *
        SoftClipAmp.ar((((((eventclipneg.neg)+symneg)*0.5).reciprocal) *
        (clippedsigneg.neg + ax)))) + clippedsigneg;

    saturatedSignal = Select.ar((ax+1)/2, [satpos, satneg]);
    asum3 = saturatedSignal + (aflute1 * ifeedbk2);
    avalue = LPF.ar(asum3, lpf);
    delayedSignal = DelayC.ar(avalue, ifqc.reciprocal - block, afqc);

    LocalOut.ar(delayedSignal);
    lowSignal = LeakDC.ar(avalue);

    // High frequency model
    source = exciter_noise;
    vib = SinOsc.ar(vibfreq, 0, vibamp * exciter_env);
    freq = ifqc + vib;

    res1 = Resonz.ar(source, freq, 0.01, 2);
    res2 = Resonz.ar(source, freq * 2, 0.02, 1);
    res3 = Resonz.ar(source, freq * 3, 0.03, 0.5);

    mixed = (res1 + res2 + res3) * 4;
    mixed = mixed * (1 + (mixed * push * polarity)) + (offset * polarity);

    eventclippos = Clip.kr(hipos, 0.00001, 0.99999) * sympos;
    eventclipneg = Clip.kr(hineg, 0.00001, 0.99999) * symneg;
    clippedsigpos = Clip.ar(mixed, eventclippos.neg, eventclippos);
    clippedsigneg = Clip.ar(mixed, eventclipneg.neg, eventclipneg);

    satpos = ((((eventclippos.neg)+sympos)*0.5) *
        SoftClipAmp.ar((((((eventclippos.neg)+sympos)*0.5).reciprocal) *
        (clippedsigpos.neg + mixed)))) + clippedsigpos;

    satneg = ((((eventclipneg.neg)+symneg)*0.5) *
        SoftClipAmp.ar((((((eventclipneg.neg)+symneg)*0.5).reciprocal) *
        (clippedsigneg.neg + mixed)))) + clippedsigneg;

    saturatedSignal = Select.ar((mixed+1)/2, [satpos, satneg]);
    highSignal = BPF.ar(saturatedSignal, [500, 1200, 2400], [0.1, 0.1, 0.1], [1, 0.5, 0.25]).sum;

    // Final mix
    hybridSignal = XFade2.ar(
        lowSignal,
        highSignal,
        (pch - crossoverNote).linlin(0, 12, -1, 1).clip(-1, 1)
    );

    hybridSignal = hybridSignal * levels;
    hybridSignal = Limiter.ar(hybridSignal);

    Out.ar(OceanodeOutput.kr(\out), hybridSignal);
}, description: "Original hybrid physical flute", category: "Compatibility/OceanAudio");
)
