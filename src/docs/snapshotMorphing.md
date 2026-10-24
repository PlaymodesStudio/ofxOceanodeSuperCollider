# Snapshot audio morph policies

`scSnapshots::morph` owns the shared audio policy evaluator (`scMorphPolicies.h`).
`scSnapshots` prepares it for every recall, A/B morph and timed glide before
calling the host's existing `apply` function. `Host::morphChanged` then resends
resolved audio values, including when the editor's boolean has not changed.

Checkboxes, published parameters and normal snapshot fields retain their types.
The engines read effective activation/depth/weights instead of converting the
editor boolean to 0/1. Unregistered switches retain Morph Switch's Start/Middle/End
behavior. Existing numeric/logarithmic interpolation remains unchanged.

## Explicit policies

- `bypass(parameter, optionalReader)`: a continuous 0..1 activation. Use
  `activation(parameter)` for DSP values **and** effect lifecycle decisions.
- `continuous(parameter, optionalReader)`: interpolate the numeric DSP value
  without clamping to 0..1. Use `value(parameter)` for signed switches such as
  SteamPipe feedback polarity. The editor can retain discrete endpoint values.
- `enabledDepth(enable, depth, optionalEnableReader, optionalDepthReader)`:
  interpolate endpoint depths after replacing disabled endpoint depths with zero.
  Use `value(depth)`; do not multiply this result by activation again. An enabled
  endpoint is a boolean true or a positive selector (e.g. FM source 0 = None).
- `resolved(parameter, evaluator)`: interpolate evaluated arrays (waveforms,
  envelopes or pitch/response tables) instead of switching a selector. Read them
  with `resolvedValue`; metadata retains the evaluated intermediate result.
- `neutralDepth(mode, depth, container)`: for stateful algorithms with a neutral
  carrier/dry path, fade depth to zero at the midpoint and restore it afterward.
  Call `pinSelectors` on copies passed to the ordinary parameter loader, so the
  switch occurs at zero depth even when Morph Switch is Start or End. Use
  `scModulation::morphVoiceValue` to fade routed modulation along with the base.
  This strategy changes the depth trajectory; it does not duplicate the engine.
- `neutralMode(weights, offIndex)`: resolve a single stateful processor's active
  algorithm and wet amount. Off-to-on starts at the first nonzero weight. Between
  active algorithms the wet amount reaches zero before the dominant mode changes.
  GrainBox uses this for its output filter, retaining the dry signal.
- `mode(parameter, count, optionalReader)`: interpolate one-hot mode weights.
  `weights(parameter, voice)` supports scalar and per-voice selectors and blends the actual endpoint modes, without sweeping through
  other enum values. Parallel DSP paths consume these weights. A subsequent morph
  can retain more than two nonzero weights when its starting sound was a blend.

Call `edited(parameter)` **before** its normal audio sender on manual changes.
During snapshot application this leaves effective values intact. `morphBlocked`
checks incoming connections so they keep precedence, including connections made
while a partial morph is held. Remove dynamic parameters with `erase` before
freeing them (SampleBox pads).

`field(key, "controls")` reads AnalogBox's nested controls. SampleBox registers a
reader for each pad's relative key; GrainBox's serialized scalar booleans and old
snapshots without policy metadata are also supported.

Captures add `_morphAudio` only for effective values that differ from the normal
stored state. This preserves partial bypasses, depths and mode distributions on
Store, recall and interrupted glides. Ordinary endpoint snapshots need no extra
metadata. Old application versions can still read their normal fields, but will
ignore a stored intermediate audio blend.

## Engine coverage

| Engine | Continuous activation / depth | Parallel mode blending |
| --- | --- | --- |
| PolyComb | Temporary voice-count banks; interpolation of evaluated oscillator/resonator map pitches | Count-dependent voice-bank outputs; inherited/independent resonator tuning |
| AnalogBox | All insert/filter bypasses; filter algorithm/type via zero wet; sync; frequency-shift sidebands; FM depth; oscillator presence, including raw FM/ring signals | FM source signals; echo filter types; free/synced oscillators |
| PhysicalBox | Saturation, EQ, echo, reverb, space bypasses; radiator/sympathetic-set changes through neutral depth | Echo filter types |
| AliasingBox | Shared inserts; internal filter, notch, DC block and bidirectional read | Echo filter types; read/write quality; internal filter LFO waveform |
| SteamPipeBox | Shared inserts; allpass activation, noise-filter slope and signed feedback polarity | Echo filter types |
| FMBox | Shared inserts; matrix/envelope model, sync and operator ratio/fixed blend; native LFO wave/sync via zero modulation depth | Echo filters, operator keyboard-scaling curves, algorithm-dependent feedback-cell weights |
| SlicerBox | EQ, filter, compressor, saturation, echo and reverb; automatic gain toggles | Filter types/slopes and echo filter types |
| EchoBox | Reverb activation, with existing tail handling | — |
| AdditiveBox | EQ/echo/reverb wet amounts; shared common voices and count-dependent presence; unison changes at zero side-voice depth | Evaluated spectral/filter/sweep/pan modes and echo filters |
| GrainBox | EQ/echo/reverb wet amounts; individual voice presence; output-filter activation/algorithm via dry neutral path | Echo filter types |
| PulsarBox | Temporary voice/layer configuration banks; continuous Hz/ratio formant selection | Evaluated pulsaret/envelope tables and pan modes |
| WavetableBox | B-source changes at zero FM/feedback depth, including routed amounts | Per-voice warp modes |
| SampleBox | Per-pad filter and all pad insert bypasses | Per-pad filter types |

