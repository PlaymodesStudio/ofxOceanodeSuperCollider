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

The production C++ routing, noise algorithm, LFO publication and SteamPipe
panning regression checks pass. The SteamPipeBox source passes a syntax check
using OceanAudio's compiler flags; the 172 original-mode voice renders retain
their reference hashes.

The final combined application build, SynthDef compilation and offline audio
regression remain pending after an approval-review credit failure. These
changes have not been installed in OceanAudio. Build and install the matching
application and SynthDefs together before testing the updated controls.
