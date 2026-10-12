// rave.scd — SynthDefs for the RAVE node (nn.ar neural audio models)
//
// One SynthDef per model shape: rave_<inputs>_<outputs>, inputs 1..4,
// outputs 1..8. The C++ node reads the shape of the model's method from the
// info file nn.ar writes on /nn_load and picks the matching SynthDef.
//
// Model slot, method and buffer size are scalar (.ir) controls, set as
// /s_new arguments: NNUGen reads them once, in its constructor, so a SynthDef
// does not have to be compiled per slot.
//
//   model    nn.ar model slot (the id sent with /cmd /nn_load)
//   method   method index inside the model ("forward" for RAVE)
//   bufsize  -1 = the model's own buffer size; larger powers of two trade
//            latency for fewer dropouts
//   warmup   silent passes run on the compute thread before audio comes out
//   inputgain  gain into the model; levels: gain after it
//   limit    1 = peak limiter at -0.45 dBFS after levels (10 ms lookahead)
//
// Writes CompiledSynthdefs/Rave/ next to this file. Copy that folder to
// <app>/bin/data/Supercollider/Synthdefs/Defaults/Rave/ to deploy it.

(
var d, subDir;
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};
subDir = d +/+ "Rave";
File.mkdir(d);
File.mkdir(subDir);

(1..4).do({ |numIn|
    (1..8).do({ |numOut|
        SynthDef(("rave_" ++ numIn ++ "_" ++ numOut).asSymbol, {
            var levels = \levels.kr(1.0).lag(1/30);
            var limit = \limit.kr(1).lag(1/30);
            var sig = In.ar(\in.kr(0), numIn) * \inputgain.kr(1.0).lag(1/30);
            sig = NNUGen.ar(\model.ir(0), \method.ir(0), \bufsize.ir(-1), numOut,
                \warmup.ir(2), 0, 1, sig.asArray);
            // Models often generate far above full scale whatever the input
            // level (+17 dBFS measured): limit after the output level
            sig = Sanitize.ar(LeakDC.ar(sig)) * levels;
            sig = (sig * (1 - limit)) + (Limiter.ar(sig, 0.95, 0.01) * limit);
            Out.ar(\out.kr(0), sig);
        }).writeDefFile(subDir);
    });
});

("RAVE SynthDefs written to: " + subDir).postln;
)