PolyComb snapshot voice counts now use temporary banks, as described in
`polyCombVoiceMorphing.md`. PulsarBox uses temporary configuration banks, while AdditiveBox, AnalogBox and
GrainBox share independent voices/oscillators and fade their presence. Other
structural layouts retain their existing behavior.
PolyComb pitch-map modes, scales and geometry interpolate their output pitches;
see `polyCombPitchMorphing.md`. This requires no additional audio bank for maps.
The shared layer also remains compatible with the other `scSnapshots` hosts.
Sequencing decisions, sample replacement, remaining graph/layout changes, routing,
retrigger/freeze semantics and unsupported engine modes still use their existing
discrete behavior. They require dedicated
engine support before registering a continuous policy. This is not a full
second-preset-engine crossfade.

The shared MixerPro echo/reverb retain their existing input-gating/tail behavior.
SampleBox and the legacy AdditiveBox/GrainBox effects fade wet contribution to
zero before leaving the chain. AnalogBox sync runs two oscillator paths. Evaluated mode blends reuse existing
alternatives; stateful filter algorithm changes use the dry neutral path.

## Validation

The common entry point keeps per-check logs and a machine-readable report:

```sh
python3 tools/snapshot_morph_test/regression.py --group quick
python3 tools/snapshot_morph_test/regression.py --group native
python3 tools/snapshot_morph_test/regression.py --group audio
```

Use `--check NAME` for one check, `--list` to inspect coverage, and
`--report-dir PATH` to keep separate runs. Passing these checks verifies their
listed behaviors; it does not certify the outstanding modes in the audit below.

- `python3 tools/snapshot_morph_test/run.py`: production policy code, including
  interrupted/reversed/stored morphs, mode weights, manual edits, incoming
  connections, defaults and nested/serialized snapshot schemas.
- `python3 tools/snapshot_morph_test/sound_controls_test.py`: execute production
  SteamPipe/Aliasing/FM policy registration and audio-send loops, including
  0.001 and both sides of the midpoint.
- `python3 tools/steampipebox/morph_test.py`: compile the production native kernel
  and verify fractional allpass/noise-filter mixing with non-silent inputs.
- `python3 tools/snapshot_morph_test/fx_lifecycle_test.py`: exercise the production
  effect replan method for preserved state, ordering, routing and channel changes.
- `python3 tools/snapshot_morph_test/audio_test.py`: compile the changed SynthDefs,
  render offline fractional bypasses and check continuity across the former
  midpoint mode switch. Requires the bundled sclang and installed scsynth.
- `python3 tools/steampipebox/primary_test.py` and
  `python3 tools/steampipebox/v2_test.py`: existing native DSP reference checks.

Rebuild the app for C++ changes and reload the recompiled SynthDefs. SteamPipe's
native source is generated from the EEL kernels; run
`ugens/OceanodeSteamPipe/build.sh` when changing them. Its rebuilt universal plugin
is included with this change.

## Audio-switch audit (October 2026)

A policy registration alone is insufficient: the snapshot reader, final audio
sender, DSP and effect lifecycle must all retain its fractional value. The
numeric-switch regression executes the production registration and sender loops
rather than testing only a reimplementation of the policy formulas.

