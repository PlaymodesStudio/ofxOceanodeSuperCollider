# Shared snapshot and MacroMixer sound morphs: PolyComb pilot

See the [consolidated reference](polyCombMorphing.md) for current build and
regression instructions across the complete PolyComb morph implementation.

PolyComb's MacroMixer integration now uses the same policy-aware callbacks as
its snapshots. A different Voices value no longer replaces the PolyComb node.
Its temporary voice banks fade before the existing shared effects, then the
running destination bank is adopted and temporary resources are released.
Normal playback and same-count morphs allocate no extra DSP.

## Ownership and execution

`ofxOceanodeNodeModel::morphKeys().blended` declares saved fields that the node
owns during a morph. These may also be registered node parameters. MacroMixer
keeps those fields in the node-state comparison, reports their changes in Morph
details, calls the node's morph hooks, and excludes them from generic parameter
interpolation. A control must have one writer during a morph.

PolyComb includes its registered `Voices` field alongside its hidden controls,
modulation matrix, source counts and modulation parameter state. Its converter
maps the native preset's `Voices` to the snapshot's `voices`. Missing counts in
older saves retain the snapshot engine's patch-count fallback. Output Channels
remain structural. Incoming connections keep precedence inside the synth.

`scSnapshots::captureForExternalMorph()` captures the current sound through the
existing metadata-aware callback and stops any local timed glide.
`applyExternalMorph(a,b,t,switched)` calls the existing policy-aware application
callback. The external caller supplies progress and its discrete switch decision;
the snapshot curve is not applied a second time. These operations do not recall
a snapshot slot or change the library.

PolyComb's MacroMixer hooks convert the destination saved state once, capture
the current sound, and use these entry points for progress and cancellation.
Captured intermediate count distributions remain available when a subsequent
sound morph starts. Cancelling restores the captured source sound through the
same path, including its effective blend.

## Every other Box (2026-10-29)

AnalogBox, AdditiveBox, AliasingBox, DrumKitBox, EchoBox, FMBox, GrainBox,
PhysicalBox, PulsarBox, SampleBox, SlicerBox, SteamPipeBox and WavetableBox
have the same adapter through `shared/scBoxMacroMorph.h`: `snapshotOf()`
converts the saved patch to the Box's snapshot format, and begin/apply/end go
through `captureForExternalMorph` / `applyExternalMorph`. Before this only
PolyComb had it, so any other Box whose saved sound differed was replaced by a
crossfaded second instance, and an int Voices was interpolated step by step
(one synth rebuild per step).

- `blended` is exactly what `snapshotOf` reads. Keys the snapshot does not carry
  (published lists, timeline, Random Shared, sample files, slice markers) stay
  out, so a difference there still replaces the node. UI-only keys are `ignored`.
- Voices: Boxes whose snapshots blend the count (AdditiveBox, PulsarBox,
  WavetableBox, GrainBox, PolyComb) own it; elsewhere it is `MorphHint::Switch`
  and MacroMixer sets it once, half way. Output channels stay Structural.
- A different sample/file (SampleBox, DrumKitBox, SlicerBox, WavetableBox), pad
  count, or an old preset that needs a full recall's migration fails
  `morphCheck`, so MacroMixer replaces the node as before.
- Flat-saved Boxes read published controls through
  `scBoxMacroMorph::nativeValue` (Oceanode saves them as text, a one-value
  vector as a bare number).

`python3 tools/boxes/macro_morph_test.py [box ...]`: production MacroMixer and
Boxes, ASan/UBSan, no SC server. Per Box: same node, no replacement in Details,
Voices once, arrival, cancellation.

## Validation

- `python3 ../ofxOceanode/tests/macro_mixer_polycomb_regression.py`: production
  MacroMixer and PolyComb with real OF libraries, ASan/UBSan, no SC server.
  Checks count-only Morph details, node identity, activation before 0.0011,
  forward/reverse endpoints, cancellation, connection precedence, structural
  Channels, node-owned registered controls without duplicate writes, and the
  shared snapshot policy wrapper including interrupted fractional activation.
- `python3 tools/polycomb/voice_morph_test.py`: production MacroMixer hooks,
  snapshot policy wrapper and voice-bank manager with a simulated SC backend.
  Compares snapshot/MacroMixer weights, interruption/cancellation, retained node
  identities and return to baseline synth/bus counts after endpoint cleanup.
- `python3 ../ofxOceanode/tests/macro_mixer_morph_regression.py`: broader native
  graph morph regression.
- `python3 tools/polycomb/voice_morph_render_test.py`: actual offline SC mixer
  DSP at 1/2/16 channels, promotion/removal continuity and shared-effect tails.

The full graph regression currently fails its DotMaker `X_out` output-flag
assertion with both the modified and original MacroMixer source. A diagnostic
copy omitting only the two DotMaker output-flag assertions passes all remaining
checks. The repository regression is left intact; this unrelated failure is not
reported as a full-suite pass.

These are headless and offline checks; they do not audition a running MacroMixer
through an audio device. Rebuild the host application against both addons and
compile current PolyComb Parts/Extras as described in the consolidated reference.
