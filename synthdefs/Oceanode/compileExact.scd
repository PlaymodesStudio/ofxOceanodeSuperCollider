// Headless entry point used by Oceanode (scSynthdefCompiler) to build one
// channel count of a ~synthCreator synth:
//
//     sclang compileExact.scd <outDir> <source.scd> <numChannels> [variable values...]
//
// Writes <outDir>/<Name>/<Name><numChannels>[_<v>...].scsyndef (no metadata
// SynthDef) and exits with 0, or with 1 if anything threw. A source that fails
// to parse is only reported on the post window, so the caller must also check
// that the expected file exists.

var args = thisProcess.argv;
var ok = false;

try {
    (thisProcess.nowExecutingPath.dirname +/+ "mainDefinitions.scd").load;
    d = args[0];
    File.mkdir(d);
    ~useExactSynthCreator.(
        args[2].asInteger,
        args.copyRange(3, args.size - 1).collect(_.asInteger)
    );
    args[1].load;
    ok = true;
} { |error|
    error.reportError;
};

(if(ok) { "COMPILE_EXACT_OK" } { "COMPILE_EXACT_FAILED" }).postln;
(if(ok) { 0 } { 1 }).exit;
