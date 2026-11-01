# Shared LFO outputs

Right-click the bars at the top of an LFO card and choose **Publish LFO output
to Node GUI**. The `LFO 1 Out` (etc.) output carries a float vector in 0..1,
one value per voice. AdditiveBox exports the displayed partial vector when
that LFO is running across partials. SampleBox pads and GrainBox engines
have separately named outputs.

The outputs keep updating when the editor is closed. Server values are polled
at approximately 30 Hz and forwarded on node updates; these are Oceanode
control values, rather than an audio-rate cable. A removed/inactive LFO emits
zeros. Disconnect an output before unpublishing it.

Publication is saved with the patch and restored before connections, separately
from sound snapshots. Removing a pad or engine removes its output pins too.
No output parameters or extra server polling are created until publication.

Run `python3 tools/modulation/lfo_outputs_test.py` for lifecycle, persistence,
hidden-window polling, vector sizing and connection protection checks.

## Scalar controls and voice modulation

A voice control can remain a scalar in the editor and node GUI. Hosts register
these controls with `addVoiceParam()` and read `voiceValue()` / `voiceValues()`
when sending their DSP inputs. The parameter itself stays at its shared base;
each voice receives that base plus its own modulation, clipped to the control's
range. For LFOs the offset is `(value - 0.5) * amount * range`. Envelopes and
velocity add unipolar offsets. Shared effects and structural selectors keep
shared controls.

`voiceParamsChanged` also refreshes DSP inputs when only later voices change.
Snapshots save the scalar base and modulation routes, without storing live
per-voice values. Existing generic routes gain per-voice behavior automatically
when their host registers the control; matrix target indices are unchanged.

Noise LFOs interpolate random values with a smooth quintic curve, consistently in shared
ModLfo/ModLfoX, AdditiveBox, GrainBox and C++ previews. Each voice has staggered interpolation knots. There is no gradient lattice
forcing all voices through the middle value at integer times. Noise Scale
zero intentionally gives all voices the same noise trajectory.

Additional checks:

- `python3 tools/modulation/voice_params_test.py`: production routing code,
  stable scalar bases, edits, secondary-voice changes, resize and removal.
- `python3 tools/modulation/noise_algorithm_test.py`: production C++ algorithm,
  collective crossings/stops, continuity, field independence and shared SC code.
- `python3 tools/modulation/noise_render_test.py`: offline audio regression for
  noise independence and independent envelope attacks (compile SynthDefs first).

### Validation status (2026-10-22)

The combined OceanAudio Release build and all default SynthDef compilations
passed. The latest sources and matching compiled definitions are installed in
OceanAudio. The full audit found no duplicate filenames or missing generic
metadata/mono/stereo builds across 418 generic source files.

`noise_render_test.py` and `tools/steampipebox/panning_render_test.py` pass
against OceanAudio's installed SynthDefs, including independently phased LFO
indices, live Pan telemetry, ring wrapping and independent envelope attacks.
Both render tests accept `--synthdefs /path/to/Supercollider/Synthdefs`.

The production C++ routing, noise algorithm, LFO publication and factory panning
regression checks also pass. The original-mode SteamPipe kernel reference
hashes remain unchanged. Restart the application/server to replace any synths
already running with older definitions.

## Shared Ramp generators

**+ Add Ramp** adds a Ramp to any box using the shared modulation editor.
Ramps and LFOs share eight source slots, preserving existing route IDs. Route
**Ramp N** to Pos or any other target in Routing. The GrainBox editor calls
this tab **Modulators**. Gate / Go starts each voice independently; seconds
or Beats set the duration. After Time can hold, loop, or continue the final
slope. Bounds can clamp or wrap the output, or leave it unbounded.

From / To set values; **Integrate rate** instead integrates those rates in
spans per cycle, which supports acceleration and tape braking. Curve shapes
the transition. **Pow** and **BiPow** ease normalized time, using the shared
LFO's rational shaping. Both default to zero. In rate mode, easing time also
changes instantaneous speed; continuation follows the eased endpoint slope.
The preview and AdditiveBox partial tables use the same C++ reference math
as the Ramp DSP. Existing LFO presets still select the LFO engine.

GrainFxBox factory Motion effects and saved Motion settings translate on
recall to Ramp -> Pos. **Freeze On Gate** controls recording independently;
**Follow Scan Pitch** derives grain pitch from Pos movement where needed.
The separate Tape / legacy motion UI and DSP have been removed. Saved preset
files are not rewritten. Migration uses an unused generator slot and preserves
other routes; a save using all eight slots needs a slot freed for conversion.

Run `python3 tools/grainfxbox/routing_test.py` for migration, easing/preview
agreement, Reaktor reference trajectories and rendered tape/stutter checks.
