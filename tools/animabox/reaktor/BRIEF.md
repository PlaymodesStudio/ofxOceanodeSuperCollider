# AnimaMundi Reaktor ensemble — decoding brief (shared by all analysis agents)

Goal of the overall project: re-implement the Reaktor ensemble `~/Downloads/AnimaMundi2_tips.ens`
as a SuperCollider synth ("AnimaBox") in the ofxOceanodeSuperCollider addon
(`/Users/santiagovilanova/Documents/OF/of_playmodes/openFrameworks/addons/ofxOceanodeSuperCollider`).
Your job is ONLY to decode one section into a precise, implementable DSP/maths spec. Do not edit
any file outside the scratchpad `anima/` folder. Do not build or run the app.

## Files
- Decoded report: `SCRATCH/anima/am.json` (50 MB; don't cat it, query it with python).
  SCRATCH = /private/tmp/claude-501/-Users-santiagovilanova-Documents-OF-of-playmodes-openFrameworks-addons-ofxOceanodeSuperCollider/7768c92a-ac06-40a9-b1b7-5a4548cbf300/scratchpad
- Primary (non-Core) macro netlist viewer, run from the addon root:
  `python3 -I SCRATCH/net2.py SCRATCH/anima/am.json <macro_object_id> [more ids]`
  prints children (type, label, constants `=value`, fader ranges, descriptions) and wires
  `Src#id.outPort -> Dst#id.inPort` with port labels from the Reaktor module catalog.
- Core cell viewer (from the addon root):
  `python3 -I tools/ReaktorPatchExtractor/core_view.py SCRATCH/anima/am.json --list`
  `... --cell <object_id> --tree` then `... --cell <id> --net <path>` for a structure netlist.
  Read the docstring at the top of core_view.py for notation (qbusN, SR, #constants).
- Module docs (port help, maths of every Primary module): `tools/ReaktorPatchExtractor/module_catalog.json`
  (key = module kind; has inputs/outputs with label+help, description). Core module catalog:
  `tools/ReaktorPatchExtractor/core_module_catalog.json`.
- Panel controls with values in each of the 20 snapshots: `SCRATCH/anima/controls.txt`
  (columns: object id, type, path, label, [range/step], then value in snapshot 0..19;
  snapshot names on line 1; snapshot 19 "init/Soul Bank" is garbage, ignore it).
  Switch values there are 1-based positions; -1 means not stored (use the saved panel value).
- Saved panel state of a control (when not in snapshots): in the .ens at module offset
  (field `offset` in am.json) + 0x112: little-endian float32 = normalized 0..1 value for
  faders (value = lo + (hi-lo)*norm), int32 1-based position for switches.

## Ensemble overview (already established)
- Instrument#2696 "Body" (exciter, stereo): Noise/Saw/Pulse/Impulse/Geiger oscillators or the
  live audio IN, ADSR on note gate, pitch through a Scale Fitter. Feeds Instrument#3.
- Instrument#3 = the filter bank. It is POLYPHONIC: each Reaktor voice is one band
  (Bands fader 0..120). Filters#19: per voice a Multi/LP 4-Pole BP4 output, stereo L/R,
  Distributor/Panner + Voice Info stereo placement, Audio Voice Combiner, Saturator.
  BP Index Behavior#2093 computes per-voice pitch / energy / resonance:
  Pitch Index#12294 -> Harmony#10661 (scales/chords; will be replaced by an external
  scale input in the port, but decode what it outputs) -> Pitch Modifiers#5460 -> pitch;
  LFO BAND ENERGY#6808 -> Band Energy#2342 -> amplitude; LFO Q#7649 -> Q#2119 / Band Q#3284 -> resonance.
  Then Post Filter#15262 (Reaktor multimode library filter).
- Instrument#12635 delay2 (Core "Delay 4p"), Instrument#12218 Metaverb (Core "Metaverb 2.7b").
- Reaktor conventions: pitch inputs are MIDI note numbers (1 semitone/unit, 69 = 440 Hz);
  "Voice Info" gives voice index; polyphonic event signals are per voice.

## What a good spec looks like
Write to `SCRATCH/anima/spec_<section>.md`. For every panel control in your section: object id,
label/purpose, range, how it enters the maths (exact formula, units, constants). Give the
signal flow as equations/pseudocode a SuperCollider or C++ programmer can implement without
Reaktor, including per-voice (band index) dependence, rates (audio/control/event), smoothing,
clipping, and any hidden constants. Mark anything you could not determine as UNCERTAIN with
your best guess. Keep it factual and compact (no essay); tables + pseudocode. Your final reply
should be a short summary (<= 25 lines) and the spec path.
