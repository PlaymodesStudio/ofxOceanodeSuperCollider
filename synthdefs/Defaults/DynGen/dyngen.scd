// dyngen.scd — DynGen slot SynthDef generator
//
// Generates DynGenWrapper_<numChans>_<slot> for every (channel, slot) pair.
// Also generates DynGenPolyWrapper_<slot>: mono wrappers with 64 controls for
// DynGen Poly, which creates one independent wrapper per output channel.
//
// ─── Feature 1: Per-channel array NamedControls ───────────────────────────────
//   Each param slot p<i> uses an array NamedControl with n elements so EEL2
//   code can receive a distinct value per audio channel.
//
//   EEL2 input layout:
//     in(0)..in(n-1)                 → audio channels
//     in(n + pi*n + ci)              → param pi, channel ci
//
//   Backward-compatible with mono (n=1):
//     in(1 + pi*1 + 0) = in(1 + pi) ← same index as the old scalar layout ✓
//
//   C++ sends values via synth->set("p0", expandedVector) where expandedVector
//   has numChannels elements.  A 1-element Oceanode vector is broadcast to all
//   channels; an N-element vector is applied per-channel.
// ─────────────────────────────────────────────────────────────────────────────
//
// After evaluating this file:
//   1. The kSlotHashes block is printed to the post window.
//      Copy it into scDynGenNode.cpp.
//   2. Copy CompiledSynthdefs/dyngen/ to the app data path:
//      <data>/Supercollider/Synthdefs/dyngen/
//
// Requirements: DynGen extension installed, server rebooted.

var d, dynDir;
var maxSlots  = 16;   // keep in sync with kMaxSlots  in scDynGenNode.h
var maxChans  = 16;   // keep in sync with kMaxChans   in scDynGenNode.h
var maxParams = 8;    // keep in sync with kMaxParams  in scDynGenNode.h
var polyMaxParams = 64; // keep in sync with kMaxParams in scDynGenPolyNode.h
var totalCreated = 0;

if(thisProcess.nowExecutingPath.notNil) {
	d = thisProcess.nowExecutingPath.dirname +/+ "/CompiledSynthdefs";
} {
	d = Platform.userAppSupportDir +/+ "/SuperCollider/CompiledSynthdefs";
};

dynDir = d +/+ "/dyngen";
File.mkdir(d);
File.mkdir(dynDir);

("Generating DynGen SynthDefs (Feature 1: per-channel array params)...").postln;
("Output dir: " ++ dynDir).postln;

(1..maxChans).do({|n|
	(0..(maxSlots - 1)).do({|slot|
		var slotSym = ("dyngenSlot" ++ slot).asSymbol;
		var defName = ("DynGenWrapper_" ++ n ++ "_" ++ slot).asSymbol;

		SynthDef(defName, {
			// out, in, update, sync remain scalar controls
			|out=0, in=0, update=1.0, sync=0.0|

			// Audio inputs: n channels from the bus (with feedback support)
			var audioInputs = Array.fill(n, { |i| InFeedback.ar(in + i) });

			// Per-channel array NamedControls for each param slot.
			// For n==1: scalar NamedControl (same wire format as before).
			// For n>1:  array NamedControl with n slots.
			// C++ sends the full n-element vector via /n_setn.
			// Layout after flat: [p0_ch0, p0_ch1, ..., p0_ch(n-1), p1_ch0, ...]
			// EEL2 index:        in(n + pi*n + ci)
			var paramInputs = maxParams.collect({|pk|
				if(n == 1) {
					// Scalar NamedControl — backward-compatible wire index
					K2A.ar(NamedControl.kr(("p" ++ pk).asSymbol, 0.0))
				} {
					// Array NamedControl — n slots, one per audio channel
					K2A.ar(NamedControl.kr(("p" ++ pk).asSymbol, 0.0.dup(n)))
				}
			}).flat;

			// DynGen gets: n audio inputs + n*maxParams param inputs
			var result = DynGen.ar(
				n,
				slotSym,
				audioInputs ++ paramInputs,
				update: update,
				sync:   sync
			);

			Out.ar(out, result);

		}).writeDefFile(dynDir);

		totalCreated = totalCreated + 1;
	});
});

// DynGen Poly uses one mono SynthDef per voice, so it can expose 64 controls
// without multiplying them by the output-channel count inside one SynthDef.
(0..(maxSlots - 1)).do({|slot|
	var slotSym = ("dyngenSlot" ++ slot).asSymbol;
	var defName = ("DynGenPolyWrapper_" ++ slot).asSymbol;

	SynthDef(defName, {
		|out=0, in=0, update=1.0, sync=0.0|
		var audioInput = InFeedback.ar(in);
		var paramInputs = polyMaxParams.collect({|pk|
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
