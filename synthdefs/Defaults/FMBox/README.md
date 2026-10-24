# fmBox — DX7 / .afx

fmBox now uses an adaptation of **Aziz Ege Gönül's DX7-Supercollider** engine.
It replaces the earlier SuperFM-derived implementation. The editable routing
matrix, dockable editor, effects, publishing, timeline and shared Box snapshot
and modulation workflows remain.

Add **fmBox**, enable **Show**, and play from Gate/Pitch/Velocity or the computer
keyboard. The node face remains Show, Voices, Channels, Gate, Pitch, Velocity,
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
16,251 names and source bank labels from the upstream PDF catalogue, plus 133
blank names shown as Unnamed. A bank dropdown limits the list to the selected
bank's presets. Rows show only preset names; hover for numbers and algorithms.
The list adapts to one to four columns, reading down each column in preset order.
Search and Previous/Next operate within the selected bank. Selecting a bank
does not change the sound until a preset is chosen. The selected bank is saved
with the Oceanode patch. Search is case insensitive.
Names are explicitly unverified: the author reported uncertainty about the
PDF's correspondence to the bank. Per-patch fingerprints prevent these labels
from being applied to unrelated or reordered patches. A missing catalogue
falls back to numbered presets. `tools/fmbox/extract_names.py` regenerates the
metadata from `pdftotext -layout` output of the upstream PDF.
The first 128 original sounds also seed the Factory snapshot bank. Any sound
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
