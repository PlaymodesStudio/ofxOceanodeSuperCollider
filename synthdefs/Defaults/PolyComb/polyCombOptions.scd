// Optional PolyComb inserts. The original PCFilter/PCComb definitions in
// polyCombParts.scd are deliberately untouched: existing presets use them.
// Needs the existing AnalogFilter UGen from ugens/OceanodeAnalog.
(
var root, dir;
root = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
dir = root +/+ "PolyComb";
File.mkdir(root); File.mkdir(dir);
(1..32).do { |n|
    var kv = { |name, def| NamedControl.kr(name, def ! n) };
    var modIn = { |name| In.kr(NamedControl.kr(name,0),n) };
    var base = { In.kr(\note.kr(0)+n,n) };
    // Adds to the voice bus after PCAmp, before spatialization and shared FX.
    // Its own gate envelope replaces NI's audio-following sub dynamics.
    SynthDef(("PCSub" ++ n).asSymbol, {
        var gate = In.ar(\gate.kr(0), n);
        var on = \on.kr(1,0.03);
        var cable=A2K.kr(NamedControl.ar(\pitch_ar,60!n));
        // One oscillator at the input root (first pitch), shared by the voice
        // envelopes. Chord tones must never become extra sub frequencies.
        var pitch = Select.kr(\pitch_sel.kr(0), [kv.(\pitch,60).asArray[0], cable.asArray[0]]);
        var atk=kv.(\atk,0.01).asArray, dec=kv.(\dec,0.5).asArray, sus=kv.(\sus,1).asArray, rel=kv.(\rel,0.5).asArray;
        var curves=[kv.(\cA,-4),kv.(\cD,-4),kv.(\cR,-4)].collect(_.asArray);
        var env = n.collect { |i| EnvGen.ar(Env.adsr(atk[i],dec[i],sus[i],rel[i],
            curve:curves.collect { |c| c[i] }),gate.asArray[i]) };
        var velocity = (1-kv.(\velsens, 1)) + (In.kr(\vel.kr(0),n).clip(0,1)*kv.(\velsens, 1));
        // One vibrato LFO for the single root tone, independent of main voices.
        var rate = \vibrate.kr(5,0.03) * (1 + (Rand(-1,1) * \vibrand.kr(0,0.03).clip(0,1)));
        var vibrato = SinOsc.kr(rate, Rand(0,2pi)) * \vibamt.kr(0,0.03);
        var tuned = K2A.ar(Lag.kr(pitch+\transpose.kr(-12),0.02)+vibrato);
        var sig = SinOsc.ar(tuned.clip(0,135).midicps.min(SampleRate.ir*0.45));
        Out.kr(\vis.kr(0), A2K.kr(env)*on);
        Out.ar(\out.kr(0),sig*env*velocity*\level.kr(0.25,0.03)*\master.kr(0.8)*on);
    }).writeDefFile(dir);
    SynthDef(("PCFilterSlopes" ++ n).asSymbol, {
        var sig = In.ar(\in.kr(0),n);
        var cut = kv.(\cutoff,100)+(kv.(\keytrack, 0)*(base.()-60))+modIn.(\mcut);
        var fc = cut.clip(0,135).midicps.clip(20,(SampleRate.ir*0.45).min(20000));
        var rq = (1-(kv.(\res,0.3)+modIn.(\mres))).clip(0.02,1);
        var slope = \slope.kr(1).clip(1,3)-1;
        var lp2 = BLowPass.ar(sig,fc,rq), hp2 = BHiPass.ar(sig,fc,rq);
        var lp4 = BLowPass4.ar(sig,fc,rq), hp4 = BHiPass4.ar(sig,fc,rq);
        var bp2 = BPF.ar(sig,fc,rq), br2 = BRF.ar(sig,fc,rq);
        var bp4 = BPF.ar(bp2,fc,rq), br4 = BRF.ar(br2,fc,rq);
        // Four one-pole sections: 6 dB high-pass + 18 dB low-pass. Unlike
        // NI's proprietary multimode cell, this branch has no resonance loop.
        var pole = (-2pi*fc*SampleDur.ir).exp;
        var asym = sig-OnePole.ar(sig,pole);
        var lp, hp, bp, br, wet;
        3.do { asym = OnePole.ar(asym,pole) };
        lp=Select.ar(slope,[lp2,lp4,lp4]);hp=Select.ar(slope,[hp2,hp4,hp4]);
        bp=Select.ar(slope,[bp2,bp4,asym]);br=Select.ar(slope,[br2,br4,br4]);
        wet=Select.ar(\type.kr(0).clip(0,3),[lp,hp,bp,br]);
        Out.ar(\out.kr(0),LinXFade2.ar(sig,wet,\on.kr(1,0.03)*2-1));
    }).writeDefFile(dir);
};
"PolyComb optional inserts written to ".post;dir.postln;
)
