# fmBox — DX7 / .afx

fmBox now uses an adaptation of **Aziz Ege Gönül's DX7-Supercollider** engine.
It replaces the earlier SuperFM-derived implementation. The editable routing
matrix, dockable editor, effects, publishing, timeline and shared Box snapshot
and modulation workflows remain.

Add **fmBox**, enable **Show**, and play from Gate/Pitch or the computer
keyboard. The node face remains Show, Voices, Channels, Gate, Pitch,
Master and Out. Voice count and speaker count are independent (1–16).

## Presets

The bundled `data/Supercollider/Presets/FMBox/DX7.afx` contains the upstream
16,384 presets. The **DX7 Presets** tab browses the complete bank with a clipped
list, number selector, Previous/Next and filter. **Open .afx bank…** loads another
bank. The AFX Preset selector can be published or timelined by right-clicking.

This is the upstream decimal `.afx` format, **not arbitrary .afx audio files or
raw DX7 SysEx**: one preset per line, 145 fields, exactly two decimal digits per
field (290 digits). Empty lines and CRLF line endings are accepted. A bad line
or out-of-range field rejects the new bank and leaves the current bank intact.

`.afx` contains no patch names. The accompanying `DX7.names.tsv` supplies
bank labels and names recovered from original SysEx parameter matches across
all 512 banks. Of 16,384 voices, 16,381 match all 145 AFX fields after the
existing conversion's bit masking and range clamping. This includes 110 blank
source names, shown as Unnamed. Three voices without exact matches are labelled
Unverified (preset numbers 2273, 2305 and 2306). A bank dropdown limits the list to the selected
bank's presets. Rows show only preset names; hover for numbers and algorithms.
The list adapts to one to four columns, reading down each column in preset order.
Search and Previous/Next operate within the selected bank. Selecting a bank
does not change the sound until a preset is chosen. The selected bank is saved
with the Oceanode patch. Search is case insensitive.
The historical upstream PDF describes a different bank ordering and is no
longer used to generate labels. Per-patch fingerprints prevent the corrected
labels from being applied to unrelated or reordered patches. A missing
catalogue falls back to numbered presets. The AFX sound data and numeric preset
indexes are unchanged, preserving saved sounds and automation.

