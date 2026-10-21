// Headless entry point used by Oceanode's "Sync with SynthDefs Git"
// (scSynthdefSync) to build many sources in one sclang run:
//
//     sclang syncCompile.scd <mode> <outDir> <source.scd> [source.scd ...]
//
//   stereo  a ~synthCreator source: its metadata SynthDef plus 1 and 2 channels,
//           into <outDir>/<i>/<Name>/ (i = the source's position).
//           <source.scd>::<builds> builds those instead, a comma-separated
//           list of <channels>[_<variable value>...], e.g. lpf.scd::1,2,16
//   full    a ~synthCreator source under Defaults/: every channel count of
//           mainDefinitions' ~synthCreator, into <outDir>/<i>/<Name>/
//   script  a Defaults script: run as it is, writing into the
//           CompiledSynthdefs/ folder next to it (<outDir> is unused)
//
// After each source it posts OCEANODE_SYNC_OK <source> or
// OCEANODE_SYNC_FAILED <source> :: <reason>, and exits with 0.

var args = thisProcess.argv;
var mode = args[0].asSymbol;
var outDir = args[1];
var sources = args.copyRange(2, args.size - 1);
var definitions = thisProcess.nowExecutingPath.dirname +/+ "mainDefinitions.scd";

sources.do { |entry, i|
    var ok = false, reason = "";
    var parts = entry.split($:).reject(_.isEmpty);
    var source = parts[0];
    var builds = if(parts.size > 1) {
        parts[1].split($,).collect { |build|
            var numbers = build.split($_).collect(_.asInteger);
            [numbers[0], numbers.copyRange(1, numbers.size - 1)]
        }
    } { [[1, []], [2, []]] };
    try {
        switch(mode,
            \stereo, {
                definitions.load;
                d = outDir +/+ i.asString;
                File.mkdir(d);
                ~useSyncSynthCreator.(builds);
                ~syncBuilt = 0;
                source.load;
                // A source that does not parse is only reported on the post
                // window: it then built nothing.
                ok = ~syncBuilt > 0;
                if(ok.not) { reason = "it built no SynthDef (see the log)" };
            },
            \full, {
                definitions.load;
                d = outDir +/+ i.asString;
                File.mkdir(d);
                source.load;
                ok = true;
            },
            \script, {
                // Scripts that do not choose their own folder write into d
                d = source.dirname +/+ "CompiledSynthdefs";
                File.mkdir(d);
                source.load;
                ok = true;
            },
            { reason = "unknown mode " ++ mode }
        );
    } { |error|
        error.reportError;
        reason = error.errorString;
    };
    if(ok) {
        ("OCEANODE_SYNC_OK " ++ source).postln;
    } {
        ("OCEANODE_SYNC_FAILED " ++ source ++ " :: " ++ reason.replace("\n", " ")).postln;
    };
};

0.exit;
