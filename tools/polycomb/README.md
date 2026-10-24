# PolyComb development and regression tools

For current snapshot/MacroMixer development, start with the
[consolidated morph reference](../../src/docs/polyCombMorphing.md).

```sh
python3 tools/polycomb/morph_regression.py                 # quick checks
python3 tools/polycomb/morph_regression.py --group audio   # compile + offline DSP
python3 tools/polycomb/morph_regression.py --group native  # fresh OF/MacroMixer build
python3 tools/polycomb/morph_regression.py --group all
```

Every run retains logs and a JSON report. Missing prerequisites and failures are
reported explicitly. Stable input pairs live in `fixtures/morph_reference.json`;
they are regression states, not an installed demo bank. Collection import and
earlier migration notes below are separate from the current morph contract.

## Pitch editor update

The pitch tab has oscillator and resonator map panels, each with three columns:
scale/sequence, index geometry, and tuning/vibrato. The tuning column groups
Transpose, Fine Tune, Note Snap, Detune and vibrato rate/randomness/amplitude;
the oscillator column also includes Glide. Sound tuning stays editable with
incoming pitches or an inherited resonator map. The generated-note piano roll
remains beneath each map, with MIDI/note labels and the actual range. Windows
narrower than 1320 scaled pixels stack the two map panels.

The Oscillators and Resonator tabs omit the duplicate pitch displays and
Transpose widgets. Existing saved transpose parameters remain compatible.
The resonator has independent per-voice vibrato, defaulting to zero amplitude
for older patches and snapshots. Recompile/reload the `PCCombR1..32` SynthDefs
to use it. The post-filter offers the same independent model, type, cutoff,
resonance and key tracking as the pre-filter. Older sounds inherit the previous
shared filter model and retain zero post-filter key tracking.

The shared Classic LFO morph waveform defines geometry with Skew, Roundness and
PW; shape menus and direction/pattern buttons have been removed. Skew -1 rises,
0 is symmetric, +1 falls; Roundness morphs straight edges through sine to pulse.
Num Waves samples periodic geometry across exactly the active voice count,
without including the next cycle endpoint (12 voices / 3 waves repeats every
4 voices). Cycle Shift adds a whole-index offset on each successive waveform
cycle before the remaining geometry and pitch mapping. It wraps in the active
voice range: 0 keeps exact repeats, +1 moves each repeat up one index, -1 moves
it down. It is publishable on both maps; existing snapshots default to 0.
New seed on gate is an independent, publishable option on each map. A rising
Oceanode/timeline Gate voice chooses a fresh Scatter seed once for that event,
shared across servers; held gates and releases keep it. Existing snapshots
default this option off. Audio-rate Gate cables do not regenerate the C++ map.
The index amplitude also uses the active count, so a one-wave ramp
advances one index per voice. Num Waves repeats geometry; Symmetry folds it; Comb, Group and Scatter alter it.
Pow acts last: normalize the actual final range, apply the logarithmic (+) or
exponential (-) curve, restore the range, then round to integer indices. Pow 0
is neutral. Mirrored shapes keep their symmetry. Only the final piano roll is
shown; mapped voices expand the first incoming pitch as a shared root.
Map Transpose snaps to whole semitones; Fine Tune (formerly Final Shift) is a
continuous -1..+1 semitone adjustment after mapping. Num Waves and Octave Jump
snap to 0.25, and Step to whole degrees, at parameter level for the editor,
published connections and snapshot recall. Older Offset values migrate into
coarse Transpose plus Fine Tune. Harmonic maps support these pitch adjustments too.
With Resonators: own map off, its grey source controls and preview follow the
oscillator map, while the resonator Transpose/Fine Tune stay active. The DSP
follows the oscillator note bus with those offsets, preserving live pitch/glide;
ringmod following Resonators map also follows this effective map.
The index domain is always 0..(active voices - 1), including Scatter. Start,
Symmetry and Group ranges follow the active voice count. The scale library selector is a
registered dropdown: right-click it to Publish to Node GUI. Published scale and
chord selections update the editable notes; manual edits synchronize the choice.
Octave jump independently sets the octave distance between repeats of the scale
in 0.25 steps. Step selects whole degrees; Num Waves moves in 0.25 steps.
Selecting User scale starts with Step 1 and Octave jump 1.
Groups start at index zero, so grouping cannot push the first note below the scale.
Snapshot recall retains saved spacing; missing Octave Jump and Num Waves default to 1.
Ringmod selects External pitch, Oscillators, or Resonators map, then applies
its own Transpose and Ratio. Older snapshots follow Oscillators. Its compact
pitch-vector view shows source bars and nominal carrier dots; hover a voice for
source/carrier note names, MIDI pitches, ratio and frequency. Oscillator sources
include the main transpose. The view adds no audio polling or OSC traffic.

Legacy Shape/Bend fields migrate to Skew/Roundness/PW/Pow on recall, without
rewriting saved files. Earlier curved/mirrored maps can sound different. The migration measurements below were
recorded for that earlier revision, not a guarantee for this integer geometry.