SteamPipe's allpass already received fractional activation before this audit,
but it mixed delay taps **inside** the feedback loop. Changing the loop phase can
move its dominant resonance abruptly even with a continuous control. The native
wrapper now clones its current state on entering a partial activation and blends
two independent feedback-loop outputs. At either endpoint it retains that loop's
running state and parks the other: there is no second DSP loop running at idle.
A second delay buffer is reserved at construction so morphing never allocates
large buffers on the audio thread. The native regression verifies the exact
weighted output of the independent loops at 0.001 and across the midpoint.
Noise One Pole and Polarity were separate hard switches, now continuous too.
The rebuilt native plugin includes both the loop transition and noise-filter blend. Restart the SC server
after the startup synchronizer installs a changed plugin; reloading a SynthDef
alone cannot replace native code already loaded into scsynth.

The removed SteamPipe post-mix Delay/R66 controls remain in imported bank data
for compatibility; they no longer run audio DSP or need active morph policies.

Remaining discrete cases, after inspecting the Box-family snapshot and DSP paths:

| Box / shared subsystem | Dedicated transition work still required |
| --- | --- |
| AnalogBox | Filter count/routing and output/voice-pool layout changes. Oscillator counts preserve common oscillators; filter algorithms/types now use neutral wet depth. |
| SteamPipeBox | Original/curved ADSR engine, V1/V2 resonator algorithm, legato/retrigger and gate rules; placement. The node Voices pin is not part of its sound snapshots. |
| AliasingBox | Gate-sync/retrigger rules and placement (Voices is outside sound snapshots). Audio read/write quality and internal filter LFO waveform now blend endpoint weights. |
| FMBox | DX7 quantization tables and note/retrigger rules. Fixed coarse tuning now blends evaluated endpoint decades. LFO wave/sync now uses zero depth; keyboard-scaling curves and algorithm-dependent feedback-cell selection blend. |
| AdditiveBox | Partial-cap changes now retain source/core graphs in temporary weighted banks; count-dependent modulation generators can still reset when their width changes. Spectral/filter/sweep/pan modes now blend evaluated values; unison changes use zero side-voice depth. |
| GrainBox | Sample/source replacement, grainless/grain variants, output layout, scale/clock/trigger rules and controller-capacity changes. Individual voice counts now fade presence and retire after the gain settles. |
| SampleBox | Sample replacement, playback/trigger/loop modes and pad layout. Filter and insert bypasses/types use existing policies. |
| PhysicalBox | Body-family/doublet changes and gate rules (Voices is outside sound snapshots). Radiator/sympathetic-set changes now use neutral depth; fractional modal counts fade new resonators and the count-dependent cascade split. |
| PulsarBox | Rhythm/sieve/trigger semantics and manually changed output layout. Tables, formant Hz/ratio selection and voice/layer snapshot counts now have explicit transitions. |
| WavetableBox | Table replacement, trigger/oversampling and geometry/count changes. Warp modes now blend per-voice phase results; B-source changes pass through zero FM and feedback, including routed modulation. |
| SlicerBox / EchoBox | Slice/trigger/clock/freeze/reverse and routing decisions. Supported filter and effect activation policies remain continuous. |
| Shared modulation | LFO model/waveform/reset/sync and envelope trigger semantics still need state-aware transitions. Route amounts interpolate; that alone does not blend source generators. |
| PolyComb | Its evaluated pitch maps and temporary voice banks retain their dedicated implementation; generic unsupported routing/trigger modes remain discrete. |

This list distinguishes supported audio transitions from outstanding engine work;
it is not a claim that every preset pair in every Box is already seamless.
Sequencer/data/analysis Boxes do not get sound-engine crossfades. For a new audio
mode, first look for parallel results already computed by the DSP. Blend those
with endpoint weights; reserve temporary additional processing for stateful
alternatives that cannot share a path. Never allocate a permanent second engine
just to make an ordinary bypass smooth.

### AdditiveBox count transitions and teardown

`scVoiceCountMorph` shares endpoint weights and stored/interrupted count metadata
with PolyComb. AdditiveBox uses its `presence()` strategy because its oscillator
voices are individually addressable: common voices retain their synths, extra
voices appear at the first nonzero morph amount, and outgoing voices fade before
a short retirement delay. Only the active endpoint's voices remain afterward.
Both the computed voice amp and each unison core's amp consume the presence gain.
Modulation route amounts now interpolate instead of switching their whole matrix.
The connected Voices input and incoming published controls keep precedence.

AdditiveBox blends evaluated spectral/filter/sweep/pan results and count-dependent
spectral normalization, blur, noise and sweep coefficients. Partial-cap changes
retain the existing bank and introduce a temporary destination bank at the first
nonzero weight. Both share one stable note writer per voice, so gate, velocity
and random state survive bank promotion. Outgoing banks retire after their gain
settles; settled endpoints run one bank. Changes within a cap blend coefficients
without creating a second bank. Modulation-generator width changes remain an
explicit audit item.

