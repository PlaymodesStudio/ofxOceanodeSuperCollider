// rave.scd — RAVE neural audio timbre transfer SynthDefs
//
// Uses NNUGen.ar directly with fixed scalar params so the SynthDef can be
// compiled WITHOUT having any model loaded (no sclang model store needed).
//
// Model slot:  always integer 0  (C++ always loads to slot 0)
// Method:      index 0           (RAVE "forward" method)
// Buffer size: -1                (use model's native buffer size)
// Channels:    1..8              (set N Chan on the C++ node to match model output)
//
// After evaluating this file in SuperCollider:
//   Copy CompiledSynthdefs/rave/ to <appdata>/Supercollider/Synthdefs/rave/
//   The server will load them via /d_loadDir on startup.

(
var d;
if(thisProcess.nowExecutingPath.notNil) {
    d = thisProcess.nowExecutingPath.dirname +/+ "CompiledSynthdefs";
} {
    d = Platform.userAppSupportDir +/+ "SuperCollider/CompiledSynthdefs";
};
File.mkdir(d);
File.mkdir(d +/+ "rave");

(1..8).do({ |n|
    SynthDef(("rave" ++ n).asSymbol, {
        arg in = 0, out = 0;
        // Single scalar levels so channel count doesn't need to match amp size
        var levels = \levels.kr(1.0, 1/30);
        var sig = In.ar(in, 1);  // RAVE models take mono input
        // NNUGen arguments: modelSlot=0, methodIdx=0 (forward),
        //   bufSize=-1 (model native), numOutputs=n, warmup=0, debug=0, batches=1
        var processed = NNUGen.ar(0, 0, -1, n, 0, 0, 1, sig);
        processed = LeakDC.ar(processed);   // remove DC offset
        processed = processed.tanh;          // safety limiting
        Out.ar(out, processed * levels);
    }).writeDefFile(d +/+ "rave");
});

("RAVE SynthDefs (rave1..rave8) compiled to: " + (d +/+ "rave")).postln;
("Copy that directory to <appdata>/Supercollider/Synthdefs/rave/").postln;
)