## Earlier model and pitch-map migration

PolyComb now uses only Reaktor's comb and ring modulator. The old model keys are
retained for file compatibility and fixed to 1; the selectors are removed.

User replaces Table: N distinct integer pitch classes (0–11), looked up by
scale degree. Every N degrees adds an octave, so a three-note scale fills
32 voices without adding sliders. Right-click a slider for exact entry; a
piano reference and final-map MIDI/note tooltips are included. User and
Harmonic series share the voice-index pipeline and its shaping controls.
See `tools/polycomb/pitch-map-macro.md` for the extracted reference.

PolyComb's LFO dropdown is Off / Classic / Noise / Random. Off is an exact
manual 0–1 output. Random holds one value per cycle, with Smooth in seconds.
This leaves other nodes' modulation interfaces unchanged. Legacy Smooth and
Jumps are approximations of Random; Jumps' old wrapped hybrid is retired.

All bundled banks are migrated. `model-migration-audit.json` records the
32-voice pitch differences for the 64 old Table maps: average absolute error
about 0.68 semitones, worst individual deviation 12 semitones. Ring modulation
that formerly used a fixed note now tracks the voice via a ratio matched at
MIDI 60. Exact equivalence under note changes cannot be preserved.
Previously saved render reports describe the earlier bank revision.

## Reaktor collections — static approximations

PolyComb uses the Reaktor PolyComb 1.7 comb for every snapshot, including legacy snapshots.
The classic comb and its model selector have been retired. Saturation level, pitch,
feedback, tone and mix retain their saved values. The 2018 Comb2 remains an
approximation using this Reaktor comb.

296 recovered snapshots are available as 3 banks:

| Source | Banks | Snapshots |
| --- | ---: | ---: |
| PolyComb 1.7 | 2 | 210 |
| PolyComb 9 / 32 OSC, **2018** bank | 1 | 86 |

The POLYCOMB 2025 / 8ch collection is no longer translated or installed (its
Harmony instrument and resonator are not ported); the importer's `--profile 2025`
remains for reference.

These are **candidate translations, not audio-identical recreations**. The user
chose static pitch approximations instead of adding the later pitch-map and
harmony engines. Some source presets therefore become the same static sound;
`equivalent_translation_groups` in each translation audit lists these groups.
Sample-based sounds and other unsupported layers remain incomplete.

## Access

After rebuilding Oceanode, open PolyComb's top **Banks → Bundled collections**
menu. Entries are labelled **(approx.)**. Selecting a collection first saves the
current slots, including empty slots and the active selection, as a bank named
`Before collection - <timestamp>` in the existing bank directory. If the backup
fails, the active library is not replaced. The backup is available through the
normal Load bank menu. Startup does not install a collection or replace slots.

The node finds these read-only JSON files in the addon checkout or in deployed
`Supercollider/SynthdefSources/Defaults/PolyComb/ReaktorSnapshots` assets. The
existing `tools/deploy_synthdef_compiler.sh` copies those source assets for a
standalone deployment. A packaged copy can also live under
`Supercollider/Synthdefs/Defaults/polyComb/ReaktorSnapshots`.

Banks use the existing 128-slot format. Translated collections leave Voices,
Channels, Pitch, BPM and Master under the node's existing settings; native
snapshots can now retain voice counts. Translation assumes **16 voices, MIDI 60 and 120 BPM**;
the 32-voice source is sampled into the native 16 voices. Other voice counts use
the corresponding prefix of each prepared vector. Choose 8 output channels to
audition the 8ch material spatially. Start with a low Master: the measured
candidate banks contain peaks above full scale. No limiter or hidden per-preset
gain normalization has been inserted.

## Approved synth additions

- The original four filter Type indices and their compiled DSP remain intact.
  Appended choices add LP/HP 12/24 dB, 2/4-pole band-pass, asymmetric BL4
  (6 dB HP / 18 dB LP), and 4-pole notch in both filter stages. There is no
  Classic mode. BL4's resonance loop and the later source's 6 dB selections
  remain approximations.
- **Saturate feedback** adds a comb-loop saturation toggle and Sat Level. It
  reuses the existing AnalogFilter UGen's cubic, per-sample feedback comb,
  including positive/negative feedback. Its damping, DC removal, normalization
  and saturation differ from NI's original cell. No block-delay loop was added.
- **Compressor** reuses MixerPro's existing `mixerProComp` SynthDefs. Its Effects
  panel has the transfer curve, live gain-reduction meter, threshold, ratio,
  knee, makeup, auto makeup, attack/release, peak/RMS, detector HPF and mix.
  Controls support snapshots, publishing and timeline automation. Source ratio,
  exponential times and gain are translated; NI's headroom saturation and
  detector/ballistics/stereo-link behavior are not identical.

