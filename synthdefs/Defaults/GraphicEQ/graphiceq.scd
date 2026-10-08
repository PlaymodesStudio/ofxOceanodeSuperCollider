// graphiceq.scd - 5-band parametric EQ SynthDef
// Bands: Low Shelf, Low-Mid Peak, Mid Peak, High-Mid Peak, High Shelf
// BPeakEQ uses rq (1/Q), not Q directly.
//
// Per-channel modulation: each NamedControl is initialised with an array of
// numChannels values so that C++ can send a per-channel vector via /n_setn.
// SC's multichannel expansion then routes value[ch] to channel ch.
//
// Mix (0=dry, 1=wet) uses XFade2.ar for equal-power crossfade.

var d, eqDir;

if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "/CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "/SuperCollider/CompiledSynthdefs";
};

eqDir = d +/+ "/graphiceq";
File.mkdir(d);
File.mkdir(eqDir);

(1..16).do({|numChannels|
    var synthName = ("graphiceq" ++ numChannels).asSymbol;

    // Array defaults of length numChannels so /n_setn maps index→channel
    var zeros   = 0.0   ! numChannels;
    var ones    = 1.0   ! numChannels;
    var f1def   = 80.0   ! numChannels;
    var f2def   = 250.0  ! numChannels;
    var f3def   = 1000.0 ! numChannels;
    var f4def   = 4000.0 ! numChannels;
    var f5def   = 12000.0! numChannels;

    SynthDef(synthName, {
        arg in=0, out=0;
        var sig, dry, wet;
        var b1gain, b1freq, b1slope;
        var b2gain, b2freq, b2rq;
        var b3gain, b3freq, b3rq;
        var b4gain, b4freq, b4rq;
        var b5gain, b5freq, b5slope;
        var mix;

        // Band 1: Low Shelf
        b1gain  = NamedControl.kr(\b1gain,  zeros);
        b1freq  = NamedControl.kr(\b1freq,  f1def);
        b1slope = NamedControl.kr(\b1slope, ones);

        // Band 2: Low-Mid Peak  (b2rq = 1/Q, C++ sends 1/Q)
        b2gain  = NamedControl.kr(\b2gain,  zeros);
        b2freq  = NamedControl.kr(\b2freq,  f2def);
        b2rq    = NamedControl.kr(\b2rq,    ones);

        // Band 3: Mid Peak
        b3gain  = NamedControl.kr(\b3gain,  zeros);
        b3freq  = NamedControl.kr(\b3freq,  f3def);
        b3rq    = NamedControl.kr(\b3rq,    ones);

        // Band 4: High-Mid Peak
        b4gain  = NamedControl.kr(\b4gain,  zeros);
        b4freq  = NamedControl.kr(\b4freq,  f4def);
        b4rq    = NamedControl.kr(\b4rq,    ones);

        // Band 5: High Shelf
        b5gain  = NamedControl.kr(\b5gain,  zeros);
        b5freq  = NamedControl.kr(\b5freq,  f5def);
        b5slope = NamedControl.kr(\b5slope, ones);

        // Dry/wet mix: 0 = fully dry, 1 = fully wet
        mix = NamedControl.kr(\mix, 1.0);

        sig = In.ar(in, numChannels);
        dry = sig;

        // Apply 5 bands sequentially (cascade topology).
        wet = BLowShelf.ar(sig, b1freq, b1slope, b1gain);
        wet = BPeakEQ.ar(wet,   b2freq, b2rq,    b2gain);
        wet = BPeakEQ.ar(wet,   b3freq, b3rq,    b3gain);
        wet = BPeakEQ.ar(wet,   b4freq, b4rq,    b4gain);
        wet = BHiShelf.ar(wet,  b5freq, b5slope, b5gain);

        // Equal-power crossfade: XFade2 expects pan in -1..+1
        sig = XFade2.ar(dry, wet, mix * 2 - 1);

        Out.ar(out, sig);

    }).writeDefFile(eqDir);

    if([1, 2, 4, 8, 16, 32].includes(numChannels)) {
        ("Created: " ++ synthName).postln;
    };
});

"GraphicEQ SynthDefs created successfully.".postln;
