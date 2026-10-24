// dyngen.scd — DynGen slot SynthDef generator
// Generates one mono wrapper per slot, with 64 parameter controls.
// DynGen creates one independent wrapper instance per output channel.
// Keep the deployed DynGenPolyWrapper_<slot> names for compatibility.
// EEL2 inputs: in(0) = audio; in(1)..in(64) = p0..p63; output = out0.
// Requires the DynGen extension. Evaluate, then deploy CompiledSynthdefs/dyngen/.

var d, dynDir;
var maxSlots  = 16;   // keep in sync with kMaxSlots  in scDynGenNode.h
var maxParams = 64;   // keep in sync with kMaxParams in scDynGenNode.h
var totalCreated = 0;

if(thisProcess.nowExecutingPath.notNil) {
	d = thisProcess.nowExecutingPath.dirname +/+ "/CompiledSynthdefs";
} {
	d = Platform.userAppSupportDir +/+ "/SuperCollider/CompiledSynthdefs";
};

dynDir = d +/+ "/dyngen";
File.mkdir(d);
File.mkdir(dynDir);

("Generating DynGen mono SynthDefs...").postln;
("Output dir: " ++ dynDir).postln;

(0..(maxSlots - 1)).do({|slot|
	var slotSym = ("dyngenSlot" ++ slot).asSymbol;
	var defName = ("DynGenPolyWrapper_" ++ slot).asSymbol;

	SynthDef(defName, {
		|out=0, in=0, update=1.0, sync=0.0|
		var audioInput = InFeedback.ar(in);
		var paramInputs = maxParams.collect({|pk|
			K2A.ar(NamedControl.kr(("p" ++ pk).asSymbol, 0.0))
		});
		var result = DynGen.ar(
			1,
			slotSym,
			[audioInput] ++ paramInputs,
			update: update,
			sync: sync
		);

		Out.ar(out, result);
	}).writeDefFile(dynDir);

	totalCreated = totalCreated + 1;
});

("Created " ++ totalCreated ++ " SynthDefs in " ++ dynDir).postln;

// ─── Print slot hashes for C++ hardcoding ────────────────────────────────────
"".postln;
"// ===== Copy into scDynGenNode.cpp (replace the kSlotHashes initializer) =====".postln;
("const int scDynGenNode::kSlotHashes[scDynGenNode::kMaxSlots] = {").postln;
(0..(maxSlots - 1)).do({|i|
	var h      = DynGenDef.prHashSymbol(("dyngenSlot" ++ i).asSymbol);
	var suffix = if(i < (maxSlots - 1), {","}, {""});
	("    " ++ h.asString ++ suffix ++ " // slot " ++ i).postln;
});
"};".postln;
"// ===========================================================================".postln;
"".postln;
"Done. Paste the hash values above into scDynGenNode.cpp, then recompile.".postln;