- **Independent Sub** adds a sine layer in the Oscillators tab, with input-relative
  tuning, level, its own editable ADSR/curves and live envelope display. It sums
  after the main amp and before spatialization/shared FX. Publishing, snapshots
  and automation use the normal node controls. NI uses an audio follower instead;
  this port uses gate ADSR, initialized from the main envelope and source follower
  release. Source 0..16 gain is normalized to 0..1 in the older banks. The later
  signed amplitude and pitch modulation are frozen at their midpoint (amplitude
  magnitude); tremolo, follower input dynamics and sine shaping still differ.

All four additions default off or preserve the original selection when an old
snapshot is recalled. The production recall/morph regression restores the real
MOVING_SAWS values, matrix, source counts and routes after a saturated,
compressed sound with the sub enabled. The original migration audit recorded
**272 compiled PolyComb definitions** byte-identical to its saved baseline.
Subsequent pitch smoothing and morph DSP changes intentionally update some
definitions; that historical hash audit is separate from current source checks.
The live MOVING_SAWS fixture is untouched.
Its SHA-256 is `0650ef23fe5868c2105f059a6b586222e95dc2418da87d6a5cc7176424b25652`.
This is state and DSP-identity verification, not a full live-app waveform null.

`polyCombOptions.scd` supplies the optional filter/comb definitions. Current
pitch, sub, resonator and morph processing also uses `polyCombParts.scd` and
`polyCombExtras.scd`; the audio regression group compiles both before rendering.
Use the matching OceanodeAnalog class and plugin. These source builds update
the addon's ignored cache, not the installed application.

## Fidelity and measurement data

Each version directory contains its bank manifest, native control schema,
source controls, recovered raw records, and translation audit. Recovery retains
**240,681 state records** in total, original names, table payloads where present,
saved global controls and source SHA-256 values. Snapshot IDs are scoped to
instruments; the later importer explicitly prioritizes the main PolyComb
instrument for overlapping IDs. Unsupported and unresolved state is preserved
for further investigation.

Pitch vectors freeze simplified interval/index grids. Dynamic chord/probability
chains, exact NI quantizers, transfer curves, gated/compound LFOs, randomization,
exact sub-follower dynamics, sample layers, cabinet/tube/limiter processing and source EQ/FX
algorithms are not fully recreated. The shared sources and routes approximate
only the supported controls. `clipped` records source values outside native
ranges; a valid native range does not establish sonic equivalence.

The sine/pulse morph-position error in the initial translator is corrected and
verified against rendered native LFO output. Clipped LFO endpoints and static
pitch/filter offsets are fitted explicitly. Asymmetric interior clipping and
power curves still differ.

`render-summary.json` summarizes all three collections. Per-version
`render-measurements.json` and `render-conditions.json` contain the offline
checks per collection. The test uses 16 voices, MIDI 60, full gates, Master 0.8
and 12 seconds per snapshot, measuring the full note/release window as well as
sustained RMS. It is a finite-audio/level check, **not a Reaktor A/B comparison**.
The sub layer is enabled in 98 translated snapshots. Ten presets with a muted
main layer now have an active sub. Seventeen later presets still have neither
main level nor a usable translated sub level; these retain an incomplete warning.
Source modulation/additive sends and unsupported layers may be necessary for
those sounds. Do not invent a base level for saved inactive layers.
Only snapshots with the sub enabled were re-rendered after this addition;
unchanged sub-off snapshots retain their previous measurements. Each version
records that verification scope in its render conditions.
Long pulses can also be silent during that window: lullaby produced audio in a
separate 60-second check. Level peaks and incomplete source layers remain
reasons to treat these collections as drafts.

## Install in OceanAudio

`python3 tools/polycomb/install_banks.py /path/to/OceanAudio/bin/data` installs
all 10 banks into both the regular Load bank menu and Bundled collections. It
also installs the 48 optional PolyComb SynthDefs and MixerPro compressor defs.
The installer verifies every copied file, leaves active snapshot files untouched,
saves a restore bank before the first change, and backs up replaced assets. Use
`--dry-run` to inspect pending changes. Rebuild OceanAudio for the new controls.

## Reproduce

From the addon root:

```sh
python3 tools/polycomb/import_ensemble.py /path/to/PolyComb_1.7.ens
python3 tools/polycomb/import_ensemble.py /path/to/PolyComb_9_32OSC.ens.ens --profile 2018 --bank 2018 --output synthdefs/Defaults/PolyComb/ReaktorSnapshots/2018
python3 tools/polycomb/validate_banks.py --collection all
python3 tools/polycomb/validate_banks.py --collection 2018 --render
python3 tools/polycomb/translation_test.py
python3 tools/polycomb/options_render_test.py
python3 tools/polycomb/compressor_render_test.py
python3 tools/polycomb/sub_render_test.py
python3 tools/polycomb/moving_saws_test.py
python3 tools/polycomb/bank_loading_test.py
python3 tools/polycomb/voice_rebuild_test.py
python3 tools/polycomb/resonator_vibrato_render_test.py
python3 tools/polycomb/post_filter_test.py
```

Import writes only the recovery directories. Temporary rendering files go under
the system temporary directory and no audio device is opened. Tests that compile
production sources also update the addon's ignored `CompiledSynthdefs` cache.
Tests never edit the installed app's preset library.
