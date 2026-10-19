# AliasingBox

`aliasingBox` is a dedicated Defaults instrument based on Dietrich Pank's
Aliasing Synth and the supplied `aliasing_synth_midi.ens`. It starts with the
first original snapshot, **quak**, five voices and stereo output. Send Gate,
Pitch (MIDI notes) and Velocity. The header has Voices, Channels, Master and
Auto/Gated/Drone controls in the same layout as PolyComb. Auto drones until
Gate is connected, timelined or receives a note; Gated follows Gate and Drone
always plays. Older patches retain their saved Drone/Gated behavior.

The old `Source/Special/aliasingsynth.scd` remains available. This node has its
own DSP and does not change old patches.

## Build and use

Evaluate `aliasingBox.scd` in SuperCollider. It writes `AliasingBoxVoice`
into `CompiledSynthdefs/AliasingBox`. The node also
uses the existing `Defaults/Modulation/modulation.scd` and
`Defaults/PolyComb/polyCombParts.scd` and the shared effects in
`Defaults/MixerPro/mixerpro.scd`; compile these if their compiled folders
are missing. Rebuild the Oceanode app to include the new C++ node. Normal OF
addon builds discover the new `.cpp` automatically; manually maintained IDE
source lists need `src/nodes/scAliasingBox.cpp` added.

The node exposes Show, Voices (1–16), Channels (1–16), Gate, Pitch, Velocity,
Master and Reset (LFOs), like PolyComb and AnalogBox. Gate Mode
(Auto/Gated/Drone) is in the window header and can be published from there;
patches saved with the first version's Drone toggle load it as Gate Mode.

Its window has a live table row for every voice, stepped traces or a green/blue intensity
view, separate read/write cursors, and voice frequency readouts, oscillators, filters, original envelopes, dynamic
Envelopes/LFOs/Routing, an Effects tab, and the shared snapshot library in the top toolbar. Extra sources start
empty; add up to eight LFOs, four envelopes and eight external sources.
Routing an added envelope to Amp enables a VCA; the original amp envelope
continues to gate the voice as well.

The range bar under the table is a sound control: it scales both read/write
addresses and the region displayed, as in Reaktor. Drag it to change the range;
double-click restores the full table. It can be stored, morphed, published,
automated and modulated. The initial display is five rows, following the
provided Reaktor views. Display mode is saved with the patch.

Right-click a window control to enter a value, publish/unpublish it, add it
to a timeline track or modulate it. The node's title-bar menu exposes the
standard timeline instrument action when timeline support is compiled in.
Gate/Pitch/Velocity accept vectors or audio cables and register scheduled
note handlers. Snapshot recall leaves externally driven controls alone.
The shared snapshot system supplies 128 slots, named presets, factory banks,
store/recall, timed glide and A/B morphing, with continuous matrix blending.

The built-in amp and filter ADSRs use the same compact, four-column card grid
and draggable curve editor as added envelopes, with per-voice level meters.
Times are displayed in seconds while preserving the original imported units
internally, including 0.1 ms attacks. Tabs: Sound (write and read oscillators, output, filter chain, HP/LP, notch and
the original filter LFO, in five columns), Envelopes, LFOs, Routing and Effects,
the same modulation tabs as the other boxes. Header, snapshot bar and table
stay in place while a tab scrolls. Oscillator rows use the other boxes' inline
labels and fixed gutters.
HP/LP/notch cards show logarithmic response plots and frequency readouts;
drag their plots to edit cutoff and resonance (gain for the notch). They show
the base linear response; FM, envelopes, LFOs and saturation are not represented
by a static curve.

The Effects tab reuses PolyComb's five-band EQ editor, tempo-synced echo and
reverb panels, plus AnalogBox's saturation controls, backed by the exact shared
MixerPro SynthDefs at the selected speaker count. Chain: spread -> saturation
-> EQ -> echo -> reverb. Stages start disabled. Echo/reverb keep their tails
when disabled, then pause. All effect and native envelope curve parameters
are stored/morphed in snapshots and available through the usual right-click
publish/timeline menus. Preset selection and manager access live in the top
bar; there is no duplicate Snapshots tab. A separator divides the snapshot
bar from the table display. Reset LFOs resets the added modulation LFOs.

## Synths

Like PolyComb: a root group with one subgroup per stage (gates, modulation
sources, routes, voices, spreader, then one per effect). The gates, sources,
routes, spreader and effects are the shared `ModGate`/`ModLfo`/`ModEnv`/
`ModRoute`, `PolyCombSpread` and MixerPro synths; added LFOs/envelopes/routes
and effects run only while in use. The voice is one `AliasingBoxVoice` per
voice, on purpose: split into parts, each filter would start from another
synth's bus value (a different start-up transient), and Notch / HP-LP must
keep running while off so that a snapshot turning them on finds them warm,
as in the original — there is no CPU to gain and the sound would change.
All 38 factory snapshots null sample for sample against the previous voice
synth. The voice no longer computes an unused 64-tap scope, and it reads 16
of its 256 table display cells per control block (the whole table every 16
blocks) instead of all of them.

