# PolyComb morphing: consolidated reference

PolyComb's local snapshots and MacroMixer share one sound-morph path. This is
the reference implementation before adding adapters for other Box synths.
Parameter discovery, user exclusions and broader modulation-routing policies
are separate future work.

## Code boundaries

| Component | Responsibility |
| --- | --- |
| `scSnapshots` | Sound capture/application with audio policies; local snapshot UI and external morph entry points |
| `scPolyComb::applyState` | Apply sound controls, modulation state and effective pitch/count blends |
| `scPolyCombPitchMap.h` | Pure map generation; `playedPitches` supplies the live root, permutation and final clipping for both playback and morphs |
| `scPolyCombPitchMorph.h` | Weighted endpoint maps and snapshot metadata; interpolate generated pitches |
| `scVoiceCountMorph.h` | Voice-count distributions and their snapshot metadata |
| `scPolyCombVoiceMorph.inl` | Temporary banks, startup, shared-FX routing, adoption and cleanup |
| `scModulation::adoptServerState` | Transfer live modulation resources without restarting them |
| MacroMixer node morph hooks | Convert saved macro state, capture the current sound and delegate progress/cancellation |

The renderer implementation remains an included `.inl`, so existing projects do
not need another translation unit. There is no general synth-graph abstraction
or new parameter registry in this change.

## Behavior to preserve

- Ordinary playback and same-count morphs allocate no extra voice banks, mixer
  synths or buses. Different counts need additional banks only during transition
  and cleanup. Endpoint cleanup returns to the normal resource count.
- Destination voices contribute on the first positive morph step. Realtime
  startup waits for node acknowledgments; it is not tied to the midpoint.
- Complete endpoint pitch maps are evaluated independently, then their MIDI
  pitches are blended. Scales and map modes do not replace the generated tuning
  at 0.5. The editor dropdown still shows an endpoint's selection.
- Played roots and connected map controls remain live. The inherited fraction
  of a resonator map keeps following the oscillator bus.
- Shared effects and the adopted bank's oscillator, envelope and modulation
  state survive count promotion. Promotion does not rebuild the live bank.
- Stored/interrupted local snapshot blends retain `_voiceCountBlend` and
  `_pitchMapBlend` metadata. Old snapshots keep their existing fallbacks.
- A published connected control remains under its driver. MacroMixer does not
  also interpolate registered fields owned by the node's sound morph.
- Cancellation restores the sound captured at transition start through the same
  application path. A failed application restores the listener-suppression flag;
  this is not a transactional rollback of every partially applied control.

Details: [voice banks](polyCombVoiceMorphing.md),
[pitch maps](polyCombPitchMorphing.md),
[MacroMixer integration](macroMixerSoundMorphing.md),
[shared audio policies](snapshotMorphing.md).

## Repeatable regression commands

From the addon root:

```sh
python3 tools/polycomb/morph_regression.py
python3 tools/polycomb/morph_regression.py --group audio
python3 tools/polycomb/morph_regression.py --group native
# Or run all three groups sequentially:
python3 tools/polycomb/morph_regression.py --group all
```

`--list` shows the selected checks. `--report-dir /tmp/my-polycomb-check` selects
where logs and `report.json` are retained; otherwise a unique temporary directory
is created. Failures, timeouts and missing prerequisites make the command fail.
Checks run sequentially because some compile into the same SynthDef cache.

| Group | Coverage and prerequisites |
| --- | --- |
| `quick` (default) | Pitch arithmetic/geometry, metadata, lifecycle and ownership with simulated backends, seed/note routing, legacy recall and shared policies. Requires clang++ and the openFrameworks JSON headers. No SC executable or device is needed. |
| `audio` | Compiles current Parts/Extras first, then renders count mixing, resonator map/follow, pitch lag, vibrato and structural startup. Requires bundled sclang, macOS SuperCollider/scsynth, OceanodeAnalog and NumPy for the vibrato analyses. No audio device is opened. |
| `native` | Fresh ASan/UBSan build of real OF, MacroMixer and PolyComb; captures control messages and checks node identity, driver precedence, interruption and voice-bank pitch targets. Requires neighboring Oceanode, SuperCollider, OSC and ImGui addons plus the compiled macOS OF libraries. Backend acknowledgments are simulated; it does not boot scsynth or audition a running application. |

The native group rebuilds all translation units. The former development-only
object-reuse option has been removed to avoid stale header/ownership layouts.
The quick legacy-state check explicitly omits the separate historical compiled
binary-hash audit; it does not weaken or change that audit's default command.
The broader Oceanode graph-morph suite is separate from this PolyComb suite;
its existing DotMaker assertion issue is described in the integration notes.

## Reference cases and limits

`tools/polycomb/fixtures/morph_reference.json` preserves three sparse regression
pairs: different counts, different map modes with inheritance, and different
scale lengths/geometry. The map-mode pair has fixed expected quarter-morph
pitches used by arithmetic and native integration tests. These states assume a
fresh default node; they are not an installed or auditioned musical demo bank.
The report records the fixture hash so a changed baseline is visible.

Output-channel changes retain structural behavior. Pitch External retains its
existing source-selection behavior. Unsupported discrete operations remain
discrete; this work does not promise seamless arbitrary graph transformations.
Connected-control tests do not establish all fixed-to-modulated or
modulated-to-modulated graph transitions. Those still need dedicated testing.

Resource-count checks establish allocation/cleanup behavior, not measured CPU
cost. Profiling and live MacroMixer audition are separate next steps. Suite
durations are test wall times, not DSP performance measurements.

## Build and verification

Rebuild/restart the host against both addons. Compile current PolyComb Parts and
Extras and reload the definitions before creating the synths; the `audio` group
performs that compilation into the addon's ignored cache. Tests do not install
into or replace the application's snapshot library. Temporary scores/renders go
under the system temporary directory.

Before extending another synth, require the quick, audio and native groups to
pass, then audition representative saved sounds through both local snapshots
and MacroMixer. Keep changes to this contract explicit.
