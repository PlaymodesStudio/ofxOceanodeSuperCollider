# PolyComb pitch-map morphing

See the [consolidated reference](polyCombMorphing.md) for the shared contract
and regression runner.

Snapshots and MacroMixer evaluate each endpoint's complete oscillator and
resonator map, then interpolate the resulting MIDI pitches per voice. Semitones,
user scales, scale snapping and harmonic series can therefore morph without a
mode switch changing the generated notes at 0.5. Scale contents, quantization,
index geometry and scrambling stay inside their respective endpoints. The editor
dropdown still shows one endpoint's mode; generated notes show the audible blend.

The played Pitch root stays live. Connected map controls override the respective
field in every endpoint; manual map edits leave the stored blend and use the
editor's current settings. Pitch External retains its existing source-selection
behavior; this change concerns the internal maps.
Normal playback and endpoint evaluation both use `pcmap::playedPitches` for
root transposition, voice permutation and final pitch clipping.

For inherited versus independent resonator maps, let `O` be the blended
oscillator map, `R` the blended resonator pitches, and `w` the independent-map
weight. The resonator receives `combmode = w` and `combpitch = R - (1-w)*O`.
Its existing live oscillator bus supplies the remaining `(1-w)` contribution.
This preserves inherited oscillator transpose/detune and the resonator's own
glide, transpose, detune, modulation and vibrato. Its existing audio-rate minimum
pitch lag smooths control updates. PCCombR's mode control now accepts fractions.

Optional `_pitchMapBlend` snapshot metadata retains weighted endpoint
descriptions when storing, recalling, reversing or interrupting a morph. It
stores maps, not frozen absolute notes. Temporary voice-count banks receive this
metadata and evaluate the maps at their own widths. A normal endpoint releases
the override; map changes alone allocate no synths or audio buses.

Rebuild the application and reload/recompile
`synthdefs/Defaults/PolyComb/polyCombExtras.scd` for fractional resonator follow.

## Checks

- `python3 tools/polycomb/pitch_map_morph_test.py`: ASan/UBSan; every map-mode
  pair at all 1–32 voice counts, tiny progress, scale/geometry changes, live roots,
  overrides and stored/interrupted blends.
- `python3 tools/polycomb/pitch_map_morph_integration_test.py`: real PolyComb and
  MacroMixer code with ASan/UBSan and captured OSC; exact pitch targets before
  0.0011 and across 0.5, inherited resonators, live roots, interruption/cancellation,
  incoming map controls, manual edits and a 12-voice destination bank. Map-only
  changes retain the same pitch and resonator synths. No audio device is used.
- `python3 tools/polycomb/pitch_map_morph_render_test.py`: actual SC resonator
  pitch expression at 1/4/12/32 voices, fractional follow and smoothing.
- Existing `pitch_smoothing_render_test.py`, `resonator_vibrato_render_test.py`,
  `note_traffic_test.py`, `voice_rebuild_test.py` and `voice_morph_test.py` cover
  pitch lag, resonator audio/vibrato, note traffic and voice-bank lifecycle.