The shared `ofxSuperCollider` synth destructor previously replayed buffered
controls when deleting a wrapper, including after its parent group was freed.
It now discards those queues; `free()` already sends its command immediately.
This prevents late `/n_set(n)` and mapping commands to destroyed nodes during
fast transitions, including pending synths whose `/n_go` has not arrived.
Rebuild with the matching `ofxSuperCollider` change as well as this addon.

- `python3 tools/additivebox/morph_test.py`: production apply/load/count/lifecycle
  code with server adapters; early presence, stable common IDs, retirement,
  stored/interrupted blends, route amounts and incoming-control precedence.
- `python3 tools/additivebox/partial_morph_render_test.py`: capture production
  C++ OSC and render 8 ↔ 256 partials, including reversal, linear early presence,
  retained IDs, stable note writers and retirement; ASan/UBSan checks lifecycle.
- `python3 tools/physicalbox/morph_test.py`: native fractional modal-count audio
  and cascade-boundary checks. `--baseline-plugin <directory>` additionally null
  tests integer endpoints against an earlier plugin build.
- `python3 tools/additivebox/morph_render_test.py`: render the production additive
  core with early/forward/reversed voice-presence gains and an unchanged common voice.
- `python3 tools/snapshot_morph_test/node_lifecycle_test.py`: real
  ofxSuperCollider OSC capture, built with ASan/UBSan; no queued writes after
  group destruction at 1/4/16 voices.


## Shared routing and evaluated modes (October 24 audit)

`scModulationMorph` interpolates legacy route matrices and arbitrary parameter
bases/amounts across Analog, Additive, Aliasing, SteamPipe, FM, Physical, Echo,
Grain, Sample and Wavetable. Pulsar also uses it. Absent routes contribute zero;
source counts stay large enough for both endpoints during an interior morph.
Incoming published controls retain precedence. Analog keeps its existing nested
matrix schema; Sample rewrites per-pad keys before applying the common helper.
Stored/interrupted blends retain their resulting route amounts and bases.

`scConfigurationMorph` provides validated, normalized configuration weights and
stored/interrupted metadata. Pulsar consumes it for temporary voice/layer graphs.
Both graphs continue receiving notes, pitch cables, tables and modulation. Their
output gains fade, zero-weight graphs retire after the lag settles, and only the
settled configuration runs afterward. Reversing preserves a contributing graph;
modulation stays ordered before every rendering bank. This helper is deliberately
separate from UI parameter interpolation: graph ownership remains in the engine.

FM's algorithm/scaling weights and evaluated fixed-coarse tuning are packed after the legacy `algorithm` control's
first element. Scalar clients still set that first element normally. This avoids
exceeding SuperCollider's limit of 255 control names in the DX7 voice. Evaluated
decades blend the actual endpoints (coarse 2 and 6 both mean 100 Hz), while live
coarse modulation offsets that tuning continuously during the blend. Scalar
legacy clients retain the original decade lookup. Stored/interrupted morphs keep
the evaluated tuning through the shared resolved-value metadata.

PhysicalVoice accepts fractional modal counts: it keeps existing resonators,
fades contact/pickup contributions of the newly entering mode, and interpolates
count-dependent normalization and cascade partition. Integer endpoints retain
the previous DSP result exactly. Rebuild/use the bundled OceanodePhysical plugin
to pick up this native change.

Additional checks:

- `tools/fmbox/tuning_morph_test.py`: actual FM audio stays identical across the
  full coarse 2 → 6 morph, including .001 and both sides of .5.
- `tools/snapshot_morph_test/modulation_test.py`: production shared route/base
  interpolation, missing routes, stored blends and incoming control precedence.
- `tools/pulsarbox/morph_test.py`: production graph-bank lifecycle; early creation,
  retained IDs, midpoint, reversal/store/resume, deferred retirement and idle cleanup.
- `tools/snapshot_morph_test/engine_contract_test.py`: real Box setup and snapshot
  entry points with real OF/core code under ASan/UBSan; no audio device or server.
- `tools/snapshot_morph_test/bank_audio_test.py`: production Pulsar/Grain DSP at
  gains 0, .001, .499, .501 and 1; reduced generator iteration ranges and an
  unscaled Grain signal tap for exact comparison of each random stream.
- `tools/wavetablebox/render_test.py`: all 64 1D/2D/live/channel variants, note
  gates, release and per-voice pitch routing. The generator now normalizes its
  single-channel controls to arrays, fixing a source compilation failure found
  while compiling this audit's warp changes.

These checks cover specific transitions, not every arbitrary preset pair or live
hardware behavior. The remaining discrete cases above are known limitations.
