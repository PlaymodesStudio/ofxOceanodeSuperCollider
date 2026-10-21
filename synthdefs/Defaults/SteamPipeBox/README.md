# SteamPipeBox

`steampipeBox` is a dedicated Defaults instrument reconstructed from the supplied
`steampipe_santi.ens` with **Reaktor Patch Extractor**. It starts with **THRILLER**,
six voices, stereo output and Master 1. Gate, Pitch and Velocity accept vectors,
audio cables and scheduled timeline notes. Auto/Gated/Drone follows the other boxes.

The window supplies the shared snapshot bar and manager, right-click value entry,
publishing, timelines and modulation, a draggable Steam ADSR, dynamic envelopes,
LFOs, external modulation inputs, a routing matrix and the optional MixerPro
saturation/EQ/echo/reverb inserts. Added modulation sources and insert effects
start disabled. Original tempo delay and R66 controls are in **Original FX**;
they retain their imported settings independently of the extra box inserts.
BPM follows Oceanode transport through the usual `setBpm` path. Snapshot recall
preserves connected controls; A/B morphing blends continuous values and routes,
while polarity and switches change discretely.

## Installation

The universal, signed **OceanodeSteamPipe.scx** is committed with its source.
Use the usual SynthDefs Git sync to install `ugens/OceanodeSteamPipe` into the
app's `Supercollider/Extensions/OceanodeSteamPipe` and restart the server.
For standalone SuperCollider, install that folder's `.sc` and `.scx` in its
Extensions folder. The class also ships with the bundled headless compiler.

Evaluate `steampipeBox.scd` after loading the class. It writes `SteamPipeBoxVoice`,
`SteamPipeBoxGate1..16`, `SteamPipeBoxSpread1..16` and `SteamPipeBoxOriginal1..16` into
`CompiledSynthdefs/SteamPipeBox`. Compile the shared Defaults `Modulation`,
`MixerPro/mixerpro.scd` if needed. Rebuild Oceanode
for the registered C++ node; manually maintained source lists must include
`src/nodes/scSteamPipeBox.cpp`.

## DSP and fidelity

Each voice contains its own noise generator, steam envelope, tuned fractional
delay, optional allpass delay, asymmetric saturation, MW HP/LP filters and
feedback damping. Its output continues to ring after the steam envelope closes.
The original gain/saturator is after voice mixing, followed by tempo delay and
R66. The original effects run independently per speaker pair for multichannel
output. The separate MixerPro inserts follow them.

Both acoustic feedback loops and the original effects execute **per sample**
in native C++ UGens. There is no LocalIn/LocalOut or audio-bus feedback inside
a physical loop, and no DynGen/JIT dependency. Identical direct-kernel renders
at block sizes 1, 64 and 256 are required by the render test.

This is a structural reconstruction, **not a verified audio-identical clone**.
The original graph, parameter ranges and snapshot values are retained, including
reversed ranges, switch input selection, two-frame button states, both axes of
XY controls, snapshot isolation and R66's independent channel bypass switches.
The delay LFO feeds pitch before P2D, as wired in the recovered Primary graph.
The delay step time is `240 / BPM / 16 * steps` seconds. R66 preserves the
recovered early/late delay pitches, diffuser coefficients and cross-coupling.

The default **Original ADSR** uses Reaktor's linear attack, exponential decay and
release, velocity peak, continuation on retrigger, and pitch-dependent time
scaling. Decay/release controls are time constants; the graph shows five time
constants. Switch Original ADSR off to use the added finite curve editor.
The shared modulation envelopes remain independently editable.

Primary HP/LP uses the original float32 Euler recurrence, 0.99 coefficient cap,
and HP output taken before integrating LP. Multi 2-Pole uses the original 11x11
cutoff/resonance coefficient grid and state-variable recurrence. Pitch/time
conversion follows the interpolated exponential table. Delay interpolation is
linear; Diffuser Delay retains its additional sample of state delay. Feedback
keytracking is `(pitch - 60) * amount`: the stored -60 Constant was renamed
"0" in the ensemble, so the numeric field, rather than its display label, must
be decoded. The feedback time conversion has no 0.1 Hz floor. R66 damping
uses its stored 112-pitch shelf constant, whose label says 96. Voice
outputs are summed through a linear distributor before the original gain and
saturator, with no voice-count normalization.

Gate, velocity, pitch and native parameters use an unsmoothed control-to-audio
converter. A rising gate therefore latches the requested velocity immediately,
instead of a small fraction at the start of a K2A ramp. Positive changes of the
manual gate level deliver a new gate event, so dragging the slider upward does
not retain the tiny velocity from the first mouse movement. Audio-cable gate
levels remain latched at onset. Preset updates to the original effects are sent
as one control message after startup too.

`primary_test.py` checks all 60,000 ADSR samples at pitches 36/60/84 against hashes
captured from the installed Reaktor 6.5 engine, plus 20 filter coefficient pairs:
all match exactly. This verifies those primitives and note sequences, not the
entire SteamPipe output. Full ensemble A/B recordings remain unverified. R66
shelves/random modulation and the delay Core filter remain approximations;
panning uses a deterministic per-voice spread rather than Reaktor's randomizer
state. Delay buffers start empty. The old `Source/PhysicalModelling/steamPipe`
and lowercase compatibility SynthDef are retired; `steampipeBox` is the sole
SteamPipe instrument.

## Snapshots and regeneration

All **172** snapshot/container names are included, in original order:

- `Reaktor SteamPipe 001-128.json`: 128 snapshots.
- `Reaktor SteamPipe 129-172.json`: 44 snapshots.

Both banks are bundled under `presets/nodeSnapshots/SteamPipeBox/banks` and seeded
on first use, without overwriting an existing bank. The first is the default.
The factory collection uses the manager's usual 128 working slots; load the
second bank to reach the remaining 44. `factory.json` retains original inner
names and source bank/slot metadata. `controls.json` records module paths and
ranges; `graph-constants.json` audits every stored DSP constant;
`reaktor-snapshots.json` preserves every raw state, unresolved records
and the ensemble SHA-256.

```sh
python3 tools/steampipebox/import_ensemble.py /path/to/steampipe_santi.ens
bash ugens/OceanodeSteamPipe/build.sh /path/to/SuperCollider/source
python3 tools/steampipebox/primary_test.py
python3 tools/steampipebox/retrigger_test.py
python3 tools/steampipebox/resonance_test.py
python3 tools/steampipebox/render_test.py
```

The build translates the two audited `.eel2` kernels to C++ and regenerates the
SC classes. The translator supports only the expressions used here; unsupported
syntax fails the build. Runtime units reject an incompatible old SynthDef.
The test renders every factory snapshot, checks finite audible output, checks
block-size equivalence and exercises shared modulation/spreading with 1/6/16
voices and 1/2/16 speakers. A cable-gate regression begins a note 48 samples
into a block at full velocity sensitivity. Results go to `/tmp/steampipebox-render`.
The retrigger regression retains the same voices through preset recalls, checks
216 notes and six manual slider drags at block sizes 1/64/256, verifies exact
onset gate/velocity and immediate pitch changes, and checks Tarrega/PVC excitation.

OceanAudio Release is the requested installation target; OceanodeScroller was
also built during the initial port. Its existing manual-Cocoa
macro dialog unit required compilation without ARC; that source was not changed.
Restart OceanAudio and its SC server after installation to load the new
node and native plugin. `validation.json` records the measured results.

The dry resonance regression closes a 100 ms gate on Tarrega and Coolharp,
then measures tail energy and fundamental periodicity after 500 ms with no
original effects or extra inserts. Both must retain a tonal resonator tail.
