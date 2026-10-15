// Headless entry point used by the SC Code node's Promote (scCodeNode) to
// build a new ~synthCreator source the way the library is built:
//
//     sclang promote.scd <outDir> <source.scd> <maxChannels>
//
// Writes <outDir>/<Name>/<Name>.scsyndef + <Name>.txarcmeta (the node
// metadata) and <Name>1 .. <Name><maxChannels>.scsyndef. Exits with 0, or 1
// if anything threw; a source that fails to parse is only reported on the
// post window, so the caller checks that the files exist.

var args = thisProcess.argv;
var ok = false;

try {
    (thisProcess.nowExecutingPath.dirname +/+ ".." +/+ "Oceanode" +/+ "mainDefinitions.scd").load;
    d = args[0];
    File.mkdir(d);
    ~maxVoices = args[2].asInteger.max(1);
    ~synthCreator = ~synthCreatorWithMetadata;
    args[1].load;
    ok = true;
} { |error|
    error.reportError;
};

(if(ok) { "PROMOTE_OK" } { "PROMOTE_FAILED" }).postln;
(if(ok) { 0 } { 1 }).exit;
