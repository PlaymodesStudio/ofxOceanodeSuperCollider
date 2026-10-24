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
- `enabledDepth(enable, depth, optionalEnableReader, optionalDepthReader)`:
  interpolate endpoint depths after replacing disabled endpoint depths with zero.
  Use `value(depth)`; do not multiply this result by activation again. An enabled
  endpoint is a boolean true or a positive selector (e.g. FM source 0 = None).
- `mode(parameter, count, optionalReader)`: interpolate one-hot mode weights.
  `weights(parameter)` blends the actual endpoint modes, without sweeping through
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
| AnalogBox | All insert bypasses, all filter bypasses, sync, frequency-shift sidebands; FM fades from/to zero | FM source signals; echo filter types; free/synced oscillators |
| PhysicalBox | Saturation, EQ, echo, reverb, space bypasses | Echo filter types |
| AliasingBox | Shared inserts; internal filter, notch and DC block | Echo filter types |
| SteamPipeBox | Shared inserts; original allpass, delay bypass and R66 left/right enables | Echo filter types |
| FMBox | Shared insert bypasses | Echo filter types |
| SlicerBox | EQ, filter, compressor, saturation, echo and reverb; automatic gain toggles | Filter types/slopes and echo filter types |
| EchoBox | Reverb activation, with existing tail handling | — |
| AdditiveBox / GrainBox | EQ, echo and reverb through their effective wet amounts; retained FX stages keep their DSP state | Echo filter types |
| SampleBox | Per-pad filter and all pad insert bypasses | Per-pad filter types |

PolyComb snapshot voice counts now use temporary banks, as described in
`polyCombVoiceMorphing.md`. Other structural counts retain their existing behavior.
PolyComb pitch-map modes, scales and geometry interpolate their output pitches;
see `polyCombPitchMorphing.md`. This requires no additional audio bank for maps.
The shared layer also remains compatible with the other `scSnapshots` hosts.
Sequencing decisions, sample replacement, voice/oscillator counts, routing,
retrigger/freeze semantics, AnalogBox filter algorithms/types and other unsupported
engine modes still use their existing discrete behavior. They require dedicated
engine support before registering a continuous policy. This is not a full
second-preset-engine crossfade.

The shared MixerPro echo/reverb retain their existing input-gating/tail behavior.
SampleBox and the legacy AdditiveBox/GrainBox effects fade wet contribution to
zero before leaving the chain. AnalogBox sync runs two oscillator paths; filter
mode crossfades reuse alternatives already computed by their DSP.

## Validation

- `python3 tools/snapshot_morph_test/run.py`: production policy code, including
  interrupted/reversed/stored morphs, mode weights, manual edits, incoming
  connections, defaults and nested/serialized snapshot schemas.
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