`tools/fmbox/catalogue_sources.json` records the reference archive URL/SHA-256,
each bank's source file and AFX digest, reordered slots and exact-match name
overrides. Source banks can contain invalid values or checksums; normalization
is explicit and every verified label is checked against all converted fields.
To regenerate or verify the catalogue, download the pinned
[DX7 All The Web archive](https://ccrma.stanford.edu/~braun/assets/DX7_AllTheWeb.zip)
and run `python3 tools/fmbox/extract_names.py ARCHIVE.zip` (add `--check` to verify
without writing). The archive is a mirror of
[Bobby Blues' collection](https://bobbyblues.recup.ch/yamaha_dx7/dx7_patches.html).

The 32 verified ROM1A sounds at browser numbers 12545–12576 seed the Factory
snapshot bank, starting with BRASS 1 and including E.PIANO 1. Existing snapshot
libraries retain their embedded sounds; choose **Factory collection** to load
the corrected bank. Any sound
from the full browser can be edited and stored in the shared snapshot library.
Snapshots and Oceanode patches embed all sound parameters and matrix/mix values;
they continue to play if the original `.afx` file moves or disappears. The source
bank path/index is separately remembered for browsing. Loading an AFX sound keeps
the current Box effects, width and auxiliary modulation routes. A running
snapshot glide stops when an AFX sound is selected.

Voices sets keyboard polyphony; Channels sets the number of audio outputs.
Width defaults to 0, centering notes on both stereo channels. At Width 1,
voices occupy separate positions across the outputs, with the first keyboard
voice at channel 1. Existing patches retain their saved Width value.

The **Panning** tab follows AnalogBox: Spread, Channels and Manual placement,
spread width and center, ring/line speaker layouts, a draggable voice display
and live Pan modulation markers. Shift-drag snaps a voice onto a speaker.
Right-click placement controls to publish or automate them; Pan connects to
the shared modulation routing. Panning settings are stored in snapshots and
Oceanode patches and are preserved when loading an AFX sound.

The replacement uses `nodeSnapshots/FMBoxDX7` so obsolete snapshots from the
previous engine are not silently interpreted as DX7 data. Their files remain
in `nodeSnapshots/FMBox`.

## Migrating SuperFM sounds

The **Matrix Model** selector offers DX7 routing units (the default) and
**Raw FM** units. Raw FM uses the coefficients directly and applies Feedback
to all six diagonal cells, preserving SuperFM's matrix semantics.
**Envelope Model → Seconds** uses explicit per-operator segment times and a
shared Envelope Curve; the migrated sounds use their original `-4` curve.
Levels retain the DX7 level scale, with zero representing silence in Seconds
mode, and velocity scales all operators linearly. Existing DX7 presets retain
the original envelope and matrix behavior.

`tools/migrate_superfm_to_fmbox.py` converts patch instances and their connected
controls, creates a shared snapshot bank, and installs the updated FMBox
SynthDefs. Run it without `--apply` to preview, or with `--stage DIRECTORY` to
inspect the proposed files. Applying creates a dated backup. The matching test
renders the original production SynthDefs on the left and FMBox on the right:

```sh
python3 tools/fmbox/bienal_migration_test.py STAGE --data SHARED_DATA
```

After applying, use the live data folder as `STAGE`, add `--originals BACKUP`
and `--report BACKUP/report.json` to compare against the backed-up nodes.

Use the first render's `measurements.json` as the migration's `--calibration`
argument to match the measured level, then render the staged result again.
That matches the tested saved input values, rather than promising identical
output across every possible live carrier mix, pitch or velocity. FMBox has
DC filtering, native gate alignment and carrier normalization; these can
produce residual differences. The migration retains all downstream effects.

## Editor

- **Operators:** ratio/fixed frequency, coarse/fine/detune, operator output,
  velocity and amplitude-modulation sensitivity, keyboard breakpoint/curves/
  depths and envelope rate scaling. Operator Output changes both its carrier
  level and the signal it sends as a modulator.
- **DX7 Global:** 32 algorithms, feedback, transpose (24 = unchanged), oscillator
  sync, native LFO speed/wave/delay/sync/depth/sensitivity and pitch envelope.
  FM Amount and Envelope Time extend the original engine; 1 preserves its values.
- **FM Matrix:** imports the algorithm's original connections and carrier mix.
  An FM7-style diagonal operator grid edits the routing directly: orthogonal
  wires, vertical-drag amount junctions, self-feedback below each operator,
  and a bottom row of output levels. Double-click a junction to type a value;
  zero disconnects it. Right-click uses the usual Box publish/modulation menu.
  A slider matrix to its right edits the same route and output parameters,
  keeping both views synchronized. Narrow windows scroll horizontally.
  Columns are sources and rows are destinations; 1 means the original `4pi`
  connection. The highlighted cell is that algorithm's feedback path, including
  the off-diagonal loops of algorithms 4 and 6. All 36 routes and six direct
  output mixes remain editable, publishable, modulatable and automatable.
  Changing Algorithm restores that algorithm's matrix and carrier mix.
- **Envelopes:** four levels and four time controls for each operator. The `.afx`
  time convention is **0 fast / 99 slow**; L4 is the starting/release level, and
  L3 sustains. The level plot is schematic: actual timing depends on the upstream
  nonlinear equations, key scaling and velocity. Drag levels or edit the controls.
- **LFOs / Routing:** additional shared LFOs, envelopes, external sources,
  velocity, random and voice index. Native per-voice bus destinations include
  pitch, FM amount, feedback, amp, pan and operator coarse/detune/output.
- **Effects:** saturation → five-band EQ → echo → reverb, at speaker count.
  Unused stages stop after their tails. The native DX7 LFO is separate from
  these effects and from the added shared modulation sources.
- **Snapshots:** the usual 128 slots, banks, Store/Recall/Glide and A/B morphing.
  Editor controls retain right-click exact values, publishing, modulation and
  timeline actions; incoming connections retain priority during recall.

The window docks through the application's normal ImGui dockspace. Operator
cards wrap; wide modulation/effects panels scroll. Keyboard notes use the same
input and scheduling path as other Box synths.

## Engine adaptation and limits

The operator envelope conversion equations, measured conversion tables,
algorithm connections, feedback scaling and output normalization come from the
upstream implementation. This adaptation keeps voices persistent instead of
using the original ten-second timeout/DetectSilence node deletion, and shares
one native LFO across the instrument. A DC blocker removes the source engine's
DC offsets. Audio gates are delayed two control blocks to align native envelopes
with ModGate's velocity latch.

Fixed-frequency mode fixes an upstream duplicate assignment which overwrote its
coarse/fine values with ratio-mode values. Its fine frequency follows the
underlying logarithmic equation. Oscillator phase randomization occurs at note
on when Osc Sync is off. Native LFO wavetables are generated at compile time,
with no external buffer files. These changes mean this is an adaptation of that
clone; no claim of bit-exact hardware emulation is made.

## Build and tests

`fmBox.scd` plus `dx7Data.inc` compile the `FMBoxVoice` and `FMBoxDX7LFO`
definitions into `CompiledSynthdefs/FMBox/`. Both compiled definitions are also
bundled in addon data. The usual Modulation, PolyComb spreader and MixerPro
SynthDefs and FM7 SC3plugin are required.

Run from the addon root:

```sh
python3 tools/fmbox/render_test.py
python3 tools/fmbox/reference_test.py
```

The first test compiles the engine, executes production C++ preset conversion,
validates every bundled preset under sanitizers, renders all 32 algorithms plus
factory samples, and exercises 1/4/16 voices, modulation, release below -100 dB
and the effects chain/tails. The second extracts the original conversion
functions and voice graph and compares 24 audible preset/note/velocity cases
with both native LFOs neutralized, deterministic oscillator phases and identical
DC removal. RMS must agree within 5%; this is a regression check, not a complete
perceptual or hardware-fidelity test. Outputs go to `/tmp/fmbox-dx7-render` and
`/tmp/fmbox-reference`.

`generate_dx7_data.py` reproducibly extracts tables from the preserved original
source. `state_test.py` checks bank-switch/snapshot persistence and glide stopping.

## Credits

Based on [DX7-Supercollider](https://github.com/everythingwillbetakenaway/DX7-Supercollider)
by Aziz Ege Gönül, pinned to commit
`1128e24c5f68a3bc2586927d2b8bbdeb311852ca`. The source also credits Hexter and
Jamie Bullock/Sean Bolton's TX7 measurements for conversion tables.
Original source and **GPL-3.0** license are preserved under `upstream/`;
`upstream/provenance.json` records hashes. The `.txt` suffix prevents SynthDef
sync from executing the original standalone application as another Defaults
script. The modified engine and generated tables retain GPL-3.0 notices.

## DSP and control-update performance

The voice graph shares gate/phase triggers, native-LFO smoothing, pitch and
velocity transforms, model weights and FM scaling across its operators. Native
and seconds envelopes share their amplitude conversions and cyclic level-map
selectors. Both envelope/model branches remain live so morphs retain their
original state and timing; muted drones are not paused.

The C++ sender resolves snapshot metadata once per update, reuses packed-control
storage across voices and compares the packed morph/tuning vector directly.
Native-LFO parameters are sent only to the shared native-LFO node. Effect-only
edits coalesce into a stage update without traversing the voice controls. Full
replays and synth recreation invalidate the packed cache explicitly.