## Recovered structure

Each voice has an independent, continuously rewritten 1024-sample table.
A read oscillator supplies both the value being written and the read address;
a separate write oscillator supplies its write address. Oscillator pitch is
converted from semitones to Hz, rather than used directly as an increment
through 1024 samples as in the previous port. Raw ramps retain the intentional
aliasing. The write selector chooses a ramp, a bipolar variable-symmetry
triangle or the read oscillator; the read selector chooses a directional
triangle/ramp or a raw ramp. Original oscillator LFO depth is raised to the
fifth power, with the recovered per-voice rate variation (a single voice is
at index 0.5). Fine Tune is in semitones. The port models Primary Tri/Par
from its rising zero crossing; raw ramps start at zero. The relative phase
is important to the table's DC offset. Stereo mixing is normalized by voice
count, as in the recovered graph, instead of arbitrary per-voice attenuation.

The table output goes through the notch, the resonant four-pole HP filter,
saturation, the four-pole LP filter, optional DC removal and saturation.
Both filter FM inputs use the signal before the HP/LP chain. The port uses
`BHiPass4`, `BLowPass4` and `tanh`, following `Effect/Filter/satfmfilter.scd`.
Original amp/filter ADSRs remain available alongside the added modulators.

This is a structural port, **not a verified audio-identical emulation**.
NI's proprietary Primary filter/saturator implementations are approximated;
the Core filter LFO now uses its recovered eight-output selector, tempo
numerator/denominator, unipolar power shaping, gate reset and the decoded
triangle/sine arithmetic. Its random generator and smoothed/cubic-random
outputs remain approximations to NI's algorithms. The supplied table-properties image confirms no interpolation, Clip XY,
normalized X, indexed Y and Backup With Structure initialization. The SC
table uses nearest-cell reads and clipped addresses, but clears memory on
voice creation instead of loading Reaktor's backed-up table. Exact oscillator
phase/reset timing and the filtered tone still need further A/B work. Original snapshot
values are retained even where the corresponding DSP is an approximation.

## Snapshot recovery

`factory.json` contains all 38 names and the recovered sound values.
`controls.json` records control ranges, module IDs and stable snapshot IDs.
`reaktor-snapshots.json` preserves every original state record, including
isolated/null controls, and the source SHA-256. MIDI envelopes added after
the original bank, and snapshot-isolated controls, use the saved panel state.
Activation/reset/display-only states are retained in the raw audit file;
the node supplies its own Gate/Drone, per-voice tables and displays.

Older v3/v4 builds of the generic extractor associate each legacy
snapshot state with the ID following it. Here the ID actually precedes the
state. Its fallback offset then produces plausible but wrong references to
meters and arithmetic modules. This importer bypasses that inferred mapping:
it reads the stored panel reference (10000 + stable ID), handles the extra
link word in some controls, and decodes the preceding state ID directly.
Switch indices start at 1; inverted control ranges are preserved. The extractor has now been upgraded to v5: it resolves the stacked macro,
all 533 Primary wires, all 38 x 47 snapshot identities, linked control
properties and numeric constants. The importer remains compatible with old
extractor reports and decodes the original bytes independently.

Regenerate all data, including the embedded C++ factory bank, with:

```sh
python3 tools/aliasingbox/import_ensemble.py /path/to/aliasing_synth_midi.ens
```

## Verification

After compiling the SynthDefs:

```sh
python3 tools/aliasingbox/render_test.py
```

This renders all factory sounds offline without opening an audio device,
checks for finite/unclipped output, then exercises shared LFOs, envelopes,
routes and stereo spreading at 1, 4 and 16 voices, checking that closed gates
release to silence. Outputs and measurements go to `/tmp/aliasingbox-render`.
`python3 tools/aliasingbox/table_view_test.py` exercises both real ImGui
table renderers at 1, 5 and 16 voices, including finite geometry and 16-bit
vertex-index limits, filter-response geometry and sub-millisecond envelope times.
`python3 tools/aliasingbox/effects_test.py` checks shared effects in mono,
stereo and 16 channels: transparent bypass, audible processing, tails after
gate-off and direct routing after stopping the stages. `compare_reference.py --dry /path/to/dry.aif --wet
/path/to/wet.aif --note 60 --voices 5` renders matched test cases and records
carrier, DC, RMS, peak and an audible-change check for Table Range. Recordings
are analyzed at their native 44.1 kHz; free-running phase means a sample null
is not a valid comparison between separate takes.

C++ node and registration headers were syntax-checked against the local OF
project with timeline enabled. Live editor interactions and an A/B audio
comparison in Reaktor still need an application run.
