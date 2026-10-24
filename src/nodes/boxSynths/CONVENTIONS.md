# Box synth conventions

This is the interaction and implementation reference for the Box family in
**SuperCollider → boxSynths**. Use it when adding a Box, extending an existing
one, or checking whether an editor behaves consistently with the others.

Use one subfolder per synth for its node, engine, data and views; keep common
components in `shared/`. The folder includes instruments, effects and sequencers. Note input, a piano
and instrument tracks apply to instruments. The shared helpers define the
common behavior; each engine chooses its sound controls, effects and targets.
The [coverage table](#current-coverage) distinguishes conventions from features
that individual nodes currently implement.

## Node and editor

- Keep the patch node compact: **Show**, essential performance/routing
  controls, and controls explicitly published by the user.
- **Show** opens the full editor. Organize sound controls in tabs or sections,
  keep the snapshot bar accessible, and place the instrument piano at the bottom.
- A published control and its editor widget edit the **same parameter**.
  Publishing must not create another independently stored value.
- Preserve names, types, ranges and dropdown options when publishing.
  A named mode selector should remain a named dropdown on the node.
- Implement `getWindowTitle()` and `getWindowToggle()` so Mixer Pro's
  **Go to origin synth window** can open the correct instance.
- Respect UI zoom. Controls must remain reachable when resizing the window;
  the piano stays below a scrollable content area.

## Right-click behavior

Right-click a parameter widget to offer the applicable operations below.
Older hosts may use slightly different labels, but their meaning should agree.

| Operation | Expected behavior |
| --- | --- |
| **Value** / set value | Type an exact value in the existing type and units. Enter applies and closes; leaving an edited field also applies. |
| **Publish to Node GUI** | Expose the existing parameter on the compact node. Reveal its widget if it was published only for timeline use. |
| **Unpublish from Node** | Remove its widget without resetting the sound. Protect existing input/output connections. Keep a hidden parameter if MIDI or the timeline still needs it. |
| **Add to Timeline Track** | Bind the parameter to a new or existing compatible automation track. |
| **Add to Timeline as Piano Roll...** | Open note-group setup using this parameter as a starting point. This differs from registering the entire synth as an instrument. |
| **Remove from Timeline** | Remove its binding; offer a track choice when there are several. |
| **Bind MIDI** | Learn the next MIDI control/note using Oceanode's native binding system. Publish the parameter internally if needed. |
| **Unbind last MIDI** | Remove its most recently added ordinary MIDI binding; keep the parameter while any other MIDI or timeline binding still needs it. |
| **Modulate with...** | Assign a supported numeric control to a modulation source, using a fixed DSP target or an additional Routing column. |

For vectors, accept one value or comma-separated values, such as
`60, 64, 67`. Instrument vectors normally repeat cyclically to fill the active
voices; pad/channel mappings can have their own meaning. Scalars retain their
scalar type. Boolean and enum parsing follows the host editor; numeric
`0` / `1` is the portable boolean form.

A **bottom underline** marks controls visibly published on the node.
A **colored top line** marks timeline automation in the track's color.
Both marks may appear on the same control.

### Hidden publication for timeline and MIDI

Automation needs a real node parameter with a stable path. Adding an
unpublished editor control to a track publishes it internally and hides its
node widget, so automation does not expand the patch node unnecessarily.

**Publish to Node GUI** reveals this hidden parameter. Unpublishing while a
track still uses it hides the widget instead of breaking the binding. Once no
track or MIDI binding uses a hidden parameter, the helper removes its temporary
publication.
Restore publications and hidden flags before timeline bindings resolve.

Reuse [scTimelineMenu](../../ui/scTimelineMenu.h), including its update and
save/load hooks. Drawing its menu alone does not provide cleanup or restoration.

## MIDI binding from editor controls

Offer **Bind MIDI** and **Unbind last MIDI** in parameter right-click menus,
including sound, effect, LFO/envelope and snapshot/morph controls. Use the same
host-container binding API as Oceanode's normal node parameter menu, so learning,
port/channel selection, mappings and patch persistence use the existing MIDI
system. After choosing Bind MIDI, move the controller or send the note to learn.
Multiple bindings can target the same parameter; unbinding removes the last one.

The actions require a build with `OFXOCEANODE_USE_MIDI`. They remain available
when the optional timeline addon is absent. Supported parameters are float,
int, bool, trigger, and float/int vectors; read-only outputs are excluded.
For vector mapping details, use the native MIDI binding settings.

An unpublished editor control is published internally with its node widget
hidden when MIDI learning begins. **Publish to Node GUI** reveals it. Hiding
its widget must preserve the MIDI binding, and pruning must retain it while
ordinary **or persistent** MIDI bindings exist. Removing the final ordinary
binding releases a hidden publication only if neither another MIDI binding nor
a timeline track needs it. Persistent bindings remain managed by the host's
persistent MIDI configuration; **Unbind last MIDI** follows the ordinary node
menu and does not remove those bindings.

Reuse `scTimelineMenu::menuItems()` for these MIDI actions as well as timeline
actions. The helper retains its historical name, but handles both systems.
Do not duplicate MIDI learn/listener or save/load code in individual synths.
Existing node-level controls already use Oceanode's native parameter popup;
custom editor popups must call the shared helper. Unsupported publication or
binding attempts must not leave orphaned hidden parameters.

## Snapshot manager, banks and morph

Use [scSnapshots](shared/scSnapshots.h), including its common bar and manager,
rather than introducing another preset system for a new Box.

| Control | Purpose |
| --- | --- |
| **Snapshot**, previous/next arrows | Recall a stored sound. The shared collection supports up to 128 slots. |
| **Store...** | Capture the current sound, choose a name and destination bank, and create or replace a snapshot. |
| **Manage...** | Rename, delete and reorder snapshots. |
| **Banks** | Create/load banks, save under a name, restore the factory collection, import from a file, or clear the collection. |
| **Glide**, **Time** | Transition on recall over seconds or tempo-relative beats. Alt reverses the Glide choice for a GUI recall. |
| **Morph A**, **Morph B**, **Morph** | Blend the selected endpoints: 0 is A, 1 is B. |
| Morph **...** settings | Choose Linear, Ease, Exp or Log progress and the start/middle/end switch point for ordinary switches. |

Snapshot and morph parameters support their usual publish/automation context
menus. Slot parameters use 1-based snapshot numbers; 0 means no selection.

### Capture and restore

Capture sound controls, applicable effects, modulation settings, source counts,
matrix amounts and additional parameter routes. Save modulation **base values**,
not transient values written by a running source. Recreate required pads,
engines and sources before applying their settings.

Define exclusions explicitly. Live gates, momentary triggers and
connection-fed input data must not become held notes or fire unexpectedly on
recall. Other exclusions, such as live pitch or master level, depend on the
host. Existing Boxes do not all capture identical fields.

Externally connected controls retain their driver's precedence during recall
and morph. Supply the host's connection checks as well as its capture/apply
callbacks. Patch presets additionally preserve publication, editor state and
timeline-only parameters: a sound snapshot and an entire patch have different
scopes.

Interpolate continuous values and modulation amounts, using logarithmic
interpolation where appropriate. Ordinary enums and structural changes switch
at the selected point unless the DSP explicitly implements a crossfade.

Where supported, register continuous bypass, enabled-depth and mode-weight
policies with `scSnapshots::morph`. DSP must consume the resolved values, and
manual edits must notify the policy layer. Stored intermediate sounds and
interrupted transitions should retain the effective blend.

Morph does not automatically crossfade two complete synth graphs. Sample
replacement, new filter algorithms and voice-count transitions need
engine-specific handling. See [snapshot audio policies](../../docs/snapshotMorphing.md)
and the [PolyComb reference](../../docs/polyCombMorphing.md) for supported behavior.

### Storage

These paths are relative to the application's data directory:

```text
nodeSnapshots/<BoxName>/
    library.json
    snapshot_<slot>.json
    banks/<bank>.json
    Presets/<named preset>.json
```

The library is shared between instances of the same node type. Keep folder
names and serialized parameter keys stable. Renames need migration for both
libraries and saved patch/macro node references. Factory sounds should begin
with complete defaults so omitted fields do not inherit a previously played sound.

## Modulation routing matrix

Use [scModulation](shared/scModulation.h) for source controls, editor cards,
right-click modulation and the Routing tab.

The shared layout provides up to **8 LFOs**, **4 envelopes**, **Velocity**,
**Random**, **8 external sources** and **Voice**. Hosts can set smaller limits
or add sources. Create sources as needed rather than running every possible one.

In Routing, sources are rows and destinations are columns:

- Drag a cell horizontally to change its signed amount.
- Hold **Shift** for fine adjustment.
- Double-click to reset a cell to zero.
- Right-click to type an amount or access row/column operations.
- Zero means no contribution; negative amounts reverse the contribution.
- Interpret depth through the target's registered range and displayed units.
  Amount 1 means that modulation range, not necessarily one native unit.

Fixed engine targets use server modulation buses. LFOs are bipolar for ordinary
targets and unipolar for explicitly registered targets such as some positions.
An envelope routed to Amp can act as the VCA. Keep its release audible after
Gate closes; do not put another hard gate after it.

**Modulate with...** can add other numeric controls as matrix columns.
Generic parameter routes normally write at the node update rate; they are not
automatically audio-rate modulation. Scalar voice controls can register with
`addVoiceParam()` and use `voiceValue()` / `voiceValues()` to preserve a stable
shared base while applying different values to each voice. Shared effects and
structural controls remain shared unless explicitly handled otherwise.

Keep target indices stable: append new targets so saved matrices retain their
meaning. Save route amounts and bases, then use the shared load/after-load hooks.

## Note gates and velocity

All Box instrument **Gate** inputs (SampleBox calls it **Trigger/Gate**) use
`ofParameter<std::vector<float>>`, normalized to 0..1, including single-voice
instruments. Zero releases the note;
a positive value opens it and carries its normalized MIDI velocity (MIDI
velocity / 127). Preserve fractional levels through parameter connections,
scheduled notes, audio cables and DSP sends. Do not round, cast to int or
replace the level with a boolean before extracting velocity. A binary gate
of 1 remains a full-velocity note.

Use the gate level for the Velocity modulation source and envelope velocity
tracking. Do not expose a separate note **Velocity** input. Keep any internal
DSP velocity multiplier fixed at 1, with its separate cable selector disabled.
Velocity sensitivity controls, envelope tracking amounts and the Velocity
modulation source remain available.
Use a separate `gate > 0` signal for envelope timing and trigger detection.
Document any engine-specific retrigger/legato behavior for positive level
changes; preserve release tails after Gate returns to zero.

Older patches may have connected or automated a separate Velocity input.
Move that source to Gate (combine it with a binary note gate by multiplication
if needed). Recreate existing timeline note-group bindings that still refer
to the removed Velocity parameter; sound snapshots retain their sensitivity
settings.

## Envelopes

Use the shared ADSR cards: Attack, Decay, Sustain, Release, curve controls and
available velocity controls. The editable curve and live voice bars should
agree with the running envelopes. Offer source addition/removal within the
host's supported limit.

Feed envelopes with the instrument's actual note gates. Distinguish modulation
envelopes from any dedicated amp envelope. An envelope changes the sound only
when routed, unless the host explicitly initializes a role such as Env 1 → Amp.

Document gate-mode precedence. A common **Auto / Gated / Drone** pattern drones
until Gate is used, then follows notes; explicit Drone keeps continuous
playback. Sample trigger/one-shot behavior has its own rules.

## LFOs

Reuse shared LFO cards: synced beats or free Hz, phase, waveform shape,
noise/random behavior, shaping and quantization, as supported by the host.
Per-voice hosts also expose phase distribution/indexer controls. AdditiveBox's
per-partial spread and GrainBox's sample-region timing are engine extensions.

The graph shows the shape, live bars show source values, and markers show voice
phase offsets. Reset existing LFOs without rebuilding the entire sound graph.
Forward host tempo through `setBpm()`.

Right-click the graph/bars to **Publish LFO output to Node GUI**. The pin is a
read-only float vector in 0..1. It keeps updating with the editor closed; an
inactive/removed LFO emits zeros. Disconnect outgoing cables before unpublishing.
Save and restore output publication before patch connections, independently of
sound snapshots. These pins carry control values, not audio cables.

## Shared effects and audio routing

Reuse established effect DSP and editors where appropriate. Common stages are
saturation, the shared EQ editor, echo and reverb. AnalogBox additionally offers
shaper, ring modulation, frequency shift and chorus; PhysicalBox also has Space.
Stage availability, order and whether processing is per pad, per voice or after
the mix belong to the engine, not to the Box name.

Effects should provide consistent enable/bypass and meaningful controls, with
the same typed-value, publish and timeline behavior as other sound controls.
Include effect settings in snapshots and use supported audio morph policies.

For effects created on demand:

- Configure parameters before starting them.
- Preserve a valid direct route while definitions load or creation is pending.
  A C++ wrapper alone does not prove that the server created a synth.
- Insert the stage after acknowledgment; preserve working audio if startup
  fails. Reuse [scFxRouting](../../audio/scFxRouting.h).
- Preserve delay/reverb tails before pausing/removing stages, according to the
  effect's implementation.
- Keep upstream input and downstream mixer routing valid when adding,
  bypassing, resuming or removing stages. Retain running DSP state across
  edits/morphs when the topology permits it.

Distinguish voice count from output speaker count where both exist. Audio
cables, matrix buses and timeline bindings are separate routing systems.

## Keyboard strokes and piano visualization

Instrument editors use [scKeyboardPiano](shared/scKeyboardPiano.h). Focus the
editor to play. The initial base is **C4 / MIDI 60**.

| Keys | Notes / action |
| --- | --- |
| `z x c v b n m , .` | White notes C4 D4 E4 F4 G4 A4 B4 C5 D5. |
| `s d g h j l ;` | Black notes C♯4 D♯4 F♯4 G♯4 A♯4 C♯5 D♯5. The semicolon position is **ñ** on a Spanish keyboard. |
| `q w e r t y u i o p` | White notes C5 D5 E5 F5 G5 A5 B5 C6 D6 E6. |
| `2 3 5 6 7 9 0` | Black notes C♯5 D♯5 F♯5 G♯5 A♯5 C♯6 D♯6. |
| **Up / Down**, **Oct + / Oct -** | Shift new notes by an octave. Held notes retain their pitches. |

`a f k` and `1 4 8` are deliberate gaps. Text entry and Ctrl/Cmd/Alt/Shift
combinations do not play notes. Physical-key notes release when the editor
loses focus or stops servicing keyboard input.

Hold several keys for chords up to the active voice count. Newest notes win
when voices are full; mono uses last-note priority. Keyboard and mouse notes
write ordinary Pitch/Gate parameters, preserving existing listeners, gate modes
and audio-input precedence.

The footer highlights actual Pitch/Gate activity, including external parameter
input. Click to play and drag for glissando. **Hold** latches clicked notes until
clicked again; disabling Hold releases them. **Release** stops local notes.
Folding **Piano** preserves latched notes and computer-keyboard playing while
reclaiming space. Hiding/collapsing the editor releases its local notes.

Use `beginContent()` / `endContent()` for the fixed footer, and call
`finishFrame()` even when hidden/collapsed. Release local notes during teardown.
SampleBox targets the selected pad/channel; GrainBox targets the selected engine
and respects MIDI/relative pitch mode. Preserve these specialized mappings.

## MIDI keyboard from the node title

With `OFXOCEANODE_USE_MIDI`, the node title's context menu offers
**Bind MIDI Keyboard (Pitch / Gate)** and **Unbind MIDI Keyboard (Pitch / Gate)**.
The first played note learns the device and channel. One shared
`ofxOceanodeMidiKeyboardBinding` drives Pitch and the float Gate level,
with polyphony, sustain, last-note priority and note release on unbinding.
Other ordinary MIDI mappings remain intact. Keyboard bindings save with the preset.

The host uses `getMidiKeyboardInstrument()` and the same role/voice descriptor
as the timeline. Nodes with unpublished note inputs can publish them when
`bind = true`; ordinary capability queries must not change the node.
GrainBox uses its first engine in MIDI pitch mode; SampleBox uses kit channels.

## Add as an instrument track in the timeline

There are two related entry points:

1. A parameter menu's **Add to Timeline Track** automates that parameter.
   **Add to Timeline as Piano Roll...** starts note-group configuration.
2. The **node's context menu** offers **Add to Timeline as Instrument...**
   when the model reports an instrument. Setup receives its note roles and
   voice mapping together.

Implement `getTimelineInstrument()` with valid node parameter handles for
Gate and Pitch, plus the input voice count. Set `out.velocity = nullptr` so
MIDI keyboard bindings and new timeline instrument tracks send note velocity
through the float Gate level. A kit sets `kit = true`:
MIDI key 36 / C1 maps to slot 0, with
successive keys selecting successive slots.

Keep handles stable while the timeline needs them. Report input note slots,
which can differ from an internal pool of extra release voices. GrainBox
currently advertises its first engine in MIDI pitch mode; SampleBox advertises
kit channels.

For scheduled delivery, use [scScheduledInputs](../../timing/scScheduledInputs.h)
and scheduling compatibility helpers. Register note parameters, respect
backend-send suppression in listeners, read values in effect when sending, and
unregister during teardown. Send scheduled notes with their timetags and avoid
a duplicate backend send when the GUI catches up.

Timeline menus require the optional timeline addon to be enabled and attached
to the host container. Compile instrument hooks under the appropriate feature
guard. A piano widget alone does not make a model a timeline instrument.

## Current coverage

This describes the current source. “Shared modulation” means the common
LFO/envelope/Routing module; specialized processing may exist independently.

| Nodes | Shared snapshots | Shared modulation | Piano | Instrument descriptor |
| --- | --- | --- | --- | --- |
| AdditiveBox, AliasingBox, AnalogBox, FMBox, PhysicalBox, PulsarBox, SteamPipeBox | Yes | Yes | Yes | Yes |
| GrainBox | Yes | Per engine | Per engine | First engine, MIDI pitch mode |
| SampleBox | Yes | Per pad | Selected pad/channel | Kit channels |
| WavetableBox | Yes | Yes | Yes | Yes, when Pitch and Gate are published |
| EchoBox | Yes | Yes | Not applicable | Not applicable |
| SlicerBox | Yes | No shared module | No | No |
| ArpeggiatorBox, ChordBox, EuclideanBox, MetroBox, RhythmBox | Yes | No shared module | No shared piano | No instrument descriptor |
| DataBox, SpectralFilterBox | No shared snapshot manager | No shared module | No | No |

WavetableBox currently has no shared insert-effects chain or
`scScheduledInputs` integration. These are separate from its modulation,
piano and parameter automation. Scheduled-note delivery remains work needed
for full instrument parity.

## Implementation and verification reference

| Responsibility | Reuse / examples |
| --- | --- |
| Control registry and publishing | [AnalogBox](scAnalogBox.cpp), [scDropdowns](../../ui/scDropdowns.h), [scTypedValue](../../ui/scTypedValue.h) |
| MIDI/timeline menus and hidden publication | [scTimelineMenu](../../ui/scTimelineMenu.h) and its implementation |
| Capture, banks, manager and morph | [scSnapshots](shared/scSnapshots.h), [audio policies](../../docs/snapshotMorphing.md) |
| LFOs, envelopes and routes | [scModulation](shared/scModulation.h) and its implementation |
| Piano and keyboard lifecycle | [scKeyboardPiano](shared/scKeyboardPiano.h) |
| Note roles and scheduled sends | [AnalogBox](scAnalogBox.cpp), [SampleBox](scSampleBox.cpp), [scScheduledInputs](../../timing/scScheduledInputs.h) |
| EQ and effect routing | [scEQEditor](../../ui/scEQEditor.h), [scFxRouting](../../audio/scFxRouting.h) |

Run relevant checks from the addon root:

```sh
python3 tools/parameter_menu_test/run.py
python3 tools/keyboard_piano_test/run.py
python3 tools/modulation/lfo_outputs_test.py
python3 tools/modulation/voice_params_test.py
python3 tools/snapshot_morph_test/run.py
python3 tools/snapshot_morph_test/fx_lifecycle_test.py
python3 tools/audio_routing_test/box_fx_startup_test.py
python3 tools/wavetablebox/render_test.py
```

Audio checks require current SynthDefs and installed SC/UGen dependencies.
Check patch restoration before connections, editor-close note release,
connected-control precedence, source/output resizing, tail preservation and
realtime/non-realtime ordering when relevant. Update the coverage table when
adding a missing convention to a Box.

### FMBox migration controls

FMBox defaults to its native DX7 matrix and envelopes. For imported SuperFM
sounds, **Matrix Model → Raw FM** keeps direct coefficients and all diagonal
feedback, while **Envelope Model → Seconds** keeps explicit stage durations
and **Envelope Curve**. The seconds fields, model selectors and curve use the
same publication, MIDI, timeline, snapshot and morph registry as the other
sound controls. AFX recall restores the native DX7 models.
