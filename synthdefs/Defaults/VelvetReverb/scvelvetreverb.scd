// scvelvetreverb.scd
//
// scconvolution.scd + post-reverb LPF/HPF + LocalIn/LocalOut feedback.
// Single |specbuf = 0| regular arg — identical to scconvolution.scd.
//
// fftsize = 2048  →  partition = 1024 samples ≈ 23 ms latency at 44.1 kHz

var d, dir;
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "/CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "/SuperCollider/CompiledSynthdefs";
};
dir = d +/+ "/scvelvetreverb";
File.mkdir(d); File.mkdir(dir);

(1..16).do({ |n|
    var name = ("scvelvetreverb" ++ n).asSymbol;

    SynthDef(name, {
        |in = 0, out = 0, specbuf = 0, wet = 0.0, level = 1.0,
         lpf = 12000, hpf = 20, fbAmt = 0|

        var dry, fb, convIn, conv, smoothWet, mix;

        dry    = In.ar(in, n);
        fb     = (LocalIn.ar(n, 0) * fbAmt).tanh;
        convIn = (dry + fb).asArray;

        conv = convIn.collect { |ch| PartConv.ar(ch, 2048, specbuf) };

        conv = LPF.ar(conv, lpf.clip(20, 20000));
        conv = HPF.ar(conv, hpf.clip(20, 15000));

        LocalOut.ar(conv.as(Array));

        smoothWet = Lag.kr(wet, 0.025);
        mix = XFade2.ar(dry.asArray, conv, smoothWet * 2.0 - 1.0);

        Out.ar(out, mix * level);

    }).writeDefFile(dir);

    ("Created: " ++ name).postln;
});

"SC Velvet Reverb SynthDefs written.".postln;
